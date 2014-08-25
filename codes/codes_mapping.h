/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

/* SUMMARY:
 * CODES custom mapping file for ROSS
 */
#include "configuration.h"
#include "codes.h"
#include "lp-type-lookup.h"
#define MAX_NAME_LENGTH 256

/* Returns number of LPs on the current PE */
int codes_mapping_get_lps_for_pe(void);

/* Takes the global LP ID and returns the rank (PE id) on which the LP is mapped.*/
tw_peid codes_mapping( tw_lpid gid);

/* loads the configuration file and sets up the number of LPs on each PE. */
void codes_mapping_setup(void);

/* set up lps with an RNG offset
 *
 * NOTE: manual seeding is discouraged by the ROSS folks, who instead suggest to
 * set the number of RNGs each LP will "skip". offset here is a multiplier by
 * the global number of LPs
 */
void codes_mapping_setup_with_seed_offset(int offset);

/*Takes the group name and returns the number of repetitions in the group */
int codes_mapping_get_group_reps(const char* group_name);

/* Calculates the count for LPs of the given type
 *
 * group_name         - name of group. If NULL, count is across all groups.
 * ignore_repetitions - if group_name is given, then don't include repetitions
 *                      in count. This exists at the moment to support some
 *                      uses of the function that haven't been fleshed out in
 *                      other parts of the API (used by the dragonfly/torus
 *                      models)
 * lp_type_name       - name of LP type
 * annotation         - optional annotation. If NULL, entry is considered
 *                      unannotated
 * ignore_annos       - If non-zero, then count across all annotations (and
 *                      ignore whatever annotation parameter is passed in) 
 *
 * returns the number of LPs found (0 in the case of some combination of group,
 * lp_type_name, and annotation not being found)
 */
int codes_mapping_get_lp_count(
        const char * group_name,
        int          ignore_repetitions,
        const char * lp_type_name,
        const char * annotation,
        int          ignore_annos);

/* Calculates the global LP ID given config identifying info. 
 *
 * group_name   - name of group
 * lp_type_name - name of LP type
 * annotation   - optional annotation (NULL -> unannotated)
 * ignore_anno  - ignores annotation and sets gid to the first found LP type
 *                with matching names
 * rep_id       - repetition within the group
 * offset       - lp offset within the repetition
 * gid          - output ID
 *
 * If the LP is unable to be found, a tw_error will occur
 */
void codes_mapping_get_lp_id(
        const char * group_name,
        const char * lp_type_name,
        const char * annotation,
        int          ignore_anno,
        int          rep_id,
        int          offset,
        tw_lpid    * gid);

/* Calculates the LP ID relative to other LPs (0..N-1, where N is the number of
 * LPs sharing the same type)
 *
 * gid             - LP ID
 * group_wise      - whether to compute id relative to the LP's group
 * annotation_wise - whether to compute id relative to the annotation the LP has
 */
int codes_mapping_get_lp_relative_id(
        tw_lpid gid,
        int     group_wise,
        int     annotation_wise);

/* Calculates the ROSS global LP ID of an LP given the relative ID and LP type
 * information
 *
 * relative_id     - LP id relative to set of "like" LPs
 * group_name      - name of LP's group. If non-NULL, ID is considered local
 *                   to that group. If NULL, then ID is considered across all
 *                   groups
 * lp_type_name    - name of the LP to look up. Must be provided
 * annotation      - LP's annotation. If NULL, ID is considered among
 *                   unannotated LPs
 * annotation_wise - whether to consider ID across a specific annotation (using
 *                   the annotation parameter) or all annotations (ignoring the
 *                   annotation parameter)
 */
tw_lpid codes_mapping_get_lpid_from_relative(
        int          relative_id,
        const char * group_name,
        const char * lp_type_name,
        const char * annotation,
        int          annotation_wise);


/* Returns configuration group information for a given LP-id
 *
 * gid           - LP to look up
 * group_name    - output LP group name
 * group_index   - index in config struct of group (mostly used internally)
 * lp_type_name  - output LP type name
 * lp_type_index - index in config struct of lp type (mostly used internally)
 * annotation    - output annotation (given that the caller is responsible for
 *                 providing the memory, if there's no annotation then the empty
 *                 string is returned)
 * rep_id        - output repetition within the group
 * offset        - output LP offset within the LP (for multiple lps in a rep.)
 *
 * The *_name and annotation parameters can be NULL, in which case the result
 * strings aren't copied to them. This is useful when you just need the
 * repetition ID and/or the offset. Otherwise, the caller is expected to pass in
 * properly-allocated buffers for each (of size MAX_NAME_LENGTH)
 */
void codes_mapping_get_lp_info(
        tw_lpid gid,
        char  * group_name,
        int   * group_index,
        char  * lp_type_name,
        int   * lp_type_index,
        char  * annotation,
        int   * rep_id,
        int   * offset);

//void codes_mapping_get_lp_info(tw_lpid gid, char* group_name, int* grp_id, int* lp_type_id, char* lp_type_name, int* grp_rep_id, int* offset);

/* Returns the annotation for the given LP.
 *
 * group_name   - group name of LP
 * lp_type_name - name of the LP
 *
 * NOTE: This function returns the annotation for the first found LP with
 * lp_type_name within the group. In the case of having multiple LP types with
 * different annotations in the same group, use the _by_lpid version.
 *
 * Either the annotation string or NULL (in the case of an unannotated entry) is
 * returned. */
const char* codes_mapping_get_annotation_by_name(
        const char * group_name,
        const char * lp_type_name);

/* Returns the annotation for the given LP. 
 *
 * gid - LP id to look up
 *
 * NOTE: both functions have equivalent results if there is only one LP type in
 * the requested group. The different ways of accessing are for convenience.
 * This function is the one to use if there are multiple group entries of the
 * same LP type (and different annotations)
 *
 * Either the annotation string or NULL (in the case of an unannotated entry) is
 * returned. */
const char* codes_mapping_get_annotation_by_lpid(tw_lpid gid);

/*
 * Returns a mapping of LP name to all annotations used with the type
 *
 * lp_name - lp name as used in the configuration
 */
const config_anno_map_t * 
codes_mapping_get_lp_anno_map(const char *lp_name);

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
