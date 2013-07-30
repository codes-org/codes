#ifndef __CONFIGURATION_H__
#define __CONFIGURATION_H__

#include <stdint.h>
#include <stdlib.h>

#define CONFIGURATION_MAX_NAME 256
#define CONFIGURATION_MAX_GROUPS 10
#define CONFIGURATION_MAX_TYPES 10

typedef struct config_lptype_s
{
    char     name[CONFIGURATION_MAX_NAME];
    uint64_t count;
} config_lptype_t;

typedef struct config_lpgroup_s
{
    char     name[CONFIGURATION_MAX_NAME];
    uint64_t repetitions;
    config_lptype_t lptypes[CONFIGURATION_MAX_TYPES];
    uint64_t lptypes_count;
} config_lpgroup_t;

typedef struct configuration_s
{
    config_lpgroup_t lpgroups[CONFIGURATION_MAX_GROUPS];
    uint64_t lpgroups_count;
} configuration_t;

int configuration_load (const char * filepath, configuration_t *config);
int configuration_dump (configuration_t *config);

#endif
