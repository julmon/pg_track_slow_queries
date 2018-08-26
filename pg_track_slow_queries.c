/*
 * pg_track_slow_queries
 *
 * Get a track of individual slow queries, including their execution end
 * datetime, duration, execution plan, number of tuples returned, cache
 * hit-ratio, blocks written by temporary files, user name and database name.
 * Inspired by pg_stat_kcache, auto_explain and pg_stat_statements.
 *
 * Each query dataset is serialized, compressed if possible using
 * pglz_compress(), and then stored on disk.
 *
 */

#include "postgres.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "commands/dbcommands.h"
#include "commands/explain.h"
#include "common/pg_lzcompress.h"
#include "executor/executor.h"
#include "lib/stringinfo.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/fmgrprotos.h"
#include "utils/timestamp.h"

PG_MODULE_MAGIC;

#define TSQ_FILE PGSTAT_STAT_PERMANENT_DIRECTORY "/pg_track_slow_queries.stat"
#define TSQ_COLS 9

/* GUC variable */
static int tsq_log_min_duration = 0; /* sec (>=1) or 0 (disabled) */

/* Saved hook values in case of unload */
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd = NULL;
static ExecutorStart_hook_type prev_ExecutorStart = NULL;
static ExecutorRun_hook_type prev_ExecutorRun = NULL;
static ExecutorFinish_hook_type prev_ExecutorFinish = NULL;

#define tsq_enabled() \
	(tsq_log_min_duration > 0)

void _PG_init(void);
void _PG_fini(void);

typedef struct TSQEntry {
	char	*datetime;			/* Execution end datetime */
	double	duration;			/* Duration in ms */
	char	*username;			/* Username running the query */
	char	*dbname;			/* Database name */
	long	temp_blks_written;	/* Blocks written for temp. files usage */
	float	hitratio;			/* Cache hit-ratio */
	uint64	ntuples;			/* Number of tuples returned or affected */
	char	*querytxt;			/* Text representation of the query */
	char	*plantxt;			/* JSON representation of the exec. plan */
} TSQEntry;

typedef struct TSQSharedState {
	/* Lock to prevent concurrent updates on the storage file */
	LWLockId lock;
} TSQSharedState;

extern PGDLLEXPORT Datum pg_track_slow_queries_reset(PG_FUNCTION_ARGS);
extern PGDLLEXPORT Datum pg_track_slow_queries(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(pg_track_slow_queries_reset);
PG_FUNCTION_INFO_V1(pg_track_slow_queries);

static void pgtsq_shmem_startup(void);
static void pgtsq_ExecutorEnd(QueryDesc *queryDesc);
static void pgtsq_ExecutorStart(QueryDesc *queryDesc, int eflags);
static void pgtsq_ExecutorRun(QueryDesc *queryDesc,
							  ScanDirection direction,
							  uint64 count, bool execute_once);
static void pgtsq_ExecutorFinish(QueryDesc *queryDesc);
static uint32 pgtsq_store_entry(TSQEntry * tsqe);
static void pg_track_slow_queries_internal(FunctionCallInfo fcinfo);
static void pgtsq_truncate_file(void);
static bool pgtsq_parse_row(char * row, TSQEntry * tsqe);

/* Current nesting depth of ExecutorRun calls */
static int nesting_level = 0;

/* Link to shared memory state */
static TSQSharedState * pgtsqss = NULL;

/*
 * ExecutorStart Hook function that only starts query timing
 * instrumentalization if the feature is enabled.
 */
static void
pgtsq_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
	if (prev_ExecutorStart)
		prev_ExecutorStart(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);

	if (tsq_enabled() && queryDesc->totaltime == NULL)
	{
		MemoryContext oldcxt;
		/* Move to query memory context */
		oldcxt = MemoryContextSwitchTo(queryDesc->estate->es_query_cxt);
		queryDesc->totaltime = InstrAlloc(1, INSTRUMENT_ALL);
		/* Back to previous mem. context */
		MemoryContextSwitchTo(oldcxt);
	}
}

/*
 * ExecutorEnd Hook function that fetches and store query informations, only
 * if execution time exceeds log_min_duration.
 */
