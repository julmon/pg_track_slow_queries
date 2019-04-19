EXTENSION    = pg_track_slow_queries
EXTVERSION   = $(grep default_version pg_track_slow_queries.control | sed "s/^default_version = '\([^']\+\)'$/\1/g")
PG_CONFIG    ?= pg_config
MODULE_big   = pg_track_slow_queries
OBJS         = pg_track_slow_queries.o worker.o utils.o

all:

DATA = $(wildcard *--*.sql)
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
