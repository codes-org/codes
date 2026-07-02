cmake_minimum_required(VERSION 3.17)

# set the project name and version
project(codes LANGUAGES C CXX VERSION 2.0)

include(CMakePrintHelpers)
cmake_print_variables(CMAKE_CURRENT_SOURCE_DIR)

# C++17 is the project-wide baseline (was previously only enabled when Torch
# was found). Required, no compiler extensions, so the build is portable
# across gcc/clang/AppleClang.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED True)

# Build CODES's own targets with -Wundef so the generated config-header feature
# macros (CODES_HAVE_<X>, always 0/1 — see codes_config.h.cmake.in) flag a
# forgotten `#include "codes_config.h"` or a typo'd name as a warning instead of
# a silently-disabled feature. Applies only to this project's compiles, not to
# consumers; ROSS/MPI headers arrive via -isystem and are exempt.
add_compile_options(-Wundef)

# Optional gcov/--coverage instrumentation, applied to the `codes` target (and
# propagated to the binaries/tests that link it) in src/CMakeLists.txt. Off by
# default; the coverage CI job turns it on. gcc/clang only.
option(CODES_ENABLE_COVERAGE "Instrument the codes library with --coverage (gcov)" OFF)

# Optional sanitizer instrumentation, applied globally to ALL CODES targets built
# below (the library, the executables, and the tests) via add_*_options — unlike
# coverage, which scopes to the `codes` target. ROSS is a prebuilt imported target
# (find_package(ROSS) below) and is NOT reinstrumented, so this is CODES-only for
# now; bugs reached through ROSS internals won't be flagged until ROSS itself is
# built with a sanitizer.
#
# One string knob rather than a bool per sanitizer, so a build dir carries exactly
# one sanitizer (matching the separate asan/ubsan presets and CI lanes); pass a
# comma list ("address,undefined") to combine if ever wanted. Off by default; the
# sanitizer CI jobs turn one on. gcc/clang only, and mutually exclusive with
# coverage (the two instrumentations don't compose).
set(CODES_SANITIZER "" CACHE STRING "Build CODES with a sanitizer: '' (none), 'address', or 'undefined'")
set_property(CACHE CODES_SANITIZER PROPERTY STRINGS "" "address" "undefined")
if(CODES_SANITIZER)
    if(CODES_ENABLE_COVERAGE)
        message(FATAL_ERROR "CODES_SANITIZER and CODES_ENABLE_COVERAGE are mutually exclusive; pick one.")
    endif()
    message(STATUS "Building CODES with sanitizer: ${CODES_SANITIZER}")
    # -fno-sanitize-recover=all makes a UBSan finding abort with a nonzero exit
    # (UBSan otherwise prints and continues, so ctest/CI would pass over real UB).
    # -fno-omit-frame-pointer keeps stacks readable. Both compile and link must
    # carry -fsanitize so the runtime is linked into every executable.
    add_compile_options(-fsanitize=${CODES_SANITIZER} -fno-omit-frame-pointer -fno-sanitize-recover=all)
    add_link_options(-fsanitize=${CODES_SANITIZER})
endif()

# Opt-in Doxygen API docs. OFF by default and never part of `all`; the docs
# subdir registers an on-demand `docs` target (see doc/CMakeLists.txt). Output
# lands in the build tree, not the source tree.
option(CODES_BUILD_DOXYGEN "Build Doxygen API docs" OFF)