static void
pgtsq_ExecutorEnd(QueryDesc *queryDesc)
{
	if (tsq_enabled() && queryDesc->totaltime)
	{
		/* End query timing instrumentalization */
		InstrEndLoop(queryDesc->totaltime);
	}
	if (tsq_enabled() && queryDesc->totaltime &&
		queryDesc->totaltime->total > tsq_log_min_duration)
	{
		ExplainState	*es = NewExplainState();
		TSQEntry		*tsqe = NULL;
		BufferUsage		bu;

		bu = queryDesc->totaltime->bufusage;

		tsqe = (TSQEntry *) palloc0(sizeof(TSQEntry));
		/* Fetch data */
		tsqe->username = GetUserNameFromId(GetUserId(), false);
		tsqe->dbname = get_database_name(MyDatabaseId);
		/* Get current timestamp as query's end of execution datetime */
		tsqe->datetime = strdup(timestamptz_to_str(GetCurrentTimestamp()));
		tsqe->duration = queryDesc->totaltime->total * 1000.0;
		tsqe->querytxt = strdup(queryDesc->sourceText);
		tsqe->temp_blks_written = bu.temp_blks_written;
		if ((bu.shared_blks_hit + bu.local_blks_hit +
			 bu.shared_blks_read + bu.local_blks_read) > 0)
		{
			tsqe->hitratio = ((float) (bu.shared_blks_hit + bu.local_blks_hit) /
							  (float) (bu.shared_blks_hit + bu.local_blks_hit +
									   bu.shared_blks_read + bu.local_blks_read)) * 100;
		} else
			tsqe->hitratio = 100.0;
		tsqe->ntuples = (uint64) queryDesc->totaltime->ntuples;

		/* Get Execution Plan as text */
		es->verbose = 1;
		es->analyze = 0;
		es->buffers = 0;
		es->timing = 0;
		es->summary = 0;
		es->format = EXPLAIN_FORMAT_JSON;

		ExplainBeginOutput(es);
		ExplainPrintPlan(es, queryDesc);
		ExplainEndOutput(es);

		/* Remove last line break */
		if (es->str->len > 0 && es->str->data[es->str->len - 1] == '\n')
			es->str->data[--es->str->len] = '\0';
		/* Fix JSON to output an object */
		es->str->data[0] = '{';
		es->str->data[es->str->len - 1] = '}';

		tsqe->plantxt = es->str->data;

		if (pgtsq_store_entry(tsqe) == -1)
		{
			/* XXX: Error */
		}
		pfree(tsqe->username);
		pfree(tsqe->dbname);
		pfree(tsqe->plantxt);
		pfree(tsqe);
		pfree(es);

	}

	if (prev_ExecutorEnd)
		prev_ExecutorEnd(queryDesc);
	else
		standard_ExecutorEnd(queryDesc);
}

/*
 * Compress with pglz_compress and stores an entry.
 */
static uint32 pgtsq_store_entry(TSQEntry * tsqe)
{
	StringInfo	si;
	char		*buff;
	uint32		buff_size;
	FILE		*file = NULL;

	/*
	 * Serialize entry as a StringInfo, each element is preceded by its length
	 * (hex repr).
	 */
	si = makeStringInfo();
	appendStringInfo(si, "%08x%s", (int) strlen(tsqe->datetime), tsqe->datetime);
	appendStringInfo(si, "%08x%016.02f", 16, tsqe->duration);
	appendStringInfo(si, "%08x%s", (int) strlen(tsqe->username), tsqe->username);
	appendStringInfo(si, "%08x%s", (int) strlen(tsqe->dbname), tsqe->dbname);
	appendStringInfo(si, "%08x%016li", 16, tsqe->temp_blks_written);
	appendStringInfo(si, "%08x%010.06f", 10, tsqe->hitratio);
	appendStringInfo(si, "%08x%016lu", 16, tsqe->ntuples);
	appendStringInfo(si, "%08x%s", (uint32) strlen(tsqe->querytxt), tsqe->querytxt);
	appendStringInfo(si, "%08x%s", (uint32) strlen(tsqe->plantxt), tsqe->plantxt);

	/*
	 * Allocate a buffer for compression as long as the original string in order
	 * to be sure to have enough space.
	 */
	buff = (char *) palloc0(si->len);
	buff_size = pglz_compress(si->data, si->len, buff, NULL);
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
	if (fwrite(&si->len, 1, sizeof(uint32), file) != sizeof(si->len))
		goto write_error;

	if (buff_size == 0)
	{
		/*
		 * If buff_size == 0 it means pglz_compress hasn't compressed the
		 * StringInfo for some reasons, probably because there is no gain to
		 * compress it. In this case, let's store data uncompressed.
		 */
		if (fwrite(si->data, 1, si->len, file) != si->len)
			goto write_error;
	} else {
		if (fwrite(buff, 1, buff_size, file) != buff_size)
			goto write_error;
	}

	LWLockRelease(pgtsqss->lock);
	FreeFile(file);

	pfree(si->data);
	pfree(si);
	pfree(buff);

	return buff_size;

write_error:
	if (pgtsqss->lock)
		LWLockRelease(pgtsqss->lock);
	if (file)
		FreeFile(file);
	ereport(LOG,
			(errcode_for_file_access(),
			 errmsg("could not write file \"%s\": %m",
					TSQ_FILE)));
	return -1;
}

