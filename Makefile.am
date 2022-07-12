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
AM_LDFLAGS =


# pkgconfig files
pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = maint/codes.pc
$(pkgconfig_DATA): config.status

EXTRA_DIST += \
  prepare.sh LICENSE.md configure.ac uc-codes.cfg reformat.sh \
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

if USE_DEBUG
AM_CPPFLAGS += -g
AM_CFLAGS   += -g
AM_CXXFLAGS += -g
endif

if USE_DARSHAN
AM_CPPFLAGS += ${DARSHAN_CFLAGS} -DUSE_DARSHAN=1
src_libcodes_la_SOURCES += src/workload/methods/codes-darshan3-io-wrkld.c
LDADD += ${DARSHAN_LIBS}
TESTS += tests/workload/darshan-dump.sh
endif

if USE_RECORDER
AM_CPPFLAGS += ${RECORDER_CPPFLAGS}
src_libcodes_la_SOURCES += src/workload/methods/codes-recorder-io-wrkld.c
endif

if USE_ONLINE
AM_CPPFLAGS += ${ARGOBOTS_CFLAGS} ${SWM_CFLAGS} -DUSE_ONLINE=1
LDADD += ${ARGOBOTS_LIBS}
if USE_SWM
AM_CPPFLAGS += -DUSE_SWM=1
LDADD += ${SWM_LIBS}
src_libcodes_la_SOURCES += src/workload/methods/codes-online-comm-wrkld.C
endif
if USE_CONC
src_libcodes_la_SOURCES += src/workload/methods/codes-conc-online-comm-wrkld.C
AM_CPPFLAGS += ${CONCEPTUAL_CFLAGS} -DUSE_CONC=1
LDADD += ${CONCEPTUAL_LIBS}
endif
endif

if USE_DUMPI
AM_CPPFLAGS += ${DUMPI_CFLAGS} -DUSE_DUMPI=1
src_libcodes_la_SOURCES += src/workload/methods/codes-dumpi-trace-nw-wrkld.c
TESTS += tests/modelnet-test-dragonfly-traces.sh \
		 tests/modelnet-test-dragonfly-custom-traces.sh \
		 tests/modelnet-test-slimfly-traces.sh	\
		 tests/modelnet-test-torus-traces.sh
check_PROGRAMS += src/network-workloads/model-net-mpi-replay
if USE_CORTEX
if USE_PYTHON
if USE_CORTEX_PYTHON
AM_CPPFLAGS += ${CORTEX_PYTHON_CFLAGS} -DENABLE_CORTEX_PYTHON=1
LDADD += ${CORTEX_PYTHON_LIBS}
AM_CPPFLAGS += ${PYTHON_CFLAGS}
LDADD += ${PYTHON_LIBS}
endif
endif
AM_CPPFLAGS += ${CORTEX_CFLAGS} -DENABLE_CORTEX=1
LDADD += ${CORTEX_LIBS}
endif
LDADD += ${DUMPI_LIBS}
endif

if USE_RDAMARIS
AM_CPPFLAGS += ${ROSS_Damaris_CFLAGS} -DUSE_RDAMARIS=1
LDADD += ${ROSS_Damaris_LIBS}
endif
