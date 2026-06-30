enable_testing()

configure_file(run-test.sh.in run-test.sh)

# codes_add_equivalence_test — register an equivalence/determinism test.
#
# Runs a model binary two or more times and asserts a marker line (default
# "Net Events Processed") is identical across runs. Replaces the copy-and-edit
# per-scenario shell scripts: adding a test is one call here, no new file.
#
#   codes_add_equivalence_test(
#       NAME     <ctest name>
#       BINARY   <CMake target>            # e.g. tutorial-synthetic-ping-pong
#       CONFIG   <path under build dir>     # e.g. doc/example/tutorial-ping-pong.conf
#       [NP      <ranks>]                   # default 3
#       [ARGS    <args shared by all runs>]
#       [REPEAT  <n>]                       # default 2; identical runs (reproducibility)
#       [VARIANTS "<extra args run 1>" "<extra args run 2>" ...]  # one run each, appended
#       [MARKER  <line to compare>]         # default "Net Events Processed"
#       [SETUP   <script under source dir>] # sourced before runs (env + conf generation)
#   )
#
# REPEAT and VARIANTS are mutually exclusive ways to describe the runs:
# REPEAT for "same command N times", VARIANTS when only a small fragment differs
# (e.g. VARIANTS "--sync=1" "--sync=3" for a seq-vs-optimistic check).
function(codes_add_equivalence_test)
    cmake_parse_arguments(T "" "NAME;BINARY;NP;CONFIG;MARKER;REPEAT;SETUP" "ARGS;VARIANTS" ${ARGN})

    if(NOT T_NAME OR NOT T_BINARY OR NOT T_CONFIG)
        message(FATAL_ERROR "codes_add_equivalence_test: NAME, BINARY and CONFIG are required")
    endif()
    if(NOT T_NP)
        set(T_NP 3)
    endif()
    if(NOT T_MARKER)
        set(T_MARKER "Net Events Processed")
    endif()
    if(NOT T_REPEAT)
        set(T_REPEAT 2)
    endif()

    set(_bin    "$<TARGET_FILE:${T_BINARY}>")
    set(_config "${CODES_BINARY_DIR}/${T_CONFIG}")
    set(_launch ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} ${T_NP} ${MPIEXEC_PREFLAGS})

    set(_cmd "")
    if(T_VARIANTS)
        foreach(_extra IN LISTS T_VARIANTS)
            separate_arguments(_extra_list UNIX_COMMAND "${_extra}")
            list(APPEND _cmd @@ ${_launch} "${_bin}" ${T_ARGS} ${_extra_list}
                             ${MPIEXEC_POSTFLAGS} -- "${_config}")
        endforeach()
    else()
        foreach(_i RANGE 1 ${T_REPEAT})
            list(APPEND _cmd @@ ${_launch} "${_bin}" ${T_ARGS}
                             ${MPIEXEC_POSTFLAGS} -- "${_config}")
        endforeach()
    endif()

    set(_setup "")
    if(T_SETUP)
        set(_setup --setup "${CMAKE_CURRENT_SOURCE_DIR}/${T_SETUP}")
    endif()

    add_test(NAME ${T_NAME}
        COMMAND "${CMAKE_CURRENT_BINARY_DIR}/run-test.sh"
                bash "${CMAKE_CURRENT_SOURCE_DIR}/equivalence-run.sh"
                --marker "${T_MARKER}" ${_setup} ${_cmd}
        WORKING_DIRECTORY "${CODES_BINARY_DIR}")
endfunction()

# codes_add_run_test — register a single-run smoke / unit test.
#
# Runs a model or test binary once under MPI; the test passes on clean exit
# (run-test.sh additionally fails if the binary leaves an empty ross.csv). The
# caller supplies the full post-binary argument list, which absorbs the various
# ways CODES binaries take their config: positional, "-- <conf>",
# "--codes-config=<conf>", "--conf=<conf>", or none. Config files live in the
# source tree, so reference them as ${CMAKE_CURRENT_SOURCE_DIR}/... (tests/) or
# ${CMAKE_SOURCE_DIR}/... (elsewhere).
#
#   codes_add_run_test(
#       NAME   <ctest name>
#       BINARY <CMake target>
#       [NP    <ranks>]                 # default 1
#       [ARGS  <full args after the binary>]
#   )
function(codes_add_run_test)
    cmake_parse_arguments(T "" "NAME;BINARY;NP" "ARGS" ${ARGN})
    if(NOT T_NAME OR NOT T_BINARY)
        message(FATAL_ERROR "codes_add_run_test: NAME and BINARY are required")
    endif()
    if(NOT T_NP)
        set(T_NP 1)
    endif()
    add_test(NAME ${T_NAME}
        COMMAND "${CMAKE_CURRENT_BINARY_DIR}/run-test.sh"
                ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} ${T_NP} ${MPIEXEC_PREFLAGS}
                "$<TARGET_FILE:${T_BINARY}>" ${T_ARGS} ${MPIEXEC_POSTFLAGS}
        WORKING_DIRECTORY "${CODES_BINARY_DIR}")
