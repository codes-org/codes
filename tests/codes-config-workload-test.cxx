// Unit tests for the config compiler's run-level and workload configuration: the
// top-level simulation: block (end_time / pe_mem_factor), the inline workload:
// shortcut, and the multi-job jobs: block.
#include "config-compiler-test-util.h"
#include "config_compiler.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <vector>

using codes::config::compile;
using codes::config::compiled_config;
using codes::config::compiled_key;
using codes::config::compiled_section;
using codes::config::config_error;

// ---------------------------------------------------------------------------
// simulation: block -- run-level settings (end_time, pe_mem_factor) resolved to
// the PARAMS keys codes_mapping reads.
// ---------------------------------------------------------------------------

namespace {
// kFlatBase plus a trailing top-level simulation: block (body already indented).
std::string flat_with_simulation(const std::string& body) {
    return std::string(kFlatBase) + "simulation:\n" + body;
}
} // namespace

TEST(ConfigCompilerSimulation, EmitsEndTimeAndMemFactorToParams) {
    // both settings land in PARAMS -- the exact section/keys codes_mapping reads.
    compiled_config c = compile(flat_with_simulation("  end_time: 5000\n  pe_mem_factor: 512\n"));
    EXPECT_EQ(params_value(c, "end_time"), "5000");
    EXPECT_EQ(params_value(c, "pe_mem_factor"), "512");
}

TEST(ConfigCompilerSimulation, EndTimeAcceptsTimeUnits) {
    // a bare number is nanoseconds; a unit-bearing value converts to ns.
    EXPECT_EQ(params_value(compile(flat_with_simulation("  end_time: 100us\n")), "end_time"),
              "100000");
    EXPECT_EQ(params_value(compile(flat_with_simulation("  end_time: 2ms\n")), "end_time"),
              "2000000");
    EXPECT_EQ(params_value(compile(flat_with_simulation("  end_time: 250\n")), "end_time"), "250");
}

TEST(ConfigCompilerSimulation, RejectsUnknownKeyInBlock) {
    EXPECT_THROW(compile(flat_with_simulation("  bogus: 1\n")), config_error);
}

TEST(ConfigCompilerSimulation, RejectsNonPositiveMemFactor) {
    EXPECT_THROW(compile(flat_with_simulation("  pe_mem_factor: 0\n")), config_error);
    EXPECT_THROW(compile(flat_with_simulation("  pe_mem_factor: -4\n")), config_error);
}

TEST(ConfigCompilerSimulation, RejectsNonIntegerMemFactor) {
    EXPECT_THROW(compile(flat_with_simulation("  pe_mem_factor: 3.5\n")), config_error);
    EXPECT_THROW(compile(flat_with_simulation("  pe_mem_factor: abc\n")), config_error);
}

TEST(ConfigCompilerSimulation, RejectsMalformedEndTime) {
    // not a number, not positive, negative, and an unrecognized suffix.
    EXPECT_THROW(compile(flat_with_simulation("  end_time: abc\n")), config_error);
    EXPECT_THROW(compile(flat_with_simulation("  end_time: 0\n")), config_error);
    EXPECT_THROW(compile(flat_with_simulation("  end_time: -5us\n")), config_error);
    EXPECT_THROW(compile(flat_with_simulation("  end_time: 100xyz\n")), config_error);
}

TEST(ConfigCompilerSimulation, RejectsWrongQuantityUnitOnEndTime) {
    // a size or bandwidth unit on a time value is a mistake, not a silent misread.
    EXPECT_THROW(compile(flat_with_simulation("  end_time: 2KiB\n")), config_error);
    EXPECT_THROW(compile(flat_with_simulation("  end_time: 5Gbps\n")), config_error);
}

TEST(ConfigCompilerSimulation, ConflictsWithPassthroughParam) {
    // A key set both as a fabric pass-through and in simulation: would be silently
    // dropped by the config store's first-match lookup, so it is rejected.
    EXPECT_THROW(compile(dragonfly_with("pe_mem_factor", "256") +
                         "simulation:\n  pe_mem_factor: 512\n"),
                 config_error);
    EXPECT_THROW(compile(dragonfly_with("end_time", "5000") + "simulation:\n  end_time: 9000\n"),
                 config_error);
}

