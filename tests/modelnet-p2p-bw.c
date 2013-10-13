/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

/* SUMMARY:
 *
 * This test program will execute a one way, point to point bandwidth test
 * between two hosts using the specified modelnet method.  The intention is
 * to roughly mimic the behavior of a standard bandwidth test such as
 * mpptest.
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

/* TODO: these things should probably be configurable */
#define NUM_PINGPONGS 1000 /* number of pingpong exchanges per msg size */
#define MIN_SZ 4
#define NUM_SZS 25

static int net_id = 0;
static int num_routers = 0;
static int num_servers = 0;
static int offset = 2;

typedef struct svr_msg svr_msg;
typedef struct svr_state svr_state;

struct pingpong_stat
{
    int msg_sz;
    tw_stime start_time;
    tw_stime end_time;
};
struct pingpong_stat stat_array[25];

/* types of events that will constitute triton requests */
enum svr_event
{
    PING = 1,        /* request event */
    PONG,        /* ack event */
};

struct svr_state
{
    int pingpongs_completed; 
    int svr_idx;
};

struct svr_msg
{
    enum svr_event svr_event_type;
    tw_lpid src;          /* source of this request or ack */
    int size;
    int sent_size;        /* for rc */
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
     (event_f) svr_event,
     (revent_f) svr_rev_event,
     (final_f)  svr_finalize, 
     (map_f) codes_mapping,
     sizeof(svr_state),
};

extern const tw_lptype* svr_get_lp_type();
static void svr_add_lp_type();
#if 0
static tw_stime ns_to_s(tw_stime ns);
#endif
static tw_stime s_to_ns(tw_stime ns);
static void handle_pong_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp);
static void handle_ping_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp);
static void handle_pong_rev_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp);
static void handle_ping_rev_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp);

const tw_optdef app_opt [] =
{
	TWOPT_GROUP("Model net point to point ping pong benchmark" ),
	TWOPT_END()
};

int main(
    int argc,
    char **argv)
{
    int nprocs;
    int rank;
    g_tw_ts_end = s_to_ns(60*60*24*365); /* one year, in nsecs */

    tw_opt_add(app_opt);
    tw_init(&argc, &argv);

    if(argc < 2)
    {
	    printf("\n Usage: mpirun <args> --sync=2/3 -- mapping_file_name.conf\n");
	    MPI_Finalize();
	    return 0;
    }
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nprocs);
  
    configuration_load(argv[2], MPI_COMM_WORLD, &config);
    net_id=model_net_set_params();
    svr_add_lp_type();
   
    codes_mapping_setup();
    
    num_servers = codes_mapping_get_group_reps("MODELNET_GRP") * codes_mapping_get_lp_count("MODELNET_GRP", "server");
    assert(num_servers == 2);
    if(net_id == DRAGONFLY)
    {
	  num_routers = codes_mapping_get_group_reps("MODELNET_GRP") * codes_mapping_get_lp_count("MODELNET_GRP", "dragonfly_router"); 
	  offset = 1;
    }

    tw_run();
    model_net_report_stats(net_id);

    tw_end();
    return 0;
}

const tw_lptype* svr_get_lp_type()
{
	    return(&svr_lp);
}

static void svr_add_lp_type()
{
  lp_type_register("server", svr_get_lp_type());
}

static void svr_init(
    svr_state * ns,
    tw_lp * lp)
{
    tw_event *e;
    svr_msg *m;
    tw_stime kickoff_time;
    char grp_name[MAX_NAME_LENGTH];
    char lp_type_name[MAX_NAME_LENGTH];
    int grp_id, lp_type_id, grp_rep_id, offset;
    int i;
    
    memset(ns, 0, sizeof(*ns));

    /* find my own server index */
    codes_mapping_get_lp_info(lp->gid, grp_name, &grp_id,
            &lp_type_id, lp_type_name, &grp_rep_id, &offset);
    ns->svr_idx = grp_rep_id;

    /* first server sends a dummy event to itself that will kick off the real
     * simulation
     */
    if(ns->svr_idx == 0)
    {
        /* initialize statistics; measured only at first server */
        ns->pingpongs_completed = -1;
        stat_array[0].msg_sz = MIN_SZ;
        for(i=1; i<NUM_SZS; i++)
            stat_array[i].msg_sz = stat_array[i-1].msg_sz * 2;

        /* skew each kickoff event slightly to help avoid event ties later on */
        kickoff_time = g_tw_lookahead + tw_rand_unif(lp->rng); 

        e = codes_event_new(lp->gid, kickoff_time, lp);
        m = tw_event_data(e);
        m->svr_event_type = PONG;
        tw_event_send(e);
    }

    return;
}

