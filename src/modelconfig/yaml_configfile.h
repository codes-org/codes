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

/* Compile a YAML/JSON config (the user-friendly topology + component format)
 * into the same ConfigVTable the legacy .conf text parser produces, so that
 * codes_mapping and every model read it unchanged through configuration_get_*.
 *
 * This is the thin C boundary of the front-end: it runs the pure C++ core
 * (compile) and the emitter (emit), and it is the only place that translates a
 * compile-time config_error into a ROSS tw_error. data/len are the raw file
 * bytes (already read, e.g. via the MPI collective read in configuration_load).
 * On success returns a newly-allocated ConfigVTable (free with cf_free); on a
 * syntax or validation error it aborts through tw_error with a diagnostic,
 * matching how the rest of the configuration front-end reports malformed input. */
struct ConfigVTable* yaml_configfile_load(const char* data, size_t len);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
