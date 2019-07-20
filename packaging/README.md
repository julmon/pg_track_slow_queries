# Packaging

This folder contains infrastructure needed to build debian (`buster` & `stretch`) and centos7 packages using docker images.

# Requirements

It requires a working docker environment and `docker-compose` tool.

# Debian

To build packages for debian `buster`, just run:
```console
$ make deb
```

For debian `stretch`:
```console
$ make deb-stretch
```

Once `make` command's done, built packages are located in `./debian/` folder.
