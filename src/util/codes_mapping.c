/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

/* SUMMARY:
 * CODES custom mapping file for ROSS
 */
#include "codes/codes_mapping.h"

#define CODES_MAPPING_DEBUG 0

/* number of LPs assigned to the current PE (abstraction of MPI rank).
 * for lp counts which are not divisible by the number of ranks, keep 
 * modulus around */
static int lps_per_pe_floor = 0;
static int lps_leftover = 0;

static int mem_factor = 256;

static int mini(int a, int b){ return a < b ? a : b; }

// compare passed in annotation strings (NULL or nonempty) against annotation
// strings in the config (empty or nonempty)
static int cmp_anno(const char * anno_user, const char * anno_config){
    return anno_user == NULL ? anno_config[0]=='\0'
                             : !strcmp(anno_user, anno_config);
}


#if 0
// TODO: this code seems useful, but I'm not sure where to put it for the time
// being. It should be useful when we are directly reading from the
// configuration file, which has the unprocessed annotated strings

// return code matches that of strcmp and friends, checks
// lp_type_name@annotation against full name
// NOTE: no argument should be NULL or invalid strings
static int strcmp_anno(
        const char * full_name,
        const char * prefix,
        const char * annotation){
    int i;
    // first phase - check full_name against prefix
    for (i = 0; i < MAX_NAME_LENGTH; i++){
        char cf = full_name[i], cl = prefix[i];
        if (cl == '\0')
            break; // match successful
        else if (cf != cl) // captures case where cf is null char or @
            return (int)(cf-cl); // lp name on full doesn't match
    }
    if (i==MAX_NAME_LENGTH) return 1;
    // second phase - ensure the next character after the match is an @
    if (full_name[i] != '@') return -1;
    // third phase, compare the remaining full_name against annotation
    return strcmp(full_name+i+1, annotation);
}
#endif

/* char arrays for holding lp type name and group name*/
// (unused var atm) static char local_group_name[MAX_NAME_LENGTH];
static char local_lp_name[MAX_NAME_LENGTH],
            local_annotation[MAX_NAME_LENGTH];

int codes_mapping_get_lps_for_pe()
{
    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
#if CODES_MAPPING_DEBUG
    printf("%d lps for rank %d\n", lps_per_pe_floor+(g_tw_mynode < lps_leftover), rank);
#endif
  return lps_per_pe_floor + (g_tw_mynode < lps_leftover);
}

/* Takes the global LP ID and returns the rank (PE id) on which the LP is mapped */
tw_peid codes_mapping( tw_lpid gid)
{
    int lps_on_pes_with_leftover = lps_leftover * (lps_per_pe_floor+1);
    if (gid < lps_on_pes_with_leftover){
        return gid / (lps_per_pe_floor+1);
    }
    else{
        return (gid-lps_on_pes_with_leftover)/lps_per_pe_floor + lps_leftover;
    }
  /*return gid / lps_per_pe_floor;*/
}

int codes_mapping_get_group_reps(const char* group_name)
{
  int grp;
  for(grp = 0; grp < lpconf.lpgroups_count; grp++)
  {
     if(strcmp(lpconf.lpgroups[grp].name, group_name) == 0)
	     return lpconf.lpgroups[grp].repetitions;
  }
  return -1;
}

int codes_mapping_get_lp_count(
        const char * group_name,
        int          ignore_repetitions,
        const char * lp_type_name,
        const char * annotation,
        int          ignore_annos){
    int lp_type_ct_total = 0;

    // check - if group name is null, then disable ignore_repetitions (the
    // former takes precedence)
    if (group_name == NULL) 
        ignore_repetitions = 0;
    for (int g = 0; g < lpconf.lpgroups_count; g++){
        const config_lpgroup_t *lpg = &lpconf.lpgroups[g];
        // iterate over the lps if the group is null (count across all groups)
        // or if the group names match
        if (group_name == NULL || strcmp(lpg->name, group_name) == 0){
            for (int l = 0; l < lpg->lptypes_count; l++){
                const config_lptype_t *lpt = &lpg->lptypes[l];
                if (strcmp(lp_type_name, lpt->name) == 0){
                    // increment the count if we are ignoring annotations,
                    // query and entry are both unannotated, or if the
                    // annotations match
                    if (ignore_annos || cmp_anno(annotation, lpt->anno)){
                        if (ignore_repetitions)
                            lp_type_ct_total += lpt->count;
                        else
                            lp_type_ct_total += lpt->count * lpg->repetitions;
                    }
                }
            }
            if (group_name != NULL) break; // we've processed the exact group
        }
    }
    // no error needed here - 0 is a valid value
    return lp_type_ct_total;
}

