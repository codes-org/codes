/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#ifndef SRC_MODELCONFIG_YAML_CONFIGFILE_H
#define SRC_MODELCONFIG_YAML_CONFIGFILE_H

/**
 * @file yaml_configfile.h
 *
 * Thin `extern "C"` boundary of the YAML/JSON configuration front-end: the only
 * place that runs the pure C++ core (config_compiler.h) plus the emitter
 * (config_emitter.h) and translates a compile-time config_error into ROSS's
 * `tw_error`. Everything here consumes bytes and does no file I/O of its own --
 * the loader (configuration_load) performs the collective reads and hands the
 * bytes in.
 */

#include <stddef.h>
#include <codes/configfile.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Resolve a config's top-level `include:` list into filesystem paths.
 *
 * Parses @p data only to discover the include names and joins each with the
 * config's directory; it does NO file I/O -- the loader performs the actual
 * (collective) reads. A relative include is resolved against the directory of
 * @p path; an absolute include passes through unchanged.
 *
 * @param data   the raw bytes of the top-level config file.
 * @param len    length of @p data in bytes.
 * @param path   path of the top-level config file, used as the base directory
 *               for resolving relative includes.
 * @param count  out: the number of resolved paths returned (0, with a NULL
 *               return, if the config has no `include:`).
 * @return  a newly-malloc'd array of @p *count newly-malloc'd, NUL-terminated
 *          resolved paths, in listed order (NULL when there are none). The
 *          caller owns the result and must release it with
 *          yaml_configfile_free_includes (equivalently: free() each element,
 *          then the array).
 * @note On malformed YAML it aborts through `tw_error`, just like
 *       yaml_configfile_load; because every rank holds the identical config
 *       bytes from the collective read, that abort fires on every rank at once.
 */
char** yaml_configfile_list_includes(const char* data, size_t len, const char* path, size_t* count);

/**
 * Free an array returned by yaml_configfile_list_includes: frees each element
 * and then the array.
 *
 * @param paths  the array to free; a NULL @p paths (as returned for a config
 *               with no includes) is a no-op.
 * @param count  the element count that yaml_configfile_list_includes reported
 *               through its @p count out-parameter.
 */
void yaml_configfile_free_includes(char** paths, size_t count);

/**
 * Compile a YAML/JSON config (the user-friendly topology + component format)
 * into the same ConfigVTable the legacy `.conf` text parser produces, so that
 * codes_mapping and every model read it unchanged through configuration_get_*.
 *
 * This is the thin C boundary of the front-end: it runs the pure C++ core
 * (compile) and the emitter (emit), and it is the only place that translates a
 * compile-time config_error into a ROSS `tw_error`. It consumes bytes and does
 * no file I/O of its own; the included files are passed in already read -- e.g.
 * via the MPI collective read in configuration_load -- so they must be readable
 * by every rank.
 *
 * @param data      the raw bytes of the top-level config file.
 * @param len       length of @p data in bytes.
 * @param inc_data  the raw bytes of each of the @p n_inc included files, in the
 *                  order yaml_configfile_list_includes reported them; may be
 *                  NULL when @p n_inc is 0.
 * @param inc_lens  length of each buffer in @p inc_data; may be NULL when
 *                  @p n_inc is 0.
 * @param n_inc     number of included files (0 when the config has no
 *                  `include:`).
 * @return  a newly-allocated ConfigVTable (free with cf_free).
 * @note On a syntax or validation error it aborts through `tw_error` with a
 *       diagnostic, matching how the rest of the configuration front-end reports
 *       malformed input.
 */
struct ConfigVTable* yaml_configfile_load(const char* data, size_t len, const char* const* inc_data,
                                          const size_t* inc_lens, size_t n_inc);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
