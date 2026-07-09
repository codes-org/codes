// Unit tests for the YAML config compiler core (codes::config::compile).
//
// The core is ROSS-free by design (see config_compiler.h), so it compiles
// straight into this test alongside ryml -- no codes/ROSS/MPI link -- and the
// tests assert on the returned compiled_config plain data rather than on any
// simulator behavior. This is the first real unit-test consumer of the vendored
// GoogleTest framework; the model-net equivalence tests cover the emit path.
#include "config_compiler.h"

#include <gtest/gtest.h>

using codes::config::compile;
using codes::config::compiled_config;
using codes::config::compiled_key;
using codes::config::compiled_section;
using codes::config::config_error;
using codes::config::parse_includes;

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
