#!/bin/bash -eux

# Find package version from debian/changelog
cd /workspace/pg-track-slow-queries-src
PKGVER=$(dpkg-parsechangelog | grep -E '^Version:' | sed 's/Version: \([0-9\.]\+\)-.*/\1/g')
DEBVER=$(lsb_release -sc)

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
mkdir -p /workspace/pg-track-slow-queries-src/packaging/debian/${DEBVER}

cp /tmp/build/*.gz /workspace/pg-track-slow-queries-src/packaging/debian/${DEBVER}/.
cp /tmp/build/*.deb /workspace/pg-track-slow-queries-src/packaging/debian/${DEBVER}/.
cp /tmp/build/*.changes /workspace/pg-track-slow-queries-src/packaging/debian/${DEBVER}/.
cp /tmp/build/*.buildinfo /workspace/pg-track-slow-queries-src/packaging/debian/${DEBVER}/.
cp /tmp/build/*.build /workspace/pg-track-slow-queries-src/packaging/debian/${DEBVER}/.
cp /tmp/build/*.dsc /workspace/pg-track-slow-queries-src/packaging/debian/${DEBVER}/.
ls -lha /workspace/pg-track-slow-queries-src/packaging/debian/${DEBVER}
