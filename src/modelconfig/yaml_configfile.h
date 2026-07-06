/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#ifndef SRC_MODELCONFIG_YAML_CONFIGFILE_H
#define SRC_MODELCONFIG_YAML_CONFIGFILE_H

#include <stddef.h>
#include <codes/configfile.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Resolve a config's top-level `include:` list into filesystem paths.
 *
 * data/len are the raw bytes of the top-level config file; path is its path,
 * used to resolve each relative include against the config's directory
 * (absolute includes pass through unchanged). Returns a newly-malloc'd array of
 * *count newly-malloc'd, NUL-terminated resolved paths, in listed order, and
 * writes the element count through count (0, with a NULL return, if the config
 * has no `include:`). The caller owns the result and must release it with
 * yaml_configfile_free_includes (equivalently: free() each element, then the
 * array). This function does NO file I/O -- it only parses `data` to discover
 * the names and joins them with the config's directory; the loader performs the
 * actual (collective) reads. On malformed YAML it aborts through tw_error, just
 * like yaml_configfile_load; because every rank holds the identical config bytes
 * from the collective read, that abort fires on every rank at once. */
char** yaml_configfile_list_includes(const char* data, size_t len, const char* path, size_t* count);

/* Free an array returned by yaml_configfile_list_includes (frees each element
 * and the array; a NULL array with count 0 is a no-op). */
void yaml_configfile_free_includes(char** paths, size_t count);

/* Compile a YAML/JSON config (the user-friendly topology + component format)
 * into the same ConfigVTable the legacy .conf text parser produces, so that
 * codes_mapping and every model read it unchanged through configuration_get_*.
 *
 * This is the thin C boundary of the front-end: it runs the pure C++ core
 * (compile) and the emitter (emit), and it is the only place that translates a
 * compile-time config_error into a ROSS tw_error. It consumes bytes and does no
 * file I/O of its own: data/len are the raw bytes of the top-level config file,
 * and inc_data/inc_lens are the raw bytes of the n_inc included files (in the
 * order yaml_configfile_list_includes reported them), each already read -- e.g.
 * via the MPI collective read in configuration_load -- so included files must be
 * readable by every rank. Pass n_inc == 0 (inc_data/inc_lens may be NULL) when
 * the config has no `include:`. On success returns a newly-allocated
 * ConfigVTable (free with cf_free); on a syntax or validation error it aborts
 * through tw_error with a diagnostic, matching how the rest of the configuration
 * front-end reports malformed input. */
struct ConfigVTable* yaml_configfile_load(const char* data, size_t len, const char* const* inc_data,
                                          const size_t* inc_lens, size_t n_inc);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
