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

# pkgconfig files
pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = maint/codes-base.pc
$(pkgconfig_DATA): config.status

EXTRA_DIST += prepare.sh COPYRIGHT configure.ac reformat.sh uc-codes.cfg

AM_CPPFLAGS = -I$(top_srcdir)/src ${ROSS_CPPFLAGS}

AM_CFLAGS = ${ROSS_CFLAGS}

AM_LIBS = ${ROSS_LIBS}

AM_CXXFLAGS = $(AM_CFLAGS)

include Make.rules

include $(top_srcdir)/scripts/Makefile.subdir
include $(top_srcdir)/src/Makefile.subdir
include $(top_srcdir)/tests/Makefile.subdir
include $(top_srcdir)/doc/Makefile.subdir

if USE_DARSHAN
AM_CPPFLAGS += ${DARSHAN_CFLAGS} -DUSE_DARSHAN=1
src_libcodes_base_a_SOURCES += src/workload/methods/codes-darshan-io-wrkld.c
tests_workload_codes_workload_test_LDADD += ${DARSHAN_LIBS}
tests_workload_codes_workload_mpi_replay_LDADD += ${DARSHAN_LIBS}
endif

if USE_RECORDER
AM_CPPFLAGS += ${RECORDER_CPPFLAGS}
src_libcodes_base_a_SOURCES += src/workload/methods/codes-recorder-io-wrkld.c
endif

if USE_DUMPI
AM_CPPFLAGS += ${DUMPI_CFLAGS} -DUSE_DUMPI=1
src_libcodes_base_a_SOURCES += src/workload/methods/codes-dumpi-trace-nw-wrkld.c 
tests_workload_codes_workload_test_LDADD += ${DUMPI_LIBS}
tests_workload_codes_workload_mpi_replay_LDADD += ${DUMPI_LIBS}
AM_LIBS += ${DUMPI_LIBS}
endif
