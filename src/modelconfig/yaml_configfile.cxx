/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

/*
 * Thin extern "C" boundary of the YAML/JSON configuration front-end. It wires
 * the pure C++ core (config_compiler.h -- parse + validate, throwing on error)
 * to the dumb emitter (config_emitter.h -- IR -> ConfigVTable), and it is the
 * ONLY place in the front-end that touches ROSS: a config_error thrown by the
 * core is translated here into tw_error. Real-run behavior (abort + diagnostic)
 * is therefore unchanged from the .conf path, while "abort" stays a boundary
 * policy rather than being baked into every validation site in the core.
 *
 * Determinism: every rank reads identical bytes and runs the identical
 * deterministic compile, so malformed input throws on every rank and this
 * tw_error fires everywhere at once -- no new divergence versus the .conf path.
 */

#include "yaml_configfile.h"

#include "config_compiler.h"
#include "config_emitter.h"

#include <ross.h>

#include <string_view>

struct ConfigVTable* yaml_configfile_load(const char* data, size_t len) {
    try {
        codes::config::compiled_config cfg = codes::config::compile(std::string_view(data, len));
        return codes::config::emit(cfg);
    } catch (const codes::config::config_error& e) {
        tw_error(TW_LOC, "%s", e.what());
        return nullptr; /* unreachable: tw_error aborts */
    }
}
