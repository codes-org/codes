/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#include "config_compiler.h"

#include "unit_convert.h"

#include <codes_ryml.hpp>

#include <algorithm>
#include <cerrno>
#include <climits> /* LONG_MAX: guard dimension-product overflow */
#include <cstdlib>
#include <strings.h> /* strcasecmp: section names are case-insensitive */
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
    /* list-valued fabric keys (e.g. slimfly generator_set_X) -> multi-value PARAMS */
    std::vector<std::pair<std::string, std::vector<std::string>>> extra_lists;
    std::string hosts_component; /* hosts.component: the per-terminal workload */
};

struct friendly_config {
    std::vector<component> components;
    bool parametric = false;
    fabric fab; /* parametric topology */

    bool flat = false;          /* flat enumerated topology */
    std::string flat_component; /* the component every node runs */
    long node_count = 0;        /* number of nodes = repetitions */

    /* verbatim `sections:` blocks -- config a model reads directly (DIRECTOR,
     * surrogate, storage, ...), passed straight through to the compiled output.
     * Emitted after the topology sections. */
    std::vector<compiled_section> passthrough;

    /* explicit LP-groups topology (`format: groups`): the user lays out groups,
     * LP types, counts and annotations directly. The escape hatch for configs
     * that are not a single friendly network. Built during parse. */
    bool explicit_groups = false;
    compiled_section explicit_lpgroups{"LPGROUPS", {}, {}};
    compiled_section explicit_params{"PARAMS", {}, {}};

    /* run-level `simulation:` settings, already resolved to the PARAMS keys
     * codes_mapping reads (end_time in nanoseconds, pe_mem_factor). Empty unless
     * a simulation: block appeared; a later document overrides an earlier one key
     * by key. Appended to PARAMS once the topology is compiled. */
    kv_list simulation;

