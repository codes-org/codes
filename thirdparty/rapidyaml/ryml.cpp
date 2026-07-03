/*
 * The single translation unit that compiles the vendored RapidYAML
 * amalgamation's implementation. Every other consumer includes <codes_ryml.hpp>
 * without this define and gets only declarations, so the ~44k-line
 * implementation is compiled exactly once. See README.md.
 */
#define RYML_SINGLE_HDR_DEFINE_NOW
#include <codes_ryml.hpp>
