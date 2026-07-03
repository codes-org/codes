# GoogleTest — the unit-test framework.
#
# Test-only: built only when BUILD_TESTING is on. Wired from the thirdparty
# dispatcher (thirdparty/CMakeLists.txt), which gates its add_subdirectory of this
# wrapper on BUILD_TESTING — an option declared in the top-level CMakeLists before
# the dispatcher runs. It is never installed or exported.
#
# Vendored as a reduced copy of the upstream repo under googletest/ (see
# README.kitware.md), integrated the standard way — add_subdirectory of upstream's
# own CMake, the same approach ADIOS2 uses — rather than hand-compiling gtest-all.cc.
message_start_thirdparty()

# gtest only: gmock isn't needed (death tests live in gtest core). With BUILD_GMOCK
# OFF, upstream's root CMake recurses into googletest/ alone.
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
# Keep the framework out of the CODES install/export.
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
# Windows-only knob (link the CRT dynamically); harmless elsewhere. Matches
# upstream's recommendation for projects embedding googletest.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

# The project builds its own sources with -Wundef (see the top-level CMakeLists), a
# directory property inherited here. Upstream googletest is not -Wundef-clean, so
# disable it for gtest's own compiles — appended after the inherited -Wundef, so it
# wins — keeping third-party warnings out of the build log.
add_compile_options(-Wno-undef)

# EXCLUDE_FROM_ALL: gtest compiles only when a test target actually links it, never
# as part of `all`. Upstream exposes the gtest / gtest_main targets and the
# GTest::gtest / GTest::gtest_main aliases that unit-test targets link.
add_subdirectory(googletest EXCLUDE_FROM_ALL)

# Upstream googletest doesn't mark its headers SYSTEM, so a unit-test TU that
# #includes <gtest/gtest.h> would inherit the project's own warning flags (the
# -Wundef above, say) through the framework's headers. Re-expose the include dirs
# as SYSTEM on the gtest targets so every consumer gets them via -isystem and stays
# warning-clean.
foreach(_gtest_tgt gtest gtest_main)
  if(TARGET ${_gtest_tgt})
    get_target_property(_gtest_inc ${_gtest_tgt} INTERFACE_INCLUDE_DIRECTORIES)
    if(_gtest_inc)
      set_target_properties(${_gtest_tgt} PROPERTIES
                INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_gtest_inc}")
    endif()
  endif()
endforeach()
