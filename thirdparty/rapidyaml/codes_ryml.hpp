/*
 * Stable include point for the vendored RapidYAML amalgamation.
 *
 * Consumers #include <codes_ryml.hpp> instead of the amalgamation header
 * directly. The amalgamation has a stable, unversioned name (ryml_all.hpp,
 * regenerated in place by thirdparty/rapidyaml/update.sh), so a version bump
 * touches nothing here — not the call sites and not even this line.
 *
 * Do not edit the amalgamation itself — it is a generated upstream drop. See
 * README.md.
 */
#ifndef CODES_THIRDPARTY_RYML_HPP
#define CODES_THIRDPARTY_RYML_HPP

#include <ryml_all.hpp>

#endif /* CODES_THIRDPARTY_RYML_HPP */
