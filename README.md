# pg_track_slow_queries

PostgreSQL extension for slow queries tracking. This extension logs SQL queries and related informations into dedicated file, only if query execution duration exceeds a certain amount of time. Logged query list can be retreive using SQL function `pg_track_slow_queries()`. Log file can be truncated with `pg_track_slow_queries_reset()`.

## Status

Still under development.

## Installation

```console
$ sudo PATH=$PATH:/path/to/pgsql/bin make install
```

## Configuration

The extension library should be loaded with Postgres parameter `shared_preload_libraries`:
```ini
shared_preload_libraries='pg_track_slow_queries'
```

### Parameters

| Parameter                                  | unit   | default | description |
|--------------------------------------------|--------|---------|-------------|
| **pg_track_slow_queries.log_min_duration** | `ms`   | `-1`    | This parameter sets the minimum execution time (in ms) above which queries will be logged. `-1` (default value) means the feature is disabled. |
| **pg_track_slow_queries.compression**      | `bool` | `on`    | Enable or disable row compression. Compression could have impacts on performances but will save disk space.                                    |

## Usage

Acces to logged queries:

```console
postgres=# SELECT * FROM pg_track_slow_queries();
-[ RECORD 1 ]-----+--------------------------------------------------
datetime          | 2018-08-28 10:25:09.128408+02
duration          | 2002.26
username          | julien
dbname            | postgres
temp_blks_written | 0
hitratio          | 100
ntuples           | 1
query             | SELECT pg_sleep(2);
plan              | {                                                +
                  |   "Plan": {                                      +
                  |     "Node Type": "Result",                       +
                  |     "Parallel Aware": false,                     +
                  |     "Startup Cost": 0.00,                        +
                  |     "Total Cost": 0.01,                          +
                  |     "Plan Rows": 1,                              +
                  |     "Plan Width": 4,                             +
                  |     "Output": ["pg_sleep('2'::double precision)"]+
                  |   }                                              +
                  | }
```

Reset log file:

```console
postgres=# SELECT * FROM pg_track_slow_queries_reset();
```

## Columns

 1. `datetime`: statement's end of execution datetime (timestamptz)
 2. `duration`: execution duration, in seconds
 3. `username`: username that issued the statement
 4. `dbname`: database name
 5. `temp_blks_written`: number of blocks written for temporary files usage
 6. `hitratio`: statement cache hit-ratio
 7. `ntuples`: number of tuples affected by the statement
 8. `query`: the statement
 9. `plan`: statement execution plan (JSON)

## Caveats

 * Do not support utility statements (`VACUUM`, `REINDEX`, etc).
 * Do not tracks parameters values of prepared statements.
