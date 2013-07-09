AUTOMAKE_OPTIONS = foreign
ACLOCAL_AMFLAGS = -I m4

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
src_models_CESOURCES =


EXTRA_DIST += prepare.sh

AM_CPPFLAGS = -I$(top_srcdir)/src ${ROSS_CPPFLAGS}

AM_CFLAGS = ${ROSS_CFLAGS}

AM_LIBS = ${ROSS_LIBS}

AM_CXXFLAGS = $(AM_CFLAGS)

include Make.rules

include $(top_srcdir)/src/Makefile.subdir
