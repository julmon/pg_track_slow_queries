#!/bin/bash -eux

docker-compose run --rm debian-pg11-pgtsq
docker-compose run --rm debian-pg10-pgtsq
docker-compose run --rm debian-pg96-pgtsq
docker-compose run --rm debian-pg95-pgtsq
