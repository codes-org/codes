#ifndef SRC_COMMON_MODELCONFIG_CONFIGSTOREADAPTER_H
#define SRC_COMMON_MODELCONFIG_CONFIGSTOREADAPTER_H

#include "codes/configfile.h"
#include "configstore.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Create a new configfile interface backed by a configstore */
ConfigHandle cfsa_create (mcs_entry * e);

ConfigHandle cfsa_create_empty ();

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
