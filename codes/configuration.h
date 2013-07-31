#ifndef __CONFIGURATION_H__
#define __CONFIGURATION_H__

#include <inttypes.h>
#include <mpi.h>
#include "codes/txt_configfile.h"

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

typedef struct config_lpgroups_s
{
    config_lpgroup_t lpgroups[CONFIGURATION_MAX_GROUPS];
    uint64_t lpgroups_count;
} config_lpgroups_t;

/*
 * Load a configuration on the system (collectively)
 *
 * filepath - path and name of file
 * comm     - communicator to distribute file on
 * handle   - output of configuration
 *
 * return 0 on success
 */
int configuration_load (const char * filepath,
                        MPI_Comm comm,
                        ConfigHandle *handle);

/*
 * Free any resources allocated on load.
 *
 * handle - configuration handle
 */
int configuration_free (ConfigHandle *handle);

/*
 * Get's the value for a give section/key pair.
 * Caller's responsible for transforming it to a useable type.
 * Assumes the key name is a KEY configuration type.
 * 
 * handle - configuration handle
 * section_name - name of the section the key is in
 * key_name - name of the key
 * value - pointer to string
 * length - maximum length of string
 */
int configuration_get_value(ConfigHandle *handle,
                            const char * section_name,
                            const char * key_name,
                            char *value,
                            size_t length);

/*
 * Get's the value for a give section/key pair and converts it to an int.
 * Assumes the key name is a KEY configuration type.
 * 
 * handle - configuration handle
 * section_name - name of the section the key is in
 * key_name - name of the key
 * value - returned as a pointer to an integer
 */
int configuration_get_value_int (ConfigHandle *handle,
                                 const char *section_name,
                                 const char *key_name,
                                 int *value);

/*
 * Get's the value for a give section/key pair and converts it to an
 * unsigned int.
 * Assumes the key name is a KEY configuration type.
 * 
 * handle - configuration handle
 * section_name - name of the section the key is in
 * key_name - name of the key
 * value - returned as a pointer to an unsigned integer
 */
int configuration_get_value_uint (ConfigHandle *handle,
                                  const char *section_name,
                                  const char *key_name,
                                  unsigned int *value);

/*
 * Get's the value for a give section/key pair and converts it to a long int.
 * Assumes the key name is a KEY configuration type.
 * 
 * handle - configuration handle
 * section_name - name of the section the key is in
 * key_name - name of the key
 * value - returned as a pointer to a long integer
 */
int configuration_get_value_longint (ConfigHandle *handle,
                                     const char *section_name,
                                     const char *key_name,
                                     long int *value);

/*
 * Get's the value for a give section/key pair and converts it to a double.
 * Assumes the key name is a KEY configuration type.
 * 
 * handle - configuration handle
 * section_name - name of the section the key is in
 * key_name - name of the key
 * value - returned as a pointer to a double
 */
int configuration_get_value_double (ConfigHandle *handle,
                                    const char *section_name,
                                    const char *key_name,
                                    double *value);

/*
 * Get the LPGROUPS configuration from the config file which is stored
 * in the associated data structures.
 *
 * handle - configuration handle
 * section_name - name of section which has the lptypes
 * lpgroups - data structure to hold the lpgroup info
 */
int configuration_get_lpgroups (ConfigHandle *handle,
                                const char *section_name,
                                config_lpgroups_t *lpgroups);

/*
 * Forward reference to the configuration handle
 */
extern ConfigHandle config;

#endif
