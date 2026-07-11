// Unit tests for the YAML config compiler core (codes::config::compile).
//
// The core is ROSS-free by design (see config_compiler.h), so it compiles
// straight into this test alongside ryml -- no codes/ROSS/MPI link -- and the
// tests assert on the returned compiled_config plain data rather than on any
// simulator behavior. This is the first real unit-test consumer of the vendored
// GoogleTest framework; the model-net equivalence tests cover the emit path.
#include "config_compiler.h"
#include "unit_convert.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

using codes::config::classified_value;
using codes::config::classify_value;
using codes::config::compile;
using codes::config::compiled_config;
using codes::config::compiled_key;
using codes::config::compiled_section;
using codes::config::config_error;
using codes::config::format_number;
using codes::config::parse_includes;
using codes::config::quantity;
using codes::config::value_form;

namespace {

const compiled_section* find_section(const compiled_config& c, const char* name) {
    for (const compiled_section& s : c.sections)
        if (s.name == name)
            return &s;
    return nullptr;
}

const compiled_section* find_subsection(const compiled_section& s, const char* name) {
    for (const compiled_section& sub : s.subsections)
        if (sub.name == name)
            return &sub;
    return nullptr;
}

const compiled_key* find_key(const compiled_section& s, const char* name) {
    for (const compiled_key& k : s.keys)
        if (k.name == name)
            return &k;
    return nullptr;
}

// A minimal, valid flat-network config; tests append a `sections:` block to it.
const char* kFlatBase = R"(
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
const char* kParametricDragonfly = R"(
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
const char* kGroupsBase = R"(
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

} // namespace

// --- topology sanity (the compiler still does its normal job) ---------------

TEST(ConfigCompiler, FlatTopologyEmitsLpgroupsAndParams) {
    compiled_config c = compile(kFlatBase);
    const compiled_section* lpg = find_section(c, "LPGROUPS");
    ASSERT_NE(lpg, nullptr);
    const compiled_section* grp = find_subsection(*lpg, "MODELNET_GRP");
    ASSERT_NE(grp, nullptr);
    const compiled_key* reps = find_key(*grp, "repetitions");
    ASSERT_NE(reps, nullptr);
    EXPECT_EQ(reps->values.at(0), "4");
    ASSERT_NE(find_section(c, "PARAMS"), nullptr);
}

TEST(ConfigCompiler, RejectsUnknownTopLevelKey) {
    EXPECT_THROW(compile(std::string(kFlatBase) + "bogus: 1\n"), config_error);
}

// --- schema_version ----------------------------------------------------------

TEST(ConfigCompiler, RejectsMissingSchemaVersion) {
    EXPECT_THROW(compile(R"(
components:
  cn: { model: nw-lp, network: simplenet }
topology: { format: flat, component: cn, nodes: 4 }
)"),
                 config_error);
}

TEST(ConfigCompiler, RejectsUnsupportedSchemaVersion) {
    // any version this build doesn't know is a hard error, not a best-effort
    // guess -- a newer config can't be interpreted safely.
    EXPECT_THROW(compile(R"(
schema_version: 2
components:
  cn: { model: nw-lp, network: simplenet }
topology: { format: flat, component: cn, nodes: 4 }
)"),
                 config_error);
}

TEST(ConfigCompiler, RejectsNonIntegerSchemaVersion) {
    EXPECT_THROW(compile(R"(
schema_version: abc
components:
  cn: { model: nw-lp, network: simplenet }
topology: { format: flat, component: cn, nodes: 4 }
)"),
                 config_error);
}

// --- malformed / non-map input -----------------------------------------------

TEST(ConfigCompiler, MalformedYamlThrowsConfigError) {
    // the core must throw on a syntax error, never print-and-abort (ryml's
    // default); the extern "C" shim owns the translation to tw_error.
    EXPECT_THROW(compile("topology: [unterminated\n"), config_error);
}

TEST(ConfigCompiler, NonMapTopLevelRejected) {
    EXPECT_THROW(compile("- a\n- b\n"), config_error);      // sequence root
    EXPECT_THROW(compile("just a scalar\n"), config_error); // scalar root
}

// --- flat topology validation -------------------------------------------------

TEST(ConfigCompiler, FlatRejectsUnexpectedTopologyKey) {
    // flat models are uniform all-to-all: an edges/graph block is not part of a
    // flat topology, and an unexpected key is an error, not a silent drop.
    EXPECT_THROW(compile(R"(
schema_version: 1
components:
  cn: { model: nw-lp, network: simplenet }
topology:
  format: flat
  component: cn
  nodes: 4
  edges: { cn: [cn] }
)"),
                 config_error);
}

TEST(ConfigCompiler, FlatRejectsMalformedNodeCount) {
    // A helper that swaps in a given nodes value and compiles a flat config.
    auto with_nodes = [](const char* nodes) {
        return std::string(R"(
schema_version: 1
components:
  cn: { model: nw-lp, network: simplenet }
topology:
  format: flat
  component: cn
  nodes: )") + nodes +
               "\n";
    };
    EXPECT_THROW(compile(with_nodes("abc")), config_error); // non-integer
    EXPECT_THROW(compile(with_nodes("0")), config_error);   // not positive
    EXPECT_THROW(compile(with_nodes("-3")), config_error);  // negative
}

TEST(ConfigCompiler, FlatRejectsMissingNodes) {
    EXPECT_THROW(compile(R"(
schema_version: 1
components:
  cn: { model: nw-lp, network: simplenet }
topology: { format: flat, component: cn }
)"),
                 config_error);
}

