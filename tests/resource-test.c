/*
 * Copyright (C) 2014 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
*/

#include <codes/resource.h>
#include <codes/resource-lp.h>
#include <codes/lp-msg.h>
#include <codes/configuration.h>
#include <codes/codes_mapping.h>
#include <codes/codes-callback.h>
#include <codes/codes-mapping-context.h>
#include <stdint.h>

static int bsize = 1024;

static int s_magic = 12345;

static uint64_t maxu64(uint64_t a, uint64_t b) { return a < b ? b : a; }

enum s_type {
    S_KICKOFF,
    S_ALLOC_ACK,
    S_FREE,
};

typedef struct {
    int id;
    uint64_t mem, mem_max;
    struct codes_cb_info cb;
} s_state;

typedef struct {
    msg_header h;
    resource_return c;
    int tag;
    uint64_t mem_max_prev;
} s_msg;

static void s_init(s_state *ns, tw_lp *lp){
    ns->mem = 0;
    ns->mem_max = 0;
    INIT_CODES_CB_INFO(&ns->cb, s_msg, h, tag, c);
    ns->id = codes_mapping_get_lp_relative_id(lp->gid, 0, 0);
    tw_event *e = tw_event_new(lp->gid, codes_local_latency(lp), lp);
    s_msg *m = tw_event_data(e);
    msg_set_header(s_magic, S_KICKOFF, lp->gid, &m->h);
    tw_event_send(e);
}
static void s_finalize(s_state *ns, tw_lp *lp){
    (void)lp;
    printf("Server %d got %llu memory before failing\n", ns->id, LLU(ns->mem_max));
}

static void s_event(s_state *ns, tw_bf *bf, s_msg *m, tw_lp *lp){
    (void)bf;
    assert(m->h.magic == s_magic);
    msg_header h;
    switch(m->h.event_type){
        case S_KICKOFF: ;
            msg_set_header(s_magic, S_ALLOC_ACK, lp->gid, &h);
            resource_lp_get(bsize, 0, lp, CODES_MCTX_DEFAULT, 0, &h, &ns->cb);
            break;
        case S_ALLOC_ACK:
            if (m->c.ret == 0){
                ns->mem += bsize;
                m->mem_max_prev = ns->mem_max;
                ns->mem_max = maxu64(ns->mem, ns->mem_max);
                msg_set_header(s_magic, S_ALLOC_ACK, lp->gid, &h);
                resource_lp_get(bsize, 0, lp, CODES_MCTX_DEFAULT, 0, &h,
                        &ns->cb);
                break;
            }
            /* else fall into the free stmt */
        case S_FREE:
            resource_lp_free(bsize, lp, CODES_MCTX_DEFAULT);
            ns->mem -= bsize;
            if (ns->mem > 0){
                tw_event *e =
                    tw_event_new(lp->gid, codes_local_latency(lp), lp);
                s_msg *sm = tw_event_data(e);
                msg_set_header(s_magic, S_FREE, lp->gid, &sm->h);
                tw_event_send(e);
            }
            break;
    }
}
static void s_event_rc(s_state *ns, tw_bf * b, s_msg *m, tw_lp *lp){
    (void)b;
    assert(m->h.magic == s_magic);
    switch(m->h.event_type){
        case S_KICKOFF:
            resource_lp_get_rc(lp);
            break;
        case S_ALLOC_ACK:
            if (m->c.ret == 0){
                ns->mem -= bsize;
                ns->mem_max = m->mem_max_prev;
                resource_lp_get_rc(lp);
                break;
            }
            /* else fall into the free stmt */
        case S_FREE:
            /* undoing is unconditional given this lps logic */
            resource_lp_free_rc(lp);
            if (ns->mem > 0){
                codes_local_latency_reverse(lp);
            }
            ns->mem += bsize;
    }
}

static tw_lptype s_lp = {
    (init_f) s_init,
    (pre_run_f) NULL,
    (event_f) s_event,
    (revent_f) s_event_rc,
    (commit_f) NULL,
    (final_f) s_finalize,
    (map_f) codes_mapping,
    sizeof(s_state),
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
    g_tw_ts_end = 1e9*60*60*24*365; /* one year, in nsecs */
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

    resource_lp_init();
    lp_type_register("nw-lp", &s_lp);

    codes_mapping_setup();

    resource_lp_configure();

    tw_run();
    tw_end();
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
