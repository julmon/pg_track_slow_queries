#!/bin/sh

apk add sudo abuild gcc
adduser -S fakeuser -G abuild
sudo -u fakeuser abuild-keygen -a -n

mkdir -p /workspace/pg-track-slow-queries-src/packaging/alpine
chmod -R a+rwx /workspace/pg-track-slow-queries-src/packaging/alpine

cp -r /workspace/pg-track-slow-queries-src /tmp/build
chown -R fakeuser /tmp/build
cd /tmp/build/alpine
sudo -u fakeuser abuild -r
cp  /home/fakeuser/packages/build/x86_64/postgresql_*_pg_track_slow_queries-*.apk /workspace/pg-track-slow-queries-src/packaging/alpine/.
