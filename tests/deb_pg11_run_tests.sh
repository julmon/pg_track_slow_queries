#!/bin/bash -eux

apt update
apt install wget gnupg2 sudo gcc make -y

# Install PGDG apt repo acces
echo "deb http://apt.postgresql.org/pub/repos/apt/ stretch-pgdg main" > /etc/apt/sources.list.d/pgdg.list
wget --quiet -O - https://www.postgresql.org/media/keys/ACCC4CF8.asc | apt-key add -

apt update
apt install postgresql-11 postgresql-server-dev-11 postgresql-11-pgtap -y


#####################
# PG 11
#####################
pg_ctlcluster 11 main start

sudo -u postgres psql -c "SELECT version();"

PATH=$PATH:/usr/lib/postgresql/11/bin make -C /workspace clean install

sudo -u postgres psql -c "ALTER SYSTEM SET shared_preload_libraries TO 'pg_track_slow_queries';"

pg_ctlcluster 11 main restart

sudo -u postgres psql -c "CREATE DATABASE tap;"
sudo -u postgres psql -d tap -c "CREATE EXTENSION pgtap;"
sudo -u postgres psql -d tap -c "CREATE EXTENSION pg_track_slow_queries;"

sudo -u postgres pg_prove -d tap /workspace/tests/sql/t.sql
