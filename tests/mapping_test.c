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

typedef struct a_state_s{
    int id_global, id_by_group, id_by_anno, id_by_group_anno;
    const char * anno;
} a_state;
typedef a_state b_state;
typedef a_state c_state;

static void a_init(a_state *ns, tw_lp *lp){
    ns->anno = codes_mapping_get_annotation_by_lpid(lp->gid);
    ns->id_global = codes_mapping_get_lp_relative_id(lp->gid, 0, 0);
    ns->id_by_group = codes_mapping_get_lp_relative_id(lp->gid, 1, 0);
    ns->id_by_anno = codes_mapping_get_lp_relative_id(lp->gid, 0, 1);
    ns->id_by_group_anno = codes_mapping_get_lp_relative_id(lp->gid, 1, 1);
}
static void b_init(b_state *ns, tw_lp *lp){ a_init(ns,lp); }
static void c_init(c_state *ns, tw_lp *lp){ a_init(ns,lp); }

static void a_finalize(a_state *ns, tw_lp *lp){
    char anno[128];
    if (ns->anno == NULL)
        anno[0] = '\0';
    else
        sprintf(anno, "@%s", ns->anno);
    printf("TEST2 %2lu %2d %2d %2d %2d a%s\n", lp->gid, ns->id_global,
            ns->id_by_group, ns->id_by_anno, ns->id_by_group_anno, anno);
}
static void b_finalize(b_state *ns, tw_lp *lp){
    char anno[128];
    if (ns->anno == NULL)
        anno[0] = '\0';
    else
        sprintf(anno, "@%s", ns->anno);
    printf("TEST2 %2lu %2d %2d %2d %2d b%s\n", lp->gid, ns->id_global, 
            ns->id_by_group, ns->id_by_anno, ns->id_by_group_anno, anno);
}
static void c_finalize(c_state *ns, tw_lp *lp){
    char anno[128];
    if (ns->anno == NULL)
        anno[0] = '\0';
    else
        sprintf(anno, "@%s", ns->anno);
    printf("TEST2 %2lu %2d %2d %2d %2d c%s\n", lp->gid, ns->id_global,
            ns->id_by_group, ns->id_by_anno, ns->id_by_group_anno, anno);
}

tw_lptype a_lp = {
     (init_f) a_init,
     (event_f) NULL,
     (revent_f) NULL,
     (final_f)  a_finalize, 
     (map_f) codes_mapping,
     sizeof(a_state),
};
tw_lptype b_lp = {
     (init_f) b_init,
     (event_f) NULL,
     (revent_f) NULL,
     (final_f)  b_finalize, 
     (map_f) codes_mapping,
     sizeof(b_state),
};
tw_lptype c_lp = {
     (init_f) c_init,
     (event_f) NULL,
     (revent_f) NULL,
     (final_f)  c_finalize, 
     (map_f) codes_mapping,
     sizeof(c_state),
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
