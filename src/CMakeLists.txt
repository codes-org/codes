cmake_print_variables(CMAKE_CURRENT_SOURCE_DIR)

find_package(FLEX REQUIRED)
find_package(BISON REQUIRED)

# FLEX/BISON emit their generated sources into the build tree, not the source
# tree, so the repo never carries generated artifacts. The generated headers
# are reached via "src/modelconfig/..." includes, which resolve against
# PROJECT_BINARY_DIR (already on codes' include path below).
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/modelconfig)
flex_target(codes_lexer ${CMAKE_CURRENT_SOURCE_DIR}/modelconfig/configlex.l ${CMAKE_CURRENT_BINARY_DIR}/modelconfig/configlex.c DEFINES_FILE ${CMAKE_CURRENT_BINARY_DIR}/modelconfig/configlex.h)
bison_target(codes_parser ${CMAKE_CURRENT_SOURCE_DIR}/modelconfig/configparser.y ${CMAKE_CURRENT_BINARY_DIR}/modelconfig/configparser.c DEFINES_FILE ${CMAKE_CURRENT_BINARY_DIR}/modelconfig/configparser.h)
ADD_FLEX_BISON_DEPENDENCY(codes_lexer codes_parser)

cmake_print_variables(FLEX_codes_lexer_OUTPUTS)
cmake_print_variables(BISON_codes_parser_OUTPUTS)

list(APPEND SRCS
    networks/model-net/core/model-net.c
    networks/model-net/core/model-net-lp.c
    networks/model-net/core/model-net-sched.c
    networks/model-net/core/model-net-sched-impl.c

    networks/model-net/common-net.c
    networks/model-net/simplenet-upd.c
    networks/model-net/torus.c
    networks/model-net/express-mesh.cxx
    networks/model-net/dragonfly.c
    networks/model-net/dragonfly-custom.cxx
    networks/model-net/dragonfly-plus.cxx
    networks/model-net/dragonfly-dally.cxx
    networks/model-net/slimfly.c
    networks/model-net/fattree.c
    networks/model-net/loggp.c
    networks/model-net/simplep2p.c

    networks/model-net/network-managers/dragonfly-network-manager.cxx

    workload/codes-workload.c
    workload/methods/codes-iolang-wrkld.c
    workload/methods/codes-checkpoint-wrkld.c
    workload/methods/test-workload-method.c
	workload/methods/codes-iomock-wrkld.c

    util/codes_mapping.c
    util/lp-type-lookup.c
    util/lp-io.c
	util/lp-msg.c
    util/lookup3.c
	util/resource.c
	util/resource-lp.c
	util/local-storage-model.c
	util/codes-jobmap-method-impl.h
	util/codes-jobmap.c
	util/jobmap-impl/jobmap-dummy.c
	util/jobmap-impl/jobmap-list.c
	util/jobmap-impl/jobmap-identity.c
	util/codes-mapping-context.c
  	util/codes-comm.c
    util/rc-stack.c
    util/congestion-controller.cxx

    surrogate/init.c
    surrogate/application-surrogate.c
    surrogate/network-surrogate.c
    surrogate/app-iteration-predictor/common.c
    surrogate/app-iteration-predictor/average.c
    surrogate/packet-latency-predictor/common.c
    surrogate/packet-latency-predictor/average.c

    iokernellang/codesparser.h
    iokernellang/codesparser.c
    iokernellang/codeslexer.h
    iokernellang/codeslexer.c
    iokernellang/codesImpl.c
    iokernellang/CodesIOKernelContext.h
    iokernellang/CodesIOKernelParser.h
    iokernellang/CodesIOKernelTypes.h
    iokernellang/CodesKernelHelpers.h
    iokernellang/CodesKernelHelpers.c

    ${FLEX_codes_lexer_OUTPUTS}
    ${BISON_codes_parser_OUTPUTS}

    modelconfig/configfile.c
    modelconfig/configglue.h
    modelconfig/configglue.c
    modelconfig/configstore.h
    modelconfig/configstore.c
    modelconfig/configstoreadapter.h
    modelconfig/configstoreadapter.c
    modelconfig/configuration.c
    modelconfig/txt_configfile.h
    modelconfig/txt_configfile.c
)

list(APPEND LIBS_TO_LINK ROSS::ROSS)

if(USE_DUMPI)
    list(APPEND SRCS workload/methods/codes-dumpi-trace-nw-wrkld.c)
    list(APPEND LIBS_TO_LINK ${DUMPI_LIB})
