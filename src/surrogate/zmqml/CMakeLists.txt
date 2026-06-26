# The zmqml requester: the CLIENT half of the director / ML surrogate. It opens
# a ZeroMQ REQ socket and exchanges JSON (rapidjson) with a *separate* ML server
#
# Built STATIC so it stays a self-contained, reusable unit with no runtime .so
# to locate. A future SHARED build is a one-line change; CMake stamps the SONAME automatically.
#
# Reached only when USE_ZEROMQ resolved true, so libzmq + zmq.hpp + rapidjson are
# all known present (probed in the top-level CMakeLists).
add_library(zmqmlrequester STATIC zmqmlrequester.cpp)

# PUBLIC libzmq: consumers (codes, and the executables that link codes) need the
# zmq_* symbols on their final link line. zmq.hpp travels with PkgConfig::ZeroMQ.
target_link_libraries(zmqmlrequester PUBLIC PkgConfig::ZeroMQ)

target_include_directories(zmqmlrequester
    # zmqmlrequester.h is the public surface — director-client.cxx includes it.
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    # rapidjson is header-only and used only inside zmqmlrequester.cpp.
    PRIVATE ${RapidJSON_INCLUDE_DIR})

# keep the requester PIC so it can be archived into one.
set_target_properties(zmqmlrequester PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Standalone dev tool: drives the requester against a running zmqmlserver.py to
# smoke-test the wire protocol. Not part of CODES — off by default; turn on with
# -DCODES_BUILD_ZMQML_DEMO=ON. (Replaces the old hand-rolled Makefile + runcppdemo.sh.)
option(CODES_BUILD_ZMQML_DEMO "Build the standalone zmqml requester demo tool" OFF)
if(CODES_BUILD_ZMQML_DEMO)
    add_executable(demozmqmlrequester demozmqmlrequester.cpp)
    target_link_libraries(demozmqmlrequester PRIVATE zmqmlrequester)
endif()
