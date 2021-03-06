#define _FILE_OFFSET_BITS 64

#include "postgres.h"
#include <unistd.h>
#include "common/pg_lzcompress.h"
#include "storage/lwlock.h"
#include "storage/fd.h"
#include "lib/stringinfo.h"
#include "pgstat.h"

#include "pg_track_slow_queries.h"

/*
 * TSQEntry serialization function
 */
StringInfo pgtsq_serialize_entry(TSQEntry * tsqe)
{
	StringInfo	si;
	si = makeStringInfo();
	appendStringInfo(si, "%08x%s",
					 (int) strlen(tsqe->datetime), tsqe->datetime);
	appendStringInfo(si, "%08x%016.02f",
					 16, tsqe->duration);
	appendStringInfo(si, "%08x%s",
					 (int) strlen(tsqe->username), tsqe->username);
	appendStringInfo(si, "%08x%s",
					 (int) strlen(tsqe->appname), tsqe->appname);
	appendStringInfo(si, "%08x%s",
					 (int) strlen(tsqe->dbname), tsqe->dbname);
	appendStringInfo(si, "%08x%016li",
					 16, tsqe->temp_blks_written);
	appendStringInfo(si, "%08x%010.06f",
					 10, tsqe->hitratio);
	appendStringInfo(si, "%08x%016lu",
					 16, tsqe->ntuples);
	appendStringInfo(si, "%08x%s",
					 (uint32) strlen(tsqe->querytxt), tsqe->querytxt);
	appendStringInfo(si, "%08x%s",
					 (uint32) strlen(tsqe->plantxt), tsqe->plantxt);
	return si;
}

/*
 * Stores a row / serialized TSQEntry
 */
uint32 pgtsq_store_row(char * row, int length, bool compression, int max_file_size_kb)
{
	char		*buff = NULL;
	uint32		buff_size = -1;
	FILE		*file = NULL;
	off_t		pos = 0;
	long		row_size = 0;

	/* Try to compress data if compression is enabled */
	if (compression)
	{
		/*
		 * Allocate a buffer for compression as long as the original string in order
		 * to be sure to have enough space.
		 */
		if ((buff = (char *) palloc0(length)) == NULL)
		{
			ereport(LOG,
					(errmsg("pg_track_slow_queries: could not allocate memory")));
			return -1;
		}

		buff_size = pglz_compress(row, length, buff, NULL);
	}
	if (buff_size == -1)
		buff_size = 0;

	/* Acquire an exclusive lock before writing the entry */
	LWLockAcquire(pgtsqss->lock, LW_EXCLUSIVE);

	if ((file = AllocateFile(TSQ_FILE, PG_BINARY_A)) == NULL)
		goto write_error;

	/*
	 * If max_file_size_kb is set we have to check file size before adding
	 * a new record. We want to skip new records if file size could exceed
	 * max_file_size.
	 */
	if (max_file_size_kb != -1)
	{
		/* Get file current position */
		pos = ftello(file);

		/* Row size calculation */
		row_size += 8;
		if (buff_size > 0)
		{
			row_size += buff_size;
		} else {
			row_size += length;
		}

		if ((pos + row_size) > (max_file_size_kb * 1024))
		{
			ereport(LOG,
					(errmsg("pg_track_slow_queries: max_file_size reached")));
			buff_size = -1;
			goto end;
		}
	}

	/* Write compressed and original data size */
	if (fwrite(&buff_size, 1, sizeof(uint32), file) != sizeof(buff_size))
		goto write_error;
	if (fwrite(&length, 1, sizeof(int), file) != sizeof(length))
		goto write_error;

	if (buff_size == 0)
	{
		/*
		 * If buff_size == 0 it means pglz_compress hasn't compressed the
		 * StringInfo for some reasons, probably because there is no gain to
		 * compress it. In this case, let's store data uncompressed.
		 */
		if (fwrite(row, 1, length, file) != length)
			goto write_error;
	} else {
		if (fwrite(buff, 1, buff_size, file) != buff_size)
			goto write_error;
	}

end:
	LWLockRelease(pgtsqss->lock);
	FreeFile(file);

	if (buff != NULL)
		pfree(buff);

	return buff_size;

write_error:
	if (pgtsqss->lock)
		LWLockRelease(pgtsqss->lock);
	if (file)
		FreeFile(file);
	ereport(LOG,
			(errcode_for_file_access(),
			 errmsg("pg_track_slow_queries: could not write file \"%s\": %m",
					TSQ_FILE)));
	return -1;
}