void codes_mapping_get_lp_id(
        const char * group_name,
        const char * lp_type_name,
        const char * annotation,
        int          ignore_anno,
        int          rep_id,
        int          offset,
        tw_lpid    * gid){
    // sanity checks
    if (rep_id < 0 || offset < 0 || group_name == NULL || lp_type_name == NULL)
        goto ERROR;
    *gid = 0;
    // for each group
    for (int g = 0; g < lpconf.lpgroups_count; g++){
        const config_lpgroup_t *lpg = &lpconf.lpgroups[g];
        // in any case, count up the lps (need for repetition handling)
        tw_lpid rep_count = 0;
        for (int l = 0; l < lpg->lptypes_count; l++){
            rep_count += lpg->lptypes[l].count;
        }
        // does group name match?
        if (strcmp(lpg->name, group_name) == 0){
            tw_lpid local_lp_count = 0;
            // for each lp type
            for (int l = 0; l < lpg->lptypes_count; l++){
                const config_lptype_t *lpt = &lpg->lptypes[l];
                // does lp name match?
                if (strcmp(lpt->name, lp_type_name) == 0){
                    // does annotation match (or are we ignoring annotations?)
                    if (ignore_anno || cmp_anno(annotation, lpt->anno)){
                        // return if sane offset 
                        if (offset >= (int) lpt->count){
                            goto ERROR;
                        }
                        else{
                            *gid += (rep_count * (tw_lpid)rep_id) + 
                                local_lp_count + (tw_lpid)offset;
                            return;
                        }
                    }
                }
                local_lp_count += lpt->count;
            }
            // after a group match, short circuit to end if lp not found
            goto ERROR;
        }
        // group name doesn't match, add count to running total and move on
        else{
            *gid += lpg->repetitions * rep_count;
        }
    }
ERROR:
    // LP not found
    tw_error(TW_LOC, "Unable to find LP id given "
                     "group \"%s\", "
                     "typename \"%s\", "
                     "annotation \"%s\", "
                     "repetition %d, and offset %d",
                     group_name==NULL   ? "<NULL>" : group_name,
                     lp_type_name==NULL ? "<NULL>" : lp_type_name,
                     annotation==NULL   ? "<NULL>" : annotation,
                     rep_id, offset);
}

int codes_mapping_get_lp_relative_id(
        tw_lpid gid,
        int     group_wise,
        int     annotation_wise){
    int group_index, lp_type_index, rep_id, offset;
    codes_mapping_get_lp_info(gid, NULL, &group_index,
            local_lp_name, &lp_type_index, local_annotation, &rep_id, &offset);
    const char * anno = (local_annotation[0]=='\0') ? NULL : local_annotation;

    uint64_t group_lp_count = 0;
    // if not group_specific, then count up LPs of all preceding groups
    if (!group_wise){
        for (int g = 0; g < group_index; g++){
            uint64_t lp_count = 0;
            const config_lpgroup_t *lpg = &lpconf.lpgroups[g];
            for (int l = 0; l < lpg->lptypes_count; l++){
                const config_lptype_t *lpt = &lpg->lptypes[l];
                if (strcmp(local_lp_name, lpt->name) == 0){
                    // increment the count if we are ignoring annotations,
                    // both LPs are unannotated, or if the annotations match
                    if (!annotation_wise || cmp_anno(anno, lpt->anno)){
                        lp_count += lpt->count;
                    }
                }
            }
            group_lp_count += lp_count * lpg->repetitions;
        }
    }
    // count up LPs within my group occuring before me 
    // (the loop is necessary because different annotated LPs may exist in the
    // same group)
    uint64_t lp_count = 0;
    uint64_t lp_pre_count = 0;
    for (int l = 0; l < lpconf.lpgroups[group_index].lptypes_count; l++){
        const config_lptype_t *lpt = &lpconf.lpgroups[group_index].lptypes[l];
        if (strcmp(local_lp_name, lpt->name) == 0){
            if (!annotation_wise || cmp_anno(anno, lpt->anno)){
                lp_count += lpt->count;
                // special case: if we find an LP entry that matches, but is not
                // the same entry where the input gid comes from, then increment
                // a separate "pre" counter
                if (l < lp_type_index){
                    lp_pre_count += lpt->count;
                }
            }
        }
    }
    // now we have the necessary counts
    // return lps in groups that came before + 
    // lps within the group that came before the target LP
    return (int) (group_lp_count + (lp_count * rep_id) + lp_pre_count + offset);
}

