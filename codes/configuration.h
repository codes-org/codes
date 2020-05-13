/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#ifndef __CONFIGURATION_H__
#define __CONFIGURATION_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <inttypes.h>
#include <mpi.h>

#define CONFIGURATION_MAX_NAME 128
#define CONFIGURATION_MAX_GROUPS 10
#define CONFIGURATION_MAX_TYPES 10
#define CONFIGURATION_MAX_ANNOS 10

// NOTE: direct offsets are used when first building the structure as we're
// building up one big buffer to hold entity names. Pointers are set after the
// initialization process during configuration_load
typedef union {
    char const * ptr;
    int offset;
} config_name_u;

typedef struct config_lptype_s
{
    config_name_u name;
    config_name_u anno;
    int count;
} config_lptype_t;

typedef struct config_lpgroup_s
{
    config_name_u name;
    int repetitions;
    int lptypes_count;
    config_lptype_t lptypes[CONFIGURATION_MAX_TYPES];
} config_lpgroup_t;

// mapping of lp type to the list of annotations used. Used for convenience when
// models are performing configuraiton code 
typedef struct config_anno_map_s
{
    config_name_u lp_name;
    // only explicit annotations tracked here - use a flag to indicate a
    // non-annotated LP type
    int has_unanno_lp;
    // for configuration/mapping functions to be able to provide "default"
    // (annotation-ignoring) lookup semantics, provide a flag to determine if
    // the unannotated lp type is first
    int is_unanno_first;
    int num_annos;
    // maintain the number of lps that have the particular annotation 
    int num_anno_lps[CONFIGURATION_MAX_ANNOS];
    config_name_u annotations[CONFIGURATION_MAX_ANNOS];
} config_anno_map_t;

typedef struct config_lpgroups_s
{
    // counts for the underlying structures, not the number of string entities
    int lpgroups_count;
    int lpannos_count;
    config_lpgroup_t lpgroups[CONFIGURATION_MAX_GROUPS];
    config_anno_map_t lpannos[CONFIGURATION_MAX_TYPES];
    int num_uniq_lptypes;
    int num_uniq_annos;
    // ptrs into the name buffers
    char const ** group_names;
    char const ** lp_names;
    char const ** anno_names;
    char * group_names_buf;
    char * lp_names_buf;
    char * anno_names_buf;
} config_lpgroups_t;

typedef struct ConfigVTable * ConfigHandle;

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
 * annotation - optional annotation to look for (NULL -> no annotation)
 * value - pointer to string
 * length - maximum length of string
 */
int configuration_get_value(ConfigHandle *handle,
                            const char * section_name,
                            const char * key_name,
                            const char * annotation,
                            char *value,
                            size_t length);

/*
 * Gets the value for a given section/key pair, and interprets it as a path
 * relative to the location of the configuration file.
 * Assumes the key name is a KEY configuration type.
 * Assumes unix path conventions.
 *
 * handle - configuration handle
 * section_name - name of the section the key is in
 * key_name - name of the key
 * annotation - optional annotation to look for (NULL -> no annotation)
 * value - pointer to string
 * length - maximum length of string */
int configuration_get_value_relpath(
        ConfigHandle *handle,
        const char * section_name,
        const char * key_name,
        const char * annotation,
        char *value,
        size_t length);


/*
 * Get's the values for a give section/key pair which has multiple values.
 * Caller's responsible for transforming it to a useable type.
 * Assumes the key name is a MULTIKEY configuration type.
 * 
 * handle - configuration handle
 * section_name - name of the section the key is in
 * key_name - name of the key
 * annotation - optional annotation to look for (NULL -> no annotation)
 * values - array of points to values (must be freed by caller)
 * length - number of value items
 */
int configuration_get_multivalue(ConfigHandle *handle,
                                 const char * section_name,
                                 const char * key_name,
                                 const char * annotation,
                                 char ***values,
                                 size_t *length);

/*
 * Get's the value for a give section/key pair and converts it to an int.
 * Assumes the key name is a KEY configuration type.
 * 
 * handle - configuration handle
 * section_name - name of the section the key is in
 * key_name - name of the key
 * annotation - optional annotation to look for (NULL -> no annotation)
 * value - returned as a pointer to an integer
 */
int configuration_get_value_int (ConfigHandle *handle,
                                 const char *section_name,
                                 const char *key_name,
                                 const char * annotation,
                                 int *value);

/*
 * Get's the value for a give section/key pair and converts it to an
 * unsigned int.
 * Assumes the key name is a KEY configuration type.
 * 
 * handle - configuration handle
 * section_name - name of the section the key is in
 * key_name - name of the key
 * annotation - optional annotation to look for (NULL -> no annotation)
 * value - returned as a pointer to an unsigned integer
 */
int configuration_get_value_uint (ConfigHandle *handle,
                                  const char *section_name,
                                  const char *key_name,
                                  const char * annotation,
                                  unsigned int *value);

/*
 * Get's the value for a give section/key pair and converts it to a long int.
 * Assumes the key name is a KEY configuration type.
 * 
 * handle - configuration handle
 * section_name - name of the section the key is in
 * key_name - name of the key
 * annotation - optional annotation to look for (NULL -> no annotation)
 * value - returned as a pointer to a long integer
 */
int configuration_get_value_longint (ConfigHandle *handle,
                                     const char *section_name,
                                     const char *key_name,
                                     const char * annotation,
                                     long int *value);

/*
 * Get's the value for a give section/key pair and converts it to a double.
 * Assumes the key name is a KEY configuration type.
 * 
 * handle - configuration handle
 * section_name - name of the section the key is in
 * key_name - name of the key
 * annotation - optional annotation to look for (NULL -> no annotation)
 * value - returned as a pointer to a double
 */
int configuration_get_value_double (ConfigHandle *handle,
                                    const char *section_name,
                                    const char *key_name,
                                    const char * annotation,
                                    double *value);

/*
 * Get's the values for a give section/key pair which has multiple values
 * and converts them to an int.
 * Assumes the key name is a MULTIKEY configuration type.
 * 
 * handle - configuration handle
 * section_name - name of the section the key is in
 * key_name - name of the key
 * annotation - optional annotation to look for (NULL -> no annotation)
 * values - array of points to values (must be freed by caller)
 * length - number of value items
 */
int configuration_get_multivalue_int(ConfigHandle *handle,
                                    const char * section_name,
                                    const char * key_name,
                                    const char * annotation,
                                    int **values,
                                    size_t *length);


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
 * Helper function - get the position in the LP annotation list of the
 * given annotation. Used for configuration schemes where an array of
 * configuration values is generated based on the annotations in
 * config_anno_map_t
 * If annotation is not found, -1 is returned */
int configuration_get_annotation_index(const char *              anno,
                                       const config_anno_map_t * anno_map);

/*
 * Forward reference to the configuration handle
 */
extern ConfigHandle config;

extern config_lpgroups_t lpconf;

#ifdef __cplusplus
}
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
