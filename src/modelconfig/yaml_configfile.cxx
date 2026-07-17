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
 * Determinism: every rank runs the identical deterministic compile over
 * identical bytes -- the top-level config and every included file are read
 * collectively by the loader (configuration_load), so malformed input throws on
 * every rank and this tw_error fires everywhere at once. The shim itself does no
 * file I/O: it consumes the bytes it is handed. It only computes the *names* of
 * the include files (yaml_configfile_list_includes), leaving the reads to the
 * loader's collective path.
 */

#include "yaml_configfile.h"

#include "config_compiler.h"
#include "config_emitter.h"

#include <ross.h>

#include <cstdlib>
#include <cstring>
#include <exception>
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

/* malloc a NUL-terminated copy of s, for handoff to a C caller that frees it. */
char* dup_cstr(const std::string& s) {
    char* out = static_cast<char*>(std::malloc(s.size() + 1));
    if (!out)
        throw std::bad_alloc();
    std::memcpy(out, s.c_str(), s.size() + 1);
    return out;
}

} // namespace

char** yaml_configfile_list_includes(const char* data, size_t len, const char* path,
                                     size_t* count) {
    try {
        std::vector<std::string> rels = codes::config::parse_includes(std::string_view(data, len));
        *count = rels.size();
        if (rels.empty())
            return nullptr;
        std::string base_dir = dir_of(path);
        char** paths = static_cast<char**>(std::malloc(rels.size() * sizeof(char*)));
        if (!paths)
            throw std::bad_alloc();
        for (size_t i = 0; i < rels.size(); i++)
            paths[i] = dup_cstr(resolve_path(base_dir, rels[i]));
        return paths;
    } catch (const codes::config::config_error& e) {
        tw_error(TW_LOC, "%s", e.what());
        return nullptr; /* unreachable: tw_error aborts */
    } catch (const std::exception& e) {
        tw_error(TW_LOC, "config error (internal): %s", e.what());
        return nullptr; /* unreachable: tw_error aborts */
    }
}

void yaml_configfile_free_includes(char** paths, size_t count) {
    if (!paths)
        return;
    for (size_t i = 0; i < count; i++)
        std::free(paths[i]);
    std::free(paths);
}

struct ConfigVTable* yaml_configfile_load(const char* data, size_t len, const char* const* inc_data,
                                          const size_t* inc_lens, size_t n_inc) {
    try {
        std::string_view main_doc(data, len);
        std::vector<std::string> base_docs;
        base_docs.reserve(n_inc);
        for (size_t i = 0; i < n_inc; i++)
            base_docs.emplace_back(inc_data[i], inc_lens[i]);
        codes::config::compiled_config cfg = codes::config::compile(main_doc, base_docs);
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
