/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#include <string.h>
#include <assert.h>
#include <ross.h>

#include "codes/model-net.h"
#include "codes/lp-io.h"
#include "codes/codes.h"
#include "codes/codes_mapping.h"
#include "codes/configuration.h"
#include "codes/lp-type-lookup.h"
#include "codes/lp-msg.h"
#include "codes/model-net-sched.h"

#define PAYLOAD_SZ 8192 /* size of simulated data payload, bytes  */
#define NUM_PRIOS 10
#define NUM_SERVERS 2 

static int net_id = 0;
static int prog_rtn = 0;

typedef struct svr_msg svr_msg;
typedef struct svr_state svr_state;

/* types of events that will constitute triton requests */
enum svr_event
{
    KICKOFF,    /* initial event */
    RECV,       /* message received at sink */
    ACK,        /* message received at source */
};

struct svr_state
{
    int server_idx;
    int num_recv[NUM_SERVERS];
    // for receiver - order messages are recv'd
    // NOTE: we're doing a many-to-one to generate some rollbacks
    int recv_order[NUM_SERVERS][NUM_PRIOS];
    // for sender - order messages are sent
    int random_order[NUM_PRIOS];
};

struct svr_msg
{
    msg_header h;
    int src_svr_idx;
    int msg_prio; // to check against message receipt order
};

static void svr_init(
    svr_state * ns,
    tw_lp * lp);
static void svr_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp);
static void svr_rev_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp);
static void svr_finalize(
    svr_state * ns,
    tw_lp * lp);

tw_lptype svr_lp = {
    (init_f) svr_init,
    (pre_run_f) NULL,
    (event_f) svr_event,
    (revent_f) svr_rev_event,
    (final_f)  svr_finalize, 
    (map_f) codes_mapping,
    sizeof(svr_state),
};

static void svr_add_lp_type();
static tw_stime s_to_ns(tw_stime ns);
static void handle_kickoff_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp);
static void handle_recv_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp);
static void handle_kickoff_rev_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp);
static void handle_recv_rev_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp);

const tw_optdef app_opt [] =
{
	TWOPT_GROUP("Model net test case" ),
	TWOPT_END()
};

int main(
    int argc,
    char **argv)
{
    int num_nets;
    int *net_ids;
    g_tw_ts_end = s_to_ns(60*60*24*365); /* one year, in nsecs */

    tw_opt_add(app_opt);
    tw_init(&argc, &argv);

    if(argc < 2)
    {
	    printf("\n Usage: mpirun <args> --sync=[1,3] -- mapping_file_name.conf (optional --nkp) ");
	    MPI_Finalize();
	    return 0;
    }

    configuration_load(argv[2], MPI_COMM_WORLD, &config);

    model_net_register();
    svr_add_lp_type();
    
    codes_mapping_setup();
    
    net_ids = model_net_configure(&num_nets);
    assert(num_nets==1);
    net_id = *net_ids;
    free(net_ids);

    assert(net_id == SIMPLENET);
    assert(NUM_SERVERS == codes_mapping_get_lp_count("MODELNET_GRP", 0,
                "server", NULL, 1));

    tw_run();

    tw_end();
    return prog_rtn;
}

static void svr_add_lp_type()
{
  lp_type_register("server", &svr_lp);
}

static void svr_init(
    svr_state * ns,
    tw_lp * lp)
{
    ns->server_idx = lp->gid / 2;
    if (ns->server_idx < NUM_SERVERS-1){
        for (int i = 0; i < NUM_PRIOS; i++){
            ns->random_order[i] = -1;
        }
        for (int i = 0; i < NUM_PRIOS; i++){
            for (;;){
                int idx = tw_rand_integer(lp->rng, 0, NUM_PRIOS-1);
                // not sure whether rand_integer is inclusive or not...
                assert(idx < NUM_PRIOS);
                if (ns->random_order[idx] == -1){
                    ns->random_order[idx] = i;
                    break;
                }
            }
        }
        tw_event *e = codes_event_new(lp->gid, codes_local_latency(lp), lp);
        svr_msg * m = tw_event_data(e);
        msg_set_header(666, KICKOFF, lp->gid, &m->h);
        tw_event_send(e);
    }
    else {
        memset(ns->num_recv, 0, NUM_SERVERS*sizeof(*ns->num_recv));
    }
}

