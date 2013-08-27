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

/*Takes the group name and returns the number of repetitions in the group */
int codes_mapping_get_group_reps(char* grp_name);

/* Takes the group name and lp type name, returns the count for that lp type */
int codes_mapping_get_lp_count(char* grp_name, char* lp_type_name);

/* Takes the group name , type name, rep ID and offset (for that lp type + repetition) and then returns the global LP ID. */
void codes_mapping_get_lp_id(char* grp_name, char* lp_type_name, int rep_id, int offset, tw_lpid* gid);

/* takes the LP ID and returns its grp name and index, lp type name and ID, repetition ID and the offset of the LP 
 * (for multiple LPs in a repetition). */
void codes_mapping_get_lp_info(tw_lpid gid, char* grp_name, int* grp_id, int* lp_type_id, char* lp_type_name, int* grp_rep_id, int* offset);


/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
