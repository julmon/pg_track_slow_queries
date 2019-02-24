#ifndef _PG_TRACK_SLOW_QUERIES_H_
#define _PG_TRACK_SLOW_QUERIES_H_

#define TSQ_FILE PGSTAT_STAT_PERMANENT_DIRECTORY "/pg_track_slow_queries.stat"
/* Number of columns */
#define TSQ_COLS			9
#define PGSTAT_MIN_RCVBUF	(100 * 1024)
#define MSG_BUFFER_SIZE		(64 * 1024)

#define tsq_enabled() \
	(tsq_log_min_duration >= 0)

#define tsq_compression_enabled() \
	(tsq_compression == true)

#define tsq_log_plan_enabled() \
	(tsq_log_plan == true)

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
	LWLockId	lock;	/* Lock to prevent concurrent updates on the storage file */
	int			socket;	/* UDP socket file descriptor */

} TSQSharedState;

typedef struct TSQItem {
	uint32	length;
	char	*data;
} TSQItem;


extern uint32 pgtsq_store_row(char * row, int length, bool compression, int max_file_size_mb);
extern void pgtsq_truncate_file(void);
extern bool pgtsq_check_row(char * row);
extern bool pgtsq_parse_row(char * row, TSQEntry * tsqe);
extern void pgtsq_worker(Datum main_arg);
extern void pgtsq_worker_sigterm(SIGNAL_ARGS);
extern void pgtsq_worker_sighup(SIGNAL_ARGS);
extern StringInfo pgtsq_serialize_entry(TSQEntry * tsqe);
extern void pgtsq_parse_item(char * buffer, uint32 p, TSQItem * item);

extern TSQSharedState * pgtsqss;


#endif
