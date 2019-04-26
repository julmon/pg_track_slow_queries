#!/bin/bash -eux

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"

PGPORT="${PGPORT:-5433}"
PGVERSION="${PGVERSION:-11}"

pg_ctlcluster $PGVERSION main start

sudo -u postgres psql -p $PGPORT -c "SELECT version();"

PG_CONFIG=/usr/lib/postgresql/$PGVERSION/bin/pg_config make -C ${DIR}/.. clean install
PG_CONFIG=/usr/lib/postgresql/$PGVERSION/bin/pg_config make -C ${DIR}/.. clean

sudo -u postgres psql -p $PGPORT -c "ALTER SYSTEM SET shared_preload_libraries TO 'pg_track_slow_queries';"

pg_ctlcluster $PGVERSION main restart

sudo -u postgres psql -p $PGPORT -c "CREATE DATABASE tap;"
sudo -u postgres psql -p $PGPORT -d tap -c "CREATE EXTENSION pgtap;"
sudo -u postgres psql -p $PGPORT -d tap -c "CREATE EXTENSION pg_track_slow_queries;"
cp ${DIR}/sql/t.sql /tmp/t.sql
sudo -u postgres pg_prove -f -p $PGPORT -d tap /tmp/t.sql
