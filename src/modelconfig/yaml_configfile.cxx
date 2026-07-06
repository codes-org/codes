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

#include <exception>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

/* Directory portion of a path ("a/b/c.yaml" -> "a/b"), or "." if none. */
std::string dir_of(const char* path) {
    std::string p = path ? path : "";
    std::string::size_type slash = p.find_last_of('/');
    return slash == std::string::npos ? std::string(".") : p.substr(0, slash);
}

/* Resolve an include path: absolute as-is, otherwise relative to base_dir. */
std::string resolve_path(const std::string& base_dir, const std::string& rel) {
    if (!rel.empty() && rel[0] == '/')
        return rel;
    return base_dir + "/" + rel;
}

/* Read a whole file into a string, throwing config_error if it can't be read. */
std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw codes::config::config_error("config error: cannot read included file \"" + path +
                                          "\"");
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/* Resolve the main document's top-level `include:` list into the contents of the
 * referenced files (in listed order), read relative to the config's directory. */
std::vector<std::string> read_includes(std::string_view main_doc, const char* path) {
    std::vector<std::string> docs;
    std::string base_dir = dir_of(path);
    for (const std::string& rel : codes::config::parse_includes(main_doc))
        docs.push_back(read_file(resolve_path(base_dir, rel)));
    return docs;
}

} // namespace

struct ConfigVTable* yaml_configfile_load(const char* data, size_t len, const char* path) {
    try {
        std::string_view main_doc(data, len);
        std::vector<std::string> includes = read_includes(main_doc, path);
        codes::config::compiled_config cfg = codes::config::compile(main_doc, includes);
        return codes::config::emit(cfg);
    } catch (const codes::config::config_error& e) {
        tw_error(TW_LOC, "%s", e.what());
        return nullptr; /* unreachable: tw_error aborts */
    } catch (const std::exception& e) {
        /* Any non-config_error exception (std::bad_alloc, a ryml internal, ...)
         * must not escape this extern "C" frame -- that would call
         * std::terminate with no diagnostic. Route it to tw_error too. */
        tw_error(TW_LOC, "config error (internal): %s", e.what());
        return nullptr; /* unreachable: tw_error aborts */
    }
}
