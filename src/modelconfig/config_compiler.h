/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#ifndef SRC_MODELCONFIG_CONFIG_COMPILER_H
#define SRC_MODELCONFIG_CONFIG_COMPILER_H

/**
 * @file config_compiler.h
 *
 * Pure C++ core of the YAML/JSON configuration front-end.
 *
 * codes::config::compile() turns the user-friendly topology + component text
 * into a @ref codes::config::compiled_config -- an ordered, plain-data image of
 * the LPGROUPS/PARAMS configuration the legacy `.conf` parser produces. It does
 * all parsing and validation and has **no** dependency on ROSS, MPI, or
 * `abort`: on any invalid input it throws @ref codes::config::config_error.
 *
 * The dumb emitter (config_emitter.h) turns a compiled_config into a
 * ConfigVTable, and a thin `extern "C"` shim (yaml_configfile.h) is the *only*
 * place that translates a config_error into ROSS's `tw_error`. Keeping the core
 * free of ROSS makes it unit-testable in isolation: tests drive compile() and
 * assert on the returned compiled_config, never on simulator behavior. This
 * "pure core that throws + boundary shim that owns the tw_error translation" is
 * the template for future C++ subsystems in codes.
 */

#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace codes {
namespace config {

/**
 * Thrown by compile() on any syntax or validation failure. The message is a
 * finished, user-facing diagnostic; the shim hands it verbatim to tw_error.
 */
struct config_error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/** One key and its value(s): a scalar carries one value, a list-valued key
 *  (e.g. `modelnet_order`) several, emitted as `("a","b",...)`. */
struct compiled_key {
    std::string name;
    std::vector<std::string> values;
};

/** One configuration section: named keys plus nested subsections. LPGROUPS
 *  holds a MODELNET_GRP subsection; PARAMS is flat. Insertion order is
 *  preserved throughout, so a compiled config is byte-comparable to the `.conf`
 *  it replaces. */
struct compiled_section {
    std::string name;
    std::vector<compiled_key> keys;
    std::vector<compiled_section> subsections;

    /** Append a scalar key. */
    void add_key(std::string key, std::string value);
    /** Append a list-valued key. */
    void add_key(std::string key, std::vector<std::string> values);
    /** Append and return a nested subsection. */
    compiled_section& add_subsection(std::string subname);
};

/** The whole compiled config: the top-level (ROOT) sections, in order. */
struct compiled_config {
    std::vector<compiled_section> sections;

    /** Append and return a top-level section. */
    compiled_section& add_section(std::string name);
};

/**
 * Compile YAML/JSON configuration text into the compiled_config IR.
 *
 * @param text  the raw config bytes (JSON is a subset of YAML, so one parser
 *              handles both).
 * @throws config_error on malformed YAML or any schema violation.
 */
compiled_config compile(std::string_view text);

} // namespace config
} // namespace codes

#endif
