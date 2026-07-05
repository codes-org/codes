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
