#include "unit_convert.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace codes {
namespace config {

namespace {

/* One accepted unit suffix: its exact spelling, the quantity it measures, and
 * how many canonical base units make up one of it (time base = ns, size base =
 * bytes, bandwidth base = bytes/second). Matching is exact and case-sensitive. */
struct suffix_entry {
    const char* suffix;
    quantity kind;
    double factor;
};

/* Powers used below, spelled out so the intent (binary IEC vs decimal SI) is
 * obvious at each entry. */
constexpr double KIB = 1024.0;
constexpr double MIB = 1024.0 * 1024.0;
constexpr double GIB = 1024.0 * 1024.0 * 1024.0;

/* The full accepted suffix table.
 *
 *  - time (base ns): ns, us (microseconds), ms, s.
 *  - size (base bytes): B, KiB/MiB/GiB (binary, 1024-based) and KB/MB/GB
 *    (decimal, 1000-based) -- both accepted; use the IEC forms to be exact.
 *  - bandwidth (base bytes/second): lowercase-b bit rates (bps, Kbps, Mbps,
 *    Gbps -- decimal, /8 to bytes) and uppercase-B byte rates in decimal (Bps,
 *    KBps, MBps, GBps) and binary (KiBps, MiBps, GiBps).
 *
 * The bit-vs-byte distinction rides on the case of 'b'/'B', so lookups must be
 * case-sensitive. */
const suffix_entry suffixes[] = {
    /* time -> nanoseconds */
    {"ns", quantity::time, 1.0},
    {"us", quantity::time, 1.0e3},
    {"ms", quantity::time, 1.0e6},
    {"s", quantity::time, 1.0e9},

    /* size -> bytes (binary IEC) */
    {"B", quantity::size, 1.0},
    {"KiB", quantity::size, KIB},
    {"MiB", quantity::size, MIB},
    {"GiB", quantity::size, GIB},
    /* size -> bytes (decimal SI) */
    {"KB", quantity::size, 1.0e3},
    {"MB", quantity::size, 1.0e6},
    {"GB", quantity::size, 1.0e9},

    /* bandwidth -> bytes/second: bit rates (decimal, /8) */
    {"bps", quantity::bandwidth, 1.0 / 8.0},
    {"Kbps", quantity::bandwidth, 1.0e3 / 8.0},
    {"Mbps", quantity::bandwidth, 1.0e6 / 8.0},
    {"Gbps", quantity::bandwidth, 1.0e9 / 8.0},
    /* bandwidth -> bytes/second: byte rates (decimal) */
    {"Bps", quantity::bandwidth, 1.0},
    {"KBps", quantity::bandwidth, 1.0e3},
    {"MBps", quantity::bandwidth, 1.0e6},
    {"GBps", quantity::bandwidth, 1.0e9},
    /* bandwidth -> bytes/second: byte rates (binary IEC) */
    {"KiBps", quantity::bandwidth, KIB},
    {"MiBps", quantity::bandwidth, MIB},
    {"GiBps", quantity::bandwidth, GIB},
};

} // namespace

classified_value classify_value(std::string_view text) {
    classified_value r;

    /* trim surrounding ASCII whitespace */
    size_t b = 0, e = text.size();
    while (b < e && std::isspace(static_cast<unsigned char>(text[b])))
        ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(text[e - 1])))
        --e;
    std::string s(text.substr(b, e - b));
    if (s.empty()) {
        r.form = value_form::plain;
        return r;
    }

    /* parse a leading base-10 number (strtod also accepts scientific notation,
     * which we treat as a bare number since a model's reader accepts it too) */
    char* end = nullptr;
    double num = std::strtod(s.c_str(), &end);
    if (end == s.c_str()) {
        /* no number at the front: a name, path, or enum -- not a quantity */
        r.form = value_form::plain;
        return r;
    }
    r.number = num;

    std::string rest(end); /* everything after the number */
    if (rest.empty()) {
        r.form = value_form::bare_number;
        return r;
    }

    for (const suffix_entry& se : suffixes) {
        if (rest == se.suffix) {
            r.form = value_form::with_unit;
            r.kind = se.kind;
            r.canonical = num * se.factor;
            return r;
        }
    }

    /* a number followed by text that is not a recognized unit */
    r.form = value_form::unknown_suffix;
    return r;
}

std::string format_number(double v) {
    char buf[64];

    /* An integer-valued result is emitted without a decimal point. The bound
     * keeps us within the range where a double represents every integer exactly,
     * so "%.0f" is lossless; realistic converted values sit far below it. */
    if (std::isfinite(v) && std::fabs(v) < 1.0e15 && v == std::floor(v)) {
        std::snprintf(buf, sizeof(buf), "%.0f", v);
        return std::string(buf);
    }

    /* Otherwise emit the shortest fixed-notation decimal that round-trips to the
     * same double. The minimal precision that round-trips never leaves a trailing
     * zero, so the result is already tidy. Fixed notation (never %g/%e) keeps an
     * exponent out of the string a C reader will atof(). */
    for (int prec = 1; prec <= 17; ++prec) {
        std::snprintf(buf, sizeof(buf), "%.*f", prec, v);
        if (std::strtod(buf, nullptr) == v)
            return std::string(buf);
    }

    /* Fallback for an extreme magnitude no config conversion produces: the
     * shortest round-tripping form, which may use an exponent. */
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    return std::string(buf);
}

} // namespace config
} // namespace codes