endif()

if(USE_ONLINE)
    # Boost (header-only property_tree / foreach) is pulled in by the SWM model
    # headers both online sources include — needed on either the SWM-only or the
    # UNION path. Boost::headers is an INTERFACE IMPORTED target: it carries the
    # include dir and, being imported, contributes it as SYSTEM.
    list(APPEND LIBS_TO_LINK Boost::headers)
    if(USE_SWM)
        list(APPEND SRCS workload/methods/codes-online-comm-wrkld.cxx)
        list(APPEND LIBS_TO_LINK PkgConfig::SWM)
        list(APPEND LIBS_TO_LINK PkgConfig::ARGOBOTS)
    endif()
    if(USE_UNION)
        list(APPEND SRCS workload/methods/codes-conc-online-comm-wrkld.cxx)
        list(APPEND LIBS_TO_LINK PkgConfig::SWM)
        list(APPEND LIBS_TO_LINK PkgConfig::UNION)
        list(APPEND LIBS_TO_LINK PkgConfig::ARGOBOTS)
    endif()
endif()


if(USE_RECORDER)
    list(APPEND SRCS workload/methods/codes-recorder-io-wrkld.c)
endif()

# if(USE_DARSHAN)
#     list(APPEND SRCS workload/methods/codes-darshan3-io-wrkld.c)
# endif()

if(USE_TORCH)
    list(APPEND SRCS surrogate/packet-latency-predictor/torch-jit.cxx)
    list(APPEND LIBS_TO_LINK ${TORCH_LIBRARIES})
endif()

# ZeroMQ / director-client (opt-in via CODES_USE_ZEROMQ; resolved to the internal
# USE_ZEROMQ bool in the top-level CMakeLists). director-client.cxx is part of
# libcodes; the zmqml requester it calls is its own static lib (built in the
# surrogate/zmqml subdir and linked into codes below).
if(USE_ZEROMQ)
    list(APPEND SRCS surrogate/director-client.cxx)
    add_subdirectory(surrogate/zmqml)
endif()

add_library(codes STATIC ${SRCS})

# Coverage instrumentation (opt-in via CODES_ENABLE_COVERAGE). PUBLIC so the
# library objects are instrumented AND every binary/test that links codes pulls
# in the gcov runtime, so `ctest` emits .gcda files for them.
if(CODES_ENABLE_COVERAGE)
    target_compile_options(codes PUBLIC --coverage)
    target_link_options(codes PUBLIC --coverage)
endif()

# Optional-feature state is exposed through the generated codes_config.h
# (CODES_HAVE_<X>), included by sources that need it — no -DUSE_<X> compile
# definitions, so nothing leaks onto consumers' command lines.

# MPI imported targets carry the include dirs, compile flags, and link libs.
# Linked PUBLIC (via LIBS_TO_LINK below) so codes' own sources and every
# consumer (executables, tests) inherit them.
list(APPEND LIBS_TO_LINK MPI::MPI_C MPI::MPI_CXX)

# SWM / UNION / argobots / Boost / ZeroMQ include dirs are NOT added by hand:
# each rides its imported target (PkgConfig::SWM/UNION/ARGOBOTS, Boost::headers,
# and PkgConfig::ZeroMQ via zmqmlrequester), all linked PUBLIC above. Because
# those targets are IMPORTED, CMake contributes their include dirs as SYSTEM
# (-isystem), which also keeps the project's -Wundef build clean of third-party
# header noise. codesConfig.cmake recreates the same targets via find_dependency
# so installed consumers inherit these paths too.
#
# DUMPI is the exception: it's located by absolute path (find_library), not an
# imported target, so its include is attached here — SYSTEM for -Wundef hygiene,
# BUILD_INTERFACE for the in-tree build. codesConfig.cmake re-attaches it for
# installed heavy-deps consumers.
if(USE_DUMPI)
    target_include_directories(codes SYSTEM PUBLIC "$<BUILD_INTERFACE:${DUMPI_INCLUDE}>")
endif()

# Public include dirs travel with the target. BUILD_INTERFACE entries serve
# CODES's own compilation and in-tree consumers; INSTALL_INTERFACE entries serve
# installed consumers — include/ for `<codes/...>` and include/codes/ for the
# sibling codes_config.h and bare codes headers. The src/ paths are build-only:
# they are CODES-internal and not part of the installed surface.
target_include_directories(codes PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}>
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/codes>
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src/modelconfig>
    $<INSTALL_INTERFACE:include>
    $<INSTALL_INTERFACE:include/codes>
)

