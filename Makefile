# pg_typesafe — PostgreSQL client for TypeSafe AI (Jev)

MODULE_big = typesafe
OBJS = \
	$(WIN32RES) \
	typesafe.o

EXTENSION = typesafe
DATA = typesafe--0.0.1.sql
PGFILEDESC = "typesafe - TypeSafe AI client for categorical classification"

REGRESS = typesafe
TAP_TESTS = 1

# Standalone checkout uses PGXS.  In the Postgres tree, keep the contrib path.
ifeq ($(wildcard ../../src/Makefile.global),)
  USE_PGXS ?= 1
endif

ifdef USE_PGXS
PG_CONFIG ?= pg_config
PG_CPPFLAGS += $(shell pkg-config --cflags libcurl 2>/dev/null)
SHLIB_LINK += $(shell pkg-config --libs libcurl 2>/dev/null)
ifeq ($(SHLIB_LINK),)
SHLIB_LINK += -lcurl
endif
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
PG_CPPFLAGS += $(LIBCURL_CPPFLAGS)
SHLIB_LINK += $(LIBCURL_LDFLAGS) $(LIBCURL_LDLIBS)
subdir = contrib/typesafe
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