tw_lpid codes_mapping_get_lpid_from_relative(
        int          relative_id,
        const char * group_name,
        const char * lp_type_name,
        const char * annotation,
        int          annotation_wise){
    // strategy: count up all preceding LPs. When we reach a point where an LP
    // type matches, count up the relative ID. When the accumulated relative ID
    // matches or surpasses the input, then we've found our LP
    int rel_id_count = 0;
    tw_lpid gid_count = 0;
    for (int g = 0; g < lpconf.lpgroups_count; g++){
        const config_lpgroup_t *lpg = &lpconf.lpgroups[g];
        if (group_name == NULL || strcmp(group_name, lpg->name) == 0){
            // consider this group for counting
            tw_lpid local_gid_count = 0;
            int local_rel_id_count = 0;
            for (int l = 0; l < lpg->lptypes_count; l++){
                const config_lptype_t *lpt = &lpg->lptypes[l];
                local_gid_count += lpt->count;
                if (strcmp(lp_type_name, lpt->name) == 0 &&
                        (!annotation_wise || cmp_anno(annotation, lpt->anno))){
                    local_rel_id_count += lpt->count;
                }
            }
            // is our relative id within this group?
            if (relative_id < rel_id_count +
                    lpg->repetitions * local_rel_id_count){
                tw_lpid gid = gid_count;
                int rem = relative_id - rel_id_count;
                int rep = rem / local_rel_id_count;
                rem -= (rep * local_rel_id_count);
                gid += local_gid_count * rep;
                // count up lps listed prior to this entry
                for (int l = 0; l < lpg->lptypes_count; l++){
                    const config_lptype_t *lpt = &lpg->lptypes[l];
                    if (    strcmp(lp_type_name, lpt->name) == 0 &&
                            (!annotation_wise ||
                            cmp_anno(annotation, lpt->anno))){
                        if (rem < (int) lpt->count){
                            return gid + (tw_lpid) rem;
                        }
                        else{
                            rem -= lpt->count;
                        }
                    }
                    gid += lpt->count;
                }
                // this shouldn't happen
                goto NOT_FOUND;
            }
            else if (group_name != NULL){
                // immediate error - found the group, but not the id
                goto NOT_FOUND;
            }
            else{
                // increment and move to the next group
                rel_id_count += local_rel_id_count * lpg->repetitions;
                gid_count    += local_gid_count    * lpg->repetitions;
            }
        }
        else{
            // just count up the group LPs
            tw_lpid local_gid_count = 0;
            for (int l = 0; l < lpg->lptypes_count; l++){
                local_gid_count += lpg->lptypes[l].count;
            }
            gid_count += local_gid_count * lpg->repetitions;
        }
    }
NOT_FOUND:
    tw_error(TW_LOC, "Unable to find LP-ID for ID %d relative to group %s, "
            "lp name %s, annotation %s",
            relative_id,
            group_name == NULL ? "(all)" : group_name,
            lp_type_name,
            annotation_wise ? annotation : "(all)");
    return 0; // dummy return
}