TEST(ConfigCompiler, FlatRejectsMissingComponent) {
    EXPECT_THROW(compile(R"(
schema_version: 1
components:
  cn: { model: nw-lp, network: simplenet }
topology: { format: flat, nodes: 4 }
)"),
                 config_error);
}

TEST(ConfigCompiler, FlatRejectsUndefinedComponent) {
    // the topology references a component never defined under components:.
    EXPECT_THROW(compile(R"(
schema_version: 1
components:
  cn: { model: nw-lp, network: simplenet }
topology: { format: flat, component: bogus, nodes: 4 }
)"),
                 config_error);
}

TEST(ConfigCompiler, FlatRejectsEmptyComponentModel) {
    EXPECT_THROW(compile(R"(
schema_version: 1
components:
  cn: { model: "", network: simplenet }
topology: { format: flat, component: cn, nodes: 4 }
)"),
                 config_error);
}

TEST(ConfigCompiler, FlatRejectsMissingNetwork) {
    // a flat component must name the NIC model its workload runs over.
    EXPECT_THROW(compile(R"(
schema_version: 1
components:
  cn: { model: nw-lp }
topology: { format: flat, component: cn, nodes: 4 }
)"),
                 config_error);
}

TEST(ConfigCompiler, FlatRejectsUnknownNetworkModel) {
    EXPECT_THROW(compile(R"(
schema_version: 1
components:
  cn: { model: nw-lp, network: infiniband }
topology: { format: flat, component: cn, nodes: 4 }
)"),
                 config_error);
}

// --- pass-through `sections:` -----------------------------------------------

TEST(ConfigCompiler, PassthroughSectionScalarsListsAndNesting) {
    std::string yaml = std::string(kFlatBase) + R"(
sections:
  director:
    start_iter: 100
    fixed_switch_timestamps: [25.0e6, 400.0e6]
    nested:
      foo: bar
)";
    compiled_config c = compile(yaml);

    const compiled_section* dir = find_section(c, "director");
    ASSERT_NE(dir, nullptr) << "pass-through section should appear verbatim";

    const compiled_key* start = find_key(*dir, "start_iter");
    ASSERT_NE(start, nullptr);
    EXPECT_EQ(start->values.at(0), "100");

    const compiled_key* ts = find_key(*dir, "fixed_switch_timestamps");
    ASSERT_NE(ts, nullptr);
    ASSERT_EQ(ts->values.size(), 2u) << "a YAML sequence becomes a multi-value key";
    EXPECT_EQ(ts->values.at(0), "25.0e6");
    EXPECT_EQ(ts->values.at(1), "400.0e6");

    const compiled_section* nested = find_subsection(*dir, "nested");
    ASSERT_NE(nested, nullptr) << "a nested map becomes a subsection";
    const compiled_key* foo = find_key(*nested, "foo");
    ASSERT_NE(foo, nullptr);
    EXPECT_EQ(foo->values.at(0), "bar");
}

TEST(ConfigCompiler, PassthroughPreservesSectionNameCase) {
    // The compiler emits the name as written; case-insensitive matching happens
    // later at lookup time, so it must not normalize the case here.
    std::string yaml = std::string(kFlatBase) + R"(
sections:
  NetworkSurrogate:
    enable: 1
)";
    compiled_config c = compile(yaml);
    EXPECT_NE(find_section(c, "NetworkSurrogate"), nullptr);
    EXPECT_EQ(find_section(c, "networksurrogate"), nullptr);
}

TEST(ConfigCompiler, PassthroughSectionsFollowTopology) {
    std::string yaml = std::string(kFlatBase) + R"(
sections:
  director:
    start_iter: 1
)";
    compiled_config c = compile(yaml);
    ASSERT_GE(c.sections.size(), 3u); // LPGROUPS, PARAMS, then director
    EXPECT_EQ(c.sections.back().name, "director");
}

// --- reserved names ---------------------------------------------------------

TEST(ConfigCompiler, RejectsReservedSectionNames) {
    // LPGROUPS / PARAMS are compiler-owned (emitted from the topology), matched
    // case-insensitively.
    EXPECT_THROW(compile(std::string(kFlatBase) + "sections:\n  PARAMS:\n    x: 1\n"),
                 config_error);
    EXPECT_THROW(compile(std::string(kFlatBase) + "sections:\n  lpgroups:\n    x: 1\n"),
                 config_error);
}

// --- optional open schema (required keys enforced, extras allowed) ----------

TEST(ConfigCompiler, RegisteredSchemaAcceptsRequiredKeyPlusExtras) {
    std::string yaml = std::string(kFlatBase) + R"(
sections:
  resource:
    available: 8192
    experimental_knob: 7
)";
    compiled_config c = compile(yaml);
    const compiled_section* res = find_section(c, "resource");
    ASSERT_NE(res, nullptr);
    EXPECT_NE(find_key(*res, "available"), nullptr);
    EXPECT_NE(find_key(*res, "experimental_knob"), nullptr)
        << "an open schema still passes through keys it doesn't list";
}

TEST(ConfigCompiler, RegisteredSchemaRejectsMissingRequiredKey) {
    std::string yaml = std::string(kFlatBase) + R"(
sections:
  resource:
    not_available: 1
)";
    EXPECT_THROW(compile(yaml), config_error);
}