/*
 * ExecutorRun hook: all we need do is track nesting depth
 */
static void
pgtsq_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction,
				  uint64 count, bool execute_once)
{
	nesting_level++;
	PG_TRY();
	{
		if (prev_ExecutorRun)
			prev_ExecutorRun(queryDesc, direction, count, execute_once);
		else
			standard_ExecutorRun(queryDesc, direction, count, execute_once);
		nesting_level--;
	}
	PG_CATCH();
	{
		nesting_level--;
		PG_RE_THROW();
	}
	PG_END_TRY();
}

/*
 * ExecutorFinish hook: all we need do is track nesting depth
 */
static void
pgtsq_ExecutorFinish(QueryDesc *queryDesc)
{
	nesting_level++;
	PG_TRY();
	{
		if (prev_ExecutorFinish)
			prev_ExecutorFinish(queryDesc);
		else
			standard_ExecutorFinish(queryDesc);
		nesting_level--;
	}
	PG_CATCH();
	{
		nesting_level--;
		PG_RE_THROW();
	}
	PG_END_TRY();
}

/*
 * Start up hook function
 */
static void
pgtsq_shmem_startup(void)
{
	bool	found;

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	/* Reset in case this is a restart within the postmaster */
	pgtsqss = NULL;

	/* Create or attach to the shared memory state */
	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

	/* Storage file access lock */
	pgtsqss = ShmemInitStruct("pg_track_slow_queries",
					sizeof(TSQSharedState),
					&found);

	if (!found)
	{
		pgtsqss->lock = &(GetNamedLWLockTranche("pg_track_slow_queries"))->lock;
	}

	LWLockRelease(AddinShmemInitLock);

	ereport(LOG, (errmsg("extension pg_track_slow_queries loaded")));

}

void
_PG_init(void)
{
	if (!process_shared_preload_libraries_in_progress)
	{
		elog(ERROR, "This module can only be loaded via shared_preload_libraries");
		return;
	}

	/* Define custom GUC variable. */
	DefineCustomIntVariable("pg_track_slow_queries.log_min_duration",
							"Sets the minimum execution time above which queries and plans will "
							"be logged.",
							"Zero turns this feature off.",
							&tsq_log_min_duration,
							0,
							0, INT_MAX,
							PGC_SUSET,
							GUC_UNIT_S,
							NULL,
							NULL,
							NULL);

	EmitWarningsOnPlaceholders("pg_track_slow_queries");

	RequestNamedLWLockTranche("pg_track_slow_queries", 1);

	/* Install hooks. */
	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = pgtsq_shmem_startup;
	prev_ExecutorStart = ExecutorStart_hook;
	ExecutorStart_hook = pgtsq_ExecutorStart;
	prev_ExecutorRun = ExecutorRun_hook;
	ExecutorRun_hook = pgtsq_ExecutorRun;
	prev_ExecutorFinish = ExecutorFinish_hook;
	ExecutorFinish_hook = pgtsq_ExecutorFinish;
	prev_ExecutorEnd = ExecutorEnd_hook;
	ExecutorEnd_hook = pgtsq_ExecutorEnd;
}