    /* Record a resolved simulation setting, overriding any earlier value for the
     * same key (last document wins, like components and sections). */
    void set_simulation(const std::string& name, std::string value) {
        for (auto& kv : simulation)
            if (kv.first == name) {
                kv.second = std::move(value);
                return;
            }
        simulation.emplace_back(name, std::move(value));
    }

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

/* -------------------------------------------------------------------------
 * Per-parameter unit metadata.
 *
 * A dimensioned PARAMS key carries its quantity and the model's internal unit,
 * expressed as how many canonical base units make up one internal unit (time
 * base = ns, size base = bytes, bandwidth base = bytes/second). A unit-bearing
 * value is normalized to the base unit (unit_convert) then divided by this scale
 * to get the number the model reads; a bare number is emitted verbatim, so it
 * already means the internal unit. See add_user_param.
 * ---------------------------------------------------------------------- */
struct param_unit {
    const char* name;      /* PARAMS key name */
    quantity kind;         /* time / size / bandwidth */
    double internal_scale; /* canonical base units per one internal unit */
};

/* Bandwidth internal-unit scales (bytes/second in one internal unit). CODES
 * models do NOT agree on a bandwidth unit -- these come straight from each
 * model's own byte-time arithmetic (see doc/dev/yaml-config.md). */
constexpr double BW_GIB_S = 1024.0 * 1024.0 * 1024.0;    /* GiB/s: bytes_to_ns() models */
constexpr double BW_MIB_S = 1024.0 * 1024.0;             /* MiB/s: simplenet net_bw_mbps */
constexpr double BW_BYTES_NS = 1000.0 * 1000.0 * 1000.0; /* bytes/ns (= GB/s): fattree */

/* Time internal-unit scales (ns in one internal unit). */
constexpr double T_NS = 1.0;
constexpr double T_US = 1000.0; /* the dragonfly QoS counting_* windows are read in us */

constexpr double SZ_BYTES = 1.0; /* every size param is read in bytes */

/* Dimensioned keys whose unit is the same in every model that reads them: byte
 * counts and nanosecond delays. Looked up for every model in addition to its own
 * table below, so they need not be repeated per model. */
const param_unit common_params[] = {
    {"packet_size", quantity::size, SZ_BYTES},    {"chunk_size", quantity::size, SZ_BYTES},
    {"message_size", quantity::size, SZ_BYTES},   {"credit_size", quantity::size, SZ_BYTES},
    {"buffer_size", quantity::size, SZ_BYTES},    {"vc_size", quantity::size, SZ_BYTES},
    {"cn_vc_size", quantity::size, SZ_BYTES},     {"local_vc_size", quantity::size, SZ_BYTES},
    {"global_vc_size", quantity::size, SZ_BYTES}, {"router_delay", quantity::time, T_NS},
    {"soft_delay", quantity::time, T_NS},         {"net_startup_ns", quantity::time, T_NS},
};

/* Per-model dimensioned keys: the bandwidths (whose unit varies by model) and
 * the microsecond QoS counting windows. Sizes and ns delays come from
 * common_params above. */
const param_unit dragonfly_units[] = {
    {"local_bandwidth", quantity::bandwidth, BW_GIB_S},
    {"global_bandwidth", quantity::bandwidth, BW_GIB_S},
    {"cn_bandwidth", quantity::bandwidth, BW_GIB_S},
};
const param_unit dragonfly_dally_units[] = {
    {"local_bandwidth", quantity::bandwidth, BW_GIB_S},
    {"global_bandwidth", quantity::bandwidth, BW_GIB_S},
    {"cn_bandwidth", quantity::bandwidth, BW_GIB_S},
    {"counting_start", quantity::time, T_US},
    {"counting_interval", quantity::time, T_US},
};
const param_unit dragonfly_plus_units[] = {
    {"local_bandwidth", quantity::bandwidth, BW_GIB_S},
    {"global_bandwidth", quantity::bandwidth, BW_GIB_S},
    {"cn_bandwidth", quantity::bandwidth, BW_GIB_S},
    {"counting_start", quantity::time, T_US},
    {"counting_interval", quantity::time, T_US},
    {"counting_end", quantity::time, T_US},
};
const param_unit dragonfly_custom_units[] = {
    {"local_bandwidth", quantity::bandwidth, BW_GIB_S},
    {"global_bandwidth", quantity::bandwidth, BW_GIB_S},
    {"cn_bandwidth", quantity::bandwidth, BW_GIB_S},
    {"counting_start", quantity::time, T_US},
    {"counting_interval", quantity::time, T_US},
};
const param_unit slimfly_units[] = {
    {"local_bandwidth", quantity::bandwidth, BW_GIB_S},
    {"global_bandwidth", quantity::bandwidth, BW_GIB_S},
    {"cn_bandwidth", quantity::bandwidth, BW_GIB_S},
};
const param_unit torus_units[] = {
    {"link_bandwidth", quantity::bandwidth, BW_GIB_S},
};
const param_unit express_mesh_units[] = {
    {"link_bandwidth", quantity::bandwidth, BW_GIB_S},
    {"cn_bandwidth", quantity::bandwidth, BW_GIB_S},
};
/* fattree reads bandwidth as bytes/ns (1/bandwidth ns per byte), NOT the GiB/s
 * convention the bytes_to_ns() models use -- a value means a different rate here. */
const param_unit fattree_units[] = {
    {"link_bandwidth", quantity::bandwidth, BW_BYTES_NS},
    {"cn_bandwidth", quantity::bandwidth, BW_BYTES_NS},
};

/* simplenet's sole scalar rate; despite the "mbps" spelling the model reads it in
 * MiB/s (rate_to_ns divides bytes by 1024*1024). simplep2p/loggp take their rates
 * from files, so they have no scalar bandwidth key here. */
const param_unit simplenet_units[] = {
    {"net_bw_mbps", quantity::bandwidth, BW_MIB_S},
};

/* count of a param_unit table declared as a C array */
template <class T, size_t N> constexpr size_t array_len(const T (&)[N]) {
    return N;
}

/* Find the unit metadata for a PARAMS key: the model's own table first, then the
 * shared common table; nullptr if the key is not a known dimensioned param. */
const param_unit* find_param_unit(const param_unit* model_units, size_t n_model,
                                  const std::string& key) {
    for (size_t i = 0; i < n_model; ++i)
        if (key == model_units[i].name)
            return &model_units[i];
    for (const param_unit& p : common_params)
        if (key == p.name)
            return &p;
    return nullptr;
}

struct fabric_model {
    const char* name;                       /* friendly name used in fabric.model */
    const char* terminal_lp;                /* LPGROUPS lp-type name for the NIC/terminal */
    const char* router_lp;                  /* LPGROUPS lp-type name for the router/switch */
    const char* term_method;                /* modelnet_order method name for the terminal */
    const char* router_method;              /* modelnet_order method for the router, or nullptr
                                  if the router is not a separate model-net method */
    layout (*derive)(const kv_list& shape); /* shape -> LP layout */
    const param_unit* units;                /* model's dimensioned-param unit table */
    size_t n_units;                         /* entries in `units` */
};

/* Look up a shape value by name, throwing if absent. The value is parsed
 * strictly, so a non-integer (num_groups: abc) or trailing garbage (9x) is a
 * diagnostic naming the offending key rather than a silent 0/9. */
long shape_int(const kv_list& shape, const char* key) {
    for (const auto& kv : shape) {
        if (kv.first == key) {
            std::string what = std::string("fabric shape \"") + key + "\"";
            return parse_int_strict(kv.second, what.c_str());
        }
    }
    throw config_error(std::string("config error: fabric shape is missing required key \"") + key +
                       "\"");
}

/* Look up a shape value by name, returning a default when absent. Present values
 * are parsed strictly (see shape_int). */
long shape_int_default(const kv_list& shape, const char* key, long dflt) {
    for (const auto& kv : shape) {
        if (kv.first == key) {
            std::string what = std::string("fabric shape \"") + key + "\"";
            return parse_int_strict(kv.second, what.c_str());
        }
    }
    return dflt;
}

/* Look up a shape value by name as its raw string, throwing if absent. */
const std::string& shape_str(const kv_list& shape, const char* key) {
    for (const auto& kv : shape)
        if (kv.first == key)
            return kv.second;
    throw config_error(std::string("config error: fabric shape is missing required key \"") + key +
                       "\"");
}

/* Parse a comma-separated dimension list ("4,2,2" -> {4,2,2}) strictly: a
 * non-empty, strictly comma-separated list of positive integers (optional spaces
 * around a value), rejecting empty segments ("4,,2"), a trailing comma ("4,2,"),
 * and space-separated lists ("4 2"). */
std::vector<long> parse_dim_lengths(const kv_list& shape, const char* key) {
    const std::string& s = shape_str(shape, key);
    auto bad = [&]() -> config_error {
        return config_error(std::string("config error: fabric shape \"") + key +
                            "\" must be a comma-separated list of positive integers, got \"" + s +
                            "\"");
    };
    std::vector<long> dims;
    const char* p = s.c_str();
    for (;;) {
        char* end = nullptr;
        errno = 0;
        long d = std::strtol(p, &end, 10); /* strtol skips any leading spaces */
        if (end == p || errno != 0 || d <= 0)
            throw bad();
        dims.push_back(d);
        p = end;
        while (*p == ' ') /* trailing spaces after this value */
            ++p;
        if (*p == '\0')
            break;
        if (*p != ',') /* only a comma may separate values (rejects "4 2") */
            throw bad();
        ++p; /* consume the single comma; the next value is now required */
    }
    return dims;
}

/* Node/router count of a mesh-style fabric (torus, express_mesh): the product of
 * the per-dimension lengths in dim_length. The entry count is cross-checked
 * against the n_dims shape value, and the running product is guarded against
 * signed overflow. `model_name` names the offending fabric in diagnostics. */
long mesh_node_count(const kv_list& shape, const char* model_name) {
    long n_dims = shape_int(shape, "n_dims");
    std::vector<long> dims = parse_dim_lengths(shape, "dim_length");
    if (static_cast<long>(dims.size()) != n_dims)
        throw config_error(std::string("config error: ") + model_name + " n_dims (" +
                           std::to_string(n_dims) +
                           ") does not match the number of dim_length entries (" +
                           std::to_string(dims.size()) + ")");
    long prod = 1;
    for (long d : dims) {
        if (prod > LONG_MAX / d) /* d >= 1, so the divide is safe */
            throw config_error(std::string("config error: ") + model_name +
                               " dim_length product overflows");
        prod *= d;
    }
    return prod;
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

/* Dragonfly-dally (file-enumerated): the shape counts are genuine inputs that
 * must match the connection files. total routers = num_groups * num_planes *
 * num_routers; each router hosts num_cns_per_router terminals. */
layout derive_dragonfly_dally(const kv_list& shape) {
    long num_routers = shape_int(shape, "num_routers");
    long num_groups = shape_int(shape, "num_groups");
    long num_cns = shape_int(shape, "num_cns_per_router");
    long num_planes = shape_int_default(shape, "num_planes", 1);
    /* Not part of the LP-count arithmetic, but required up front: the value must
     * match the wiring in the connection files, and a missing key would
     * otherwise fall back to the model's default (10) with only a warning --
     * silently contradicting the wiring. */
    shape_int(shape, "num_global_channels");
    return {num_groups * num_planes * num_routers, num_cns, 1};
}

/* Fat-tree (internally-generated): one repetition per edge switch, each hosting
 * switch_radix/2 terminals, with one switch LP per level. The fabric's switch is
 * not a separate model-net method, so only the terminal appears in
 * modelnet_order. */
layout derive_fattree(const kv_list& shape) {
    long switch_count = shape_int(shape, "switch_count");
    long switch_radix = shape_int(shape, "switch_radix");
    long num_levels = shape_int(shape, "num_levels");
    /* Each edge switch hosts switch_radix/2 terminals; an odd radix would
     * silently truncate that split, so reject it rather than lose a terminal. */
    if (switch_radix % 2 != 0)
        throw config_error("config error: fattree switch_radix must be even (each edge switch "
                           "hosts switch_radix/2 terminals), got " +
                           std::to_string(switch_radix));
    return {switch_count, switch_radix / 2, num_levels};
}

/* Torus (internally-generated): one repetition per torus node, each a single
 * terminal, and no separate router LP (the torus node combines routing and the
 * terminal). The node count is the product of the per-dimension lengths. */
layout derive_torus(const kv_list& shape) {
    long nodes = mesh_node_count(shape, "torus");
    return {nodes, 1, 0};
}

/* Express mesh (internally-generated): one repetition per mesh router, each
 * hosting num_cn terminals plus one router LP. The router count is the product
 * of the per-dimension lengths. */
layout derive_express_mesh(const kv_list& shape) {
    long routers = mesh_node_count(shape, "express-mesh");
    long num_cn = shape_int(shape, "num_cn");
    return {routers, num_cn, 1};
}

/* Slimfly (internally-generated, MMS topology): the two Cayley subgraphs give
 * 2 * num_routers^2 routers total, one repetition each, hosting num_terminals
 * terminals plus one router LP. */
layout derive_slimfly(const kv_list& shape) {
    long num_routers = shape_int(shape, "num_routers");
    long num_terminals = shape_int(shape, "num_terminals");
    if (num_routers <= 0)
        throw config_error("config error: slimfly num_routers must be positive");
    return {2 * num_routers * num_routers, num_terminals, 1};
}

/* Dragonfly-plus (file-enumerated): one repetition per group. Each group's
 * routers split into a spine and a leaf level (num_router_spine + num_router_leaf
 * router LPs); only the leaf routers host terminals, num_cns_per_router each. The
 * shape counts are genuine inputs that must match the connection files. */
layout derive_dragonfly_plus(const kv_list& shape) {
    long num_groups = shape_int(shape, "num_groups");
    long spine = shape_int(shape, "num_router_spine");
    long leaf = shape_int(shape, "num_router_leaf");
    long num_cns = shape_int(shape, "num_cns_per_router");
    return {num_groups, leaf * num_cns, spine + leaf};
}

/* Dragonfly-custom (file-enumerated): one repetition per router. Each group is a
 * num_router_rows x num_router_cols mesh of routers, so total routers =
 * num_groups * num_router_rows * num_router_cols; each router hosts
 * num_cns_per_router terminals plus one router LP. The shape counts are genuine
 * inputs that must match the connection files. */
layout derive_dragonfly_custom(const kv_list& shape) {
    long num_groups = shape_int(shape, "num_groups");
    long rows = shape_int(shape, "num_router_rows");
    long cols = shape_int(shape, "num_router_cols");
    long num_cns = shape_int(shape, "num_cns_per_router");
    /* Not part of the LP-count arithmetic, but required up front: the value must
     * match the wiring in the connection files, and a missing key would
     * otherwise fall back to the model's default (10) with only a warning --
     * silently contradicting the wiring. */
    shape_int(shape, "num_global_channels");
    return {num_groups * rows * cols, num_cns, 1};
}

const fabric_model fabric_models[] = {
    {"dragonfly", "modelnet_dragonfly", "modelnet_dragonfly_router", "dragonfly",
     "dragonfly_router", derive_dragonfly, dragonfly_units, array_len(dragonfly_units)},
    {"dragonfly-dally", "modelnet_dragonfly_dally", "modelnet_dragonfly_dally_router",
     "dragonfly_dally", "dragonfly_dally_router", derive_dragonfly_dally, dragonfly_dally_units,
     array_len(dragonfly_dally_units)},
    {"fattree", "modelnet_fattree", "fattree_switch", "fattree", nullptr, derive_fattree,
     fattree_units, array_len(fattree_units)},
    {"torus", "modelnet_torus", nullptr, "torus", nullptr, derive_torus, torus_units,
     array_len(torus_units)},
    {"express-mesh", "modelnet_express_mesh", "modelnet_express_mesh_router", "express_mesh",
     "express_mesh_router", derive_express_mesh, express_mesh_units, array_len(express_mesh_units)},
    {"slimfly", "modelnet_slimfly", "modelnet_slimfly_router", "slimfly", "slimfly_router",
     derive_slimfly, slimfly_units, array_len(slimfly_units)},
    {"dragonfly-plus", "modelnet_dragonfly_plus", "modelnet_dragonfly_plus_router",
     "dragonfly_plus", "dragonfly_plus_router", derive_dragonfly_plus, dragonfly_plus_units,
     array_len(dragonfly_plus_units)},
    {"dragonfly-custom", "modelnet_dragonfly_custom", "modelnet_dragonfly_custom_router",
     "dragonfly_custom", "dragonfly_custom_router", derive_dragonfly_custom, dragonfly_custom_units,
     array_len(dragonfly_custom_units)},
};

const fabric_model* find_fabric_model(const std::string& name) {
    for (const fabric_model& m : fabric_models)
        if (name == m.name)
            return &m;
    return nullptr;
}

/* A flat (enumerated) network model: one NIC LP per compute node, all peers.
 * Maps a friendly network name to the LPGROUPS lp-type name and the
 * modelnet_order method the model registers. */
struct network_model {
    const char* name;        /* friendly name used in a component's network: field */
    const char* nic_lp;      /* LPGROUPS lp-type name for the NIC */
    const char* method;      /* modelnet_order method name */
    const param_unit* units; /* model's dimensioned-param unit table */
    size_t n_units;          /* entries in `units` */
};

const network_model network_models[] = {
    {"simplenet", "modelnet_simplenet", "simplenet", simplenet_units, array_len(simplenet_units)},
    /* simplep2p and loggp read their rates from files (paths pass through as
     * non-numeric values), so they declare no scalar dimensioned param here. */
    {"simplep2p", "modelnet_simplep2p", "simplep2p", nullptr, 0},
    {"loggp", "modelnet_loggp", "loggp", nullptr, 0},
};

const network_model* find_network_model(const std::string& name) {
    for (const network_model& m : network_models)
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
                /* Reserved for a future schema version. Reject explicitly: a bare
                 * `type:` scalar would otherwise fall through to the is_keyval()
                 * branch below and silently become a model param in PARAMS. */
                throw config_error("config error: component \"" + c.key +
                                   "\": key \"type\" is reserved for a future schema version and "
                                   "is not accepted yet; remove it (the model is inferred from "
                                   "\"model:\")");
            else if (f.is_keyval())
                c.params.emplace_back(k, scalar(f));
            else
                throw config_error("config error: component \"" + c.key +
                                   "\": unexpected block \"" + k +
                                   "\"; a component takes a model, an optional network, and scalar "
                                   "params (per-node data, edges and inline workloads are not "
                                   "supported)");
        }
        /* include-merge: a later document's component overrides an earlier one of
         * the same name (within one document, keys are already unique). */
        cfg.components.erase(std::remove_if(cfg.components.begin(), cfg.components.end(),
                                            [&](const component& e) { return e.key == c.key; }),
                             cfg.components.end());
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
        } else if (c.is_seq()) {
            /* a list-valued fabric param (e.g. slimfly generator_set_X: [1, 4])
             * becomes a multi-value PARAMS key. */
            std::vector<std::string> vals;
            for (ryml::ConstNodeRef v : c.children())
                vals.push_back(scalar(v));
            fab.extra_lists.emplace_back(k, std::move(vals));
        } else if (c.is_keyval()) {
            fab.extra.emplace_back(k, scalar(c));
        } else {
            throw config_error("config error: fabric: unexpected block \"" + k + "\"");
        }
    }
}

