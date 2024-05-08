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
    surrogate/switch.c
    surrogate/lp-surrogacy/nw-buddy-lp/base.c
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

list(APPEND LIBS_TO_LINK PkgConfig::ROSS)

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

add_library(codes STATIC ${SRCS})

list(APPEND LIBS_TO_LINK ${MPI_C_LIBRARIES})
target_include_directories(codes INTERFACE ${MPI_C_INCLUDE_PATH})

# set(LIBS_TO_LINK
#     PkgConfig::ROSS
#     ${DUMPI_LIB}
#     PkgConfig::ARGOBOTS
#     PkgConfig::SWM
# )

#LINK DUMPI
# target_link_libraries(codes PUBLIC ${DUPMI_LIB})
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

#LINK ROSS
# target_link_libraries(codes PUBLIC #{pkgcfg_lib_ROSS_ROSS})
# target_link_libraries(codes PUBLIC PkgConfig::ROSS)
target_include_directories(codes PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${ROSS_INCLUDE_DIRS}
    ${PROJECT_BINARY_DIR}
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/codes
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/modelconfig
)

target_link_libraries(codes PUBLIC ${LIBS_TO_LINK})

get_target_property(CODES_INCLUDE_DIRS codes INCLUDE_DIRECTORIES)
cmake_print_variables(CODES_INCLUDE_DIRS)

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

foreach(tar IN LISTS CODES_TARGETS)
    target_include_directories(${tar} PUBLIC ${CODES_INCLUDE_DIRS} ${ROSS_INCLUDE_DIRS})
    target_link_libraries(${tar} PUBLIC codes ${LIBS_TO_LINK})
endforeach()


# configure_file(modelconfig/configlex.c ${CMAKE_CURRENT_BINARY_DIR}/modelconfig/configlex.c COPYONLY)
# configure_file(modelconfig/configlex.h ${CMAKE_CURRENT_BINARY_DIR}/modelconfig/configlex.h COPYONLY)

# configure_file(modelconfig/configparser.c ${CMAKE_CURRENT_BINARY_DIR}/modelconfig/configparser.c COPYONLY)
# configure_file(modelconfig/configparser.h ${CMAKE_CURRENT_BINARY_DIR}/modelconfig/configparser.h COPYONLY)

install(DIRECTORY "${CMAKE_SOURCE_DIR}/codes" DESTINATION include)

install(TARGETS ${CODES_TARGETS} DESTINATION bin)

install(TARGETS codes ARCHIVE DESTINATION lib LIBRARY DESTINATION lib)

