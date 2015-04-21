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

# pkgconfig files
pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = maint/codes-net.pc
$(pkgconfig_DATA): config.status

EXTRA_DIST += \
  prepare.sh TODO.txt configure.ac uc-codes.cfg reformat.sh \
  misc/README misc/ptrn_loggp-2.4.6.patch \
  doc/BUILD_STEPS doc/GETTING_STARTED doc/RELEASE_NOTES

AM_CPPFLAGS = -I$(top_srcdir)/src ${CODES_BASE_CFLAGS}

AM_CFLAGS = ${CODES_BASE_CFLAGS}

AM_LIBS = ${CODES_BASE_LIBS}

AM_CXXFLAGS = $(AM_CFLAGS)

include Make.rules

include $(top_srcdir)/src/models/Makefile.subdir
include $(top_srcdir)/tests/Makefile.subdir
