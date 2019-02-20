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
 * Stores a serialized TSQEntry
 */
uint32 pgtsq_store_entry(StringInfo tsqe_s, bool compression)
{
	char		*buff;
	uint32		buff_size = -1;
	FILE		*file = NULL;

	/*
	 * Allocate a buffer for compression as long as the original string in order
	 * to be sure to have enough space.
	 */
	buff = (char *) palloc0(tsqe_s->len);

	/* Try to compress data if compression is enabled */
	if (compression)
		buff_size = pglz_compress(tsqe_s->data, tsqe_s->len, buff, NULL);

	if (buff_size == -1)
		buff_size = 0;

	/* Acquire an exclusive lock before writing the entry */
	LWLockAcquire(pgtsqss->lock, LW_EXCLUSIVE);
	file = AllocateFile(TSQ_FILE, PG_BINARY_A);
	if (file == NULL)
		goto write_error;

	/* Write compressed and original data size */
	if (fwrite(&buff_size, 1, sizeof(uint32), file) != sizeof(buff_size))
		goto write_error;
	if (fwrite(&tsqe_s->len, 1, sizeof(uint32), file) != sizeof(tsqe_s->len))
		goto write_error;

	if (buff_size == 0)
	{
		/*
		 * If buff_size == 0 it means pglz_compress hasn't compressed the
		 * StringInfo for some reasons, probably because there is no gain to
		 * compress it. In this case, let's store data uncompressed.
		 */
		if (fwrite(tsqe_s->data, 1, tsqe_s->len, file) != tsqe_s->len)
			goto write_error;
	} else {
		if (fwrite(buff, 1, buff_size, file) != buff_size)
			goto write_error;
	}

	LWLockRelease(pgtsqss->lock);
	FreeFile(file);

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
	uint32	msg_length;
	char	header[9];

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
	item->data = strndup(buffer + p + 8, msg_length);
	item->length = msg_length;
}

/*
 * Checks if a row (serialized entry) could be parsed
 */
bool
pgtsq_check_row(char * row)
{
	uint32	p = 0;
	TSQItem	* item = NULL;

	item = (TSQItem *)palloc(sizeof(TSQItem));

	for (int c = 1; c <= TSQ_COLS; c++)
	{
		pgtsq_parse_item(row, p, item);

		if (item == NULL)
			return false;

		p += item->length + 8;
	}
	return true;
}


/*
 * Parses a row (serialized)
 */
bool
pgtsq_parse_row(char * row, TSQEntry * tsqe)
{
	uint32	p = 0;
	TSQItem	* item = NULL;

	item = (TSQItem *)palloc(sizeof(TSQItem));

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
				free(item->data);
				break;
			case 3:
				/* username */
				tsqe->username = item->data;
				break;;
			case 4:
				/* dbname */
				tsqe->dbname = item->data;
				break;
			case 5:
				/* temp_blks_written */
				tsqe->temp_blks_written = atoi(item->data);
				free(item->data);
				break;
			case 6:
				/* hitratio */
				tsqe->hitratio = atof(item->data);
				free(item->data);
				break;
			case 7:
				/* ntuples */
				tsqe->ntuples = atoi(item->data);
				free(item->data);
				break;
			case 8:
				/* querytxt */
				tsqe->querytxt = item->data;
				break;
			case 9:
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
