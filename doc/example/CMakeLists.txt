set(example-files
    example
    tutorial-synthetic-ping-pong
    )

foreach(namefile ${example-files})
    add_executable(${namefile} ${namefile}.c)
    target_link_libraries(${namefile} PUBLIC codes)
endforeach()

# Saving default config files to run experiments with
configure_file(tutorial-ping-pong.conf.in tutorial-ping-pong.template.conf.in @ONLY)
configure_file(tutorial-ping-pong-surrogate.conf.in tutorial-ping-pong-surrogate.template.conf.in @ONLY)

set(single_quote "'")
set(double_quote "\"")

set(PACKET_SIZE "4096")
set(CHUNK_SIZE "64")
set(NETWORK_TREATMENT "freeze")
set(PACKET_LATENCY_TRACE_PATH "packet-latency-trace/")
set(IGNORE_UNTIL "200e4")
set(PREDICTOR_TYPE "average")
string(REPLACE ${single_quote} ${double_quote} SWITCH_TIMESTAMPS "'1000e4', '8900e4'")
configure_file(tutorial-ping-pong.conf.in tutorial-ping-pong.conf)
configure_file(tutorial-ping-pong-surrogate.conf.in tutorial-ping-pong-surrogate.conf)