void
_PG_fini(void)
{
	/* Uninstall hooks. */
	shmem_startup_hook = prev_shmem_startup_hook;
	ExecutorStart_hook = prev_ExecutorStart;
	ExecutorRun_hook = prev_ExecutorRun;
	ExecutorFinish_hook = prev_ExecutorFinish;
	ExecutorEnd_hook = prev_ExecutorEnd;
}

PGDLLEXPORT Datum
pg_track_slow_queries_reset(PG_FUNCTION_ARGS)
{
	if (!pgtsqss)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_track_slow_queries must be loaded via shared_preload_libraries")));

	pgtsq_truncate_file();
	PG_RETURN_VOID();
}

PGDLLEXPORT Datum
pg_track_slow_queries(PG_FUNCTION_ARGS)
{
	if (!pgtsqss)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_track_slow_queries must be loaded via shared_preload_libraries")));

	pg_track_slow_queries_internal(fcinfo);
	return (Datum) 0;
}

/*
 * Parses storage file and returns data as a tuple set
 */
static void
pg_track_slow_queries_internal(FunctionCallInfo fcinfo)
{
	ReturnSetInfo	*rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	MemoryContext	per_query_ctx;
	MemoryContext	oldcontext;
	TupleDesc		tupdesc;
	Tuplestorestate	*tupstore;
	FILE			*file = NULL;
	uint32			row_size = 0;
	uint32			row_c_size = 0;
	char			*read_buff = NULL;
	char			*buff = NULL;
	TSQEntry		*tsqe = NULL;

	/* Check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not " \
							"allowed in this context")));

	/* Switch into long-lived context to construct returned data structures */
	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	/* Acquire shared lock for reading storage file */
	LWLockAcquire(pgtsqss->lock, LW_SHARED);

	file = AllocateFile(TSQ_FILE, PG_BINARY_R);
	if (file == NULL)
		goto read_error;

	/* Start by reading compressed row length */
	while (fread(&row_c_size, sizeof(uint32), 1, file) == 1)
	{
		Datum	values[TSQ_COLS];
		bool	nulls[TSQ_COLS];
		int		i = 0;

		memset(values, 0, sizeof(values));
		memset(nulls, 0, sizeof(nulls));

		/* Read row length */
		if (fread(&row_size, sizeof(uint32), 1, file) != 1)
			goto read_error;

		if (row_c_size > 0)
		{
			/* If compressed row length > 0 then read and decompress it */
			read_buff = (char *) palloc0(row_c_size);
			if (fread(read_buff, row_c_size, 1, file) != 1)
				goto read_error;
			buff = (char *) palloc0(row_size);
			if (pglz_decompress(read_buff, row_c_size, buff, row_size) != row_size)
				goto decompress_error;
			pfree(read_buff);
		} else {
			/* Uncompressed row */
			buff = (char *) palloc0(row_size);
			if (fread(buff, row_size, 1, file) != 1)
				goto read_error;
		}

		tsqe = (TSQEntry *) palloc0(sizeof(TSQEntry));
		if (!(pgtsq_parse_row(buff, tsqe)))
			goto parse_error;

		values[i++] = DirectFunctionCall3(timestamptz_in,
										  CStringGetDatum(tsqe->datetime),
										  ObjectIdGetDatum(InvalidOid),
										  Int32GetDatum(-1));
		values[i++] = Float8GetDatumFast(tsqe->duration);
		values[i++] = CStringGetTextDatum(tsqe->username);
		values[i++] = CStringGetTextDatum(tsqe->dbname);
		values[i++] = UInt32GetDatum(tsqe->temp_blks_written);
		values[i++] = Float8GetDatumFast(tsqe->hitratio);
		values[i++] = UInt64GetDatum(tsqe->ntuples);
		values[i++] = CStringGetTextDatum(tsqe->querytxt);
		values[i++] = CStringGetTextDatum(tsqe->plantxt);

		tuplestore_putvalues(tupstore, tupdesc, values, nulls);

		pfree(tsqe->dbname);
		pfree(tsqe->username);
		pfree(tsqe->datetime);
		pfree(tsqe->querytxt);
		pfree(tsqe->plantxt);
		pfree(tsqe);
		pfree(buff);
	}

	LWLockRelease(pgtsqss->lock);
	FreeFile(file);

	/* clean up and return the tuplestore */
	tuplestore_donestoring(tupstore);
	return;

read_error:
	ereport(LOG,
			(errcode_for_file_access(),
			 errmsg("could not read pg_track_slow_queries file \"%s\": %m",
					TSQ_FILE)));
	goto fail;

decompress_error:
	ereport(LOG,
			(errcode_for_file_access(),
			 errmsg("could not decompress row")));
	goto fail;

parse_error:
	ereport(LOG,
			(errcode_for_file_access(),
			 errmsg("could not parse row")));
	goto fail;

fail:
	/* Cleaning */
	if (file)
		FreeFile(file);
	if (pgtsqss->lock)
		LWLockRelease(pgtsqss->lock);
}