/*
 * Parses an item from a row buffer. First 8 chars represents item's string length
 * (hex repr)
 */
void
pgtsq_parse_item(char * buffer, uint32 p, TSQItem * item)
{
	uint32		msg_length;
	char		header[9];

	strncpy(header, buffer + p, 8);
	errno = 0;
	msg_length = (uint32) strtol(header, NULL, 16);
	if (errno)
	{
		/* Conversion to long hasn't been performed */
		pfree(item);
		item = NULL;
		return;
	}
	item->data = pnstrdup(buffer + p + 8, msg_length);
	item->length = msg_length;
}

/*
 * Checks if a row (serialized entry) could be parsed
 */
bool
pgtsq_check_row(char * row)
{
	uint32		p = 0;
	TSQItem		*item = NULL;

	if ((item = (TSQItem *)palloc(sizeof(TSQItem))) == NULL)
	{
		ereport(LOG,
				(errmsg("pg_track_slow_queries: could not allocate memory")));
		return false;
	}

	for (int c = 1; c <= TSQ_COLS; c++)
	{
		pgtsq_parse_item(row, p, item);

		if (item == NULL)
			return false;

		p += item->length + 8;
		pfree(item->data);
	}
	pfree(item);
	return true;
}


/*
 * Parses a row (serialized)
 */
bool
pgtsq_parse_row(char * row, TSQEntry * tsqe)
{
	uint32		p = 0;
	TSQItem		*item = NULL;

	if ((item = (TSQItem *)palloc(sizeof(TSQItem))) == NULL)
	{
		ereport(LOG,
				(errmsg("pg_track_slow_queries: could not allocate memory")));
		return false;
	}

	/* Row items parsing and type conversion if needed*/
	for (int c = 1; c <= TSQ_COLS; c++)
	{
		pgtsq_parse_item(row, p, item);

		if (item == NULL)
			return false;

		p += item->length + 8;

		switch (c)
		{
			case 1:
				/* datetime */
				tsqe->datetime = item->data;
				break;
			case 2:
				/* duration */
				tsqe->duration = atof(item->data);
				pfree(item->data);
				break;
			case 3:
				/* username */
				tsqe->username = item->data;
				break;
			case 4:
				/* appname */
				tsqe->appname = item->data;
				break;
			case 5:
				/* dbname */
				tsqe->dbname = item->data;
				break;
			case 6:
				/* temp_blks_written */
				tsqe->temp_blks_written = atoi(item->data);
				pfree(item->data);
				break;
			case 7:
				/* hitratio */
				tsqe->hitratio = atof(item->data);
				pfree(item->data);
				break;
			case 8:
				/* ntuples */
				tsqe->ntuples = atoi(item->data);
				pfree(item->data);
				break;
			case 9:
				/* querytxt */
				tsqe->querytxt = item->data;
				break;
			case 10:
				/* plantxt */
				tsqe->plantxt = item->data;
				break;
			default:
				/* Not yet implemented */
				break;
		}
	}
	pfree(item);
	return true;
}

/*
 * Truncates storage file
 */
void
pgtsq_truncate_file(void)
{
	FILE	*file = NULL;

	LWLockAcquire(pgtsqss->lock, LW_EXCLUSIVE);
	file = AllocateFile(TSQ_FILE, PG_BINARY_W);
	LWLockRelease(pgtsqss->lock);
	if (file == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("pg_track_slow_queries: could not write file \"%s\": %m",
					TSQ_FILE)));
	FreeFile(file);
}