// An unregistered section is passed through with no validation at all.
TEST(ConfigCompiler, UnregisteredSectionIsUnvalidated) {
    std::string yaml = std::string(kFlatBase) + R"(
sections:
  my_new_feature:
    anything_goes: 1
)";
    compiled_config c = compile(yaml);
    EXPECT_NE(find_section(c, "my_new_feature"), nullptr);
}

// --- explicit LP-groups form (`format: groups`) -----------------------------

TEST(ConfigCompiler, ExplicitGroupsEmitsLpgroupsAndParams) {
    compiled_config c = compile(kGroupsBase);

    const compiled_section* lpg = find_section(c, "LPGROUPS");
    ASSERT_NE(lpg, nullptr);

    const compiled_section* g1 = find_subsection(*lpg, "GRP1");
    ASSERT_NE(g1, nullptr);
    ASSERT_NE(find_key(*g1, "repetitions"), nullptr);
    EXPECT_EQ(find_key(*g1, "repetitions")->values.at(0), "2");
    ASSERT_NE(find_key(*g1, "b"), nullptr);
    EXPECT_EQ(find_key(*g1, "b")->values.at(0), "2");

    const compiled_section* g2 = find_subsection(*lpg, "GRP2");
    ASSERT_NE(g2, nullptr);
    EXPECT_EQ(find_key(*g2, "repetitions")->values.at(0), "3");

    const compiled_section* params = find_section(c, "PARAMS");
    ASSERT_NE(params, nullptr);
    ASSERT_NE(find_key(*params, "message_size"), nullptr);
    EXPECT_EQ(find_key(*params, "message_size")->values.at(0), "256");
}

TEST(ConfigCompiler, ExplicitGroupsCarriesAnnotationVerbatim) {
    compiled_config c = compile(kGroupsBase);
    const compiled_section* g1 = find_subsection(*find_section(c, "LPGROUPS"), "GRP1");
    ASSERT_NE(g1, nullptr);
    // the annotated LP type keeps its `type@annotation` spelling (codes_mapping
    // splits on '@') and is distinct from the un-annotated `a`.
    const compiled_key* annotated = find_key(*g1, "a@foo");
    ASSERT_NE(annotated, nullptr);
    EXPECT_EQ(annotated->values.at(0), "1");
    EXPECT_NE(find_key(*g1, "a"), nullptr);
}

TEST(ConfigCompiler, ExplicitGroupsComposeWithPassthroughSections) {
    std::string yaml = std::string(kGroupsBase) + R"(
sections:
  lsm:
    request_sizes: [0]
    write_rates: [12000.0]
)";
    compiled_config c = compile(yaml);
    EXPECT_NE(find_section(c, "LPGROUPS"), nullptr);
    const compiled_section* lsm = find_section(c, "lsm");
    ASSERT_NE(lsm, nullptr);
    EXPECT_NE(find_key(*lsm, "request_sizes"), nullptr);
}

TEST(ConfigCompiler, ExplicitGroupsRejectsMissingRepetitions) {
    EXPECT_THROW(compile(R"(
schema_version: 1
topology:
  format: groups
  groups:
    G: { lps: { a: 1 } }
)"),
                 config_error);
}

TEST(ConfigCompiler, ExplicitGroupsRejectsNonPositiveCount) {
    EXPECT_THROW(compile(R"(
schema_version: 1
topology:
  format: groups
  groups:
    G: { repetitions: 1, lps: { a: 0 } }
)"),
                 config_error);
}

TEST(ConfigCompiler, ExplicitGroupsRejectsUnknownGroupKey) {
    EXPECT_THROW(compile(R"(
schema_version: 1
topology:
  format: groups
  groups:
    G: { repetitions: 1, lps: { a: 1 }, bogus: 2 }
)"),
                 config_error);
}

// --- parametric fabrics -----------------------------------------------------

TEST(ConfigCompiler, ParametricDragonflyDerivesLayoutAndOrder) {
    compiled_config c = compile(kParametricDragonfly);

    const compiled_section* lpg = find_section(c, "LPGROUPS");
    ASSERT_NE(lpg, nullptr);
    const compiled_section* grp = find_subsection(*lpg, "MODELNET_GRP");
    ASSERT_NE(grp, nullptr);

    // num_routers: 4 -> num_cn 2, num_groups 9, repetitions 9*4 = 36.
    ASSERT_NE(find_key(*grp, "repetitions"), nullptr);
    EXPECT_EQ(find_key(*grp, "repetitions")->values.at(0), "36");
    // [workload, terminal, router] LPs: 2 terminals + 1 router per repetition.
    ASSERT_NE(find_key(*grp, "nw-lp"), nullptr);
    EXPECT_EQ(find_key(*grp, "nw-lp")->values.at(0), "2");
    ASSERT_NE(find_key(*grp, "modelnet_dragonfly"), nullptr);
    EXPECT_EQ(find_key(*grp, "modelnet_dragonfly")->values.at(0), "2");
    ASSERT_NE(find_key(*grp, "modelnet_dragonfly_router"), nullptr);
    EXPECT_EQ(find_key(*grp, "modelnet_dragonfly_router")->values.at(0), "1");

    const compiled_section* params = find_section(c, "PARAMS");
    ASSERT_NE(params, nullptr);
    const compiled_key* order = find_key(*params, "modelnet_order");
    ASSERT_NE(order, nullptr);
    ASSERT_EQ(order->values.size(), 2u) << "terminal + router are distinct model-net methods";
    EXPECT_EQ(order->values.at(0), "dragonfly");
    EXPECT_EQ(order->values.at(1), "dragonfly_router");
    // shape values pass straight through to PARAMS.
    ASSERT_NE(find_key(*params, "num_routers"), nullptr);
    EXPECT_EQ(find_key(*params, "num_routers")->values.at(0), "4");
}

