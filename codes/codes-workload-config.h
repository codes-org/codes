#ifndef CODES_WORKLOAD_CONFIG_H
#define CODES_WORKLOAD_CONFIG_H

/**
 * @file codes-workload-config.h
 *
 * Apply synthetic-workload parameters from a loaded config to a model main's
 * option globals, with command-line precedence.
 *
 * The YAML front-end compiles a `workload:` shortcut or a single all-nodes
 * `jobs:` entry into a WORKLOAD section (see the config compiler). A synthetic
 * main calls the helpers below once, after configuration_load()/
 * codes_mapping_setup(), passing each of the ROSS options it registered
 * (`traffic`, `num_messages`, `arrival_time`, `payload_size`) together with that
 * option's compiled-in default. For each knob the precedence is:
 *
 *     command line  >  config (WORKLOAD section)  >  the model's built-in default
 *
 * Command-line detection follows the same pattern codes_mapping uses for the
 * simulation end time: ROSS records no "was this option set" flag, so the helper
 * compares the current global against the registered default. An unchanged value
 * means the command line did not set it, so a configured value may take over; any
 * other value is treated as a command-line override and left untouched. The edge
 * case is the same as end_time's: passing a value equal to the default on the
 * command line is indistinguishable from omitting it, so the config wins there.
 *
 * A legacy `.conf` run carries no WORKLOAD section, so every apply is a no-op and
 * the model keeps its command-line/default behavior exactly as before.
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Maps a friendly traffic-pattern name (as written in a YAML `workload:` block)
 * to the value of a model main's own traffic enum. Each main supplies its own
 * table because the pattern set and the enum values differ per model (only
 * "uniform" is universally 1).
 */
struct codes_workload_traffic_name {
    const char* name; /**< friendly name, e.g. "uniform" (NULL terminates a table) */
    int value;        /**< the main's traffic enum value for that pattern */
};

/**
 * Apply an integer WORKLOAD key to @p val unless the command line already set it.
 *
 * @param key         the WORKLOAD key name (e.g. "num_messages", "payload_size").
 * @param val         the option global; overwritten only when it still equals
 *                    @p cli_default and the config provides @p key.
 * @param cli_default the option's registered default (the command-line sentinel).
 */
void codes_workload_config_apply_int(const char* key, int* val, int cli_default);

/**
 * Apply a floating-point WORKLOAD key to @p val unless the command line already
 * set it. Semantics match codes_workload_config_apply_int (used for
 * `arrival_time`, resolved to nanoseconds by the compiler).
 */
void codes_workload_config_apply_double(const char* key, double* val, double cli_default);

/**
 * Apply WORKLOAD/traffic (a friendly pattern name) to @p val unless the command
 * line already set it, mapping the name through @p names (a table terminated by a
 * `{NULL, 0}` entry). A no-op if @p val already differs from @p cli_default or no
 * traffic is configured. Aborts via tw_error if the configured name is not in
 * @p names, naming the offending value.
 */
void codes_workload_config_apply_traffic(int* val, int cli_default,
                                         const struct codes_workload_traffic_name* names);

#ifdef __cplusplus
}
#endif

#endif /* CODES_WORKLOAD_CONFIG_H */
