/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#include "config_compiler.h"

#include <codes_ryml.hpp>

#include <cerrno>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace codes {
namespace config {

/* -------------------------------------------------------------------------
 * compiled_config IR builders
 * ---------------------------------------------------------------------- */

void compiled_section::add_key(std::string key, std::string value) {
    keys.push_back(compiled_key{std::move(key), {std::move(value)}});
}

void compiled_section::add_key(std::string key, std::vector<std::string> values) {
    keys.push_back(compiled_key{std::move(key), std::move(values)});
}

compiled_section& compiled_section::add_subsection(std::string subname) {
    subsections.push_back(compiled_section{std::move(subname), {}, {}});
    return subsections.back();
}

compiled_section& compiled_config::add_section(std::string name) {
    sections.push_back(compiled_section{std::move(name), {}, {}});
    return sections.back();
}

namespace {

/* -------------------------------------------------------------------------
 * ryml plumbing -- route parser errors to config_error, never to ROSS. The
 * core throws; the extern "C" shim is the only place tw_error lives.
 * ---------------------------------------------------------------------- */

[[noreturn]] void ryml_throw(ryml::csubstr msg, ryml::ErrorDataBasic const& ed, void*) {
    std::string where =
        ed.location.line ? (" at line " + std::to_string(ed.location.line)) : std::string();
    throw config_error("config error: YAML error" + where + ": " + std::string(msg.str, msg.len));
}

/* ryml raises *parse* (syntax) errors through the separate parse-error callback,
 * whose default implementation prints + aborts; install our own so a malformed
 * document throws config_error like everything else. The message ryml builds
 * already carries the location, so it is passed through as-is. */
[[noreturn]] void ryml_throw_parse(ryml::csubstr msg, ryml::ErrorDataParse const& ed, void*) {
    std::string where =
        ed.ymlloc.line ? (" at line " + std::to_string(ed.ymlloc.line)) : std::string();
    throw config_error("config error: malformed YAML" + where + ": " +
                       std::string(msg.str, msg.len));
}

/* raw scalar text of a node, preserving the exact source spelling */
std::string scalar(ryml::ConstNodeRef n) {
    ryml::csubstr v = n.val();
    return std::string(v.str, v.len);
}

std::string key_of(ryml::ConstNodeRef n) {
    ryml::csubstr k = n.key();
    return std::string(k.str, k.len);
}

bool has(ryml::ConstNodeRef n, const char* key) {
    return n.readable() && n.is_map() && n.has_child(ryml::to_csubstr(key));
}

/* Parse a scalar strictly as a base-10 integer (whole string consumed). */
long parse_int_strict(const std::string& s, const char* what) {
    errno = 0;
    char* end = nullptr;
    long v = std::strtol(s.c_str(), &end, 10);
    if (s.empty() || end == s.c_str() || *end != '\0' || errno != 0)
        throw config_error(std::string("config error: ") + what + " must be an integer, got \"" +
                           s + "\"");
    return v;
}

/* -------------------------------------------------------------------------
 * Friendly-format intermediate representation
 * ---------------------------------------------------------------------- */

using kv_list = std::vector<std::pair<std::string, std::string>>;

/* A custom component: a model paired with configured parameters. */
struct component {
    std::string key;     /* the components: key referenced by a topology */
    std::string model;   /* ComponentModel name (nw-lp, ...) */
    std::string network; /* enumerated flat models: the NIC model a compute node
                            runs its workload over (added with the flat path) */
    kv_list params;      /* scalar model params, raw text, in source order */
};

/* A per-link-class parameter block (e.g. dragonfly local/global/cn). */
struct link_class {
    std::string name; /* class name; combines with each param as <name>_<param> */
    kv_list params;
};

/* A parametric fabric: an HPC topology described by shape parameters. */
struct fabric {
    std::string model;             /* network model, e.g. "dragonfly" */
    kv_list shape;                 /* shape parameters (also drive count derivation) */
    std::vector<link_class> links; /* per-link-class bandwidth / vc_size */
    kv_list routing;               /* routing.* (algorithm maps to PARAMS "routing") */
    kv_list connections;           /* connections.{intra,inter}: file-enumerated wiring */
    kv_list extra;                 /* other scalar fabric keys -> PARAMS verbatim */
    std::string hosts_component;   /* hosts.component: the per-terminal workload */
};

struct friendly_config {
    std::vector<component> components;
    bool parametric = false;
    fabric fab; /* parametric topology */