TEST(ConfigCompiler, ParametricRejectsMissingShapeKey) {
    // dragonfly-dally needs num_groups etc.; drop a required shape key.
    EXPECT_THROW(compile(R"(
schema_version: 1
components:
  compute_host: { model: nw-lp }
topology:
  format: parametric
  fabric:
    model: dragonfly-dally
    shape:
      num_routers: 4
      num_cns_per_router: 2
  hosts:
    component: compute_host
)"),
                 config_error);
}

TEST(ConfigCompiler, ParametricRejectsNonIntegerShapeValue) {
    // num_routers: abc previously parsed as 0; it is now a diagnostic.
    EXPECT_THROW(compile(R"(
schema_version: 1
components:
  compute_host: { model: nw-lp }
topology:
  format: parametric
  fabric:
    model: dragonfly
    shape:
      num_routers: abc
  hosts:
    component: compute_host
)"),
                 config_error);
}

TEST(ConfigCompiler, ParametricRejectsDegenerateDerivedLayout) {
    // num_groups: 0 -> repetitions 0; the backstop rejects it before codes_mapping.
    EXPECT_THROW(compile(R"(
schema_version: 1
components:
  compute_host: { model: nw-lp }
topology:
  format: parametric
  fabric:
    model: dragonfly-dally
    shape:
      num_routers: 4
      num_groups: 0
      num_cns_per_router: 2
      num_global_channels: 2
  hosts:
    component: compute_host
)"),
                 config_error);
}

// dally/custom require num_global_channels even though it plays no part in the
// LP counts: the model would otherwise default it to 10 with only a warning,
// silently contradicting the binary wiring files. Each test first compiles the
// same config WITH the key, so the throw can only be about the dropped key.

TEST(ConfigCompiler, DallyShapeRequiresNumGlobalChannels) {
    const std::string head = R"(
schema_version: 1
components:
  compute_host: { model: nw-lp }
topology:
  format: parametric
  fabric:
    model: dragonfly-dally
    shape:
      num_routers: 4
      num_groups: 9
      num_cns_per_router: 2
)";
    const std::string tail = R"(  hosts:
    component: compute_host
)";
    EXPECT_NO_THROW(compile(head + "      num_global_channels: 2\n" + tail));
    EXPECT_THROW(compile(head + tail), config_error);
}

TEST(ConfigCompiler, CustomShapeRequiresNumGlobalChannels) {
    const std::string head = R"(
schema_version: 1
components:
  compute_host: { model: nw-lp }
topology:
  format: parametric
  fabric:
    model: dragonfly-custom
    shape:
      num_router_rows: 6
      num_router_cols: 16
      num_groups: 8
      num_cns_per_router: 4
)";
    const std::string tail = R"(  hosts:
    component: compute_host
)";
    EXPECT_NO_THROW(compile(head + "      num_global_channels: 4\n" + tail));
    EXPECT_THROW(compile(head + tail), config_error);
}

TEST(ConfigCompiler, ParametricRejectsEmptyComponentModel) {
    EXPECT_THROW(compile(R"(
schema_version: 1
components:
  compute_host: { model: "" }
topology:
  format: parametric
  fabric:
    model: dragonfly
    shape:
      num_routers: 4
  hosts:
    component: compute_host
)"),
                 config_error);
}

TEST(ConfigCompiler, ParametricRejectsExtraHostsKey) {
    EXPECT_THROW(compile(R"(
schema_version: 1
components:
  compute_host: { model: nw-lp }
topology:
  format: parametric
  fabric:
    model: dragonfly
    shape:
      num_routers: 4
  hosts:
    component: compute_host
    bogus: 1
)"),
                 config_error);
}

TEST(ConfigCompiler, ComponentTypeKeyRejected) {
    // `type:` is reserved for a future schema version, not silently dropped nor
    // (worse) folded into PARAMS as a model param.
    EXPECT_THROW(compile(R"(
schema_version: 1
components:
  compute_host: { model: nw-lp, type: router }
topology:
  format: parametric
  fabric:
    model: dragonfly
    shape:
      num_routers: 4
  hosts:
    component: compute_host
)"),
                 config_error);
}

TEST(ConfigCompiler, ParametricRejectsNetworkOnHostComponent) {
    // network: is a flat-topology concept; the fabric defines the network itself.
    EXPECT_THROW(compile(R"(
schema_version: 1
components:
  compute_host: { model: nw-lp, network: simplenet }
topology:
  format: parametric
  fabric:
    model: dragonfly
    shape:
      num_routers: 4
  hosts:
    component: compute_host
)"),
                 config_error);
}

TEST(ConfigCompiler, FattreeRejectsOddSwitchRadix) {
    EXPECT_THROW(compile(R"(
schema_version: 1
components:
  compute_host: { model: nw-lp }
topology:
  format: parametric
  fabric:
    model: fattree
    shape:
      num_levels: 3
      switch_count: 32
      switch_radix: 7
  hosts:
    component: compute_host
)"),
                 config_error);
}

