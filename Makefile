EXTENSION    = pg_track_slow_queries
EXTVERSION   = 1.0
PG_CONFIG    = pg_config
MODULE_big   = pg_track_slow_queries
OBJS         = pg_track_slow_queries.o

all:

DATA = $(wildcard *--*.sql)
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