/* defined with the other pass-through helpers below; used here to build PARAMS
 * for the explicit-groups form. */
compiled_section build_passthrough_section(const std::string& name, ryml::ConstNodeRef node);

/* Build the LPGROUPS section from an explicit `groups` map. Each group names its
 * repetitions and its LP types with counts; an LP-type key may carry an
 * annotation as `type@annotation` (the .conf spelling codes_mapping splits on
 * '@'). This is the general layout escape hatch for configs that are not a
 * single friendly network -- storage clusters, multi-partition, mapping tests --
 * where the compiler derives nothing and the user lays out the LPs directly. */
void parse_explicit_groups(ryml::ConstNodeRef groups, friendly_config& cfg) {
    if (!groups.is_map() || groups.num_children() == 0)
        throw config_error("config error: topology.groups must be a non-empty map of "
                           "group-name -> { repetitions, lps }");
    for (ryml::ConstNodeRef g : groups.children()) {
        std::string gname = key_of(g);
        if (!g.is_map())
            throw config_error("config error: topology.groups: \"" + gname +
                               "\" must be a block with repetitions and lps");
        for (ryml::ConstNodeRef c : g.children()) {
            std::string k = key_of(c);
            if (k != "repetitions" && k != "lps")
                throw config_error("config error: topology.groups: \"" + gname +
                                   "\": unexpected key \"" + k + "\" (only repetitions and lps)");
        }
        if (!has(g, "repetitions"))
            throw config_error("config error: topology.groups: \"" + gname +
                               "\" needs a repetitions count");
        std::string reps_what = "topology.groups \"" + gname + "\" repetitions";
        long reps = parse_int_strict(scalar(g["repetitions"]), reps_what.c_str());
        if (reps <= 0)
            throw config_error("config error: topology.groups: \"" + gname +
                               "\" repetitions must be positive");
        if (!has(g, "lps") || !g["lps"].is_map() || g["lps"].num_children() == 0)
            throw config_error("config error: topology.groups: \"" + gname +
                               "\" needs a non-empty lps map of lp-type -> count");
        compiled_section& grp = cfg.explicit_lpgroups.add_subsection(gname);
        grp.add_key("repetitions", std::to_string(reps));
        for (ryml::ConstNodeRef lp : g["lps"].children()) {
            std::string lptype = key_of(lp);
            std::string count_what =
                "topology.groups \"" + gname + "\" lp \"" + lptype + "\" count";
            long count = parse_int_strict(scalar(lp), count_what.c_str());
            if (count <= 0)
                throw config_error("config error: topology.groups: \"" + gname + "\": lp \"" +
                                   lptype + "\" count must be positive");
            grp.add_key(lptype, std::to_string(count));
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
        if (!has(topo, "hosts"))
            throw config_error("config error: parametric topology needs hosts.component naming the "
                               "per-terminal workload");
        ryml::ConstNodeRef hosts = topo["hosts"];
        /* only `component` is consumed under hosts; anything else is an error
         * rather than a silent drop. */
        for (ryml::ConstNodeRef h : hosts.children()) {
            std::string k = key_of(h);
            if (k != "component")
                throw config_error("config error: topology.hosts: unexpected key \"" + k +
                                   "\" (only \"component\" is supported)");
        }
        if (!has(hosts, "component"))
            throw config_error("config error: parametric topology needs hosts.component naming the "
                               "per-terminal workload");
        cfg.fab.hosts_component = scalar(hosts["component"]);
    } else if (format == "flat") {
        cfg.flat = true;
        /* only these keys are consumed for a flat topology. */
        for (ryml::ConstNodeRef c : topo.children()) {
            std::string k = key_of(c);
            if (k != "format" && k != "component" && k != "nodes")
                throw config_error("config error: topology: unexpected key \"" + k +
                                   "\" for a flat topology");
        }
        if (!has(topo, "component"))
            throw config_error("config error: flat topology needs a \"component\" naming the "
                               "compute node (workload + its NIC model)");
        cfg.flat_component = scalar(topo["component"]);
        if (!has(topo, "nodes"))
            throw config_error("config error: flat topology needs a \"nodes\" count");
        cfg.node_count = parse_int_strict(scalar(topo["nodes"]), "topology.nodes");
        if (cfg.node_count <= 0)
            throw config_error("config error: topology.nodes must be positive");
    } else if (format == "groups") {
        cfg.explicit_groups = true;
        /* only these keys are consumed for an explicit-groups topology. */
        for (ryml::ConstNodeRef c : topo.children()) {
            std::string k = key_of(c);
            if (k != "format" && k != "groups" && k != "params")
                throw config_error("config error: topology: unexpected key \"" + k +
                                   "\" for an explicit-groups topology");
        }
        if (!has(topo, "groups"))
            throw config_error("config error: explicit-groups topology needs a \"groups\" block");
        parse_explicit_groups(topo["groups"], cfg);
        /* PARAMS is written out directly here (the compiler derives nothing for
         * this form); a scalar/list/nested map passes through like any section. */
        if (has(topo, "params")) {
            if (!topo["params"].is_map())
                throw config_error("config error: topology.params must be a map of key -> value");
            cfg.explicit_params = build_passthrough_section("PARAMS", topo["params"]);
        }
    } else if (format.empty()) {
        throw config_error(
            "config error: topology needs a \"format\" (flat, parametric, or groups)");
    } else {
        throw config_error("config error: unknown topology format \"" + format + "\"");
    }
}

/* -------------------------------------------------------------------------
 * Pass-through sections (`sections:`)
 *
 * Config a model reads directly by name (DIRECTOR, NETWORK_SURROGATE, storage,
 * resource, ...) is not something the compiler derives or transforms, so it is
 * carried through verbatim rather than modeled key-by-key. A new feature can
 * add its section here with no compiler change: write the block under
 * `sections:` and read it in the model. Section names are case-insensitive
 * (matched so at lookup time); a section may optionally register a schema below
 * to enforce required keys while still allowing any other key through.
 * ---------------------------------------------------------------------- */

bool iequals(const std::string& a, const char* b) {
    return strcasecmp(a.c_str(), b) == 0;
}

/* Recursively turn a ryml map node into a compiled_section: scalars become
 * single-value keys, sequences multi-value keys, nested maps subsections. */
compiled_section build_passthrough_section(const std::string& name, ryml::ConstNodeRef node) {
    compiled_section sec{name, {}, {}};
    for (ryml::ConstNodeRef c : node.children()) {
        std::string k = key_of(c);
        if (c.is_seq()) {
            std::vector<std::string> vals;
            for (ryml::ConstNodeRef v : c.children())
                vals.push_back(scalar(v));
            sec.add_key(std::move(k), std::move(vals));
        } else if (c.is_map()) {
            sec.subsections.push_back(build_passthrough_section(k, c));
        } else if (c.is_keyval()) {
            sec.add_key(std::move(k), scalar(c));
        } else {
            throw config_error("config error: sections: \"" + name + "\": \"" + k +
                               "\" must be a scalar, a list, or a nested block");
        }
    }
    return sec;
}

/* Optional, open schema for a pass-through section: it enforces required keys
 * but does NOT restrict the rest, so a section can carry keys not listed here
 * (useful while a feature's config is still in flux). A section with no entry is
 * passed through entirely unvalidated. To register one, add a row -- see
 * doc/dev/yaml-config.md ("Adding a config section"). */
struct section_schema {
    const char* name;            /* section name, matched case-insensitively */
    const char* const* required; /* nullptr-terminated required key names */
};

/* The resource LP aborts at runtime if its `resource` section lacks `available`
 * (src/util/resource-lp.c); catch it here with a clearer, earlier diagnostic. */
const char* const resource_required[] = {"available", nullptr};

const section_schema section_schemas[] = {
    {"resource", resource_required},
};

const section_schema* find_section_schema(const std::string& name) {
    for (const section_schema& s : section_schemas)
        if (iequals(name, s.name))
            return &s;
    return nullptr;
}

/* Required-key presence is checked case-sensitively: a model reads its keys by
 * exact name, so the required key must be spelled as the model reads it. */
bool section_has_key(const compiled_section& sec, const char* key) {
    for (const compiled_key& k : sec.keys)
        if (k.name == key)
            return true;
    return false;
}

void validate_section_schema(const compiled_section& sec) {
    const section_schema* schema = find_section_schema(sec.name);
    if (!schema || !schema->required)
        return;
    for (const char* const* r = schema->required; *r; ++r)
        if (!section_has_key(sec, *r))
            throw config_error("config error: section \"" + sec.name +
                               "\" is missing required key \"" + std::string(*r) + "\"");
}

void parse_sections(ryml::ConstNodeRef root, friendly_config& cfg) {
    if (!has(root, "sections"))
        return;
    ryml::ConstNodeRef secs = root["sections"];
    if (!secs.is_map())
        throw config_error("config error: \"sections\" must be a map of section-name -> keys");
    for (ryml::ConstNodeRef s : secs.children()) {
        std::string name = key_of(s);
        if (!s.is_map())
            throw config_error("config error: sections: \"" + name + "\" must be a block of keys");
        /* the compiler emits LPGROUPS and PARAMS from the topology; a pass-through
         * section must not shadow them. */
        if (iequals(name, "LPGROUPS") || iequals(name, "PARAMS"))
            throw config_error("config error: sections: \"" + name +
                               "\" is reserved -- the compiler emits it from the topology; put "
                               "model parameters on the component or fabric instead");
        compiled_section sec = build_passthrough_section(name, s);
        validate_section_schema(sec);
        /* include-merge: a later document's section overrides an earlier one of
         * the same (case-insensitive) name. */
        cfg.passthrough.erase(std::remove_if(cfg.passthrough.begin(), cfg.passthrough.end(),
                                             [&](const compiled_section& e) {
                                                 return iequals(e.name, sec.name.c_str());
                                             }),
                              cfg.passthrough.end());
        cfg.passthrough.push_back(std::move(sec));
    }
}

/* -------------------------------------------------------------------------
 * Run-level settings (`simulation:`)
 *
 * The top-level `simulation:` block carries settings that belong to the run
 * itself rather than to the topology or a model: the simulation end time and the
 * ROSS per-PE event-pool factor. Each is resolved to the PARAMS key codes_mapping
 * already reads (end_time in nanoseconds, pe_mem_factor), so no model or
 * downstream reader changes. A legacy `.conf` user writes those PARAMS keys
 * directly, exactly as before.
 * ---------------------------------------------------------------------- */

/* forward-declared: defined with the other emit-phase helpers below. */
const char* quantity_name(quantity q);

/* Resolve a time-valued setting to nanoseconds (the unit every model uses). 
 * A bare number is already nanoseconds; a unit-bearing value must be a time and
 * converts to ns. The value must be strictly positive. `what` names the setting
 * in diagnostics. */
std::string resolve_time_ns(const char* what, const std::string& raw) {
    classified_value cv = classify_value(raw);
    switch (cv.form) {
    case value_form::bare_number:
        if (cv.number <= 0.0)
            throw config_error(std::string("config error: ") + what + " must be positive, got \"" +
                               raw + "\"");
        return raw; /* already nanoseconds */
    case value_form::with_unit:
        if (cv.kind != quantity::time)
            throw config_error(std::string("config error: ") + what +
                               " takes a time value, but \"" + raw + "\" is a " +
                               quantity_name(cv.kind) + " value");
        if (cv.canonical <= 0.0)
            throw config_error(std::string("config error: ") + what + " must be positive, got \"" +
                               raw + "\"");
        return format_number(cv.canonical); /* canonical time base is ns */
    case value_form::plain:
    case value_form::unknown_suffix:
        break;
    }
    throw config_error(std::string("config error: ") + what +
                       " must be a time value: a bare number of nanoseconds or a value with a "
                       "time unit (ns/us/ms/s), got \"" +
                       raw + "\"");
}

/* Parse the top-level `simulation:` block, validating and resolving each setting
 * into cfg.simulation (emitted into PARAMS after the topology compiles). Unknown
 * keys are rejected, like everywhere else in the front-end. */
void parse_simulation(ryml::ConstNodeRef root, friendly_config& cfg) {
    if (!has(root, "simulation"))
        return;
    ryml::ConstNodeRef sim = root["simulation"];
    if (!sim.is_map())
        throw config_error("config error: \"simulation\" must be a map of run-level settings "
                           "(end_time, pe_mem_factor)");
    for (ryml::ConstNodeRef c : sim.children()) {
        std::string k = key_of(c);
        if (!c.is_keyval())
            throw config_error("config error: simulation: \"" + k + "\" must be a scalar value");
        if (k == "end_time") {
            cfg.set_simulation("end_time", resolve_time_ns("simulation.end_time", scalar(c)));
        } else if (k == "pe_mem_factor") {
            std::string raw = scalar(c);
            long v = parse_int_strict(raw, "simulation.pe_mem_factor");
            if (v <= 0)
                throw config_error("config error: simulation.pe_mem_factor must be a positive "
                                   "integer, got \"" +
                                   raw + "\"");
            cfg.set_simulation("pe_mem_factor", std::to_string(v));
        } else {
            throw config_error("config error: simulation: unexpected key \"" + k +
                               "\" (supported: end_time, pe_mem_factor)");
        }
    }
}

/* Parse one document with our throwing error handler installed (so ryml's own
 * parse errors route through config_error, exactly like our validation errors),
 * check it is a top-level map, and hand its root to `fn`. The parser and tree
 * live for the duration of `fn`; the friendly IR copies out owned strings, so
 * nothing references the tree afterward. */
template <class F> void with_parsed_document(std::string_view text, F&& fn) {
    ryml::Callbacks cb;
    cb.set_error_basic(ryml_throw);
    cb.set_error_parse(ryml_throw_parse);
    ryml::EventHandlerTree evt_handler(cb);
    ryml::Parser parser(&evt_handler);
    ryml::Tree tree = ryml::parse_in_arena(&parser, ryml::to_csubstr("<config>"),
                                           ryml::csubstr(text.data(), text.size()));
    ryml::ConstNodeRef root = tree.rootref();
    if (!root.readable() || !root.is_map())
        throw config_error("config error: config must be a YAML/JSON mapping at the top level");
    fn(root);
}

/* reject unknown top-level keys rather than silently ignoring them. An
 * included (base) document may not itself use `include` -- nested includes are
 * not supported. */
void validate_toplevel_keys(ryml::ConstNodeRef root, bool is_base) {
    for (ryml::ConstNodeRef c : root.children()) {
        std::string k = key_of(c);
        if (k != "schema_version" && k != "components" && k != "topology" && k != "sections" &&
            k != "simulation" && k != "include")
            throw config_error("config error: unexpected top-level key \"" + k + "\"");
        if (k == "include" && is_base)
            throw config_error("config error: an included file cannot itself use \"include\" "
                               "(nested includes are not supported)");
    }
}

/* schema_version is required (on the main document), integer, and any value
 * this build doesn't know is a hard error -- a newer config can't be interpreted
 * safely. */
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

/* An included document need not restate schema_version, but if it does it must
 * agree with this build. */
void validate_schema_version_if_present(ryml::ConstNodeRef root) {
    if (has(root, "schema_version"))
        require_schema_version(root);
}

/* Clear any topology state so a later document's topology fully replaces an
 * earlier one (local-overrides-included). */
void reset_topology(friendly_config& cfg) {
    cfg.parametric = false;
    cfg.flat = false;
    cfg.explicit_groups = false;
    cfg.fab = fabric{};
    cfg.flat_component.clear();
    cfg.node_count = 0;
    cfg.explicit_lpgroups = compiled_section{"LPGROUPS", {}, {}};
    cfg.explicit_params = compiled_section{"PARAMS", {}, {}};
}

/* Merge one document into the accumulating friendly config. Components and
 * sections override by name; a topology block replaces any earlier one. */
void merge_document(ryml::ConstNodeRef root, friendly_config& cfg) {
    parse_components(root, cfg);
    if (has(root, "topology")) {
        reset_topology(cfg);
        parse_topology(root, cfg);
    }
    parse_sections(root, cfg);
    parse_simulation(root, cfg);
}

/* -------------------------------------------------------------------------
 * Compile: friendly IR -> compiled_config
 * ---------------------------------------------------------------------- */

/* PARAMS keys the compiler derives from the topology itself, rather than passing
 * through from the user. Currently just modelnet_order (computed from the fabric/
 * network model registry); the array leaves room for more without touching the
 * call sites. */
const char* const derived_params_keys[] = {"modelnet_order"};

bool is_derived_param(const std::string& key) {
    for (const char* k : derived_params_keys)
        if (key == k)
            return true;
    return false;
}

/* A model's dimensioned-param unit table, threaded from the model registry into
 * PARAMS emission so unit conversion is per-model (link_bandwidth is GiB/s in
 * torus but bytes/ns in fattree, so the same key resolves differently). */
struct model_units {
    const param_unit* tbl;
    size_t n;
};

const char* quantity_name(quantity q) {
    switch (q) {
    case quantity::time:
        return "time";
    case quantity::size:
        return "size";
    case quantity::bandwidth:
        return "bandwidth";
    }
    return "value";
}

/* Resolve one raw PARAMS value for `key` against the model's unit table:
 *
 *  - a value with a recognized unit is converted to the model's internal unit
 *    for that dimensioned key (rejecting a unit of the wrong quantity, e.g. a
 *    time on a size key, and a negative magnitude);
 *  - a bare number is emitted verbatim, so it already means the internal unit
 *    (identity for ns/bytes keys, pass-through for bandwidth, which has no safe
 *    universal default);
 *  - a non-numeric string (a name, path, enum) passes through untouched;
 *  - a unit-suffixed value on a knob the model can't classify is a trap -- the
 *    model would atof() it and silently read the bare number -- so it is
 *    rejected with a diagnostic naming the key.
 *
 * Trailing garbage after a number (an unrecognized suffix) is rejected on a
 * dimensioned key but passed through on an unclassified one (a value like a
 * "4,2,2" dim_length or a "5.dat" filename is not ours to reject). */
std::string resolve_param_value(const model_units& units, const std::string& key,
                                const std::string& raw) {
    const param_unit* pu = find_param_unit(units.tbl, units.n, key);
    classified_value cv = classify_value(raw);

    if (pu) {
        switch (cv.form) {
        case value_form::plain:
        case value_form::bare_number:
            return raw; /* verbatim: already the model's internal unit */
        case value_form::with_unit:
            if (cv.kind != pu->kind)
                throw config_error("config error: parameter \"" + key + "\" takes a " +
                                   quantity_name(pu->kind) + " value, but \"" + raw + "\" is a " +
                                   quantity_name(cv.kind) + " value");
            if (cv.canonical < 0.0)
                throw config_error("config error: parameter \"" + key + "\" value \"" + raw +
                                   "\" must not be negative");
            return format_number(cv.canonical / pu->internal_scale);
        case value_form::unknown_suffix:
            throw config_error("config error: parameter \"" + key + "\" value \"" + raw +
                               "\" has an unrecognized unit; use ns/us/ms/s (time), "
                               "B/KiB/MiB/GiB (size), or a bit/byte rate like Gbps/GiBps "
                               "(bandwidth), or write a bare number in the model's internal unit");
        }
    } else if (cv.form == value_form::with_unit) {
        throw config_error(
            "config error: parameter \"" + key +
            "\" is a pass-through model knob that is not "
            "unit-aware, so the unit-bearing value \"" +
            raw +
            "\" would be read as its bare number by the model; drop the unit and write the value "
            "in the model's internal unit, or use a recognized dimensioned parameter");
    }
    return raw;
}

/* Append a user-supplied key to PARAMS, resolving any unit-bearing values
 * against `units` (see resolve_param_value) and refusing to let it shadow a key
 * the compiler derives itself. compile_fabric/compile_flat emit the derived keys
 * (modelnet_order) first, then append the user's fabric/component params; because
 * the config store returns the FIRST match for a name, a user key of the same
 * name would land after the derived one and be silently ignored. The front-end
 * never silently drops a key, so reject the collision here with a diagnostic. (An
 * explicit-groups config derives no PARAMS at all and writes modelnet_order
 * itself, so that path builds PARAMS directly and never goes through here.) */
void add_user_param(compiled_section& params, const model_units& units, const std::string& key,
                    const std::vector<std::string>& values) {
    if (is_derived_param(key))
        throw config_error("config error: \"" + key +
                           "\" is derived by the compiler from the topology and cannot be set as a "
                           "model parameter; remove it (the compiler emits it for you)");
    std::vector<std::string> resolved;
    resolved.reserve(values.size());
    for (const std::string& v : values)
        resolved.push_back(resolve_param_value(units, key, v));
    params.add_key(key, std::move(resolved));
}

void add_user_param(compiled_section& params, const model_units& units, const std::string& key,
                    const std::string& value) {
    add_user_param(params, units, key, std::vector<std::string>{value});
}

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
    /* An empty model: would flow into LPGROUPS as an empty LP-type key and fail
     * confusingly in codes_mapping later; reject it here with a clear message. */
    if (host->model.empty())
        throw config_error("config error: hosts.component \"" + fab.hosts_component +
                           "\" has an empty \"model:\"; a component needs a model naming its "
                           "workload LP type");
    /* A parametric fabric defines the network itself, so a network: on the host
     * component is meaningless here (it only applies to flat-topology components)
     * and would otherwise be silently ignored. */
    if (!host->network.empty())
        throw config_error("config error: hosts.component \"" + fab.hosts_component +
                           "\" sets \"network:\", which is only meaningful for a flat-topology "
                           "component; a parametric fabric defines the network itself");

    layout lay = model->derive(fab.shape);

    /* A backstop over every model's derivation: a shape that produces a
     * degenerate layout (e.g. num_groups: 0) must not reach codes_mapping. Each
     * repetition needs at least one terminal; routers may legitimately be 0 for
     * mesh-style fabrics that fold routing into the terminal. */
    if (lay.repetitions <= 0 || lay.terminals_per_rep <= 0 || lay.routers_per_rep < 0)
        throw config_error(
            "config error: fabric model \"" + fab.model +
            "\" derived a degenerate layout (repetitions=" + std::to_string(lay.repetitions) +
            ", terminals_per_rep=" + std::to_string(lay.terminals_per_rep) + ", routers_per_rep=" +
            std::to_string(lay.routers_per_rep) + "); check the shape values");

    /* --- LPGROUPS: one group of `repetitions` slices, each with the
     * per-terminal workload + NIC LPs and the router/switch LPs, emitted in
     * [workload, terminal, router] order to match the layout the model
     * expects. --- */
    compiled_section& grp = out.add_section("LPGROUPS").add_subsection("MODELNET_GRP");
    grp.add_key("repetitions", std::to_string(lay.repetitions));
    grp.add_key(host->model, std::to_string(lay.terminals_per_rep));
    grp.add_key(model->terminal_lp, std::to_string(lay.terminals_per_rep));
    /* Mesh-style fabrics (torus) fold routing into the terminal node and have no
     * separate router LP; skip the router line for them. */
    if (model->router_lp)
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

    /* dimensioned params are resolved against this model's unit table (see
     * resolve_param_value): a unit-bearing value converts to the model's internal
     * unit, a bare number passes through. */
    const model_units units{model->units, model->n_units};

    /* shape parameters pass straight through (num_routers etc.). */
    for (const auto& kv : fab.shape)
        add_user_param(params, units, kv.first, kv.second);

    /* per-link-class params become <class>_<param> (local_bandwidth, ...). */
    for (const link_class& cls : fab.links)
        for (const auto& kv : cls.params)
            add_user_param(params, units, cls.name + "_" + kv.first, kv.second);

    /* routing.algorithm -> "routing"; any other routing.* passes through. */
    for (const auto& kv : fab.routing)
        add_user_param(params, units, kv.first == "algorithm" ? std::string("routing") : kv.first,
                       kv.second);

    /* connections.{intra,inter} -> the file-enumerated model's connection-file
     * keys; paths pass through verbatim (the model reads them relative to the
     * working directory). */
    for (const auto& kv : fab.connections) {
        if (kv.first == "intra")
            add_user_param(params, units, "intra-group-connections", kv.second);
        else if (kv.first == "inter")
            add_user_param(params, units, "inter-group-connections", kv.second);
        else
            add_user_param(params, units, kv.first, kv.second);
    }

    /* remaining scalar fabric keys (packet_size, chunk_size, parity pass-through
     * knobs) map to PARAMS verbatim. */
    for (const auto& kv : fab.extra)
        add_user_param(params, units, kv.first, kv.second);

    /* list-valued fabric keys (slimfly's generator_set_X / _X_prime) emit as
     * multi-value PARAMS, e.g. generator_set_X=("1","4"). */
    for (const auto& kv : fab.extra_lists)
        add_user_param(params, units, kv.first, kv.second);

    /* the workload component's own params (if any) also land in PARAMS. */
    for (const auto& kv : host->params)
        add_user_param(params, units, kv.first, kv.second);
}