void codes_mapping_get_lp_info(
        tw_lpid gid,
        char  * group_name,
        int   * group_index,
        char  * lp_type_name,
        int   * lp_type_index,
        char  * annotation,
        int   * rep_id,
        int   * offset){
    // running total of lp's we've seen so far
    tw_lpid id_total = 0;
    // for each group
    for (int g = 0; g < lpconf.lpgroups_count; g++){
        const config_lpgroup_t *lpg = &lpconf.lpgroups[g];
        tw_lpid num_id_group, num_id_per_rep = 0;
        // count up the number of ids in this group
        for (int l = 0; l < lpg->lptypes_count; l++){
            num_id_per_rep += lpg->lptypes[l].count;
        }
        num_id_group = num_id_per_rep * lpg->repetitions;
        if (num_id_group+id_total > gid){
            // we've found the group
            tw_lpid rem = gid - id_total;
            if (group_name != NULL)
                strncpy(group_name, lpg->name, MAX_NAME_LENGTH);
            *group_index = g;
            // find repetition within group
            *rep_id = (int) (rem / num_id_per_rep);
            rem -=  num_id_per_rep * (tw_lpid)*rep_id;
            num_id_per_rep = 0;
            for (int l = 0; l < lpg->lptypes_count; l++){
                const config_lptype_t *lpt = &lpg->lptypes[l];
                if (rem < num_id_per_rep + lpt->count){
                    // found the specific LP
                    if (lp_type_name != NULL)
                        strncpy(lp_type_name, lpt->name, MAX_NAME_LENGTH);
                    if (annotation != NULL)
                        strncpy(annotation, lpt->anno, MAX_NAME_LENGTH);
                    *offset = (int) (rem - num_id_per_rep);
                    *lp_type_index = l;
                    return; // done
                }
                else{
                    num_id_per_rep += lpg->lptypes[l].count;
                }
            }
        }
        else{
            id_total += num_id_group;
        }
    }
    // LP not found
    tw_error(TW_LOC, "Unable to find LP info given gid %lu", gid);
}

/* This function assigns local and global LP Ids to LPs */
static void codes_mapping_init(void)
{
     int grp_id, lpt_id, rep_id, offset;
     tw_lpid ross_gid, ross_lid; /* ross global and local IDs */
     tw_pe * pe;
     char lp_type_name[MAX_NAME_LENGTH];
     int nkp_per_pe = g_tw_nkp;
     tw_lpid         lpid, kpid;
     const tw_lptype *lptype;

     /* have 16 kps per pe, this is the optimized configuration for ROSS custom mapping */
     for(kpid = 0; kpid < nkp_per_pe; kpid++)
	tw_kp_onpe(kpid, g_tw_pe[0]);

     int lp_start =
         g_tw_mynode * lps_per_pe_floor + mini(g_tw_mynode,lps_leftover);
     int lp_end =
         (g_tw_mynode+1) * lps_per_pe_floor + mini(g_tw_mynode+1,lps_leftover);

     for (lpid = lp_start; lpid < lp_end; lpid++)
      {
	 ross_gid = lpid;
	 ross_lid = lpid - lp_start;
	 kpid = ross_lid % g_tw_nkp;
	 pe = tw_getpe(kpid % g_tw_npe);
	 codes_mapping_get_lp_info(ross_gid, NULL, &grp_id, lp_type_name,
                 &lpt_id, NULL, &rep_id, &offset);
#if CODES_MAPPING_DEBUG
         printf("lp:%lu --> kp:%lu, pe:%llu\n", ross_gid, kpid, pe->id);
#endif
	 tw_lp_onpe(ross_lid, pe, ross_gid);
	 tw_lp_onkp(g_tw_lp[ross_lid], g_tw_kp[kpid]);
         lptype = lp_type_lookup(lp_type_name);
         if (lptype == NULL)
             tw_error(TW_LOC, "could not find LP with type name \"%s\", "
                     "did you forget to register the LP?\n", lp_type_name);
         else
             /* sorry, const... */
             tw_lp_settype(ross_lid, (tw_lptype*) lptype);
     }
     return;
}

/* This function takes the global LP ID, maps it to the local LP ID and returns the LP 
 * lps have global and local LP IDs
 * global LP IDs are unique across all PEs, local LP IDs are unique within a PE */
static tw_lp * codes_mapping_to_lp( tw_lpid lpid)
{
   int index = lpid - (g_tw_mynode * lps_per_pe_floor) -
       mini(g_tw_mynode, lps_leftover);
//   printf("\n global id %d index %d lps_before %d lps_offset %d local index %d ", lpid, index, lps_before, g_tw_mynode, local_index);
   return g_tw_lp[index];
}