TEST(ConfigCompiler, TorusRejectsMalformedDimLength) {
    // A helper that swaps in a given dim_length value and compiles a torus.
    auto with_dim_length = [](const char* dim) {
        return std::string(R"(
schema_version: 1
components:
  compute_host: { model: nw-lp }
topology:
  format: parametric
  fabric:
    model: torus
    shape:
      n_dims: 3
      dim_length: ")") +
               dim + R"("
  hosts:
    component: compute_host
)";
    };
    EXPECT_THROW(compile(with_dim_length("4,,2")), config_error);  // empty segment
    EXPECT_THROW(compile(with_dim_length("abc")), config_error);   // non-integer
    EXPECT_THROW(compile(with_dim_length("4,2,")), config_error);  // trailing comma
    EXPECT_THROW(compile(with_dim_length("4 2 2")), config_error); // space-separated
}

TEST(ConfigCompiler, TorusRejectsNdimsDimLengthMismatch) {
    // n_dims 3 but only two dim_length entries.
    EXPECT_THROW(compile(R"(
schema_version: 1
components:
  compute_host: { model: nw-lp }
topology:
  format: parametric
  fabric:
    model: torus
    shape:
      n_dims: 3
      dim_length: "4,2"
  hosts:
    component: compute_host
)"),
                 config_error);
}

TEST(ConfigCompiler, TorusHappyPathMatchingNdims) {
    // n_dims 3 with three entries: node count = 4*2*2 = 16, no separate router LP.
    compiled_config c = compile(R"(
schema_version: 1
components:
  compute_host: { model: nw-lp }
topology:
  format: parametric
  fabric:
    model: torus
    shape:
      n_dims: 3
      dim_length: "4,2,2"
  hosts:
    component: compute_host
)");
    const compiled_section* grp = find_subsection(*find_section(c, "LPGROUPS"), "MODELNET_GRP");
    ASSERT_NE(grp, nullptr);
    EXPECT_EQ(find_key(*grp, "repetitions")->values.at(0), "16");
    // torus folds routing into the terminal node -> no router LP line.
    EXPECT_EQ(find_key(*grp, "modelnet_torus_router"), nullptr);
}

// --- compiler-derived PARAMS keys can't be shadowed by user params ----------
//
// The compiler emits modelnet_order (derived from the model) as the first PARAMS
// key, then appends the user's fabric/component params. The config store returns
// the first match for a name, so a user param of the same name would land after
// the derived one and be silently ignored -- the front-end must reject that
// instead of silently dropping the user's value.

TEST(ConfigCompiler, FlatRejectsUserModelnetOrderParam) {
    // reachable via a flat component's pass-through params.
    EXPECT_THROW(compile(R"(
schema_version: 1
components:
  cn:
    model: nw-lp
    network: simplenet
    modelnet_order: bogus
topology: { format: flat, component: cn, nodes: 4 }
)"),
                 config_error);
}

TEST(ConfigCompiler, ParametricRejectsUserModelnetOrderFabricKey) {
    // reachable via a fabric's pass-through scalar keys.
    EXPECT_THROW(compile(R"(
schema_version: 1
components:
  compute_host: { model: nw-lp }
topology:
  format: parametric
  fabric:
    model: dragonfly
    shape:
      num_routers: 4
    modelnet_order: bogus
  hosts:
    component: compute_host
)"),
                 config_error);
}

TEST(ConfigCompiler, ParametricRejectsUserModelnetOrderHostParam) {
    // reachable via the host component's own params (appended to PARAMS).
    EXPECT_THROW(compile(R"(
schema_version: 1
components:
  compute_host:
    model: nw-lp
    modelnet_order: bogus
topology:
  format: parametric
  fabric:
    model: dragonfly
    shape:
      num_routers: 4
  hosts:
    component: compute_host
)"),
                 config_error);
}

TEST(ConfigCompiler, DerivedKeyGuardIsExactMatchNotPrefix) {
    // a nearby, legitimate key that merely shares a prefix is accepted -- the
    // guard is an exact-name match, not a prefix ban.
    compiled_config c = compile(R"(
schema_version: 1
components:
  cn:
    model: nw-lp
    network: simplenet
    modelnet_scheduler: fcfs
topology: { format: flat, component: cn, nodes: 4 }
)");
    const compiled_section* params = find_section(c, "PARAMS");
    ASSERT_NE(params, nullptr);
    EXPECT_NE(find_key(*params, "modelnet_scheduler"), nullptr);
}

TEST(ConfigCompiler, ExplicitGroupsAllowsUserModelnetOrder) {
    // the explicit-groups form derives no PARAMS at all -- the user lays out
    // everything, including modelnet_order -- so it is NOT guarded there.
    compiled_config c = compile(R"(
schema_version: 1
topology:
  format: groups
  params:
    modelnet_order: [dragonfly, dragonfly_router]
  groups:
    G: { repetitions: 1, lps: { a: 1 } }
)");
    const compiled_section* params = find_section(c, "PARAMS");
    ASSERT_NE(params, nullptr);
    const compiled_key* order = find_key(*params, "modelnet_order");
    ASSERT_NE(order, nullptr);
    ASSERT_EQ(order->values.size(), 2u);
    EXPECT_EQ(order->values.at(0), "dragonfly");
}

// --- includes / multi-document merge ----------------------------------------

TEST(ConfigCompiler, IncludeMergesComponentsFromBaseDoc) {
    // the base defines the component; the main references it in its topology.
    std::string base = R"(
schema_version: 1
components:
  cn: { model: nw-lp, network: simplenet, message_size: 464 }
)";
    std::string main = R"(