/* Compile a flat (enumerated) network into LPGROUPS + PARAMS: `node_count`
 * peer compute nodes, each one repetition running the component's workload LP
 * over its NIC LP. simplep2p's link table stays referenced by path in the
 * component params; the friendly form supplies only the node count. */
void compile_flat(const friendly_config& cfg, compiled_config& out) {
    const component* comp = cfg.find_component(cfg.flat_component);
    if (!comp)
        throw config_error("config error: topology.component \"" + cfg.flat_component +
                           "\" is not defined under components:");
    /* An empty model: would flow into LPGROUPS as an empty LP-type key and fail
     * confusingly in codes_mapping later; reject it here with a clear message. */
    if (comp->model.empty())
        throw config_error("config error: topology.component \"" + cfg.flat_component +
                           "\" has an empty \"model:\"; a component needs a model naming its "
                           "workload LP type");
    if (comp->network.empty())
        throw config_error("config error: component \"" + cfg.flat_component +
                           "\" needs a network: field naming its NIC model");

    const network_model* net = find_network_model(comp->network);
    if (!net)
        throw config_error("config error: unknown network model \"" + comp->network + "\"");

    /* --- LPGROUPS: one repetition per node, each a workload LP + its NIC LP,
     * emitted in [workload, NIC] order to match the model's layout. --- */
    compiled_section& grp = out.add_section("LPGROUPS").add_subsection("MODELNET_GRP");
    grp.add_key("repetitions", std::to_string(cfg.node_count));
    grp.add_key(comp->model, "1");
    grp.add_key(net->nic_lp, "1");

    /* --- PARAMS: modelnet_order from the network model; the component's params
     * (message_size, packet_size, simplep2p's matrix-file references, ...) pass
     * straight through. --- */
    compiled_section& params = out.add_section("PARAMS");
    params.add_key("modelnet_order", std::vector<std::string>{net->method});
    const model_units units{net->units, net->n_units};
    for (const auto& kv : comp->params)
        add_user_param(params, units, kv.first, kv.second);
}