TEST(ConfigCompilerSimulation, PassthroughParamAloneStillWorks) {
    // Without a simulation: block, pe_mem_factor on the fabric passes through to
    // PARAMS exactly as before -- no regression for existing configs.
    compiled_config c = compile(dragonfly_with("pe_mem_factor", "256"));
    EXPECT_EQ(params_value(c, "pe_mem_factor"), "256");
}

TEST(ConfigCompilerSimulation, WorksWithParametricTopology) {
    compiled_config c =
        compile(std::string(kParametricDragonfly) + "simulation:\n  end_time: 50us\n");
    EXPECT_EQ(params_value(c, "end_time"), "50000");
}

TEST(ConfigCompilerSimulation, WorksWithExplicitGroups) {
    // the explicit-groups form builds PARAMS itself; the simulation keys still
    // land in it.
    compiled_config c = compile(std::string(kGroupsBase) + "simulation:\n  pe_mem_factor: 64\n");
    EXPECT_EQ(params_value(c, "pe_mem_factor"), "64");
}

TEST(ConfigCompilerSimulation, ConflictsWithExplicitGroupsParam) {
    // a pe_mem_factor written directly in the explicit-groups params: collides
    // with simulation.pe_mem_factor exactly like the pass-through fabric case.
    const char* yaml = R"(
schema_version: 1
topology:
  format: groups
  params:
    pe_mem_factor: 256
  groups:
    GRP:
      repetitions: 1
      lps:
        a: 1
simulation:
  pe_mem_factor: 512
)";
    EXPECT_THROW(compile(yaml), config_error);
}

// --- workload: shortcut and jobs: block -----------------------------------
//
// A node's workload (what it runs) is configured separately from the node model
// (what it is): an inline `workload:` on the topology's compute component is the
// single-workload shortcut, and a top-level `jobs:` block is the explicit
// multi-job form. Both lower to a WORKLOAD (or JOBS) section a synthetic main
// reads. Only synthetic is wired; other types are recognized but rejected.

namespace {

// Fetch the first value of a WORKLOAD-section key (empty if absent).
std::string workload_value(const compiled_config& c, const char* key) {
    const compiled_section* w = find_section(c, "WORKLOAD");
    if (!w)
        return {};
    const compiled_key* k = find_key(*w, key);
    return (k && !k->values.empty()) ? k->values.at(0) : std::string();
}

// Deep structural equality of two compiled configs (section/key/value/subsection,
// order included) -- used to prove two spellings compile to the identical tree.
bool key_eq(const compiled_key& a, const compiled_key& b) {
    return a.name == b.name && a.values == b.values;
}
bool section_eq(const compiled_section& a, const compiled_section& b) {
    if (a.name != b.name || a.keys.size() != b.keys.size() ||
        a.subsections.size() != b.subsections.size())
        return false;
    for (size_t i = 0; i < a.keys.size(); ++i)
        if (!key_eq(a.keys[i], b.keys[i]))
            return false;
    for (size_t i = 0; i < a.subsections.size(); ++i)
        if (!section_eq(a.subsections[i], b.subsections[i]))
            return false;
    return true;
}
bool config_eq(const compiled_config& a, const compiled_config& b) {
    if (a.sections.size() != b.sections.size())
        return false;
    for (size_t i = 0; i < a.sections.size(); ++i)
        if (!section_eq(a.sections[i], b.sections[i]))
            return false;
    return true;
}

// A flat 4-node config whose compute component carries an inline workload: block.
// `body` supplies the workload's keys, each indented six spaces.
std::string flat_with_workload(const std::string& body) {
    return std::string(R"(
schema_version: 1
components:
  cn:
    model: nw-lp
    network: simplenet
    message_size: 464
    workload:
)") + body +
           R"(
topology:
  format: flat
  component: cn
  nodes: 4
)";
}

// A flat 4-node config with a top-level jobs: block. `body` supplies the job
// list entries (each `- id:` indented two spaces).
std::string flat_with_jobs(const std::string& body) {
    return std::string(kFlatBase) + "jobs:\n" + body;
}

} // namespace

