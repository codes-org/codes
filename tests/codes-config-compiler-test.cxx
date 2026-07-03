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
