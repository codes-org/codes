/*
 * Copyright (C) 2011, University of Chicago
 *
 * See COPYRIGHT notice in top-level directory.
 */

/* SUMMARY:
 * CODES custom mapping file for ROSS
 */
#include "configuration.h"
#include "codes.h"
#include "lp-type-lookup.h"
#define MAX_NAME_LENGTH 256

/* number of LPs assigned to the current PE (abstraction of MPI rank) */
static int lps_for_this_pe = 0;

/* data structure to hold configuration */
configuration_t config;

/* char arrays for holding lp type name and group name*/
char local_grp_name[MAX_NAME_LENGTH], local_lp_name[MAX_NAME_LENGTH];

/* Returns number of LPs on the current PE */
int codes_mapping_get_lps_for_pe(void);

/* Takes the global LP ID and returns the rank (PE id) on which the LP is mapped.*/
tw_peid codes_mapping( tw_lpid gid);

/* loads the configuration file and sets up the number of LPs on each PE. */
void codes_mapping_setup(char* filepath);

/* Takes the group name , type name, rep ID and offset (for that lp type + repetition) and then returns the global LP ID. */
void codes_mapping_get_lp_id(char* grp_name, char* lp_type_name, int rep_id, int offset, tw_lpid* gid);

/* takes the LP ID and returns its grp name and index, lp type name and ID, repetition ID and the offset of the LP 
 * (for multiple LPs in a repetition). */
void codes_mapping_get_lp_info(tw_lpid gid, char* grp_name, int* grp_id, int* lp_type_id, char* lp_type_name, int* grp_rep_id, int* offset);

/* assigns local and global lp ids for ROSS. */
void codes_mapping_init(void);

/* Takes the global LP ID, maps it to the local LP ID and returns the LP.
 * lps have global and local LP IDs. 
 * global LP IDs are unique across all PEs, local LP IDs are unique within a PE. */
tw_lp * codes_mapping_to_lp( tw_lpid lpid);