endfunction()

# Unfortunatelly, CMake doesn't support iteration of a key-pair structure,
# otherwise the following lists could be easily compressed into a single
# list/dictionary/structure. Instead each C file name **MUST** match each
# binary file name. This would be handled differently if maintaining the
# autoconfig build was not a **MUST** (in which case, the name of the file and
# its binary should match, leaving us with a single list!)
set(test-c-files
    lp-io-test.c
    mapping_test.c
    jobmap-test.c
    map-ctx-test.c
    modelnet-prio-sched-test.c
    modelnet-test-dragonfly.c
    modelnet-test.c
    resource-test.c
    rc-stack-test.c
    modelnet-p2p-bw.c
    modelnet-simplep2p-test.c
    local-storage-model-test.c
    )

set(test-bin-names
    lp-io-test
    mapping_test
    jobmap-test
    map-ctx-test
    modelnet-prio-sched-test
    modelnet-test-dragonfly
    modelnet-test
    resource-test
    rc-stack-test
    modelnet-p2p-bw
    modelnet-simplep2p-test
    lsm-test
    )

foreach(testname cfile IN ZIP_LISTS test-bin-names test-c-files)
    add_executable(${testname} ${cfile})
    target_link_libraries(${testname} PUBLIC codes)
endforeach()

# Additional binary which is not self contained
add_executable(codes-workload-test
    workload/codes-workload-test.c
    workload/codes-workload-test-svr-lp.c
    workload/codes-workload-test-cn-lp.c
    )
target_link_libraries(codes-workload-test PUBLIC codes)

# Tests are also not consistent with the files to compile, but
# that's ok, there are more tests than binary files
# Scripts still run as per-scenario shell files. Most single-run tests were
# migrated to codes_add_run_test() and the simple determinism test to
# codes_add_equivalence_test() (both below). What remains here either has
# custom logic, is held back for review, or is migrated in a later chunk.
set(test-shell-files
    # mapping_test diffs binary output against tests/expected/mapping_test.out
    mapping_test.sh
    # Surrogate / no-logging determinism tests -- migrated via SETUP next chunk.
    example-ping-pong-surrogate-1.sh
    example-ping-pong-surrogate-2.sh
    example-ping-pong-surrogate-3.sh
    example-ping-pong-no-logging.sh
    example-ping-pong-surrogate-determinism-1.sh
    example-ping-pong-surrogate-determinism-2.sh
    )

configure_file(conf/union-milc-jacobi-workload/dfdally-72-par.conf.in conf/union-milc-jacobi-workload/dfdally-72-par.conf.in @ONLY)

if(USE_UNION)
    list(APPEND test-shell-files
        union-workload-test-surrogate.sh
        union-workload-test-surrogate-smaller-chunk-size.sh
        union-workload-test-surrogate-parallel.sh
        union-workload-test-surrogate-parallel-deterministic-1.sh
        union-workload-test-surrogate-parallel-deterministic-2.sh
        union-workload-test-surrogate-parallel-deterministic-3.sh
        union-workload-test-surrogate-parallel-deterministic-4.sh
    )
endif()

foreach(testname ${test-shell-files})
    add_test(NAME ${testname}
        COMMAND "${CMAKE_CURRENT_BINARY_DIR}/run-test.sh" "${CMAKE_CURRENT_SOURCE_DIR}/${testname}"
        WORKING_DIRECTORY "${CODES_BINARY_DIR}")
endforeach()

# Equivalence / determinism tests (replacing per-scenario shell scripts).
# example-ping-pong-determinism.sh: run the optimistic sim twice, compare
# committed-event counts (reproducibility).
codes_add_equivalence_test(
    NAME   example-ping-pong-determinism
    BINARY tutorial-synthetic-ping-pong
    NP     3
    ARGS   --sync=3 --num_messages=10 --payload_sz=8192
    CONFIG doc/example/tutorial-ping-pong.conf)

