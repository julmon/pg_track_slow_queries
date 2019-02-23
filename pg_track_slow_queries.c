/*
 * pg_track_slow_queries
 *
 * Get a track of individual slow queries, including their execution end
 * datetime, duration, execution plan, number of tuples returned, shared
 * buffers hit-ratio, blocks written by temporary files, username and
 * database name.
 *
 * Collected informations are serialized as a StringInfo. If row size is lower
 * to 65KiB, then it is sent to the collector (BackgroundWorker), else, row
 * storage is done by the backend itself. Row storage function compresses data
 * with pglz_compress().
 *
 * Deeply inspired by auto_explain and pg_stat_statements contribs.
 */

#include "postgres.h"

#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>

#include "funcapi.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "commands/dbcommands.h"
#include "commands/explain.h"
#include "common/ip.h"
#include "common/pg_lzcompress.h"
#include "executor/executor.h"
#include "lib/stringinfo.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "storage/shmem.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/fmgrprotos.h"
#include "utils/timestamp.h"

#include "pg_track_slow_queries.h"

PG_MODULE_MAGIC;

void _PG_init(void);
void _PG_fini(void);

static void pg_track_slow_queries_internal(FunctionCallInfo fcinfo);
PGDLLEXPORT Datum pg_track_slow_queries_reset(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum pg_track_slow_queries(PG_FUNCTION_ARGS);
static void pgtsq_ExecutorEnd(QueryDesc *queryDesc);
static void pgtsq_ExecutorStart(QueryDesc *queryDesc, int eflags);
static void pgtsq_ExecutorRun(QueryDesc *queryDesc,
							  ScanDirection direction,
							  uint64 count, bool execute_once);
static void pgtsq_ExecutorFinish(QueryDesc *queryDesc);
static void pgtsq_shmem_startup(void);
static int pgtsq_init_socket(void);

/* GUC variable */
static int tsq_log_min_duration = 0;	/* ms (>=0) or -1 (disabled) */
static bool tsq_compression = true;		/* enable row compression */
static int tsq_max_file_size_mb = -1;	/* storage file max size in MB */

/* Saved hook values in case of unload */
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd = NULL;
static ExecutorStart_hook_type prev_ExecutorStart = NULL;
static ExecutorRun_hook_type prev_ExecutorRun = NULL;
static ExecutorFinish_hook_type prev_ExecutorFinish = NULL;

/* Current nesting depth of ExecutorRun calls */
static int nesting_level = 0;

/* Link to shared memory state */
TSQSharedState * pgtsqss = NULL;

static struct sockaddr_storage pgStatAddr;

PG_FUNCTION_INFO_V1(pg_track_slow_queries_reset);
PG_FUNCTION_INFO_V1(pg_track_slow_queries);

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
 * if execution time exceeds pg_track_slow_queries.log_min_duration.
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
		(queryDesc->totaltime->total * 1000.0) > tsq_log_min_duration)
	{
		ExplainState	*es = NewExplainState();
		TSQEntry		*tsqe = NULL;
		BufferUsage		bu = queryDesc->totaltime->bufusage;
		StringInfo		tsqe_s;
		ssize_t			sent;

		if ((tsqe = (TSQEntry *) palloc0(sizeof(TSQEntry))) == NULL)
		{
			ereport(LOG,
				(errmsg("pg_track_slow_queries: could not allocate memory")));
			goto end;
		}

		/* Fetch data */
		tsqe->username = GetUserNameFromId(GetUserId(), false);
		tsqe->dbname = get_database_name(MyDatabaseId);
		/* Get current timestamp as query's end of execution datetime */
		tsqe->datetime = strdup(timestamptz_to_str(GetCurrentTimestamp()));
		/* Duration time in ms */
		tsqe->duration = queryDesc->totaltime->total * 1000.0;
		tsqe->querytxt = strdup(queryDesc->sourceText);
		tsqe->temp_blks_written = bu.temp_blks_written;
		/* Shared buffers hit ratio */
		if ((bu.shared_blks_hit + bu.local_blks_hit +
			 bu.shared_blks_read + bu.local_blks_read) > 0)
		{
			tsqe->hitratio = ((float) (bu.shared_blks_hit + bu.local_blks_hit) /
							  (float) (bu.shared_blks_hit + bu.local_blks_hit +
									   bu.shared_blks_read + bu.local_blks_read)) * 100;
		} else
			tsqe->hitratio = 100.0;
		tsqe->ntuples = (uint64) queryDesc->totaltime->ntuples;

		/* Get Execution Plan as JSON */
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

		/* Data serialization */
		tsqe_s = pgtsq_serialize_entry(tsqe);

		if (pgtsqss->socket != PGINVALID_SOCKET && tsqe_s->len < 65000)
		{
			/* If row length is lower to 65kiB (UDP datagram max size) then we try to
			 * send it to the collector */
			sent = send(pgtsqss->socket, tsqe_s->data, tsqe_s->len, 0);
			if (sent != tsqe_s->len)
			{
				ereport(LOG,
					(errmsg("pg_track_slow_queries: could not send data to the collector")));
			}
		} else {
			/* Row storage is done by the backend itself */
			if (pgtsq_store_row(tsqe_s->data, tsqe_s->len, tsq_compression_enabled(),
					tsq_max_file_size_mb) == -1)
			{
				ereport(LOG,
					(errmsg("pg_track_slow_queries: could not store data")));
			}
		}
		/* Free memory */
		pfree(tsqe->username);
		pfree(tsqe->dbname);
		pfree(tsqe->plantxt);
		pfree(tsqe);
		pfree(tsqe_s->data);
		pfree(tsqe_s);
		pfree(es);
	}

