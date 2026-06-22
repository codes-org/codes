cmake_minimum_required(VERSION 3.17)

# set the project name and version
project(codes LANGUAGES C CXX VERSION 2.0)

include(CMakePrintHelpers)
cmake_print_variables(CMAKE_CURRENT_SOURCE_DIR)

set(CMAKE_INSTALL_PREFIX "${PROJECT_BINARY_DIR}" CACHE PATH "Where to install CODES")

# C++17 is the project-wide baseline (was previously only enabled when Torch
# was found). Required, no compiler extensions, so the build is portable
# across gcc/clang/AppleClang.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED True)

#prevent cmake from stripping the runtime path (important if shared libraries are imported)
SET(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

set(SWM_PKG_CONFIG_PATH "" CACHE PATH "Where is the SWM PKG_CONFIG installed?")
set(UNION_PKG_CONFIG_PATH "" CACHE PATH "Where is the Union PKG_CONFIG installed?")
set(ARGOBOTS_PKG_CONFIG_PATH "" CACHE PATH "Where is argobots PKG_COPNFIG installed? Necessary for SWM")
set(DAMARIS_PKG_CONFIG_PATH "" CACHE PATH "Where is the damaris PKG_CONFIG installed?")


# ROSS — modern CMake-config discovery. find_package(ROSS) resolves
# <prefix>/lib/cmake/ROSS/ROSSConfig.cmake; point CMAKE_PREFIX_PATH at the
# ROSS install prefix. The ROSS::ROSS imported target carries the include
# dirs, the MPI dependency, and link libraries (linked in src/CMakeLists.txt).
find_package(ROSS CONFIG REQUIRED)

# PkgConfig still discovers the optional SWM/UNION/ARGOBOTS deps below
# (modernized to imported targets in a later step); their *_PKG_CONFIG_PATH
# cache vars feed PKG_CONFIG_PATH here.
find_package(PkgConfig REQUIRED)
set(ENV{PKG_CONFIG_PATH} "${SWM_PKG_CONFIG_PATH}:${UNION_PKG_CONFIG_PATH}:${ARGOBOTS_PKG_CONFIG_PATH}")

# MPI — modern imported-target discovery. find_package(MPI) provides the
# MPI::MPI_C / MPI::MPI_CXX imported targets, which carry the include dirs,
# compile flags, and link libraries; targets link them directly (see
# src/CMakeLists.txt). No need to set CC=mpicc — FindMPI locates the wrapper
# compiler automatically and derives the flags from it.
find_package(MPI REQUIRED)


## DUMPI
set(DUMPI_BUILD_PATH "" CACHE PATH "Directory where dumpi include and lib are installed")
find_library(DUMPI_LIB undumpi PATHS ${DUMPI_BUILD_PATH}/lib)
if(NOT DUMPI_LIB)
        message(STATUS "Undumpi library not found, DUMPI trace workloads disabled")
        unset(USE_DUMPI)
else(DUMPI_LIB)
        message(STATUS "Undumpi library found ${DUMPI_LIB}")
        set(DUMPI_INCLUDE "${DUMPI_BUILD_PATH}/include" CACHE PATH "Dumpi library include")
        list(APPEND CODES_COMPILE_DEFINITIONS USE_DUMPI=1)
        set(USE_DUMPI true)
endif()

# SWM and UNION (both require ARGOBOTS to function)
pkg_check_modules(SWM IMPORTED_TARGET swm)
if(NOT SWM_FOUND)
    message(STATUS "SWM Library Not Found, Online workloads disabled")

else(SWM_FOUND)
    message(STATUS "SWM Library Found: ${SWM_LIBRARIES}")
    pkg_check_modules(ARGOBOTS REQUIRED IMPORTED_TARGET argobots)
    if(NOT ARGOBOTS_FOUND)
        message(STATUS "Argobots Library Not Found, Online workloads disabled")

    else(ARGOBOTS_FOUND)
        message(STATUS "Argobots Library Found: ${ARGOBOTS_LIBRARIES}")

        pkg_get_variable(SWM_DATAROOTDIR swm datarootdir)
        cmake_print_variables(SWM_DATAROOTDIR)

        list(APPEND CODES_COMPILE_DEFINITIONS USE_ONLINE=1)
        set(USE_ONLINE true)

        pkg_check_modules(UNION IMPORTED_TARGET union)
        if(NOT UNION_FOUND)
            message(STATUS "UNION Library Not Found, SWM-only online workloads enabled")
            list(APPEND CODES_COMPILE_DEFINITIONS USE_SWM=1)
            set(USE_SWM true)
        else(UNION_FOUND)
            message(STATUS "UNION Library Found: ${UNION_LIBRARIES}")
            pkg_get_variable(UNION_DATAROOTDIR union datarootdir)
            cmake_print_variables(UNION_DATAROOTDIR)
            
            list(APPEND CODES_COMPILE_DEFINITIONS USE_UNION=1)
            set(USE_UNION true)
        endif()
    endif()
endif()


## RECORDER
option(USE_RECORDER "use recorder io workload" ON)
if(USE_RECORDER)
        list(APPEND CODES_COMPILE_DEFINITIONS USE_RECORDER=1)
endif()

## DARSHAN


## DAMARIS
# pkg_check_modules(DAMARIS IMPORTED_TARGET)
# if(NOT DAMARIS_FOUND)
#         message(STATUS "DAMARIS Library not found, Damaris disabled")
# else(DAMARIS_FOUND)
#         set(USE_DAMARIS true)
# endif()

## TORCH loading ML models
if((NOT DEFINED USE_TORCH) OR USE_TORCH)
    find_package(Torch)
    if(Torch_FOUND)
        list(APPEND CODES_COMPILE_DEFINITIONS USE_TORCH)
        set(USE_TORCH true)
        message(STATUS "Loading TORCH models enabled.")
    else()
        set(USE_TORCH false)
        message(STATUS "Torch library not found. Loading TORCH models disabled.")
    endif()
else()
    message(STATUS "Loading TORCH models NOT enabled.")
endif()

add_subdirectory(src)


configure_file(codes_config.h.cmake.in codes_config.h)

add_subdirectory(doc/example)

string(COMPARE NOTEQUAL "RELEASE" "${CMAKE_BUILD_TYPE}" not_release)
if(BUILD_TESTING AND not_release)
    include(CTest)
    set(CODES_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    set(CODES_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}")
    add_subdirectory(tests)
endif()
