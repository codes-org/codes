/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#ifndef SRC_COMMON_MODELCONFIG_CONFIGSTOREADAPTER_H
#define SRC_COMMON_MODELCONFIG_CONFIGSTOREADAPTER_H

#include "configfile.h"
#include "configstore.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Create a new configfile interface backed by a configstore */
struct ConfigVTable * cfsa_create (mcs_entry * e);

struct ConfigVTable * cfsa_create_empty ();

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