/* Append the resolved `simulation:` settings to PARAMS -- the section
 * codes_mapping reads (PARAMS/end_time, PARAMS/pe_mem_factor). A model parameter
 * of the same name (set on a component or fabric, or in an explicit-groups
 * params: block) lands earlier in PARAMS and would win the config store's
 * first-match lookup, silently shadowing the simulation setting; the front-end
 * never drops a key silently, so the collision is rejected instead. Setting the
 * key in only one place resolves it (a pass-through param alone, or the
 * simulation: block alone, are both fine). */
void apply_simulation(const friendly_config& cfg, compiled_config& out) {
    if (cfg.simulation.empty())
        return;
    compiled_section* params = nullptr;
    for (compiled_section& s : out.sections)
        if (s.name == "PARAMS") {
            params = &s;
            break;
        }
    /* every topology form emits a PARAMS section, so this is defensive. */
    if (!params)
        params = &out.add_section("PARAMS");
    for (const auto& kv : cfg.simulation) {
        for (const compiled_key& existing : params->keys)
            if (existing.name == kv.first)
                throw config_error("config error: \"" + kv.first +
                                   "\" is set both in the simulation: block and as a model "
                                   "parameter; set it in only one place");
        params->add_key(kv.first, kv.second);
    }
}

} // namespace

