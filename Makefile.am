AUTOMAKE_OPTIONS = foreign
ACLOCAL_AMFLAGS = -I m4

bin_PROGRAMS =
bin_SCRIPTS =
noinst_LIBRARIES =
noinst_PROGRAMS =
lib_LTLIBRARIES =
noinst_HEADERS =
TESTS =
check_PROGRAMS =
EXTRA_PROGRAMS =
CLEANFILES = $(bin_SCRIPTS)
EXTRA_DIST =
BUILT_SOURCES =

# pkgconfig files
pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = maint/codes.pc
$(pkgconfig_DATA): config.status

EXTRA_DIST += \
  prepare.sh COPYRIGHT configure.ac uc-codes.cfg reformat.sh \
  misc/README misc/ptrn_loggp-2.4.6.patch CONTRIBUTORS.md \
  README.md

AM_CPPFLAGS = -I$(top_srcdir)/src ${ROSS_CFLAGS} 

AM_CFLAGS =

AM_CXXFLAGS = $(AM_CFLAGS)

LDADD = $(lib_LTLIBRARIES) $(ROSS_LIBS)

include Make.rules

include $(top_srcdir)/scripts/Makefile.subdir
include $(top_srcdir)/src/Makefile.subdir
include $(top_srcdir)/tests/Makefile.subdir
include $(top_srcdir)/doc/Makefile.subdir

if USE_DARSHAN
AM_CPPFLAGS += ${DARSHAN_CFLAGS} -DUSE_DARSHAN=1
src_libcodes_la_SOURCES += src/workload/methods/codes-darshan-io-wrkld.c
LDADD += ${DARSHAN_LIBS}
TESTS += tests/workload/darshan-dump.sh
endif

if USE_RECORDER
AM_CPPFLAGS += ${RECORDER_CPPFLAGS}
src_libcodes_la_SOURCES += src/workload/methods/codes-recorder-io-wrkld.c
endif

if USE_DUMPI
AM_CPPFLAGS += ${DUMPI_CFLAGS} -DUSE_DUMPI=1
src_libcodes_la_SOURCES += src/workload/methods/codes-dumpi-trace-nw-wrkld.c
LDADD += ${DUMPI_LIBS}
if USE_CORTEX
if USE_PYTHON
if USE_BOOST
AM_CPPFLAGS += ${CORTEX_CFLAGS} -DENABLE_CORTEX=1
LDADD += ${CORTEX_LIBS}
AM_CPPFLAGS += ${PYTHON_CFLAGS}
LDADD += ${PYTHON_LIBS}
AM_CPPFLAGS += ${BOOST_CFLAGS}
LDADD += ${BOOST_LIBS}
endif
endif
endif
endif

