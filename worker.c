#include "postgres.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/ipc.h"
#include "utils/guc.h"
#include "pg_track_slow_queries.h"


/* flags set by signal handlers */
static volatile sig_atomic_t got_sighup = false;
static volatile sig_atomic_t got_sigterm = false;

/*
 * Collector worker main function
 */
void
pgtsq_worker(Datum main_arg)
{
	fd_set rfds;
	struct timeval tv;
	int timeout = 1;
	int retval;
	char msgbuf[MSG_BUFFER_SIZE];
	int n;
	StringInfo si;

	si = makeStringInfo();

	/* Establish signal handlers before unblocking signals. */
	pqsignal(SIGHUP, pgtsq_worker_sighup);
	pqsignal(SIGTERM, pgtsq_worker_sigterm);

	/* We're now ready to receive signals */
	BackgroundWorkerUnblockSignals();
	/*
	 * Main loop: do this until the SIGTERM handler tells us to terminate
	 */
	while (!got_sigterm)
	{
		tv.tv_sec = timeout;
		tv.tv_usec = 0;

		FD_ZERO(&rfds);
		FD_SET(pgtsqss->socket, &rfds);
		retval = select(pgtsqss->socket+1, &rfds, NULL, NULL, &tv);
		if (retval > 0)
		{
			memset(msgbuf, 0, sizeof(msgbuf));
			while ((n = recv(pgtsqss->socket, msgbuf, sizeof(msgbuf), 0)) > 0)
			{
				resetStringInfo(si);
				appendStringInfoString(si, msgbuf);

				if (pgtsq_check_row(si->data))
					pgtsq_store_entry(si);
				else
					ereport(LOG,
						(errmsg("pg_track_slow_queries: could not parse row")));

				memset(msgbuf, 0, sizeof(msgbuf));
				CHECK_FOR_INTERRUPTS();
			}
		}
		CHECK_FOR_INTERRUPTS();

		/*
		 * In case of a SIGHUP, just reload the configuration.
		 */
		if (got_sighup)
		{
			got_sighup = false;
			ProcessConfigFile(PGC_SIGHUP);
		}
	}

	proc_exit(1);
}

/*
 * Signal handler for SIGTERM
 * Set a flag to let the main loop to terminate, and set our latch to wake
 * it up.
 */
void
pgtsq_worker_sigterm(SIGNAL_ARGS)
{
	int			save_errno = errno;

	got_sigterm = true;
	SetLatch(MyLatch);

	errno = save_errno;
}

/*
 * Signal handler for SIGHUP
 * Set a flag to tell the main loop to reread the config file, and set
 * our latch to wake it up.
 */
void
pgtsq_worker_sighup(SIGNAL_ARGS)
{
	int			save_errno = errno;

	got_sighup = true;
	SetLatch(MyLatch);

	errno = save_errno;
}