#prevent cmake from stripping the runtime path (important if shared libraries are imported)
SET(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

# DEPRECATED dep-location knobs (see the deprecation check after find_package
# below). Prefer CMAKE_PREFIX_PATH — pkg_check_modules searches it the same way
# find_package does. Kept working for one release cycle.
set(SWM_PKG_CONFIG_PATH "" CACHE PATH "DEPRECATED: use CMAKE_PREFIX_PATH. Where the SWM pkg-config (.pc) is installed.")
set(UNION_PKG_CONFIG_PATH "" CACHE PATH "DEPRECATED: use CMAKE_PREFIX_PATH. Where the Union pkg-config (.pc) is installed.")
set(ARGOBOTS_PKG_CONFIG_PATH "" CACHE PATH "DEPRECATED: use CMAKE_PREFIX_PATH. Where the argobots pkg-config (.pc) is installed (needed for SWM).")


# ROSS — modern CMake-config discovery. find_package(ROSS) resolves
# <prefix>/lib/cmake/ROSS/ROSSConfig.cmake; point CMAKE_PREFIX_PATH at the
# ROSS install prefix. The ROSS::ROSS imported target carries the include
# dirs, the MPI dependency, and link libraries (linked in src/CMakeLists.txt).
find_package(ROSS CONFIG REQUIRED)

# Compatibility for older ROSS CMake package configs.
# Some ROSS installs provide ROSSConfig.cmake but do not define the modern
# imported target ROSS::ROSS. This CODES tree links against ROSS::ROSS, so
# synthesize that target from the installed ROSS prefix when needed.
if(NOT TARGET ROSS::ROSS)
    message(WARNING "ROSS package did not define ROSS::ROSS; creating compatibility imported target.")

    if(DEFINED ROSS_DIR)
        get_filename_component(_ROSS_CONFIG_DIR "${ROSS_DIR}" ABSOLUTE)
    else()
        set(_ROSS_CONFIG_DIR "")
    endif()

    # In this install layout, ROSSConfig.cmake is under:
    #   <prefix>/lib/ROSSConfig.cmake
    # so the prefix is one directory above ROSS_DIR.
    get_filename_component(_ROSS_PREFIX "${_ROSS_CONFIG_DIR}/.." ABSOLUTE)

    find_library(_ROSS_COMPAT_LIBRARY
        NAMES ROSS ross
        PATHS
            "${_ROSS_PREFIX}/lib"
            "${_ROSS_CONFIG_DIR}"
        NO_DEFAULT_PATH
    )

    find_path(_ROSS_COMPAT_INCLUDE_DIR
        NAMES ross.h
        PATHS
            "${_ROSS_PREFIX}/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/../ross/core"
            "$ENV{HOME}/ross/core"
        NO_DEFAULT_PATH
    )

    if(NOT _ROSS_COMPAT_LIBRARY)
        message(FATAL_ERROR "Could not locate ROSS library for compatibility target. Checked ${_ROSS_PREFIX}/lib and ${_ROSS_CONFIG_DIR}.")
    endif()

    if(NOT _ROSS_COMPAT_INCLUDE_DIR)
        message(FATAL_ERROR "Could not locate ross.h for compatibility target. Checked ${_ROSS_PREFIX}/include and ~/ross/core.")
    endif()

    add_library(ROSS::ROSS UNKNOWN IMPORTED)
    set_target_properties(ROSS::ROSS PROPERTIES
        IMPORTED_LOCATION "${_ROSS_COMPAT_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${_ROSS_COMPAT_INCLUDE_DIR}"
    )

    message(STATUS "Using compatibility ROSS::ROSS library: ${_ROSS_COMPAT_LIBRARY}")
    message(STATUS "Using compatibility ROSS::ROSS include dir: ${_ROSS_COMPAT_INCLUDE_DIR}")
endif()

# PkgConfig discovers the optional SWM/UNION/ARGOBOTS deps below (as imported
# targets). The recommended way to point at a non-standard install is
# CMAKE_PREFIX_PATH (pkg_check_modules searches <prefix>/lib/pkgconfig etc. under
# it automatically, the same variable that locates ROSS) — e.g.
#   -DCMAKE_PREFIX_PATH="/opt/swm;/opt/union;/opt/argobots"
# The legacy *_PKG_CONFIG_PATH cache vars still work for one release cycle: each
# set value is *prepended* to the environment's PKG_CONFIG_PATH (not clobbering
# it) and triggers a deprecation warning so downstream invocations migrate.
find_package(PkgConfig REQUIRED)

foreach(_legacy_dep SWM UNION ARGOBOTS)
    if(${_legacy_dep}_PKG_CONFIG_PATH)
        message(DEPRECATION
            "${_legacy_dep}_PKG_CONFIG_PATH is deprecated and will be removed in a "
            "future release. Point CMAKE_PREFIX_PATH at the dependency's install "
            "prefix instead (pkg_check_modules searches <prefix>/lib/pkgconfig under "
            "CMAKE_PREFIX_PATH), e.g. -DCMAKE_PREFIX_PATH=\"/opt/swm;/opt/union;/opt/argobots\".")
    endif()
endforeach()

set(ENV{PKG_CONFIG_PATH} "$ENV{PKG_CONFIG_PATH}:${SWM_PKG_CONFIG_PATH}:${UNION_PKG_CONFIG_PATH}:${ARGOBOTS_PKG_CONFIG_PATH}")

# MPI — modern imported-target discovery. find_package(MPI) provides the
# MPI::MPI_C / MPI::MPI_CXX imported targets, which carry the include dirs,
# compile flags, and link libraries; targets link them directly (see
# src/CMakeLists.txt). No need to set CC=mpicc — FindMPI locates the wrapper
# compiler automatically and derives the flags from it.
find_package(MPI REQUIRED)


# ============================================================================
# Optional dependencies — tri-state CODES_USE_<X> (AUTO / ON / OFF)
# ----------------------------------------------------------------------------
#   AUTO : probe; enable if found, disable quietly if not.
#   ON   : probe; hard error if not found.
#   OFF  : don't probe; feature disabled.
# Each resolves to an internal USE_<X> bool (names unchanged) consumed by
# src/CMakeLists.txt and the C sources' #if sites.
# ============================================================================

set(DUMPI_BUILD_PATH "" CACHE PATH "Directory where dumpi include and lib are installed")

set(CODES_USE_DUMPI    AUTO CACHE STRING "DUMPI trace workloads (AUTO/ON/OFF)")
set(CODES_USE_SWM      AUTO CACHE STRING "SWM online workloads; requires argobots (AUTO/ON/OFF)")
set(CODES_USE_UNION    AUTO CACHE STRING "UNION online workloads; implies SWM (AUTO/ON/OFF)")
set(CODES_USE_RECORDER AUTO CACHE STRING "Recorder I/O workload, no external dep (AUTO/ON/OFF)")
set(CODES_USE_TORCH    AUTO CACHE STRING "Torch ML models (AUTO/ON/OFF)")
set(CODES_USE_DARSHAN  OFF  CACHE STRING "Darshan I/O workload — not yet wired (AUTO/ON/OFF)")
set(CODES_USE_ZEROMQ   AUTO CACHE STRING "ZeroMQ director-client/zmqml surrogate; needs libzmq + zmq.hpp + rapidjson (AUTO/ON/OFF)")
foreach(_dep DUMPI SWM UNION RECORDER TORCH DARSHAN ZEROMQ)
    set_property(CACHE CODES_USE_${_dep} PROPERTY STRINGS AUTO ON OFF)
endforeach()

# Back-compat: the pre-rename option names map onto CODES_USE_* for one
# deprecation cycle. Remove once downstream invocations have migrated.
foreach(_old TORCH RECORDER)
    if(DEFINED USE_${_old})
        message(WARNING "USE_${_old} is deprecated; use CODES_USE_${_old} (AUTO/ON/OFF).")
        if(USE_${_old})
            set(CODES_USE_${_old} ON CACHE STRING "" FORCE)
        else()
            set(CODES_USE_${_old} OFF CACHE STRING "" FORCE)
        endif()
    endif()
endforeach()

# Resolve a tri-state CODES_USE_<name> against a probe result (found = a path,
# a *_FOUND var, or a bool). Sets the internal ${out_var} bool in the caller.
function(codes_resolve_dep name found out_var)
    if(CODES_USE_${name} STREQUAL "OFF")
        set(${out_var} FALSE PARENT_SCOPE)
    elseif(found)
        set(${out_var} TRUE PARENT_SCOPE)
    elseif(CODES_USE_${name} STREQUAL "ON")
        message(FATAL_ERROR "CODES_USE_${name}=ON but ${name} could not be found/enabled.")
    else()
        set(${out_var} FALSE PARENT_SCOPE)
    endif()
endfunction()


## DUMPI — trace workloads
if(NOT CODES_USE_DUMPI STREQUAL "OFF")
    find_library(DUMPI_LIB undumpi PATHS ${DUMPI_BUILD_PATH}/lib)
endif()
codes_resolve_dep(DUMPI "${DUMPI_LIB}" USE_DUMPI)
if(USE_DUMPI)
    message(STATUS "DUMPI trace workloads enabled (${DUMPI_LIB})")
    set(DUMPI_INCLUDE "${DUMPI_BUILD_PATH}/include" CACHE PATH "Dumpi library include")
else()
    message(STATUS "DUMPI trace workloads disabled")
endif()


## SWM / UNION — online workloads (UNION ⇒ SWM ⇒ argobots)
set(_swm_ok FALSE)
if(NOT CODES_USE_SWM STREQUAL "OFF")
    pkg_check_modules(SWM IMPORTED_TARGET swm)
    if(SWM_FOUND)
        pkg_check_modules(ARGOBOTS IMPORTED_TARGET argobots)
        if(ARGOBOTS_FOUND)
            set(_swm_ok TRUE)
        endif()
    endif()
endif()
codes_resolve_dep(SWM "${_swm_ok}" USE_ONLINE)

if(USE_ONLINE)
    message(STATUS "SWM online workloads enabled (${SWM_LIBRARIES})")
    pkg_get_variable(SWM_DATAROOTDIR swm datarootdir)

    # The online-workload sources (codes-online-comm-wrkld.cxx,
    # codes-conc-online-comm-wrkld.cxx) include SWM model headers that pull in
    # Boost's header-only property_tree / foreach. Declare it explicitly so a
    # missing Boost fails here with a clear message instead of as a cryptic
    # mid-compile error, and so its include dir is attached to the codes target
    # deliberately (via Boost::headers in src/CMakeLists.txt) rather than relying
    # on Boost sitting on the default system include path.
    find_package(Boost REQUIRED)

    set(_union_found FALSE)
    if(NOT CODES_USE_UNION STREQUAL "OFF")
        pkg_check_modules(UNION IMPORTED_TARGET union)
        set(_union_found ${UNION_FOUND})
    endif()
    codes_resolve_dep(UNION "${_union_found}" USE_UNION)
    if(USE_UNION)
        message(STATUS "UNION online workloads enabled (${UNION_LIBRARIES})")
        pkg_get_variable(UNION_DATAROOTDIR union datarootdir)
    else()
        set(USE_SWM TRUE)
    endif()
else()
    message(STATUS "SWM/UNION online workloads disabled")
    if(CODES_USE_UNION STREQUAL "ON")
        message(FATAL_ERROR "CODES_USE_UNION=ON requires SWM/online (set CODES_USE_SWM and provide SWM+argobots).")
    endif()
endif()


## RECORDER — built-in I/O workload, no external dependency
codes_resolve_dep(RECORDER TRUE USE_RECORDER)


## DARSHAN — stub: the probe + the commented darshan source in src/ aren't
## wired up yet, so this resolves OFF today (CODES_USE_DARSHAN=ON errors).
codes_resolve_dep(DARSHAN FALSE USE_DARSHAN)


## TORCH — ML models
if(NOT CODES_USE_TORCH STREQUAL "OFF")
    find_package(Torch QUIET)
endif()
codes_resolve_dep(TORCH "${Torch_FOUND}" USE_TORCH)
if(USE_TORCH)
    message(STATUS "Torch ML models enabled")
else()
    message(STATUS "Torch ML models disabled")
endif()


## ZeroMQ — director-client / zmqml surrogate (the CLIENT half; the ML server is
## a separate process reached over a socket). The requester needs the libzmq C++
## binding (zmq.hpp) and the header-only rapidjson, both build-time only. AUTO-
## probed like the other optional deps; the requester target + linkage live in
## src/surrogate/zmqml + src/CMakeLists.txt, gated on the internal USE_ZEROMQ bool.
set(_zeromq_found FALSE)
if(NOT CODES_USE_ZEROMQ STREQUAL "OFF")
    pkg_check_modules(ZeroMQ IMPORTED_TARGET libzmq)
    # zmq.hpp (cppzmq) ships with libzmq-dev on some distros, separately on others.
    find_path(ZMQ_CPP_INCLUDE_DIR zmq.hpp HINTS ${ZeroMQ_INCLUDE_DIRS})
    find_path(RapidJSON_INCLUDE_DIR rapidjson/document.h)
    if(ZeroMQ_FOUND AND ZMQ_CPP_INCLUDE_DIR AND RapidJSON_INCLUDE_DIR)
        set(_zeromq_found TRUE)
    endif()
endif()
codes_resolve_dep(ZEROMQ "${_zeromq_found}" USE_ZEROMQ)
if(USE_ZEROMQ)
    message(STATUS "ZeroMQ director-client/zmqml surrogate enabled "
        "(${ZeroMQ_LIBRARIES}; rapidjson at ${RapidJSON_INCLUDE_DIR})")
else()
    message(STATUS "ZeroMQ director-client/zmqml surrogate disabled")
endif()

add_subdirectory(src)


# Feature-availability macros consumed by codes_config.h.cmake.in. CODES_HAVE_<X>
# mirrors the resolved USE_<X> bool; #cmakedefine emits the #define only when set.
# This replaces the old -DUSE_<X> command-line defines: feature state now lives in
# the namespaced, installed codes_config.h instead of leaking onto compile lines.
set(CODES_HAVE_DUMPI    ${USE_DUMPI})
set(CODES_HAVE_ONLINE   ${USE_ONLINE})
set(CODES_HAVE_SWM      ${USE_SWM})
set(CODES_HAVE_UNION    ${USE_UNION})
set(CODES_HAVE_RECORDER ${USE_RECORDER})
set(CODES_HAVE_TORCH    ${USE_TORCH})
set(CODES_HAVE_ZEROMQ   ${USE_ZEROMQ})
set(CODES_HAVE_DARSHAN  ${USE_DARSHAN})

configure_file(codes_config.h.cmake.in codes_config.h)

add_subdirectory(doc/example)

if(CODES_BUILD_DOXYGEN)
    add_subdirectory(doc)
endif()

# Tests are gated solely on BUILD_TESTING (the canonical CTest knob). A
# $<CONFIG:...> genex can't gate this since add_subdirectory() runs at
# configure time, and the old `STREQUAL "RELEASE"` compare never matched the
# conventional `Release` spelling anyway, so tests already built in every
# standard config.
if(BUILD_TESTING)
    include(CTest)
    set(CODES_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    set(CODES_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}")
    add_subdirectory(tests)
endif()