end:
	if (prev_ExecutorEnd)
		prev_ExecutorEnd(queryDesc);
	else
		standard_ExecutorEnd(queryDesc);
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
		/* Init socket for backends to collector IPC */
		pgtsqss->socket = pgtsq_init_socket();
	}

	LWLockRelease(AddinShmemInitLock);

	ereport(LOG, (errmsg("pg_track_slow_queries: extension loaded")));
}

/*
 * Extension init function
 */
void
_PG_init(void)
{
	BackgroundWorker worker;

	if (!process_shared_preload_libraries_in_progress)
	{
		elog(ERROR, "This module can only be loaded via shared_preload_libraries");
		return;
	}

	/* Define custom GUC variable. */
	DefineCustomIntVariable("pg_track_slow_queries.log_min_duration",
							"Sets the minimum execution time above which queries and plans will "
							"be logged.",
							"-1 turns this feature off.",
							&tsq_log_min_duration,
							-1,
							-1, INT_MAX,
							PGC_SUSET,
							GUC_UNIT_MS,
							NULL,
							NULL,
							NULL);

	DefineCustomBoolVariable("pg_track_slow_queries.compression",
							"Enables data compression.",
							NULL,
							&tsq_compression,
							true,
							PGC_SUSET,
							0,
							NULL,
							NULL,
							NULL);

	DefineCustomIntVariable("pg_track_slow_queries.max_file_size",
							"Sets the maximum storage file size.",
							"-1 turns this feature off.",
							&tsq_max_file_size_mb,
							-1,
							-1, MAX_KILOBYTES,
							PGC_SUSET,
							GUC_UNIT_MB,
							NULL,
							NULL,
							NULL);


	EmitWarningsOnPlaceholders("pg_track_slow_queries");

	RequestNamedLWLockTranche("pg_track_slow_queries", 1);

	/* Register background worker */
	memset(&worker, 0, sizeof(worker));
	worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
	worker.bgw_restart_time = -1;
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS;
	snprintf(worker.bgw_library_name, BGW_MAXLEN, "pg_track_slow_queries");
	snprintf(worker.bgw_function_name, BGW_MAXLEN, "pgtsq_worker");
	snprintf(worker.bgw_name, BGW_MAXLEN, "pg_track_slow_queries writer");
	snprintf(worker.bgw_type, BGW_MAXLEN, "pgtsq_worker");
	worker.bgw_notify_pid = 0;
	worker.bgw_main_arg = (Datum) 0;
	RegisterBackgroundWorker(&worker);

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
				 errmsg("pg_track_slow_queries: must be loaded via shared_preload_libraries")));

	pgtsq_truncate_file();
	PG_RETURN_VOID();
}

PGDLLEXPORT Datum
pg_track_slow_queries(PG_FUNCTION_ARGS)
{
	if (!pgtsqss)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_track_slow_queries: must be loaded via shared_preload_libraries")));

	pg_track_slow_queries_internal(fcinfo);
	return (Datum) 0;
}

/*
 * Reads, parses, and returns data as a tuple set
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
				 errmsg("pg_track_slow_queries: set-valued function called in context " \
							"that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("pg_track_slow_queries: materialize mode required, but it is not " \
							"allowed in this context")));

	/* Switch into long-lived context to construct returned data structures */
	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "pg_track_slow_queries: return type must be a row type");

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

		free(tsqe->dbname);
		free(tsqe->username);
		free(tsqe->datetime);
		free(tsqe->querytxt);
		free(tsqe->plantxt);
		pfree(buff);
		pfree(tsqe);
	}

	LWLockRelease(pgtsqss->lock);
	FreeFile(file);

	/* Clean up and return the tuplestore */
	tuplestore_donestoring(tupstore);
	return;

read_error:
	ereport(LOG,
			(errcode_for_file_access(),
			 errmsg("pg_track_slow_queries: could not read file \"%s\": %m",
					TSQ_FILE)));
	goto fail;