typedef struct TSQItem {
	char	*str;
	uint32	length;
} TSQItem;

/*
 * Parse an item from a row buffer. First 8 chars represents item's string length
 * (hex repr)
 */
static TSQItem *
pgtsq_parse_item(char * buffer, uint32 p)
{
	uint32	msg_length;
	char	header[9];
	int		j = 0;
	TSQItem	*item;

	for (int i = 0; i < 8; i++)
	{
		header[i] = buffer[p + i];
	}
	header[8] = '\0';
	errno = 0;
	msg_length = (uint32) strtol(header, NULL, 16);
	if (errno)
	{
		/* Conversion to long hasn't been performed */
		return NULL;
	}
	item = (TSQItem *)palloc(sizeof(TSQItem));
	item->str = (char *) palloc0(msg_length+1);
	for (uint32 i = (p + 8); i < (p + 8 + msg_length); i++)
	{
		item->str[j] = buffer[i];
		j++;
	}
	item->length = j;
	item->str[j] = '\0';
	return item;
}

static bool
pgtsq_parse_row(char * row, TSQEntry * tsqe)
{
	/* string buffers for numeric transformation */
	uint32	p = 0;
	TSQItem	*ti = NULL;

	/* Row items parsing and type conversion if needed*/
	/* datetime */
	ti = pgtsq_parse_item(row, p);
	if (ti == NULL)
		return false;
	tsqe->datetime = ti->str;
	p += ti->length + 8;
	pfree(ti);
	/* duration */
	ti = pgtsq_parse_item(row, p);
	if (ti == NULL)
		return false;
	tsqe->duration = atof(ti->str);
	p += ti->length + 8;
	pfree(ti->str);
	pfree(ti);
	/* username */
	ti = pgtsq_parse_item(row, p);
	if (ti == NULL)
		return false;
	tsqe->username = ti->str;
	p += ti->length + 8;
	pfree(ti);
	/* dbname */
	ti = pgtsq_parse_item(row, p);
	if (ti == NULL)
		return false;
	tsqe->dbname = ti->str;
	p += ti->length + 8;
	pfree(ti);
	/* temp_blks_written */
	ti = pgtsq_parse_item(row, p);
	if (ti == NULL)
		return false;
	tsqe->temp_blks_written = atoi(ti->str);
	p += ti->length + 8;
	pfree(ti->str);
	pfree(ti);
	/* hitratio */
	ti = pgtsq_parse_item(row, p);
	if (ti == NULL)
		return false;
	tsqe->hitratio = atof(ti->str);
	p += ti->length + 8;
	pfree(ti->str);
	pfree(ti);
	/* ntuples */
	ti = pgtsq_parse_item(row, p);
	if (ti == NULL)
		return false;
	tsqe->ntuples = atoi(ti->str);
	p += ti->length + 8;
	pfree(ti->str);
	pfree(ti);
	/* querytxt */
	ti = pgtsq_parse_item(row, p);
	if (ti == NULL)
		return false;
	tsqe->querytxt = ti->str;
	p += ti->length + 8;
	pfree(ti);
	/* plantxt */
	ti = pgtsq_parse_item(row, p);
	if (ti == NULL)
		return false;
	tsqe->plantxt = ti->str;
	p += ti->length + 8;
	pfree(ti);

	return true;
}

static void
pgtsq_truncate_file(void)
{
	FILE	*file = NULL;

	LWLockAcquire(pgtsqss->lock, LW_EXCLUSIVE);
	file = AllocateFile(TSQ_FILE, PG_BINARY_W);
	LWLockRelease(pgtsqss->lock);
	FreeFile(file);
}
