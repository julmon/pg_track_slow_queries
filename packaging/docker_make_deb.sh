#!/bin/bash -eux

# Install dependancies
apt update
apt install curl gnupg2 lsb-release -y
curl https://www.postgresql.org/media/keys/ACCC4CF8.asc | apt-key add -
sh -c 'echo "deb http://apt.postgresql.org/pub/repos/apt/ $(lsb_release -cs)-pgdg main" > /etc/apt/sources.list.d/pgdg.list'
apt update
apt install make gcc dh-make postgresql-common postgresql-server-dev-all devscripts build-essential lintian -y

# Find package version from debian/changelog
cd /workspace/pg-track-slow-queries-src
PKGVER=$(dpkg-parsechangelog | grep -E '^Version:' | sed 's/Version: \([0-9\.]\+\)-.*/\1/g')

# Make a copy of source tree and work with it
mkdir /tmp/build
cp -r /workspace/pg-track-slow-queries-src /tmp/build/pg-track-slow-queries-${PKGVER}
cd /tmp/build/pg-track-slow-queries-${PKGVER}
rm -rf .git

# Packages building
pg_buildext updatecontrol
make -f debian/rules orig
debuild -us -uc -sa

# Copy .deb files into workspace
mkdir -p /workspace/pg-track-slow-queries-src/packaging/debian
chmod -R a+rwx /workspace/pg-track-slow-queries-src/packaging/debian

cp /tmp/build/*.gz /workspace/pg-track-slow-queries-src/packaging/debian/.
cp /tmp/build/*.deb /workspace/pg-track-slow-queries-src/packaging/debian/.
cp /tmp/build/*.changes /workspace/pg-track-slow-queries-src/packaging/debian/.
cp /tmp/build/*.buildinfo /workspace/pg-track-slow-queries-src/packaging/debian/.
cp /tmp/build/*.build /workspace/pg-track-slow-queries-src/packaging/debian/.
cp /tmp/build/*.dsc /workspace/pg-track-slow-queries-src/packaging/debian/.
ls -lha /workspace/pg-track-slow-queries-src/packaging/debian/
