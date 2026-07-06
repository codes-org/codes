# RapidYAML
#
# Vendored as the upstream single-header amalgamation (see README.md and
# the rapidyaml/ subdirectory). It is always built: the config front-end is core,
# not an optional feature.
#
# We deliberately do NOT create a linkable target here. codes is a STATIC library
# in an install/export set, and linking any in-project target into it would force
# that target into the export set too -- the build-tree export() evaluates
# generator expressions, so even a $<BUILD_INTERFACE:...> build-only link counts.
# Instead we publish the amalgamation's implementation TU and include dirs, and
# src/ compiles the TU straight into codes with these headers on its PRIVATE
# include path. The ryml objects then live inside codes.a with nothing extra to
# install, export, or link downstream.
message_start_thirdparty()
set(CODES_RYML_IMPL_TU "${CMAKE_CURRENT_SOURCE_DIR}/ryml.cpp"
    CACHE INTERNAL "RapidYAML amalgamation implementation translation unit")
set(CODES_RYML_INCLUDE_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/rapidyaml"
    CACHE INTERNAL "RapidYAML amalgamation include directories (shim + upstream header)")
