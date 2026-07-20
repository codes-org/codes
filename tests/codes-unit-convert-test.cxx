// Unit tests for the config compiler's unit-bearing values. A dimensioned config
// value may be a bare number in the model's internal unit or a unit-bearing
// string that is converted to it. The parsing/conversion core (unit_convert) is
// tested directly first (classify_value / format_number), then end-to-end through
// compile().
#include "config-compiler-test-util.h"
#include "config_compiler.h"
#include "unit_convert.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

using codes::config::classified_value;
using codes::config::classify_value;
using codes::config::compile;
using codes::config::compiled_config;
using codes::config::config_error;
using codes::config::format_number;
using codes::config::quantity;
using codes::config::value_form;

namespace {

constexpr double KIB = 1024.0;
constexpr double MIB = 1024.0 * 1024.0;
constexpr double GIB = 1024.0 * 1024.0 * 1024.0;

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
