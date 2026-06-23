cmake_print_variables(CMAKE_CURRENT_SOURCE_DIR)

find_package(FLEX REQUIRED)
find_package(BISON REQUIRED)

flex_target(codes_lexer ${CMAKE_CURRENT_SOURCE_DIR}/modelconfig/configlex.l ${CMAKE_CURRENT_SOURCE_DIR}/modelconfig/configlex.c DEFINES_FILE ${CMAKE_CURRENT_SOURCE_DIR}/modelconfig/configlex.h)
bison_target(codes_parser ${CMAKE_CURRENT_SOURCE_DIR}/modelconfig/configparser.y ${CMAKE_CURRENT_SOURCE_DIR}/modelconfig/configparser.c DEFINES_FILE ${CMAKE_CURRENT_SOURCE_DIR}/modelconfig/configparser.h)
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
    networks/model-net/express-mesh.C
    networks/model-net/dragonfly.c
    networks/model-net/dragonfly-custom.C
    networks/model-net/dragonfly-plus.C
    networks/model-net/dragonfly-dally.C
    networks/model-net/slimfly.c
    networks/model-net/fattree.c
    networks/model-net/loggp.c
    networks/model-net/simplep2p.c

    networks/model-net/network-managers/dragonfly-network-manager.C

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
    util/congestion-controller.C

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
    if(USE_SWM)
        list(APPEND SRCS workload/methods/codes-online-comm-wrkld.C)
        list(APPEND LIBS_TO_LINK PkgConfig::SWM)
        list(APPEND LIBS_TO_LINK PkgConfig::ARGOBOTS)
    endif()
    if(USE_UNION)
        list(APPEND SRCS workload/methods/codes-conc-online-comm-wrkld.C)
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
    list(APPEND SRCS surrogate/packet-latency-predictor/torch-jit.C)
    list(APPEND LIBS_TO_LINK ${TORCH_LIBRARIES})
endif()

# ZeroMQ / director-client (opt-in via CODES_USE_ZEROMQ; resolved to the internal
# USE_ZEROMQ bool in the top-level CMakeLists, which also enforces that
# ZEROMQ_BUILD_PATH is set). The imported target + linkage below are gated on it.
if(USE_ZEROMQ)
    list(APPEND SRCS surrogate/director-client.C)
endif()

add_library(codes STATIC ${SRCS})

# Optional-feature state is exposed through the generated codes_config.h
# (CODES_HAVE_<X>), included by sources that need it — no -DUSE_<X> compile
# definitions, so nothing leaks onto consumers' command lines.

# MPI imported targets carry the include dirs, compile flags, and link libs.
# Linked PUBLIC (via LIBS_TO_LINK below) so codes' own sources and every
# consumer (executables, tests) inherit them.
list(APPEND LIBS_TO_LINK MPI::MPI_C MPI::MPI_CXX)

if(USE_DUMPI)
    target_include_directories(codes PUBLIC ${DUMPI_INCLUDE})
endif()

#LINK ARGOBOTS, SWM and UNION
# target_link_libraries(codes PUBLIC PkgConfig::ARGOBOTS)
if(USE_ONLINE)
    if(USE_SWM)
        target_include_directories(codes PUBLIC ${ARGOBOTS_INCLUDE_DIRS})
        # target_link_libraries(codes PUBLIC PkgConfig::SWM)
        target_include_directories(codes PUBLIC ${SWM_INCLUDE_DIRS})
    endif()
    if(USE_UNION)
        target_include_directories(codes PUBLIC ${ARGOBOTS_INCLUDE_DIRS})
        # target_link_libraries(codes PUBLIC PkgConfig::SWM)
        target_include_directories(codes PUBLIC ${SWM_INCLUDE_DIRS})
        target_include_directories(codes PUBLIC ${UNION_INCLUDE_DIRS})
    endif()
endif()

if(USE_ZEROMQ)
    add_library(zmqmlrequester SHARED IMPORTED GLOBAL)
    set_target_properties(zmqmlrequester PROPERTIES
        IMPORTED_LOCATION "${ZEROMQ_BUILD_PATH}/libzmqmlrequester.so"
        INTERFACE_INCLUDE_DIRECTORIES "${ZEROMQ_BUILD_PATH}")
endif()

target_include_directories(codes PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_BINARY_DIR}
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/codes
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/modelconfig
    $<$<BOOL:${USE_ZEROMQ}>:$<TARGET_PROPERTY:zmqmlrequester,INTERFACE_INCLUDE_DIRECTORIES>>
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

# ZMQ — only resolved + linked when USE_ZEROMQ is on; otherwise nothing
# in the codes library calls into libzmq.
if(USE_ZEROMQ)
    pkg_check_modules(PC_ZeroMQ QUIET zmq)
    find_path(ZeroMQ_INCLUDE_DIR NAMES zmq.hpp PATHS ${PC_ZeroMQ_INCLUDE_DIRS})
    find_library(ZeroMQ_LIBRARY NAMES zmq PATHS ${PC_ZeroMQ_LIBRARY_DIRS})
endif()

# Each executable links codes; its include dirs, MPI, ROSS, and optional-dep
# libs all propagate transitively through codes' PUBLIC usage requirements.
foreach(tar IN LISTS CODES_TARGETS)
    target_link_libraries(${tar} PRIVATE codes)
    if(USE_ZEROMQ)
        target_link_libraries(${tar} PRIVATE zmqmlrequester ${ZeroMQ_LIBRARY})
    endif()
endforeach()


# configure_file(modelconfig/configlex.c ${CMAKE_CURRENT_BINARY_DIR}/modelconfig/configlex.c COPYONLY)
# configure_file(modelconfig/configlex.h ${CMAKE_CURRENT_BINARY_DIR}/modelconfig/configlex.h COPYONLY)

# configure_file(modelconfig/configparser.c ${CMAKE_CURRENT_BINARY_DIR}/modelconfig/configparser.c COPYONLY)
# configure_file(modelconfig/configparser.h ${CMAKE_CURRENT_BINARY_DIR}/modelconfig/configparser.h COPYONLY)

install(DIRECTORY "${CMAKE_SOURCE_DIR}/codes" DESTINATION include)

# The generated config header is public now — codes-workload.h includes it for
# CODES_HAVE_* feature queries — so install it next to the other codes/ headers.
install(FILES "${PROJECT_BINARY_DIR}/codes_config.h" DESTINATION include/codes)

install(TARGETS ${CODES_TARGETS} DESTINATION bin)

install(TARGETS codes ARCHIVE DESTINATION lib LIBRARY DESTINATION lib)