/* This function loads the configuration file and sets up the number of LPs on each PE */
void codes_mapping_setup_with_seed_offset(int offset)
{
  int grp, lpt, message_size;
  int pes = tw_nnodes();

  lps_per_pe_floor = 0;
  for (grp = 0; grp < lpconf.lpgroups_count; grp++)
   {
    for (lpt = 0; lpt < lpconf.lpgroups[grp].lptypes_count; lpt++)
	lps_per_pe_floor += (lpconf.lpgroups[grp].lptypes[lpt].count * lpconf.lpgroups[grp].repetitions);
   }
  tw_lpid global_nlps = lps_per_pe_floor;
  lps_leftover = lps_per_pe_floor % pes;
  lps_per_pe_floor /= pes;
 //printf("\n LPs for this PE are %d reps %d ", lps_per_pe_floor,  lpconf.lpgroups[grp].repetitions);
  g_tw_mapping=CUSTOM;
  g_tw_custom_initial_mapping=&codes_mapping_init;
  g_tw_custom_lp_global_to_local_map=&codes_mapping_to_lp;

  // configure mem-factor
  int mem_factor_conf;
  int rc = configuration_get_value_int(&config, "PARAMS", "pe_mem_factor", NULL,
          &mem_factor_conf);
  if (rc == 0 && mem_factor_conf > 0)
    mem_factor = mem_factor_conf;

  g_tw_events_per_pe = mem_factor * codes_mapping_get_lps_for_pe();
  configuration_get_value_int(&config, "PARAMS", "message_size", NULL, &message_size);
  if(!message_size)
  {
      message_size = 256;
      printf("\n Warning: ross message size not defined, resetting it to %d", message_size);
  }

  // we increment the number of RNGs used to let codes_local_latency use the
  // last one
  g_tw_nRNG_per_lp++;

  tw_define_lps(codes_mapping_get_lps_for_pe(), message_size, 0);

  // use a similar computation to codes_mapping_init to compute the lpids and
  // offsets to use in tw_rand_initial_seed
  // an "offset" of 0 reverts to default RNG seeding behavior - see
  // ross/rand-clcg4.c for the specific computation
  // an "offset" < 0 is ignored
  if (offset > 0){
      for (tw_lpid l = 0; l < g_tw_nlp; l++){
          for (int i = 0; i < g_tw_nRNG_per_lp; i++){
              tw_rand_initial_seed(&g_tw_lp[l]->rng[i], (g_tw_lp[l]->gid +
                          global_nlps * offset) * g_tw_nRNG_per_lp + i);
          }
      }
  }
}

void codes_mapping_setup(){
    codes_mapping_setup_with_seed_offset(0);
}

/* given the group and LP type name, return the annotation (or NULL) */
const char* codes_mapping_get_annotation_by_name(
        const char * group_name,
        const char * lp_type_name){
    for (int g = 0; g < lpconf.lpgroups_count; g++){
        const config_lpgroup_t *lpg = &lpconf.lpgroups[g];
        if (strcmp(lpg->name, group_name) == 0){
            // group found, iterate through types
            for (int l = 0; l < lpg->lptypes_count; l++){
                const config_lptype_t *lpt = &lpg->lptypes[l];
                if (strcmp(lpt->name, lp_type_name) == 0){
                    // type found, return the annotation
                    if (lpt->anno[0] == '\0')
                        return NULL;
                    else
                        return lpt->anno;
                }
            }
        }
    }
    tw_error(TW_LOC, "unable to find annotation using "
            "group \"%s\" and lp_type_name \"%s\"", group_name, lp_type_name);
    return NULL;
}

const char* codes_mapping_get_annotation_by_lpid(tw_lpid gid){
    int group_index, lp_type_index, dummy;
    codes_mapping_get_lp_info(gid, NULL, &group_index, NULL, &lp_type_index,
            NULL, &dummy, &dummy);
    const char * anno = 
        lpconf.lpgroups[group_index].lptypes[lp_type_index].anno;
    if (anno[0] == '\0')
        return NULL;
    else
        return anno;
}

/*
 * Returns a mapping of LP name to all annotations used with the type
 *
 * lp_name - lp name as used in the configuration
 */
const config_anno_map_t * 
codes_mapping_get_lp_anno_map(const char *lp_name){
    for (uint64_t i = 0; i < lpconf.lpannos_count; i++){
        if (strcmp(lp_name, lpconf.lpannos[i].lp_name) == 0){
            return &lpconf.lpannos[i];
        }
    }
    return NULL;
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
