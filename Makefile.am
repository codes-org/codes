AUTOMAKE_OPTIONS = foreign

bin_PROGRAMS =
bin_SCRIPTS =
noinst_LIBRARIES =
noinst_PROGRAMS =
lib_LIBRARIES =
noinst_HEADERS =
TESTS =
check_PROGRAMS =
EXTRA_PROGRAMS =
CLEANFILES = $(bin_SCRIPTS)
EXTRA_DIST =
BUILT_SOURCES =

EXTRA_DIST += prepare.sh

AM_CPPFLAGS = -I$(top_srcdir)/src ${CODES_BASE_CFLAGS}

AM_CFLAGS = ${CODES_BASE_CFLAGS}

AM_LIBS = ${CODES_BASE_LIBS}

AM_CXXFLAGS = $(AM_CFLAGS)

include Make.rules

include $(top_srcdir)/src/models/Makefile.subdir
