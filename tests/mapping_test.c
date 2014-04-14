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
    int id;
} a_state;
typedef a_state b_state;
typedef a_state c_state;

static void a_init(a_state *ns, tw_lp *lp){
    ns->id = codes_mapping_get_lp_global_rel_id(lp->gid);
}
static void b_init(b_state *ns, tw_lp *lp){ a_init(ns,lp); }
static void c_init(c_state *ns, tw_lp *lp){ a_init(ns,lp); }

static void a_finalize(a_state *ns, tw_lp *lp){
    printf("TEST2 %lu %d a\n", lp->gid, ns->id);
}
static void b_finalize(b_state *ns, tw_lp *lp){
    printf("TEST2 %lu %d b\n", lp->gid, ns->id);
}
static void c_finalize(c_state *ns, tw_lp *lp){
    printf("TEST2 %lu %d c\n", lp->gid, ns->id);
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

    printf("TEST1 %d a\n", codes_mapping_get_global_lp_count("a"));
    printf("TEST1 %d b\n", codes_mapping_get_global_lp_count("b"));
    printf("TEST1 %d c\n", codes_mapping_get_global_lp_count("c"));

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
