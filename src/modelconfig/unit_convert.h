#ifndef SRC_MODELCONFIG_UNIT_CONVERT_H
#define SRC_MODELCONFIG_UNIT_CONVERT_H

/**
 * @file unit_convert.h
 *
 * Pure parsing/conversion of unit-bearing configuration values, split out of the
 * compiler core so it is unit-testable on its own. Like config_compiler it has
 * **no** dependency on ROSS, MPI, or `abort`.
 *
 * A dimensioned config value may be written either as a bare number in the
 * target model's internal unit or as a unit-bearing string (`"5ms"`, `"2KiB"`,
 * `"100Gbps"`). This module classifies a raw value string against a fixed unit
 * grammar and, for a recognized unit, normalizes it to a canonical base unit for
 * its quantity:
 *
 *   - time      -> nanoseconds
 *   - size      -> bytes
 *   - bandwidth -> bytes per second
 *
 * The compiler's per-model registry then divides that canonical value by the
 * model's internal-unit scale to get the number the model actually reads. This
 * module does not know model internals; it only parses and normalizes.
 */

#include "config_compiler.h" /* config_error */

#include <string>
#include <string_view>

namespace codes {
namespace config {

/** The physical dimension of a configuration value. */
enum class quantity {
    time,      ///< a duration; canonical base unit is the nanosecond
    size,      ///< a byte count; canonical base unit is the byte
    bandwidth, ///< a data rate; canonical base unit is the byte per second
};

/** How a raw value string classifies against the unit grammar. */
enum class value_form {
    plain,          ///< no leading number: a name, path, enum, or list value -- not a quantity
    bare_number,    ///< a number with no unit suffix
    with_unit,      ///< a number followed by a recognized unit suffix
    unknown_suffix, ///< a number followed by unrecognized trailing text
};

/**
 * The result of classify_value(). For @ref value_form::with_unit, @ref kind and
 * @ref canonical are meaningful: @ref canonical is the value expressed in the
 * canonical base unit for its @ref quantity (time -> ns, size -> bytes,
 * bandwidth -> bytes/second). @ref number is the leading numeric part and is
 * meaningful for every form except @ref value_form::plain.
 */
struct classified_value {
    value_form form = value_form::plain;
    quantity kind = quantity::time; ///< valid only when form == with_unit
    double canonical = 0.0;         ///< valid only when form == with_unit; base-unit value
    double number = 0.0;            ///< the leading numeric part (all forms but plain)
};

/**
 * Classify a raw value string against the unit grammar. Pure and total: it never
 * throws and never touches ROSS. Surrounding ASCII whitespace is ignored. The
 * suffix match is exact and case-sensitive -- the bit/byte distinction is carried
 * by case (`Gbps` gigabit/s vs `GBps` gigabyte/s), so it must be. See the .cxx
 * for the accepted suffix table.
 */
classified_value classify_value(std::string_view text);

/**
 * Format a converted numeric value as a decimal string every model's C reader
 * parses: an integer-valued result carries no decimal point (`"2048"`), and a
 * fractional result is the shortest fixed-notation decimal that round-trips to
 * the same double -- never scientific notation, so a model's atof/strtol reader
 * sees no exponent surprise.
 */
std::string format_number(double v);

} // namespace config
} // namespace codes

#endif
