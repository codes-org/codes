cmake_minimum_required(VERSION 3.17)

# set the project name and version
project(codes LANGUAGES C CXX VERSION 2.0)

include(CMakePrintHelpers)
cmake_print_variables(CMAKE_CURRENT_SOURCE_DIR)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/src/cmake/")
cmake_print_variables(CMAKE_MODULE_PATH)

set(CMAKE_INSTALL_PREFIX "${PROJECT_BINARY_DIR}" CACHE PATH "Where to install CODES")

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED True)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED True)

#prevent cmake from stripping the runtime path (important if shared libraries are imported)
SET(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

set(ROSS_PKG_CONFIG_PATH "" CACHE PATH "Where is ROSS PKG_CONFIG is installed?")
set(SWM_PKG_CONFIG_PATH "" CACHE PATH "Where is the SWM PKG_CONFIG installed?")
set(UNION_PKG_CONFIG_PATH "" CACHE PATH "Where is the Union PKG_CONFIG installed?")
set(ARGOBOTS_PKG_CONFIG_PATH "" CACHE PATH "Where is argobots PKG_COPNFIG installed? Necessary for SWM")
set(DAMARIS_PKG_CONFIG_PATH "" CACHE PATH "Where is the damaris PKG_CONFIG installed?")


find_package(PkgConfig REQUIRED)
set(ENV{PKG_CONFIG_PATH} "${ROSS_PKG_CONFIG_PATH}:${SWM_PKG_CONFIG_PATH}:${UNION_PKG_CONFIG_PATH}:${ARGOBOTS_PKG_CONFIG_PATH}")
pkg_check_modules(ROSS REQUIRED IMPORTED_TARGET ross)

# MPI
include(SetupMPI)
if(MPI_C_FOUND)
        include_directories(${MPI_C_INCLUDE_PATH})
        list(APPEND CODES_EXTERNAL_LIBS ${MPI_C_LIBRARIES})
else(MPI_C_FOUND)
        message("WARNING: Could not find MPI!")
        message("         Either add an MPI compiler to your path (using modules)")
        message("         Or force CMake to build using the correct compiler (`export CC=mpicc`)")
endif(MPI_C_FOUND)


## DUMPI
set(DUMPI_BUILD_PATH "" CACHE PATH "Directory where dumpi include and lib are installed")
find_library(DUMPI_LIB undumpi PATHS ${DUMPI_BUILD_PATH}/lib)
if(NOT DUMPI_LIB)
        message(STATUS "Undumpi library not found, DUMPI trace workloads disabled")
        unset(USE_DUMPI)
else(DUMPI_LIB)
        message(STATUS "Undumpi library found ${DUMPI_LIB}")
        set(DUMPI_INCLUDE "${DUMPI_BUILD_PATH}/include" CACHE PATH "Dumpi library include")
        set(DUMPI_CFLAGS "-I${DUMPI_INCLUDE}")
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${DUMPI_CFLAGS}")
        add_definitions(-DUSE_DUMPI=1)
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
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${ARGOBOTS_CFLAGS} -I${ARGOBOTS_INCLUDE}")

        pkg_get_variable(SWM_DATAROOTDIR swm datarootdir)
        cmake_print_variables(SWM_DATAROOTDIR)

        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${SWM_CFLAGS} -I${SWM_INCLUDE}")

        pkg_check_modules(UNION IMPORTED_TARGET union)
        if(NOT UNION_FOUND)
            message(STATUS "UNION Library Not Found, SWM-only online workloads enabled")
            add_definitions(-DUSE_SWM=1)
            set(USE_SWM true)
        else(UNION_FOUND)
            message(STATUS "UNION Library Found: ${UNION_LIBRARIES}")
            pkg_get_variable(UNION_DATAROOTDIR union datarootdir)
            cmake_print_variables(UNION_DATAROOTDIR)
            
            set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${UNION_INCLUDE}")
            foreach(INCLUDE_OPT ${UNION_CFLAGS})
                set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${INCLUDE_OPT}")
            endforeach()

            add_definitions(-DUSE_UNION=1)
            set(USE_UNION true)
        endif()
    endif()
endif()


## RECORDER
option(USE_RECORDER "use recorder io workload" ON)
if(USE_RECORDER)
        add_definitions(-DUSE_RECORDER=1)
endif()

## DARSHAN


## DAMARIS
# pkg_check_modules(DAMARIS IMPORTED_TARGET)
# if(NOT DAMARIS_FOUND)
#         message(STATUS "DAMARIS Library not found, Damaris disabled")
# else(DAMARIS_FOUND)
#         set(USE_DAMARIS true)
# endif()

cmake_print_variables(CMAKE_C_FLAGS)
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
