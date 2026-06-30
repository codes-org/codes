# CODES API documentation (Doxygen).
#
# Reached only when -DCODES_BUILD_DOXYGEN=ON (see the option in the top-level
# CMakeLists). Registers an on-demand `docs` target — not built by `all`:
#
#     cmake -S . -B build -DCODES_BUILD_DOXYGEN=ON
#     cmake --build build --target docs
#
# The doc-comment convention (which entities get /** */ comments, and that there
# is no mass backfill) lives in doc/dev/conventions.md.

find_package(Doxygen)
if(NOT DOXYGEN_FOUND)
    message(WARNING
        "CODES_BUILD_DOXYGEN=ON but Doxygen was not found; the `docs` target "
        "will not be registered. Install Doxygen or set CODES_BUILD_DOXYGEN=OFF.")
    return()
endif()

# Generated output goes to the build tree, never the source tree (the stale
# committed Doxyfile this replaced dumped HTML into doc/). /build* is gitignored.
set(DOXYGEN_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/doc")

# This is the setting that makes "infrastructure now, docs as code is touched"
# work: EXTRACT_ALL = NO means the output contains *only* entities that carry a
# Doxygen comment, so it starts near-empty and grows exactly as /** */ comments
# are added. WARN_IF_UNDOCUMENTED = NO
# keeps the build quiet about the as-yet-undocumented legacy tree instead of
# emitting thousands of warnings.
set(DOXYGEN_EXTRACT_ALL NO)
set(DOXYGEN_WARN_IF_UNDOCUMENTED NO)

set(DOXYGEN_RECURSIVE YES)
set(DOXYGEN_GENERATE_HTML YES)
set(DOXYGEN_GENERATE_LATEX NO)

set(DOXYGEN_PROJECT_NAME "CODES")
set(DOXYGEN_PROJECT_NUMBER "${PROJECT_VERSION}")

# Render the README as the docs landing page.
set(DOXYGEN_USE_MDFILE_AS_MAINPAGE "${CMAKE_SOURCE_DIR}/README.md")

# INPUT is broad (codes/ + src/); EXTRACT_ALL = NO does the filtering. Absolute
# paths so the inputs resolve regardless of WORKING_DIRECTORY.
doxygen_add_docs(docs
    "${CMAKE_SOURCE_DIR}/codes"
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/README.md"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Generating CODES API documentation (Doxygen)")
