/*
 * Copyright (C) 2014 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#include <ross.h>
#include "codes/configuration.h"
#include "codes/codes_mapping.h"
#include "codes/codes.h"

/* define three types of lps for mapping test */

typedef struct state_s{
    int id_global, id_by_group, id_by_anno, id_by_group_anno;
    char group_name[MAX_NAME_LENGTH], lp_name[MAX_NAME_LENGTH],
         anno[MAX_NAME_LENGTH];
} state;

static void init(state *ns, tw_lp *lp){
    int dummy;
    // only need the names
    codes_mapping_get_lp_info(lp->gid, ns->group_name, &dummy,
            ns->lp_name, &dummy, ns->anno, &dummy, &dummy);
    const char * anno = codes_mapping_get_annotation_by_lpid(lp->gid);
    // annotation check
    if (    (anno == NULL && ns->anno[0] != '\0') ||
            (anno != NULL && strcmp(anno, ns->anno) != 0)){
        fprintf(stderr, "LP %lu: annotations don't match: "
                "_get_lp_info:\"%s\", _get_anno_by_lpid:\"%s\"\n",
                lp->gid, ns->anno, anno);
    }

    ns->id_global = codes_mapping_get_lp_relative_id(lp->gid, 0, 0);
    ns->id_by_group = codes_mapping_get_lp_relative_id(lp->gid, 1, 0);
    ns->id_by_anno = codes_mapping_get_lp_relative_id(lp->gid, 0, 1);
    ns->id_by_group_anno = codes_mapping_get_lp_relative_id(lp->gid, 1, 1);

    // relative ID check - looking up the gid works correctly
    tw_lpid id_from_global_rel =
        codes_mapping_get_lpid_from_relative(ns->id_global, NULL,
                ns->lp_name, NULL, 0);
    tw_lpid id_from_group_rel =
        codes_mapping_get_lpid_from_relative(ns->id_by_group, ns->group_name,
                ns->lp_name, NULL, 0);
    tw_lpid id_from_anno_rel =
        codes_mapping_get_lpid_from_relative(ns->id_by_anno, NULL,
                ns->lp_name, anno, 1);
    tw_lpid id_from_group_anno_rel =
        codes_mapping_get_lpid_from_relative(ns->id_by_group_anno, ns->group_name,
                ns->lp_name, anno, 1);
    if (lp->gid != id_from_global_rel){
        fprintf(stderr, "LP %lu (%s): "
                "global relative id %d doesn't match (got %lu)\n",
                lp->gid, ns->lp_name, ns->id_global, id_from_global_rel);
    }
    if (lp->gid != id_from_group_rel){
        fprintf(stderr, "LP %lu (%s): "
                "group %s relative id %d doesn't match (got %lu)\n",
                lp->gid, ns->lp_name, ns->group_name, ns->id_by_group,
                id_from_group_rel);
    }
    if (lp->gid != id_from_anno_rel){
        fprintf(stderr, "LP %lu (%s): "
                "anno \"%s\" relative id %d doesn't match (got %lu)\n",
                lp->gid, ns->lp_name, ns->anno, ns->id_by_group,
                id_from_anno_rel);
    }
    if (lp->gid != id_from_group_anno_rel){
        fprintf(stderr, "LP %lu (%s): "
                "group %s anno \"%s\" relative id %d doesn't match (got %lu)\n",
                lp->gid, ns->lp_name, ns->group_name, ns->anno, ns->id_by_group,
                id_from_group_anno_rel);
    }


    // output-based check - print out IDs, compare against expected
    char tmp[128];
    if (ns->anno == NULL || ns->anno[0]=='\0')
        tmp[0] = '\0';
    else
        sprintf(tmp, "@%s", ns->anno);
    printf("TEST2 %2lu %2d %2d %2d %2d %s%s\n", lp->gid, ns->id_global,
            ns->id_by_group, ns->id_by_anno, ns->id_by_group_anno, ns->lp_name,
            tmp);
}

tw_lptype a_lp = {
    (init_f) init,
    (pre_run_f) NULL,
    (event_f) NULL,
    (revent_f) NULL,
    (final_f)  NULL,
    (map_f) codes_mapping,
    sizeof(state),
};
tw_lptype b_lp = {
    (init_f) init,
    (pre_run_f) NULL,
    (event_f) NULL,
    (revent_f) NULL,
    (final_f) NULL,
    (map_f) codes_mapping,
    sizeof(state),
};
tw_lptype c_lp = {
    (init_f) init,
    (pre_run_f) NULL,
    (event_f) NULL,
    (revent_f) NULL,
    (final_f) NULL,
    (map_f) codes_mapping,
    sizeof(state),
};

static char conf_file_name[128] = {'\0'};
static const tw_optdef app_opt [] =
{
	TWOPT_GROUP("codes-mapping test case" ),
    TWOPT_CHAR("codes-config", conf_file_name, "name of codes configuration file"),
	TWOPT_END()
};

int main(int argc, char *argv[])
{
    tw_opt_add(app_opt);
    tw_init(&argc, &argv);

    if (!conf_file_name[0]){
        fprintf(stderr, "Expected \"codes-config\" option, please see --help.\n");
        MPI_Finalize();
        return 1;
    }
    if (configuration_load(conf_file_name, MPI_COMM_WORLD, &config)){
        fprintf(stderr, "Error loading config file %s.\n", conf_file_name);
        MPI_Finalize();
        return 1;
    }

    lp_type_register("a", &a_lp);
    lp_type_register("b", &b_lp);
    lp_type_register("c", &c_lp);

    codes_mapping_setup();

    printf("# test 2 format:\n"
           "# <lp> rel id <global <group-wise> <anno.-wise> <both> <lp name>\n");
            
    const char * lps[]    = {"a", "b", "c"};
    const char * groups[] = {NULL, "GRP1", "GRP2"};
    const char * annos[]  = {NULL, "foo", "bar"};
    char lpnm[128];
    for (int l = 0; l < 3; l++){
        for (int g = 0; g < 3; g++){
            for (int a = 0; a < 3; a++){
                if (annos[a]==NULL)
                    sprintf(lpnm, "%s", lps[l]);
                else
                    sprintf(lpnm, "%s@%s", lps[l], annos[a]);
                printf("TEST1 %2d %6s %s\n",
                    codes_mapping_get_lp_count(groups[g], 0, lps[l],
                        annos[a], 0),
                    groups[g], lpnm);
            }
            printf("TEST1 %2d %6s %s ignore annos\n",
                codes_mapping_get_lp_count(groups[g], 0, lps[l], NULL, 1),
                groups[g], lps[l]);
        }
    }

    tw_run();
    tw_end();

    return 0;
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
