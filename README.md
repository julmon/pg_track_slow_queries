# pg_track_slow_queries

PostgreSQL (9.5+) extension for slow queries tracking. This extension logs SQL queries and related information into dedicated file, only if query execution duration exceeds a certain amount of time. Logged query list can be retrieve using SQL function `pg_track_slow_queries()`. Log file can be truncated with `pg_track_slow_queries_reset()`.


## Status

Still under development.

## Installation

```console
$ sudo PATH=$PATH:/path/to/pgsql/bin make install
```

## Configuration

The extension library should be loaded with Postgres parameter `shared_preload_libraries` and Postgres instance restarted:
```ini
shared_preload_libraries='pg_track_slow_queries'
```

Then, the extension could be created on `postgres` database with:
```SQL
CREATE EXTENSION pg_track_slow_queries;
```

### Parameters / GUCs

| Parameter                                  | unit   | default | description |
|--------------------------------------------|--------|---------|-------------|
| **pg_track_slow_queries.log_min_duration** | `ms`   | `-1`    | This parameter sets the minimum execution time (in ms) above which queries will be logged. `-1` (default value) means the feature is disabled. |
| **pg_track_slow_queries.compression**      | `bool` | `on`    | Enable or disable row compression. Compression could have impacts on performances but will save disk space.                                    |
| **pg_track_slow_queries.max_file_size**    | `kB`   | `-1`    | Sets the maximum size of storage file. `-1` means no limitation.                                                                               |
| **pg_track_slow_queries.log_plan**         | `bool` | `on`    | Enable execution plan logging.                                                                                                                 |

## Usage

Access to logged queries:

```SQL
SELECT * FROM pg_track_slow_queries();
```
```console
-[ RECORD 1 ]-----+--------------------------------------------------
datetime          | 2019-03-12 11:48:03.783453+01
duration          | 1001.24
username          | julien
appname           | psql
dbname            | postgres
temp_blks_written | 0
hitratio          | 100
ntuples           | 1
query             | SELECT pg_sleep(1);
plan              | {                                                   +
                  |   "Plan": {                                         +
                  |     "Node Type": "Result",                          +
                  |     "Parallel Aware": false,                        +
                  |     "Startup Cost": 0.00,                           +
                  |     "Total Cost": 0.01,                             +
                  |     "Plan Rows": 1,                                 +
                  |     "Plan Width": 4,                                +
                  |     "Output": ["pg_sleep('1'::double precision)"]   +
                  |   }                                                 +
                  | }
```

Reset log file:

```SQL
SELECT * FROM pg_track_slow_queries_reset();
```

## Columns

 1. `datetime`: statement's end of execution datetime (timestamptz)
 2. `duration`: execution duration, in seconds
 3. `username`: username that issued the statement
 4. `appname`: application name
 5. `dbname`: database name
 6. `temp_blks_written`: number of blocks written for temporary files usage
 7. `hitratio`: statement cache hit-ratio
 8. `ntuples`: number of tuples affected by the statement
 9. `query`: the statement
 10. `plan`: statement execution plan (JSON)

## Caveats

 * Do not support utility statements (`VACUUM`, `REINDEX`, etc).
 * Do not tracks parameters values of prepared statements.
