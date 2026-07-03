# Vendored third-party dependencies.
#
# Each subdirectory is an upstream drop managed with the Kitware third-party
# update convention (the shared update-common.sh here, driven by a per-package
# update.sh) plus a thin CMake wrapper that exposes a codes::thirdparty::<name>
# target. See each package's README.md for its upstream origin, pinned
# version, and how to re-import it.

# Disable warnings in thirdparty code
if(CMAKE_C_COMPILER_ID MATCHES
    "^(GNU|Clang|AppleClang|XLClang|XL|VisualAge|SunPro|HP|Intel|IntelLLVM|PGI|NVIDIA|NVHPC)$")
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -w")
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES
    "^(GNU|Clang|AppleClang|XLClang|XL|VisualAge|SunPro|HP|Intel|IntelLLVM|PGI|NVIDIA|NVHPC)$")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -w")
endif()

function(message_start_thirdparty)
  get_filename_component(tp_base "${CMAKE_CURRENT_LIST_DIR}" NAME_WE)
  message("")
  message(STATUS "CODES ThirdParty: Configuring ${tp_base}")
endfunction()

add_subdirectory(rapidyaml)

if(BUILD_TESTING)
  add_subdirectory(googletest)
endif()