target_link_libraries(codes PUBLIC ${LIBS_TO_LINK})

add_executable(topology-test networks/model-net/topology-test.c)
add_executable(model-net-mpi-replay network-workloads/model-net-mpi-replay.c network-workloads/model-net-mpi-replay-main.c)
if(USE_DUMPI)
    add_executable(codes-workload-dump workload/codes-workload-dump.c)
    add_executable(model-net-dumpi-traces-dump network-workloads/model-net-dumpi-traces-dump.c)
endif()
add_executable(model-net-synthetic network-workloads/model-net-synthetic.c)
add_executable(model-net-synthetic-slimfly network-workloads/model-net-synthetic-slimfly.c)
add_executable(model-net-synthetic-fattree network-workloads/model-net-synthetic-fattree.c)
add_executable(model-net-synthetic-dragonfly-all network-workloads/model-net-synthetic-dragonfly-all.c)

set(CODES_TARGETS
    topology-test
    model-net-mpi-replay
    model-net-synthetic
    model-net-synthetic-slimfly
    model-net-synthetic-fattree
    model-net-synthetic-dragonfly-all
)

if(USE_DUMPI)
    list(APPEND CODES_TARGETS codes-workload-dump)
    list(APPEND CODES_TARGETS model-net-dumpi-traces-dump)
endif()

# The zmqml requester defines the zmqml_* symbols that codes' OWN objects
# (director-client.cxx, dragonfly-dally.cxx) reference, so it is a usage
# requirement of the codes library itself — link it PUBLIC so every consumer of
# codes (the CODES_TARGETS executables AND the doc/example + tests targets, which
# are not in CODES_TARGETS) inherits it and resolves zmqml_*. As a real target it
# lands at the end of each consumer's link line after libcodes.a, the order the
# references need, and carries libzmq + its include dirs transitively.
if(USE_ZEROMQ)
    target_link_libraries(codes PUBLIC zmqmlrequester)
endif()

# Each executable links codes; its include dirs, MPI, ROSS, the optional-dep
# libs, and (under USE_ZEROMQ) the zmqml requester all propagate transitively
# through codes' PUBLIC usage requirements.
foreach(tar IN LISTS CODES_TARGETS)
    target_link_libraries(${tar} PRIVATE codes)
endforeach()


# ---------------------------------------------------------------------------
# Install + CMake package export (find_package(codes) -> codes::codes)
# ---------------------------------------------------------------------------
include(CMakePackageConfigHelpers)

# Public headers under <prefix>/include/codes/. The generated config header is
# public (codes-workload.h includes it for CODES_HAVE_* queries), so it ships
# alongside them.
install(DIRECTORY "${CMAKE_SOURCE_DIR}/codes" DESTINATION include)
install(FILES "${PROJECT_BINARY_DIR}/codes_config.h" DESTINATION include/codes)

# Application binaries — not part of the exported package.
install(TARGETS ${CODES_TARGETS} RUNTIME DESTINATION bin)

# Library target + namespaced export. codes::codes carries the public include
# dirs (INSTALL_INTERFACE) and the ROSS / MPI usage requirements. Under
# USE_ZEROMQ the zmqml requester is a real PUBLIC dependency of codes, so it must
# join the export set too (its imported PkgConfig::ZeroMQ dep stays exempt, like
# the SWM/UNION imported targets codes already exports).
set(CODES_EXPORT_TARGETS codes)
if(USE_ZEROMQ)
    list(APPEND CODES_EXPORT_TARGETS zmqmlrequester)
endif()
install(TARGETS ${CODES_EXPORT_TARGETS}
    EXPORT codesTargets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin)

install(EXPORT codesTargets
    FILE codesTargets.cmake
    NAMESPACE codes::
    DESTINATION lib/cmake/codes)

# codesConfig.cmake re-finds ROSS + MPI via find_dependency and pulls in the
# targets file; the version file gates on SameMajorVersion.
configure_package_config_file(
    ${CMAKE_SOURCE_DIR}/cmake/codesConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/codesConfig.cmake
    INSTALL_DESTINATION lib/cmake/codes)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/codesConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/codesConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/codesConfigVersion.cmake
    DESTINATION lib/cmake/codes)

# Build-tree export so a superbuild / FetchContent consumer can
# find_package(codes) against the build dir without installing.
export(EXPORT codesTargets
    FILE ${CMAKE_CURRENT_BINARY_DIR}/codesTargets.cmake
    NAMESPACE codes::)