schema_version: 1
topology: { format: flat, component: cn, nodes: 4 }
)";
    compiled_config c = compile(main, {base});
    const compiled_section* grp = find_subsection(*find_section(c, "LPGROUPS"), "MODELNET_GRP");
    ASSERT_NE(grp, nullptr);
    EXPECT_NE(find_key(*grp, "modelnet_simplenet"), nullptr); // component from base resolved
}

TEST(ConfigCompiler, IncludeLocalOverridesBaseComponent) {
    std::string base = R"(
schema_version: 1
components:
  cn: { model: nw-lp, network: simplenet, message_size: 111 }
)";
    std::string main = R"(
schema_version: 1
components:
  cn: { model: nw-lp, network: simplenet, message_size: 999 }
topology: { format: flat, component: cn, nodes: 2 }
)";
    compiled_config c = compile(main, {base});
    const compiled_section* params = find_section(c, "PARAMS");
    ASSERT_NE(params, nullptr);
    ASSERT_NE(find_key(*params, "message_size"), nullptr);
    EXPECT_EQ(find_key(*params, "message_size")->values.at(0), "999"); // local wins
}

TEST(ConfigCompiler, IncludeMergesSectionsAndTopologyFromBase) {
    // "same network, vary the rest": the base provides topology; main adds a section.
    std::string base = kFlatBase;
    std::string main = R"(
schema_version: 1
sections:
  director: { start_iter: 1 }
)";
    compiled_config c = compile(main, {base});
    EXPECT_NE(find_section(c, "LPGROUPS"), nullptr); // topology from base
    EXPECT_NE(find_section(c, "director"), nullptr); // section from main
}

TEST(ConfigCompiler, IncludedDocRejectsDisagreeingSchemaVersion) {
    // an included file need not restate schema_version, but if it does it must
    // agree with this build.
    std::string base = R"(
schema_version: 2
components:
  cn: { model: nw-lp, network: simplenet }
)";
    std::string main = R"(
schema_version: 1
topology: { format: flat, component: cn, nodes: 2 }
)";
    EXPECT_THROW(compile(main, {base}), config_error);
}

TEST(ConfigCompiler, NestedIncludeRejected) {
    std::string base = R"(
schema_version: 1
include: [other.yaml]
components:
  cn: { model: nw-lp, network: simplenet }
)";
    std::string main = R"(
schema_version: 1
topology: { format: flat, component: cn, nodes: 2 }
)";
    EXPECT_THROW(compile(main, {base}), config_error);
}

TEST(ConfigCompiler, ParseIncludesExtractsList) {
    EXPECT_TRUE(parse_includes("schema_version: 1\ntopology: {}\n").empty());

    std::vector<std::string> one = parse_includes("include: common.yaml\nschema_version: 1\n");
    ASSERT_EQ(one.size(), 1u);
    EXPECT_EQ(one.at(0), "common.yaml");

    std::vector<std::string> many =
        parse_includes("include: [a.yaml, b.yaml]\nschema_version: 1\n");
    ASSERT_EQ(many.size(), 2u);
    EXPECT_EQ(many.at(0), "a.yaml");
    EXPECT_EQ(many.at(1), "b.yaml");
}

// ============================================================================
// Unit-bearing values
// ============================================================================
//
// A dimensioned config value may be a bare number in the model's internal unit
// or a unit-bearing string that is converted to that unit. The parsing/
// conversion core (unit_convert) is tested directly first, then end-to-end
// through compile().

namespace {

constexpr double KIB = 1024.0;
constexpr double MIB = 1024.0 * 1024.0;
constexpr double GIB = 1024.0 * 1024.0 * 1024.0;

// Fetch the first value of a PARAMS key from a compiled config (empty if absent).
std::string params_value(const compiled_config& c, const char* key) {
    const compiled_section* p = find_section(c, "PARAMS");
    if (!p)
        return {};
    const compiled_key* k = find_key(*p, key);
    return (k && !k->values.empty()) ? k->values.at(0) : std::string();
}

// A parametric dragonfly (num_routers: 4) carrying one extra fabric-level scalar
// key -- the pass-through path most model knobs take into PARAMS.
std::string dragonfly_with(const std::string& key, const std::string& value) {
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

// A single-key parametric fabric of the named model, for the flat-bandwidth
// models (torus/fattree) whose link_bandwidth unit differs.
std::string torus_link_bw(const std::string& value) {
    return std::string(R"(
schema_version: 1
components:
  compute_host: { model: nw-lp }
topology:
  format: parametric
  fabric:
    model: torus
    shape:
      n_dims: 1
      dim_length: "8"
    link_bandwidth: )") +
           value + R"(
  hosts:
    component: compute_host
)";
}

std::string fattree_link_bw(const std::string& value) {
    return std::string(R"(
schema_version: 1
components:
  compute_host: { model: nw-lp }
topology:
  format: parametric
  fabric:
    model: fattree
    shape:
      num_levels: 3
      switch_count: 32
      switch_radix: 8
    link_bandwidth: )") +
           value + R"(
  hosts:
    component: compute_host
)";
}

// A flat simplenet component carrying one extra scalar key.
std::string simplenet_with(const std::string& key, const std::string& value) {
    return std::string(R"(
schema_version: 1
components:
  cn:
    model: nw-lp
    network: simplenet
    )") + key +
           ": " + value + R"(
topology:
  format: flat
  component: cn
  nodes: 4
)";
}

} // namespace

// --- classify_value: bare / plain / units --------------------------------

