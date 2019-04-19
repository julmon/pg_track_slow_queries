BEGIN;
SELECT plan(12);

SET pg_track_slow_queries.log_min_duration TO 100;
SET pg_track_slow_queries.log_plan TO on;

SELECT is(
  (SELECT COUNT(*) FROM pg_proc WHERE proname='pg_track_slow_queries')::INT,
  1,
  'pg_track_slow_queries() function exists'
);

SELECT is(
  (SELECT COUNT(*) FROM pg_proc WHERE proname='pg_track_slow_queries_reset')::INT,
  1,
  'pg_track_slow_queries_reset() function exists'
);

SELECT ok(
  (SELECT true FROM pg_track_slow_queries_reset())::BOOL,
  'pg_track_slow_queries_reset() ran without error'
);

SELECT is(
  (SELECT COUNT(*) FROM pg_track_slow_queries())::INT,
  0,
  'log file is empty'
);

SELECT ok(
  (SELECT true FROM pg_sleep(0.2))::BOOL,
  'pg_sleep(0.1) should trigger query logging'
);

SELECT is(
  (SELECT COUNT(*) FROM pg_track_slow_queries())::INT,
  1,
  'log file contains 1 row'
);

SELECT ok(
  (SELECT LENGTH(plan::TEXT) > 0 FROM pg_track_slow_queries() LIMIT 1)::BOOL,
  'plan column not empty'
);

SELECT ok(
  (SELECT (
    datetime IS NOT NULL AND
    duration is NOT NULL AND
    username IS NOT NULL AND
    appname IS NOT NULL AND
    dbname IS NOT NULL AND
    temp_blks_written IS NOT NULL AND
    hitratio IS NOT NULL AND
    ntuples IS NOT NULL AND
    query IS NOT NULL AND
    plan IS NOT NULL
  ) FROM pg_track_slow_queries() LIMIT 1)::BOOL,
  'no null value'
);

SELECT ok(
  (SELECT true FROM pg_track_slow_queries_reset())::BOOL,
  'pg_track_slow_queries_reset() ran without error'
);

SET pg_track_slow_queries.log_plan TO off;

SELECT ok(
  (SELECT true FROM pg_sleep(0.1))::BOOL,
  'pg_sleep(0.1) should trigger query logging'
);

SELECT is(
  (SELECT COUNT(*) FROM pg_track_slow_queries())::INT,
  1,
  'log file contains 1 row'
);

SELECT ok(
  (SELECT LENGTH(plan::TEXT) = 0 FROM pg_track_slow_queries() LIMIT 1)::BOOL,
  'plan column is empty'
);


ROLLBACK;