TEST(ConfigCompilerWorkload, InlineEmitsResolvedWorkloadSection) {
    // payload_size resolves to bytes, arrival_time to nanoseconds; traffic stays
    // a verbatim pattern name and num_messages a positive integer.
    compiled_config c = compile(flat_with_workload("      type: synthetic\n"
                                                   "      traffic: uniform\n"
                                                   "      num_messages: 30\n"
                                                   "      payload_size: \"2KiB\"\n"
                                                   "      arrival_time: \"1us\"\n"));
    EXPECT_EQ(workload_value(c, "type"), "synthetic");
    EXPECT_EQ(workload_value(c, "traffic"), "uniform");
    EXPECT_EQ(workload_value(c, "num_messages"), "30");
    EXPECT_EQ(workload_value(c, "payload_size"), "2048");
    EXPECT_EQ(workload_value(c, "arrival_time"), "1000");
}

TEST(ConfigCompilerWorkload, NoWorkloadEmitsNoSection) {
    // A config without workload:/jobs: emits no WORKLOAD section, so a legacy
    // config's tree is unchanged.
    compiled_config c = compile(kFlatBase);
    EXPECT_EQ(find_section(c, "WORKLOAD"), nullptr);
    EXPECT_EQ(find_section(c, "JOBS"), nullptr);
}

TEST(ConfigCompilerWorkload, RejectsUnknownKey) {
    EXPECT_THROW(compile(flat_with_workload("      type: synthetic\n      bogus: 1\n")),
                 config_error);
}

TEST(ConfigCompilerWorkload, RejectsMissingType) {
    EXPECT_THROW(compile(flat_with_workload("      traffic: uniform\n")), config_error);
}

TEST(ConfigCompilerWorkload, RejectsNonSyntheticType) {
    // dumpi etc. are recognized as a workload shape but not yet configurable here.
    EXPECT_THROW(compile(flat_with_workload("      type: dumpi\n      trace: app.dumpi\n")),
                 config_error);
}

TEST(ConfigCompilerWorkload, RejectsWrongUnitOnArrivalTime) {
    EXPECT_THROW(compile(flat_with_workload("      type: synthetic\n"
                                            "      arrival_time: \"2KiB\"\n")),
                 config_error);
}

TEST(ConfigCompilerWorkload, RejectsWrongUnitOnPayloadSize) {
    EXPECT_THROW(compile(flat_with_workload("      type: synthetic\n"
                                            "      payload_size: \"1us\"\n")),
                 config_error);
}

TEST(ConfigCompilerWorkload, RejectsNonPositiveMessageCount) {
    EXPECT_THROW(compile(flat_with_workload("      type: synthetic\n      num_messages: 0\n")),
                 config_error);
    EXPECT_THROW(compile(flat_with_workload("      type: synthetic\n      num_messages: -5\n")),
                 config_error);
}

TEST(ConfigCompilerWorkload, RejectsNonIntegerMessageCount) {
    EXPECT_THROW(compile(flat_with_workload("      type: synthetic\n      num_messages: 3.5\n")),
                 config_error);
}

TEST(ConfigCompilerWorkload, RejectsWorkloadOnNonComputeComponent) {
    // a workload: on a component the topology does not run is meaningless; reject
    // it rather than silently ignore it.
    const char* yaml = R"(
schema_version: 1
components:
  cn:
    model: nw-lp
    network: simplenet
  other:
    model: nw-lp
    workload:
      type: synthetic
topology:
  format: flat
  component: cn
  nodes: 4
)";
    EXPECT_THROW(compile(yaml), config_error);
}

TEST(ConfigCompilerWorkload, RejectsWorkloadWithExplicitGroups) {
    const char* yaml = R"(
schema_version: 1
components:
  cn:
    model: nw-lp
    workload:
      type: synthetic
topology:
  format: groups
  groups:
    GRP:
      repetitions: 1
      lps:
        a: 1
)";
    EXPECT_THROW(compile(yaml), config_error);
}

TEST(ConfigCompilerWorkload, RejectsInlineWorkloadAndJobsTogether) {
    // exactly one form is allowed.
    const char* yaml = R"(
schema_version: 1
components:
  cn:
    model: nw-lp
    network: simplenet
    workload:
      type: synthetic
topology:
  format: flat
  component: cn
  nodes: 4
jobs:
  - id: j
    workload:
      type: synthetic
    ranks: 4
    placement:
      policy: contiguous
)";
    EXPECT_THROW(compile(yaml), config_error);
}

