#ifndef CODES_TESTS_CONFIG_COMPILER_TEST_UTIL_H
#define CODES_TESTS_CONFIG_COMPILER_TEST_UTIL_H

/**
 * @file
 *
 * Shared fixtures and lookup helpers for the config-compiler unit tests. This
 * header holds only the pieces more than one of them uses: the
 * compiled_config lookup helpers, the base YAML fixtures, and the parametric
 * dragonfly config builder. Single-TU helpers stay file-local in their TU.
 */

#include "config_compiler.h"

#include <string>

// Look up a top-level section by name (nullptr if absent).
inline const codes::config::compiled_section* find_section(const codes::config::compiled_config& c,
                                                           const char* name) {
    for (const codes::config::compiled_section& s : c.sections)
        if (s.name == name)
            return &s;
    return nullptr;
}

inline const codes::config::compiled_section*
find_subsection(const codes::config::compiled_section& s, const char* name) {
    for (const codes::config::compiled_section& sub : s.subsections)
        if (sub.name == name)
            return &sub;
    return nullptr;
}

inline const codes::config::compiled_key* find_key(const codes::config::compiled_section& s,
                                                   const char* name) {
    for (const codes::config::compiled_key& k : s.keys)
        if (k.name == name)
            return &k;
    return nullptr;
}

// Fetch the first value of a PARAMS key from a compiled config (empty if absent).
inline std::string params_value(const codes::config::compiled_config& c, const char* key) {
    const codes::config::compiled_section* p = find_section(c, "PARAMS");
    if (!p)
        return {};
    const codes::config::compiled_key* k = find_key(*p, key);
    return (k && !k->values.empty()) ? k->values.at(0) : std::string();
}

// A minimal, valid flat-network config; tests append a `sections:` block to it.
inline constexpr const char* kFlatBase = R"(
schema_version: 1
components:
  cn:
    model: nw-lp
    network: simplenet
    message_size: 464
topology:
  format: flat
  component: cn
  nodes: 4
)";

// A minimal, valid parametric dragonfly config. With num_routers: 4 the
// derivation is num_cn = 4/2 = 2, num_groups = 4*2+1 = 9, so repetitions =
// num_groups*num_routers = 36, terminals per rep = 2, one router per rep.
inline constexpr const char* kParametricDragonfly = R"(
schema_version: 1
components:
  compute_host:
    model: nw-lp
topology:
  format: parametric
  fabric:
    model: dragonfly
    shape:
      num_routers: 4
  hosts:
    component: compute_host
)";

// An explicit LP-groups config: two groups, custom LP types, an annotation, and
// a PARAMS block -- the layout escape hatch for non-network configs.
inline constexpr const char* kGroupsBase = R"(
schema_version: 1
topology:
  format: groups
  params:
    message_size: 256
  groups:
    GRP1:
      repetitions: 2
      lps:
        a: 1
        b: 2
        a@foo: 1
    GRP2:
      repetitions: 3
      lps:
        c: 2
)";

// A parametric dragonfly (num_routers: 4) carrying one extra fabric-level scalar
// key -- the pass-through path most model knobs take into PARAMS.
inline std::string dragonfly_with(const std::string& key, const std::string& value) {
    return std::string(R"(
schema_version: 1
components:
  compute_host: { model: nw-lp }
topology:
  format: parametric
  fabric:
    model: dragonfly
    shape:
      num_routers: 4
    )") + key +
           ": " + value + R"(
  hosts:
    component: compute_host
)";
}

#endif /* CODES_TESTS_CONFIG_COMPILER_TEST_UTIL_H */