TEST(UnitConvert, ClassifiesBareNumberAndPlainString) {
    EXPECT_EQ(classify_value("512").form, value_form::bare_number);
    EXPECT_EQ(classify_value("5.25").form, value_form::bare_number);
    EXPECT_EQ(classify_value("1.5e6").form, value_form::bare_number); // scientific = bare
    EXPECT_EQ(classify_value("-3").form, value_form::bare_number);

    EXPECT_EQ(classify_value("adaptive").form, value_form::plain);
    EXPECT_EQ(classify_value("some/path.conf").form, value_form::plain);
    EXPECT_EQ(classify_value("").form, value_form::plain);
    EXPECT_EQ(classify_value("4,2,2").form, value_form::unknown_suffix); // number + junk
}

TEST(UnitConvert, ParsesTimeSuffixesToNanoseconds) {
    classified_value ns = classify_value("40ns");
    EXPECT_EQ(ns.form, value_form::with_unit);
    EXPECT_EQ(ns.kind, quantity::time);
    EXPECT_DOUBLE_EQ(ns.canonical, 40.0);
    EXPECT_DOUBLE_EQ(classify_value("10us").canonical, 10000.0);
    EXPECT_DOUBLE_EQ(classify_value("1.5ms").canonical, 1.5e6);
    EXPECT_DOUBLE_EQ(classify_value("2s").canonical, 2.0e9);
}

TEST(UnitConvert, ParsesSizeSuffixesToBytes) {
    classified_value b = classify_value("1500B");
    EXPECT_EQ(b.kind, quantity::size);
    EXPECT_DOUBLE_EQ(b.canonical, 1500.0);
    EXPECT_DOUBLE_EQ(classify_value("2KiB").canonical, 2048.0);
    EXPECT_DOUBLE_EQ(classify_value("4MiB").canonical, 4.0 * MIB);
    EXPECT_DOUBLE_EQ(classify_value("1GiB").canonical, GIB);
    // decimal SI forms are accepted and are 1000-based.
    EXPECT_DOUBLE_EQ(classify_value("2KB").canonical, 2000.0);
    EXPECT_DOUBLE_EQ(classify_value("1MB").canonical, 1.0e6);
}

TEST(UnitConvert, ParsesBandwidthBitAndByteRates) {
    // bit rates are decimal, /8 to bytes/second.
    classified_value g = classify_value("100Gbps");
    EXPECT_EQ(g.kind, quantity::bandwidth);
    EXPECT_DOUBLE_EQ(g.canonical, 100.0e9 / 8.0);
    EXPECT_DOUBLE_EQ(classify_value("8bps").canonical, 1.0);
    // byte rates: decimal and binary forms.
    EXPECT_DOUBLE_EQ(classify_value("2.5GBps").canonical, 2.5e9);
    EXPECT_DOUBLE_EQ(classify_value("1GiBps").canonical, GIB);
    EXPECT_DOUBLE_EQ(classify_value("1KiBps").canonical, KIB);
}

TEST(UnitConvert, SuffixMatchIsCaseSensitiveBitVsByte) {
    // lowercase 'b' = bits, uppercase 'B' = bytes: the two differ by a factor 8.
    EXPECT_DOUBLE_EQ(classify_value("1Gbps").canonical, 1.0e9 / 8.0);
    EXPECT_DOUBLE_EQ(classify_value("1GBps").canonical, 1.0e9);
}

TEST(UnitConvert, RejectsAndPassesTrailingText) {
    EXPECT_EQ(classify_value("5xz").form, value_form::unknown_suffix);
    EXPECT_EQ(classify_value("512qux").form, value_form::unknown_suffix);
    // surrounding whitespace is ignored.
    EXPECT_EQ(classify_value("  5ms  ").form, value_form::with_unit);
    EXPECT_DOUBLE_EQ(classify_value("  5ms  ").canonical, 5.0e6);
}

// --- format_number: integers vs fractions, round-trip --------------------

TEST(UnitConvert, FormatsIntegerValuesWithoutDecimalPoint) {
    EXPECT_EQ(format_number(2048.0), "2048");
    EXPECT_EQ(format_number(1500.0), "1500");
    EXPECT_EQ(format_number(0.0), "0");
    EXPECT_EQ(format_number(1.0e9), "1000000000");
}

TEST(UnitConvert, FormatsFractionsAsPlainDecimalThatRoundTrips) {
    EXPECT_EQ(format_number(1.5), "1.5");
    EXPECT_EQ(format_number(2.5), "2.5");
    // a converted bandwidth: 2.5 GB/s in GiB/s -- no exponent, round-trips.
    double v = 2.5e9 / GIB;
    std::string s = format_number(v);
    EXPECT_EQ(s.find('e'), std::string::npos);
    EXPECT_EQ(s.find('E'), std::string::npos);
    EXPECT_DOUBLE_EQ(std::strtod(s.c_str(), nullptr), v);
}

// --- end-to-end: conversion through compile() ----------------------------

TEST(ConfigCompilerUnits, ConvertsSizeSuffixToBytes) {
    EXPECT_EQ(params_value(compile(dragonfly_with("packet_size", "2KiB")), "packet_size"), "2048");
    EXPECT_EQ(params_value(compile(dragonfly_with("message_size", "1500B")), "message_size"),
              "1500");
    EXPECT_EQ(params_value(compile(dragonfly_with("chunk_size", "4MiB")), "chunk_size"),
              format_number(4.0 * MIB));
}