TEST(ConfigCompilerJobs, SingleAllNodesJobDesugarsToInlineWorkload) {
    // A single synthetic job placed contiguously on every node must compile to
    // the exact same tree as the inline workload: shortcut.
    const char* inln = R"(
schema_version: 1
components:
  cn:
    model: nw-lp
    network: simplenet
    message_size: 464
    workload:
      type: synthetic
      traffic: uniform
      num_messages: 30
topology:
  format: flat
  component: cn
  nodes: 4
)";
    const char* jobs = R"(
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
jobs:
  - id: everything
    workload:
      type: synthetic
      traffic: uniform
      num_messages: 30
    ranks: 4
    placement:
      policy: contiguous
)";
    compiled_config a = compile(inln);
    compiled_config b = compile(jobs);
    EXPECT_TRUE(config_eq(a, b));
    // and it really is the desugared shortcut: a WORKLOAD section, no JOBS.
    EXPECT_NE(find_section(b, "WORKLOAD"), nullptr);
    EXPECT_EQ(find_section(b, "JOBS"), nullptr);
}

TEST(ConfigCompilerJobs, MultiJobEmitsJobsSectionWithNodeAllocation) {
    // two explicitly-placed jobs on a 4-node topology -> a JOBS section, one
    // subsection per job carrying its workload, ranks, and resolved node list.
    compiled_config c = compile(flat_with_jobs(R"(  - id: front
    workload:
      type: synthetic
      traffic: uniform
    ranks: 2
    placement:
      nodes: [0, 1]
  - id: back
    workload:
      type: synthetic
      traffic: nearest_neighbor
    ranks: 2
    placement:
      nodes: [2, 3]
)"));
    EXPECT_EQ(find_section(c, "WORKLOAD"), nullptr);
    const compiled_section* jobs = find_section(c, "JOBS");
    ASSERT_NE(jobs, nullptr);
    EXPECT_EQ(find_key(*jobs, "num_jobs")->values.at(0), "2");
    const compiled_section* front = find_subsection(*jobs, "front");
    const compiled_section* back = find_subsection(*jobs, "back");
    ASSERT_NE(front, nullptr);
    ASSERT_NE(back, nullptr);
    EXPECT_EQ(find_key(*front, "traffic")->values.at(0), "uniform");
    EXPECT_EQ(find_key(*front, "ranks")->values.at(0), "2");
    const compiled_key* fnodes = find_key(*front, "nodes");
    ASSERT_NE(fnodes, nullptr);
    EXPECT_EQ(fnodes->values, (std::vector<std::string>{"0", "1"}));
    const compiled_key* bnodes = find_key(*back, "nodes");
    ASSERT_NE(bnodes, nullptr);
    EXPECT_EQ(bnodes->values, (std::vector<std::string>{"2", "3"}));
}

TEST(ConfigCompilerJobs, ContiguousPolicyPacksInDeclaredOrder) {
    // two contiguous jobs pack from node 0 in order: [0,1] then [2,3].
    compiled_config c = compile(flat_with_jobs(R"(  - id: a
    workload: { type: synthetic }
    ranks: 2
    placement: { policy: contiguous }
  - id: b
    workload: { type: synthetic }
    ranks: 2
    placement: { policy: contiguous }
)"));
    const compiled_section* jobs = find_section(c, "JOBS");
    ASSERT_NE(jobs, nullptr);
    EXPECT_EQ(find_key(*find_subsection(*jobs, "a"), "nodes")->values,
              (std::vector<std::string>{"0", "1"}));
    EXPECT_EQ(find_key(*find_subsection(*jobs, "b"), "nodes")->values,
              (std::vector<std::string>{"2", "3"}));
}

TEST(ConfigCompilerJobs, RejectsDuplicateId) {
    EXPECT_THROW(compile(flat_with_jobs(R"(  - id: dup
    workload: { type: synthetic }
    ranks: 2
    placement: { policy: contiguous }
  - id: dup
    workload: { type: synthetic }
    ranks: 2
    placement: { policy: contiguous }
)")),
                 config_error);
}

TEST(ConfigCompilerJobs, RejectsEmptyId) {
    EXPECT_THROW(compile(flat_with_jobs(R"(  - id: ""
    workload: { type: synthetic }
    ranks: 4
    placement: { policy: contiguous }
)")),
                 config_error);
}

TEST(ConfigCompilerJobs, RejectsMissingWorkload) {
    EXPECT_THROW(compile(flat_with_jobs(R"(  - id: j
    ranks: 4
    placement: { policy: contiguous }
)")),
                 config_error);
}

TEST(ConfigCompilerJobs, RejectsNonPositiveRanks) {
    EXPECT_THROW(compile(flat_with_jobs(R"(  - id: j
    workload: { type: synthetic }
    ranks: 0
    placement: { policy: contiguous }
)")),
                 config_error);
}

TEST(ConfigCompilerJobs, RejectsNonIntegerRanks) {
    EXPECT_THROW(compile(flat_with_jobs(R"(  - id: j
    workload: { type: synthetic }
    ranks: two
    placement: { policy: contiguous }
)")),
                 config_error);
}

TEST(ConfigCompilerJobs, RejectsMissingPlacement) {
    EXPECT_THROW(compile(flat_with_jobs(R"(  - id: j
    workload: { type: synthetic }
    ranks: 4
)")),
                 config_error);
}

TEST(ConfigCompilerJobs, RejectsPlacementWithBothForms) {
    EXPECT_THROW(compile(flat_with_jobs(R"(  - id: j
    workload: { type: synthetic }
    ranks: 4
    placement:
      policy: contiguous
      nodes: [0, 1, 2, 3]
)")),
                 config_error);
}

TEST(ConfigCompilerJobs, RejectsPlacementWithNeitherForm) {
    EXPECT_THROW(compile(flat_with_jobs(R"(  - id: j
    workload: { type: synthetic }
    ranks: 4
    placement: {}
)")),
                 config_error);
}

TEST(ConfigCompilerJobs, RejectsUnknownPlacementPolicy) {
    EXPECT_THROW(compile(flat_with_jobs(R"(  - id: j
    workload: { type: synthetic }
    ranks: 4
    placement: { policy: scattered }
)")),
                 config_error);
}

TEST(ConfigCompilerJobs, RejectsNodeOutOfRange) {
    // node 9 does not exist in a 4-node topology.
    EXPECT_THROW(compile(flat_with_jobs(R"(  - id: j
    workload: { type: synthetic }
    ranks: 1
    placement: { nodes: [9] }
)")),
                 config_error);
}

TEST(ConfigCompilerJobs, RejectsDoubleBookedNode) {
    EXPECT_THROW(compile(flat_with_jobs(R"(  - id: a
    workload: { type: synthetic }
    ranks: 2
    placement: { nodes: [0, 1] }
  - id: b
    workload: { type: synthetic }
    ranks: 2
    placement: { nodes: [1, 2] }
)")),
                 config_error);
}

TEST(ConfigCompilerJobs, RejectsRanksNodeListMismatch) {
    EXPECT_THROW(compile(flat_with_jobs(R"(  - id: j
    workload: { type: synthetic }
    ranks: 3
    placement: { nodes: [0, 1] }
)")),
                 config_error);
}

TEST(ConfigCompilerJobs, RejectsTotalRanksExceedingSlots) {
    // two contiguous jobs of 3 ranks each need 6 slots; the topology has 4.
    EXPECT_THROW(compile(flat_with_jobs(R"(  - id: a
    workload: { type: synthetic }
    ranks: 3
    placement: { policy: contiguous }
  - id: b
    workload: { type: synthetic }
    ranks: 3
    placement: { policy: contiguous }
)")),
                 config_error);
}

TEST(ConfigCompilerJobs, RejectsQosKey) {
    // qos is recognized but not yet wired.
    EXPECT_THROW(compile(flat_with_jobs(R"(  - id: j
    workload: { type: synthetic }
    ranks: 4
    placement: { policy: contiguous }
    qos: 1
)")),
                 config_error);
}

TEST(ConfigCompilerJobs, RejectsUnknownJobKey) {
    EXPECT_THROW(compile(flat_with_jobs(R"(  - id: j
    workload: { type: synthetic }
    ranks: 4
    placement: { policy: contiguous }
    bogus: 1
)")),
                 config_error);
}

TEST(ConfigCompilerJobs, RejectsNonSyntheticJobWorkload) {
    EXPECT_THROW(compile(flat_with_jobs(R"(  - id: j
    workload: { type: dumpi, trace: app.dumpi }
    ranks: 4
    placement: { policy: contiguous }
)")),
                 config_error);
}