decompress_error:
	ereport(LOG,
			(errcode_for_file_access(),
			 errmsg("pg_track_slow_queries: could not decompress row")));
	goto fail;

parse_error:
	ereport(LOG,
			(errcode_for_file_access(),
			 errmsg("pg_track_slow_queries: could not parse row")));
	goto fail;

fail:
	/* Cleaning */
	if (file)
		FreeFile(file);
	if (pgtsqss->lock)
		LWLockRelease(pgtsqss->lock);
}

/*
 * UDP socket initialization.
 * Code coming from backend/postmaster/pgstat.c
 */
static int
pgtsq_init_socket(void)
{
	ACCEPT_TYPE_ARG3 alen;
	struct addrinfo *addrs = NULL,
			   *addr,
				hints;
	int			ret;
	fd_set		rset;
	struct timeval tv;
	char		test_byte;
	int			sel_res;
	int			tries = 0;
	int			PGTSQSocket = PGINVALID_SOCKET;

#define TESTBYTEVAL ((char) 199)

	/*
	 * Create the UDP socket for sending and receiving slow queries messages
	 */
	hints.ai_flags = AI_PASSIVE;
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_protocol = 0;
	hints.ai_addrlen = 0;
	hints.ai_addr = NULL;
	hints.ai_canonname = NULL;
	hints.ai_next = NULL;
	ret = pg_getaddrinfo_all("localhost", NULL, &hints, &addrs);
	if (ret || !addrs)
	{
		ereport(LOG,
				(errmsg("pg_track_slow_queries: could not resolve " \
						"\"localhost\": %s", gai_strerror(ret))));
		goto startup_failed;
	}

	/*
	 * On some platforms, pg_getaddrinfo_all() may return multiple addresses
	 * only one of which will actually work (eg, both IPv6 and IPv4 addresses
	 * when kernel will reject IPv6).  Worse, the failure may occur at the
	 * bind() or perhaps even connect() stage.  So we must loop through the
	 * results till we find a working combination. We will generate LOG
	 * messages, but no error, for bogus combinations.
	 */
	for (addr = addrs; addr; addr = addr->ai_next)
	{
#ifdef HAVE_UNIX_SOCKETS
		/* Ignore AF_UNIX sockets, if any are returned. */
		if (addr->ai_family == AF_UNIX)
			continue;
#endif

		if (++tries > 1)
			ereport(LOG,
					(errmsg("pg_track_slow_queries: trying another address " \
							"for the collector")));

		/*
		 * Create the socket.
		 */
		if ((PGTSQSocket = socket(addr->ai_family, SOCK_DGRAM, 0)) == PGINVALID_SOCKET)
		{
			ereport(LOG,
					(errcode_for_socket_access(),
					 errmsg("pg_track_slow_queries: could not create socket: %m")));
			continue;
		}

		/*
		 * Bind it to a kernel assigned port on localhost and get the assigned
		 * port via getsockname().
		 */
		if (bind(PGTSQSocket, addr->ai_addr, addr->ai_addrlen) < 0)
		{
			ereport(LOG,
					(errcode_for_socket_access(),
					 errmsg("pg_track_slow_queries: could not bind socket: %m")));
			close(PGTSQSocket);
			PGTSQSocket = PGINVALID_SOCKET;
			continue;
		}

		alen = sizeof(pgStatAddr);
		if (getsockname(PGTSQSocket, (struct sockaddr *) &pgStatAddr, &alen) < 0)
		{
			ereport(LOG,
					(errcode_for_socket_access(),
					 errmsg("pg_track_slow_queries: could not get address of socket: %m")));
			close(PGTSQSocket);
			PGTSQSocket = PGINVALID_SOCKET;
			continue;
		}

		/*
		 * Connect the socket to its own address.  This saves a few cycles by
		 * not having to respecify the target address on every send. This also
		 * provides a kernel-level check that only packets from this same
		 * address will be received.
		 */
		if (connect(PGTSQSocket, (struct sockaddr *) &pgStatAddr, alen) < 0)
		{
			ereport(LOG,
					(errcode_for_socket_access(),
					 errmsg("pg_track_slow_queries: could not connect socket: %m")));
			close(PGTSQSocket);
			PGTSQSocket = PGINVALID_SOCKET;
			continue;
		}

		/*
		 * Try to send and receive a one-byte test message on the socket. This
		 * is to catch situations where the socket can be created but will not
		 * actually pass data (for instance, because kernel packet filtering
		 * rules prevent it).
		 */
		test_byte = TESTBYTEVAL;

retry1:
		if (send(PGTSQSocket, &test_byte, 1, 0) != 1)
		{
			if (errno == EINTR)
				goto retry1;	/* if interrupted, just retry */
			ereport(LOG,
					(errcode_for_socket_access(),
					 errmsg("pg_track_slow_queries: could not send test message on socket: %m")));
			close(PGTSQSocket);
			PGTSQSocket = PGINVALID_SOCKET;
			continue;
		}

		/*
		 * There could possibly be a little delay before the message can be
		 * received.  We arbitrarily allow up to half a second before deciding
		 * it's broken.
		 */
		for (;;)				/* need a loop to handle EINTR */
		{
			FD_ZERO(&rset);
			FD_SET(PGTSQSocket, &rset);

			tv.tv_sec = 0;
			tv.tv_usec = 500000;
			sel_res = select(PGTSQSocket + 1, &rset, NULL, NULL, &tv);
			if (sel_res >= 0 || errno != EINTR)
				break;
		}
		if (sel_res < 0)
		{
			ereport(LOG,
					(errcode_for_socket_access(),
					 errmsg("pg_track_slow_queries: select() failed: %m")));
			close(PGTSQSocket);
			PGTSQSocket = PGINVALID_SOCKET;
			continue;
		}
		if (sel_res == 0 || !FD_ISSET(PGTSQSocket, &rset))
		{
			/*
			 * This is the case we actually think is likely, so take pains to
			 * give a specific message for it.
			 *
			 * errno will not be set meaningfully here, so don't use it.
			 */
			ereport(LOG,
					(errcode(ERRCODE_CONNECTION_FAILURE),
					 errmsg("pg_track_slow_queries: test message did not get through on socket")));
			close(PGTSQSocket);
			PGTSQSocket = PGINVALID_SOCKET;
			continue;
		}

		test_byte++;			/* just make sure variable is changed */

retry2:
		if (recv(PGTSQSocket, &test_byte, 1, 0) != 1)
		{
			if (errno == EINTR)
				goto retry2;	/* if interrupted, just retry */
			ereport(LOG,
					(errcode_for_socket_access(),
					 errmsg("pg_track_slow_queries: could not receive test message on socket: %m")));
			close(PGTSQSocket);
			PGTSQSocket = PGINVALID_SOCKET;
			continue;
		}

		if (test_byte != TESTBYTEVAL)	/* strictly paranoia ... */
		{
			ereport(LOG,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("pg_track_slow_queries: incorrect test message transmission on socket")));
			close(PGTSQSocket);
			PGTSQSocket = PGINVALID_SOCKET;
			continue;
		}

		/* If we get here, we have a working socket */
		break;
	}

	/* Did we find a working address? */
	if (!addr || PGTSQSocket == PGINVALID_SOCKET)
		goto startup_failed;

	/*
	 * Set the socket to non-blocking IO.  This ensures that if the collector
	 * falls behind, statistics messages will be discarded; backends won't
	 * block waiting to send messages to the collector.
	 */
	if (!pg_set_noblock(PGTSQSocket))
	{
		ereport(LOG,
				(errcode_for_socket_access(),
				 errmsg("pg_track_slow_queries: could not set slow queries collector " \
						"socket to nonblocking mode: %m")));
		goto startup_failed;
	}

	/*
	 * Try to ensure that the socket's receive buffer is at least
	 * PGSTAT_MIN_RCVBUF bytes, so that it won't easily overflow and lose
	 * data.  Use of UDP protocol means that we are willing to lose data under
	 * heavy load, but we don't want it to happen just because of ridiculously
	 * small default buffer sizes (such as 8KB on older Windows versions).
	 */
	{
		int			old_rcvbuf;
		int			new_rcvbuf;
		ACCEPT_TYPE_ARG3 rcvbufsize = sizeof(old_rcvbuf);

		if (getsockopt(PGTSQSocket, SOL_SOCKET, SO_RCVBUF,
					   (char *) &old_rcvbuf, &rcvbufsize) < 0)
		{
			elog(LOG, "pg_track_slow_queries: getsockopt(SO_RCVBUF) failed: %m");
			/* if we can't get existing size, always try to set it */
			old_rcvbuf = 0;
		}

		new_rcvbuf = PGSTAT_MIN_RCVBUF;
		if (old_rcvbuf < new_rcvbuf)
		{
			if (setsockopt(PGTSQSocket, SOL_SOCKET, SO_RCVBUF,
						   (char *) &new_rcvbuf, sizeof(new_rcvbuf)) < 0)
				elog(LOG, "pg_track_slow_queries: setsockopt(SO_RCVBUF) failed: %m");
		}
	}

	pg_freeaddrinfo_all(hints.ai_family, addrs);

	return PGTSQSocket;

startup_failed:
	ereport(LOG,
			(errmsg("pg_track_slow_queries: disabling collector for lack of working socket")));

	if (addrs)
		pg_freeaddrinfo_all(hints.ai_family, addrs);

	if (PGTSQSocket != PGINVALID_SOCKET)
		close(PGTSQSocket);
	return PGINVALID_SOCKET;

}