TEST(ConfigCompilerUnits, ConvertsTimeSuffixToNanoseconds) {
    EXPECT_EQ(params_value(compile(dragonfly_with("router_delay", "1.5us")), "router_delay"),
              "1500");
    EXPECT_EQ(params_value(compile(simplenet_with("net_startup_ns", "5ms")), "net_startup_ns"),
              "5000000");
}

TEST(ConfigCompilerUnits, ConvertsBandwidthToGiBPerSecond) {
    // cn_bandwidth is read in GiB/s; "2.5GBps" is 2.5 GB/s (decimal) = 2.5e9 B/s.
    std::string got =
        params_value(compile(dragonfly_with("cn_bandwidth", "2.5GBps")), "cn_bandwidth");
    EXPECT_EQ(got, format_number(2.5e9 / GIB));
}

TEST(ConfigCompilerUnits, BandwidthConversionIsPerModel) {
    // The same "8Gbps" (1e9 B/s) resolves differently: torus reads GiB/s, fattree
    // reads bytes/ns (= GB/s decimal). fattree lands on a clean 1.
    std::string torus = params_value(compile(torus_link_bw("8Gbps")), "link_bandwidth");
    std::string fattree = params_value(compile(fattree_link_bw("8Gbps")), "link_bandwidth");
    EXPECT_EQ(fattree, "1");
    EXPECT_EQ(torus, format_number(1.0e9 / GIB));
    EXPECT_NE(torus, fattree);
}

TEST(ConfigCompilerUnits, ConvertsSimplenetBandwidthToMebibytesPerSecond) {
    // net_bw_mbps is read in MiB/s despite the name; 1 GiB/s = 1024 MiB/s.
    EXPECT_EQ(params_value(compile(simplenet_with("net_bw_mbps", "1GiBps")), "net_bw_mbps"),
              "1024");
}

TEST(ConfigCompilerUnits, ConvertsMicrosecondCountingWindow) {
    // dragonfly QoS counting_* windows are read in microseconds, not ns.
    EXPECT_EQ(params_value(compile(R"(
schema_version: 1
components:
  compute_host: { model: nw-lp }
topology:
  format: parametric
  fabric:
    model: dragonfly-dally
    shape:
      num_routers: 4
      num_groups: 9
      num_cns_per_router: 2
      num_global_channels: 2
    counting_start: 5ms
  hosts:
    component: compute_host
)"),
                           "counting_start"),
              "5000"); // 5 ms = 5000 us
}

TEST(ConfigCompilerUnits, ConvertsLinkClassBandwidthBlock) {
    // the links: sugar reaches the same conversion (local_bandwidth here).
    std::string got = params_value(compile(R"(
schema_version: 1
components:
  compute_host: { model: nw-lp }
topology:
  format: parametric
  fabric:
    model: dragonfly
    shape:
      num_routers: 4
    links:
      local: { bandwidth: 1GiBps }
  hosts:
    component: compute_host
)"),
                                   "local_bandwidth");
    EXPECT_EQ(got, "1"); // 1 GiB/s in GiB/s
}

// --- bare numbers keep their exact spelling (identity) --------------------

TEST(ConfigCompilerUnits, BareNumbersPassThroughVerbatim) {
    // A bare number already means the model's internal unit and is emitted
    // byte-for-byte -- this is what keeps every existing .conf twin equivalent.
    EXPECT_EQ(params_value(compile(dragonfly_with("packet_size", "512")), "packet_size"), "512");
    EXPECT_EQ(params_value(compile(dragonfly_with("cn_bandwidth", "5.25")), "cn_bandwidth"),
              "5.25");
    EXPECT_EQ(params_value(compile(simplenet_with("net_bw_mbps", "20000")), "net_bw_mbps"),
              "20000");
    // a non-unit string on an unclassified knob passes through untouched.
    const compiled_config c = compile(torus_link_bw("2.0"));
    EXPECT_EQ(params_value(c, "dim_length"), "8"); // number+junk on unclassified: verbatim
}

// --- negative suite -------------------------------------------------------

TEST(ConfigCompilerUnits, RejectsUnknownUnitSuffixOnDimensionedParam) {
    EXPECT_THROW(compile(dragonfly_with("packet_size", "512qux")), config_error);
    EXPECT_THROW(compile(dragonfly_with("chunk_size", "32xyz")), config_error);
}

TEST(ConfigCompilerUnits, RejectsWrongQuantityUnit) {
    // a time unit on a size parameter is a mistake, not a silent misread.
    EXPECT_THROW(compile(dragonfly_with("packet_size", "5ms")), config_error);
    // a size unit on a bandwidth parameter, likewise.
    EXPECT_THROW(compile(dragonfly_with("cn_bandwidth", "5MiB")), config_error);
}

TEST(ConfigCompilerUnits, RejectsNegativeDimensionedValue) {
    EXPECT_THROW(compile(dragonfly_with("cn_bandwidth", "-1Gbps")), config_error);
    EXPECT_THROW(compile(dragonfly_with("packet_size", "-5B")), config_error);
}

TEST(ConfigCompilerUnits, RejectsUnitSuffixOnUnclassifiableParam) {
    // num_vcs is a plain count the model atof()s; a unit there would be silently
    // truncated to its bare number, so it is rejected up front.
    EXPECT_THROW(compile(dragonfly_with("num_vcs", "100Gbps")), config_error);
    EXPECT_THROW(compile(dragonfly_with("num_vcs", "4KiB")), config_error);
}
