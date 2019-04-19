#!/bin/bash -eux

#####################
# PG 11
#####################
PORT=5433
pg_ctlcluster 11 main start

sudo -u postgres psql -p $PORT -c "SELECT version();"

PATH=$PATH:/usr/lib/postgresql/11/bin make -C /workspace clean install

sudo -u postgres psql -p $PORT -c "ALTER SYSTEM SET shared_preload_libraries TO 'pg_track_slow_queries';"

pg_ctlcluster 11 main restart

sudo -u postgres psql -p $PORT -c "CREATE DATABASE tap;"
sudo -u postgres psql -p $PORT -d tap -c "CREATE EXTENSION pgtap;"
sudo -u postgres psql -p $PORT -d tap -c "CREATE EXTENSION pg_track_slow_queries;"

sudo -u postgres pg_prove -p $PORT -d tap /workspace/tests/sql/t.sql