    const component* find_component(const std::string& k) const {
        for (const component& c : components)
            if (c.key == k)
                return &c;
        return nullptr;
    }
};

/* -------------------------------------------------------------------------
 * Model registry -- maps a friendly fabric model name to its LP-type names,
 * modelnet_order method names, and shape->counts derivation.
 * ---------------------------------------------------------------------- */

/* The LP layout of one repetition. */
struct layout {
    long repetitions;
    long terminals_per_rep; /* workload + NIC LP count per repetition */
    long routers_per_rep;   /* router/switch LP count per repetition */
};

struct fabric_model {
    const char* name;                       /* friendly name used in fabric.model */
    const char* terminal_lp;                /* LPGROUPS lp-type name for the NIC/terminal */
    const char* router_lp;                  /* LPGROUPS lp-type name for the router/switch */
    const char* term_method;                /* modelnet_order method name for the terminal */
    const char* router_method;              /* modelnet_order method for the router, or nullptr
                                  if the router is not a separate model-net method */
    layout (*derive)(const kv_list& shape); /* shape -> LP layout */
};

/* Look up a shape value by name, throwing if absent. */
long shape_int(const kv_list& shape, const char* key) {
    for (const auto& kv : shape)
        if (kv.first == key)
            return std::strtol(kv.second.c_str(), nullptr, 10);
    throw config_error(std::string("config error: fabric shape is missing required key \"") + key +
                       "\"");
}

/* Regular (Kim-Dally) dragonfly: every count follows from num_routers, the
 * routers per group -- the same derivation the model does internally
 * (num_cn = num_routers/2, num_groups = num_routers*num_cn + 1). */
layout derive_dragonfly(const kv_list& shape) {
    long num_routers = shape_int(shape, "num_routers");
    if (num_routers <= 0)
        throw config_error("config error: dragonfly num_routers must be positive");
    long num_cn = num_routers / 2;
    long num_groups = num_routers * num_cn + 1;
    return {num_groups * num_routers, num_cn, 1};
}

const fabric_model fabric_models[] = {
    {"dragonfly", "modelnet_dragonfly", "modelnet_dragonfly_router", "dragonfly",
     "dragonfly_router", derive_dragonfly},
};

const fabric_model* find_fabric_model(const std::string& name) {
    for (const fabric_model& m : fabric_models)
        if (name == m.name)
            return &m;
    return nullptr;
}

/* -------------------------------------------------------------------------
 * Parse: ryml tree -> friendly IR (validating as it goes -- unknown /
 * unconsumed keys are errors, not silent drops).
 * ---------------------------------------------------------------------- */

void parse_components(ryml::ConstNodeRef root, friendly_config& cfg) {
    if (!has(root, "components"))
        return;
    ryml::ConstNodeRef comps = root["components"];
    if (!comps.is_map())
        throw config_error("config error: \"components\" must be a map of name -> component");
    for (ryml::ConstNodeRef cnode : comps.children()) {
        component c;
        c.key = key_of(cnode);
        for (ryml::ConstNodeRef f : cnode.children()) {
            std::string k = key_of(f);
            if (k == "model")
                c.model = scalar(f);
            else if (k == "network")
                c.network = scalar(f);
            else if (k == "type")
                ; /* inferred from the model; not needed for the compiled config */
            else if (f.is_keyval())
                c.params.emplace_back(k, scalar(f));
            else
                throw config_error("config error: component \"" + c.key +
                                   "\": unexpected block \"" + k +
                                   "\"; a component takes a model, an optional network, and scalar "
                                   "params (per-node data, edges and inline workloads are not "
                                   "supported)");
        }
        cfg.components.push_back(std::move(c));
    }
}

void parse_fabric(ryml::ConstNodeRef fnode, fabric& fab) {
    for (ryml::ConstNodeRef c : fnode.children()) {
        std::string k = key_of(c);
        if (k == "model") {
            fab.model = scalar(c);
        } else if (k == "shape") {
            for (ryml::ConstNodeRef s : c.children())
                fab.shape.emplace_back(key_of(s), scalar(s));
        } else if (k == "links") {
            for (ryml::ConstNodeRef lc : c.children()) {
                link_class cls;
                cls.name = key_of(lc);
                for (ryml::ConstNodeRef p : lc.children())
                    cls.params.emplace_back(key_of(p), scalar(p));
                fab.links.push_back(std::move(cls));
            }
        } else if (k == "routing") {
            for (ryml::ConstNodeRef r : c.children())
                fab.routing.emplace_back(key_of(r), scalar(r));
        } else if (k == "connections") {
            /* file-enumerated dragonflies reference the binary connection files
             * by path; the compiler maps intra/inter to the model's key names. */
            for (ryml::ConstNodeRef cn : c.children())
                fab.connections.emplace_back(key_of(cn), scalar(cn));
        } else if (c.is_keyval()) {
            fab.extra.emplace_back(k, scalar(c));
        } else {
            throw config_error("config error: fabric: unexpected block \"" + k + "\"");
        }
    }
}

void parse_topology(ryml::ConstNodeRef root, friendly_config& cfg) {
    if (!has(root, "topology"))
        throw config_error("config error: missing required \"topology\" block");
    ryml::ConstNodeRef topo = root["topology"];

    std::string format = has(topo, "format") ? scalar(topo["format"]) : std::string();

    if (format == "parametric") {
        cfg.parametric = true;
        /* only these keys are consumed for a parametric topology. */
        for (ryml::ConstNodeRef c : topo.children()) {
            std::string k = key_of(c);
            if (k != "format" && k != "fabric" && k != "hosts")
                throw config_error("config error: topology: unexpected key \"" + k +
                                   "\" for a parametric topology");
        }
        if (!has(topo, "fabric"))
            throw config_error("config error: parametric topology needs a \"fabric\" block");
        parse_fabric(topo["fabric"], cfg.fab);
        if (has(topo, "hosts") && has(topo["hosts"], "component"))
            cfg.fab.hosts_component = scalar(topo["hosts"]["component"]);
        else
            throw config_error("config error: parametric topology needs hosts.component naming the "
                               "per-terminal workload");
    } else if (format.empty()) {
        throw config_error("config error: topology needs a \"format\" (e.g. parametric)");
    } else {
        throw config_error("config error: unknown topology format \"" + format + "\"");
    }
}

friendly_config parse_friendly(ryml::ConstNodeRef root) {
    /* reject unknown top-level keys rather than silently ignoring them. */
    for (ryml::ConstNodeRef c : root.children()) {
        std::string k = key_of(c);
        if (k != "schema_version" && k != "components" && k != "topology")
            throw config_error("config error: unexpected top-level key \"" + k + "\"");
    }
    friendly_config cfg;
    parse_components(root, cfg);
    parse_topology(root, cfg);
    return cfg;
}

/* schema_version is required, integer, and any value this build doesn't
 * know is a hard error -- a newer config can't be interpreted safely. */
void require_schema_version(ryml::ConstNodeRef root) {
    if (!has(root, "schema_version"))
        throw config_error(
            "config error: missing required top-level \"schema_version\" (this build understands "
            "version 1)");
    long v = parse_int_strict(scalar(root["schema_version"]), "schema_version");
    if (v != 1)
        throw config_error("config error: unsupported schema_version " + std::to_string(v) +
                           "; this build understands version 1");
}

/* -------------------------------------------------------------------------
 * Compile: friendly IR -> compiled_config
 * ---------------------------------------------------------------------- */

/* Compile a parametric fabric into LPGROUPS + PARAMS. */
void compile_fabric(const friendly_config& cfg, compiled_config& out) {
    const fabric& fab = cfg.fab;

    const fabric_model* model = find_fabric_model(fab.model);
    if (!model)
        throw config_error("config error: unknown fabric model \"" + fab.model + "\"");

    const component* host = cfg.find_component(fab.hosts_component);
    if (!host)
        throw config_error("config error: hosts.component \"" + fab.hosts_component +
                           "\" is not defined under components:");

    layout lay = model->derive(fab.shape);

    /* --- LPGROUPS: one group of `repetitions` slices, each with the
     * per-terminal workload + NIC LPs and the router/switch LPs, emitted in
     * [workload, terminal, router] order to match the layout the model
     * expects. --- */
    compiled_section& grp = out.add_section("LPGROUPS").add_subsection("MODELNET_GRP");
    grp.add_key("repetitions", std::to_string(lay.repetitions));
    grp.add_key(host->model, std::to_string(lay.terminals_per_rep));
    grp.add_key(model->terminal_lp, std::to_string(lay.terminals_per_rep));
    grp.add_key(model->router_lp, std::to_string(lay.routers_per_rep));

    /* --- PARAMS --- */
    compiled_section& params = out.add_section("PARAMS");

    /* modelnet_order is derived from the fabric model: the terminal, plus the
     * router when it is a distinct model-net method. */
    if (model->router_method)
        params.add_key("modelnet_order",
                       std::vector<std::string>{model->term_method, model->router_method});
    else
        params.add_key("modelnet_order", std::vector<std::string>{model->term_method});

    /* shape parameters pass straight through (num_routers etc.). */
    for (const auto& kv : fab.shape)
        params.add_key(kv.first, kv.second);

    /* per-link-class params become <class>_<param> (local_bandwidth, ...). */
    for (const link_class& cls : fab.links)
        for (const auto& kv : cls.params)
            params.add_key(cls.name + "_" + kv.first, kv.second);

    /* routing.algorithm -> "routing"; any other routing.* passes through. */
    for (const auto& kv : fab.routing)
        params.add_key(kv.first == "algorithm" ? std::string("routing") : kv.first, kv.second);

    /* connections.{intra,inter} -> the file-enumerated model's connection-file
     * keys; paths pass through verbatim (the model reads them relative to the
     * working directory). */
    for (const auto& kv : fab.connections) {
        if (kv.first == "intra")
            params.add_key("intra-group-connections", kv.second);
        else if (kv.first == "inter")
            params.add_key("inter-group-connections", kv.second);
        else
            params.add_key(kv.first, kv.second);
    }

    /* remaining scalar fabric keys (packet_size, chunk_size, parity pass-through
     * knobs) map to PARAMS verbatim. */
    for (const auto& kv : fab.extra)
        params.add_key(kv.first, kv.second);

    /* the workload component's own params (if any) also land in PARAMS. */
    for (const auto& kv : host->params)
        params.add_key(kv.first, kv.second);
}

} // namespace

compiled_config compile(std::string_view text) {
    /* Construct the parser with our throwing error handler so that ryml's own
     * parse errors route through config_error -> tw_error at the shim, exactly
     * like our validation errors. The no-parser parse_in_arena builds an event
     * handler with ryml's default callbacks, which print + abort directly and
     * bypass the shim, so we build the handler (and thus its callbacks)
     * explicitly here. */
    ryml::Callbacks cb(nullptr, nullptr, nullptr, ryml_throw);
    /* The Callbacks ctor installs ryml's default parse-error handler (which
     * prints + aborts); replace it with ours so parse errors reach the shim's
     * tw_error like every other error. */
    cb.set_error_parse(ryml_throw_parse);
    ryml::EventHandlerTree evt_handler(cb);
    ryml::Parser parser(&evt_handler);
    ryml::Tree tree = ryml::parse_in_arena(&parser, ryml::to_csubstr("<config>"),
                                           ryml::csubstr(text.data(), text.size()));
    ryml::ConstNodeRef root = tree.rootref();

    if (!root.readable() || !root.is_map())
        throw config_error("config error: config must be a YAML/JSON mapping at the top level");

    require_schema_version(root);
    friendly_config cfg = parse_friendly(root);

    compiled_config out;
    if (cfg.parametric)
        compile_fabric(cfg, out);
    else
        throw config_error("config error: no supported topology found");
    return out;
}

} // namespace config
} // namespace codes