# Single-run smoke / unit tests (migrated from per-scenario shell scripts).
# Config files live in the source tree; _tconf/_nwconf point at the two homes.
set(_tconf "${CMAKE_CURRENT_SOURCE_DIR}/conf")
set(_nwconf "${CMAKE_SOURCE_DIR}/src/network-workloads/conf")

codes_add_run_test(NAME lp-io-test    BINARY lp-io-test    ARGS --sync=1)
# rc-stack-test is a serial unit test (no config, no args); the old script ran
# the wrong binary (modelnet-simplep2p-test) -- a copy-paste error, fixed here.
codes_add_run_test(NAME rc-stack-test BINARY rc-stack-test)
# modelnet-prio-sched ran both schedulers in one script; split into two tests.
codes_add_run_test(NAME modelnet-prio-sched-test-seq BINARY modelnet-prio-sched-test
    NP 1 ARGS --sync=1 -- "${_tconf}/modelnet-prio-sched-test.conf")
codes_add_run_test(NAME modelnet-prio-sched-test-opt BINARY modelnet-prio-sched-test
    NP 2 ARGS --sync=3 -- "${_tconf}/modelnet-prio-sched-test.conf")
codes_add_run_test(NAME jobmap-test   BINARY jobmap-test   ARGS "${_tconf}/jobmap-test-list.conf")
codes_add_run_test(NAME map-ctx-test  BINARY map-ctx-test  ARGS "${_tconf}/map-ctx-test.conf")
codes_add_run_test(NAME resource-test BINARY resource-test ARGS --sync=1 "--codes-config=${_tconf}/buffer_test.conf")
codes_add_run_test(NAME lsm-test      BINARY lsm-test      ARGS --sync=1 "--conf=${_tconf}/lsm-test.conf")
codes_add_run_test(NAME modelnet-simplep2p-test BINARY modelnet-simplep2p-test
    ARGS --sync=1 -- "${_tconf}/modelnet-test-simplep2p.conf")
codes_add_run_test(NAME modelnet-p2p-bw-loggp BINARY modelnet-p2p-bw
    ARGS --sync=1 -- "${_tconf}/modelnet-p2p-bw-loggp.conf")
codes_add_run_test(NAME codes-workload-test BINARY codes-workload-test
    ARGS --sync=1 "${CMAKE_CURRENT_SOURCE_DIR}/workload/codes-workload-test.conf")

# modelnet-test binary exercised against one config per topology.
codes_add_run_test(NAME modelnet-test           BINARY modelnet-test ARGS --sync=1 -- "${_tconf}/modelnet-test.conf")
codes_add_run_test(NAME modelnet-test-dragonfly BINARY modelnet-test ARGS --sync=1 -- "${_tconf}/modelnet-test-dragonfly.conf")
codes_add_run_test(NAME modelnet-test-torus     BINARY modelnet-test ARGS --sync=1 -- "${_tconf}/modelnet-test-torus.conf")
codes_add_run_test(NAME modelnet-test-em        BINARY modelnet-test ARGS --sync=1 -- "${_tconf}/modelnet-test-em.conf")
codes_add_run_test(NAME modelnet-test-loggp     BINARY modelnet-test ARGS --sync=1 -- "${_tconf}/modelnet-test-loggp.conf")
codes_add_run_test(NAME modelnet-test-slimfly   BINARY modelnet-test ARGS --sync=1 -- "${_tconf}/modelnet-test-slimfly.conf")

# Synthetic-traffic binaries.
codes_add_run_test(NAME modelnet-test-dragonfly-synthetic BINARY model-net-synthetic
    ARGS --sync=1 --num_messages=1 -- "${_nwconf}/modelnet-synthetic-dragonfly.conf")
codes_add_run_test(NAME modelnet-test-fattree-synthetic BINARY model-net-synthetic-fattree
    ARGS --sync=1 -- "${_nwconf}/modelnet-synthetic-fattree.conf")
codes_add_run_test(NAME modelnet-test-slimfly-synthetic BINARY model-net-synthetic-slimfly
    ARGS --sync=1 -- "${_nwconf}/modelnet-synthetic-slimfly-min.conf")
