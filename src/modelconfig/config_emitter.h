/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#ifndef SRC_MODELCONFIG_CONFIG_EMITTER_H
#define SRC_MODELCONFIG_CONFIG_EMITTER_H

/**
 * @file config_emitter.h
 *
 * The dumb second stage of the YAML config front-end: it turns the plain-data
 * @ref codes::config::compiled_config the core produced into a ConfigVTable --
 * the same structure the legacy `.conf` text parser yields, so codes_mapping and
 * every model read it unchanged through the configuration_get_* accessors.
 *
 * It is pure data-to-data: it does no validation (compile() already did) and no
 * ROSS/tw_error. Its coverage comes from the config-equivalence tests, which
 * check a compiled `.yaml` against its golden `.conf`.
 */

#include "config_compiler.h"

struct ConfigVTable;

namespace codes {
namespace config {

/**
 * Emit a compiled_config as a freshly-allocated ConfigVTable (free with
 * cf_free). Sections, subsections, keys, and list values are emitted in the
 * order the core recorded them.
 */
struct ::ConfigVTable* emit(const compiled_config& cfg);

} // namespace config
} // namespace codes

#endif