static void svr_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp)
{
   switch (m->h.event_type)
    {
        case KICKOFF:
            handle_kickoff_event(ns, b, m, lp);
            break;
        case RECV:
            handle_recv_event(ns, b, m, lp);
            break;
        default:
	    printf("\n Invalid message type %d ", m->h.event_type);
            assert(0);
        break;
    }
}

static void svr_rev_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp)
{
    switch (m->h.event_type)
    {
        case KICKOFF:
            handle_kickoff_rev_event(ns, b, m, lp);
            break;
        case RECV:
            handle_recv_rev_event(ns, b, m, lp);
            break;
        default:
            assert(0);
            break;
    }

    return;
}

static void svr_finalize(
    svr_state * ns,
    tw_lp * lp)
{
    if (ns->server_idx != NUM_SERVERS-1)
        return;

    int errs[NUM_SERVERS];
    memset(errs, 0, NUM_SERVERS*sizeof(*errs));
    for (int i = 0; i < NUM_SERVERS-1; i++){
        for (int j = 0; j < NUM_PRIOS; j++){
            if (ns->recv_order[i][j] != j){
                errs[i]++;
            }
        }
    }
    for (int i = 0; i < NUM_SERVERS-1; i++){
        if (errs[i] > 0){
            fprintf(stderr, "ERROR: received from server %d in "
                    "non-priority order:\n", i);
            fprintf(stderr, "     ");
            for (int j = 0; j < NUM_PRIOS; j++){
                fprintf(stderr, " %d", ns->recv_order[i][j]);
            }
            fprintf(stderr, "\n");
            prog_rtn = 1;
        }
    }
}

/* convert seconds to ns */
static tw_stime s_to_ns(tw_stime ns)
{
    return(ns * (1000.0 * 1000.0 * 1000.0));
}

/* handle initial event */
static void handle_kickoff_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp)
{
    assert(ns->server_idx < NUM_SERVERS-1);

    // same msg header for all
    svr_msg m_remote;
    msg_set_header(666, RECV, lp->gid, &m_remote.h);
    m_remote.src_svr_idx = ns->server_idx;

    // dest LP is the same - the last server
    tw_lpid dest = (NUM_SERVERS-1) * 2;

    MN_START_SEQ();
    for (int i = 0; i < NUM_PRIOS; i++){
        m_remote.msg_prio = ns->random_order[i];
        //printf("%lu: sending message with prio %d to %lu\n", lp->gid,
                //m_remote.msg_prio, dest);
        model_net_set_msg_param(MN_MSG_PARAM_SCHED, MN_SCHED_PARAM_PRIO,
                (void*) &m_remote.msg_prio);
        model_net_event(net_id, "test", dest, PAYLOAD_SZ, 0.0, sizeof(svr_msg),
                &m_remote, 0, NULL, lp);
    }
    MN_END_SEQ();
}

static void handle_kickoff_rev_event(
        svr_state *ns,
        tw_bf *b,
        svr_msg *m,
        tw_lp *lp){
    assert(ns->server_idx < NUM_SERVERS-1);
    for (int i = 0; i < NUM_PRIOS; i++){
        model_net_event_rc(net_id, lp, PAYLOAD_SZ);
    }
}

static void handle_recv_event(
		svr_state * ns,
		tw_bf * b,
		svr_msg * m,
		tw_lp * lp)
{
    assert(ns->server_idx == NUM_SERVERS-1);
    //printf("%lu received msg prio %d from %d (lp %lu)\n",
            //lp->gid, m->msg_prio, m->src_svr_idx, m->h.src);
    int s = m->src_svr_idx;
    ns->recv_order[s][ns->num_recv[s]] = m->msg_prio;
    ns->num_recv[s]++;
}

static void handle_recv_rev_event(
	       svr_state * ns,
	       tw_bf * b,
	       svr_msg * m,
	       tw_lp * lp)
{
    ns->num_recv[m->src_svr_idx]--;
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