static void svr_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp)
{
   switch (m->svr_event_type)
    {
        case PING:
            handle_ping_event(ns, b, m, lp);
            break;
        case PONG:
            handle_pong_event(ns, b, m, lp);
            break;
        default:
	    printf("\n Invalid message type %d ", m->svr_event_type);
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
    switch (m->svr_event_type)
    {
        case PING:
            handle_ping_rev_event(ns, b, m, lp);
            break;
        case PONG:
            handle_pong_rev_event(ns, b, m, lp);
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
    /* TODO: print results on server 0 */
    return;
}

#if 0
/* convert ns to seconds */
static tw_stime ns_to_s(tw_stime ns)
{
    return(ns / (1000.0 * 1000.0 * 1000.0));
}
#endif

/* convert seconds to ns */
static tw_stime s_to_ns(tw_stime ns)
{
    return(ns * (1000.0 * 1000.0 * 1000.0));
}

/* reverse handler for ping event */
static void handle_ping_rev_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp)
{
    model_net_event_rc(net_id, lp, m->sent_size);
    return;
}

/* reverse handler for pong */
static void handle_pong_rev_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp)
{
    ns->pingpongs_completed--;
    model_net_event_rc(net_id, lp, m->sent_size);

    /* NOTE: we do not attempt to reverse timing information stored in
     * stat_array[].  This is will get rewritten with the correct value when
     * right forward event is processed, and we don't count on this value
     * being accurate until the simulation is complete.
     */
    return;
}

/* handle recving pong */
static void handle_pong_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp)
{
    svr_msg m_remote;
    int msg_sz_idx;
    tw_lpid peer_gid;

    /* printf("handle_pong_event(), lp %llu.\n", (unsigned long long)lp->gid); */

    assert(ns->svr_idx == 0);
    ns->pingpongs_completed++;

    /* which message size are we on now? */
    msg_sz_idx = ns->pingpongs_completed / NUM_PINGPONGS;

    if(ns->pingpongs_completed % NUM_PINGPONGS == 0)
    {
        printf("FOO: hit msg_sz_idx %d\n", msg_sz_idx);
        printf("FOO: completed %d\n", ns->pingpongs_completed);
        /* finished one msg size range; record time */
        if(msg_sz_idx < NUM_SZS)
            stat_array[msg_sz_idx].start_time = tw_now(lp);
        if(msg_sz_idx > 0)
            stat_array[msg_sz_idx].end_time = tw_now(lp);
    }

    if(msg_sz_idx >= NUM_SZS)
    {
        /* done */
        return;
    }

    codes_mapping_get_lp_id("MODELNET_GRP", "server", 1,
        0, &peer_gid);

    m_remote.svr_event_type = PING;
    m_remote.src = lp->gid;
    m_remote.size = stat_array[msg_sz_idx].msg_sz;

    /* send next ping */
    m->sent_size = m_remote.size;
    model_net_event(net_id, "ping", peer_gid, stat_array[msg_sz_idx].msg_sz, sizeof(m_remote), &m_remote, 0, NULL, lp);

    return;
}

/* handle receiving ping */
static void handle_ping_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp)
{
    svr_msg m_remote;

    assert(ns->svr_idx == 1);

    m_remote.svr_event_type = PONG;
    m_remote.src = lp->gid;
    m_remote.size = m->size;

    /* send pong msg back to sender */
    m->sent_size = m_remote.size;
    model_net_event(net_id, "pong", m->src, m->size, sizeof(m_remote), &m_remote, 0, NULL, lp);
    return;
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