std::vector<std::string> parse_includes(std::string_view doc) {
    std::vector<std::string> out;
    with_parsed_document(doc, [&](ryml::ConstNodeRef root) {
        if (!has(root, "include"))
            return;
        ryml::ConstNodeRef inc = root["include"];
        if (inc.is_seq()) {
            for (ryml::ConstNodeRef c : inc.children()) {
                if (!c.has_val())
                    throw config_error("config error: \"include\" list must contain filenames");
                out.push_back(scalar(c));
            }
        } else if (inc.has_val()) {
            out.push_back(scalar(inc));
        } else {
            throw config_error(
                "config error: \"include\" must be a filename or a list of filenames");
        }
    });
    return out;
}

compiled_config compile(std::string_view main_doc, const std::vector<std::string>& base_docs) {
    friendly_config cfg;

    /* Included documents are the base; the main document overrides them. Merge
     * the bases first, in listed order, then the main document last. */
    for (const std::string& doc : base_docs)
        with_parsed_document(doc, [&](ryml::ConstNodeRef root) {
            validate_toplevel_keys(root, /*is_base=*/true);
            validate_schema_version_if_present(root);
            merge_document(root, cfg);
        });
    with_parsed_document(main_doc, [&](ryml::ConstNodeRef root) {
        validate_toplevel_keys(root, /*is_base=*/false);
        require_schema_version(root);
        merge_document(root, cfg);
    });

    if (!cfg.parametric && !cfg.flat && !cfg.explicit_groups)
        throw config_error("config error: missing required \"topology\" block");

    compiled_config out;
    if (cfg.explicit_groups) {
        /* explicit form: LPGROUPS and PARAMS were built verbatim during parse. */
        out.sections.push_back(std::move(cfg.explicit_lpgroups));
        out.sections.push_back(std::move(cfg.explicit_params));
    } else if (cfg.parametric)
        compile_fabric(cfg, out);
    else if (cfg.flat)
        compile_flat(cfg, out);

    /* run-level `simulation:` settings are appended to the PARAMS just emitted. */
    apply_simulation(cfg, out);

    /* verbatim `sections:` blocks follow the compiler-derived topology sections. */
    for (compiled_section& s : cfg.passthrough)
        out.sections.push_back(std::move(s));
    return out;
}

} // namespace config
} // namespace codes
