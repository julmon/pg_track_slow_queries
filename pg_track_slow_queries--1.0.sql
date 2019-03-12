\echo Use "CREATE EXTENSION pg_track_slow_queries" to load this file. \quit

SET client_encoding = 'UTF8';

CREATE FUNCTION pg_track_slow_queries(
    OUT datetime TIMESTAMP WITH TIME ZONE,
    OUT duration FLOAT,
    OUT username VARCHAR(256),
    OUT appname VARCHAR(256),
    OUT dbname VARCHAR(256),
    OUT temp_blks_written BIGINT,
    OUT hitratio FLOAT,
    OUT ntuples BIGINT,
    OUT query TEXT,
    OUT plan JSON
)
RETURNS SETOF record
LANGUAGE c COST 1000
AS '$libdir/pg_track_slow_queries', 'pg_track_slow_queries';
REVOKE ALL ON FUNCTION pg_track_slow_queries() FROM public;

CREATE FUNCTION pg_track_slow_queries_reset()
    RETURNS void
    LANGUAGE c COST 1000
    AS '$libdir/pg_track_slow_queries', 'pg_track_slow_queries_reset';
REVOKE ALL ON FUNCTION pg_track_slow_queries_reset() FROM public;
