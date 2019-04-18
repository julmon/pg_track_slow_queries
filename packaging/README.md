# Packaging

This folder contains infrastructure needed to build debian (`stretch`) and centos7 packages using docker images.

# Requirements

It requires a working docker environment and `docker-compose` tool.

# Debian

For debian packages, just run:
```console
$ make deb
```

Once `make` command's done, built packages are located in `./debian/` folder.
