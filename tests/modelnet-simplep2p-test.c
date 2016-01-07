/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

/* SUMMARY:
 *
 * This is a test harness for the modelnet module.  It sets up a number of
 * servers, each of which is paired up with a simplenet LP to serve as the
 * NIC.  Each server exchanges a sequence of requests and acks with one peer
 * and measures the throughput in terms of payload bytes (ack size) moved
 * per second.
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

#define NUM_REQS 3  /* number of requests sent by each server */
#define PAYLOAD_SZ 2048 /* size of simulated data payload, bytes  */

static int net_id = 0;
static int num_servers = 0;

typedef struct svr_msg svr_msg;
typedef struct svr_state svr_state;

/* types of events that will constitute triton requests */
enum svr_event
{
    KICKOFF,    /* initial event */
    REQ,        /* request event */
    ACK,        /* ack event */
    LOCAL      /* local event */
};

struct svr_state
{
    int msg_sent_count;   /* requests sent */
    int msg_recvd_count;  /* requests recvd */
    int local_recvd_count; /* number of local messages received */
    tw_stime start_ts;    /* time that we started sending requests */
};

struct svr_msg
{
    enum svr_event svr_event_type;
//    enum net_event net_event_type; 
    tw_lpid src;          /* source of this request or ack */

    int incremented_flag; /* helper for reverse computation */
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

extern const tw_lptype* svr_get_lp_type();
static void svr_add_lp_type();
static tw_stime ns_to_s(tw_stime ns);
static tw_stime s_to_ns(tw_stime ns);
static void handle_kickoff_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp);
static void handle_ack_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp);
static void handle_req_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp);
static void handle_local_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
   tw_lp * lp);
static void handle_local_rev_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
   tw_lp * lp);
static void handle_kickoff_rev_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp);
static void handle_ack_rev_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp);
static void handle_req_rev_event(
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
    int nprocs;
    int rank;
    int num_nets, *net_ids;
    //printf("\n Config count %d ",(int) config.lpgroups_count);
    g_tw_ts_end = s_to_ns(60*60*24*365); /* one year, in nsecs */
    lp_io_handle handle;

    tw_opt_add(app_opt);
    tw_init(&argc, &argv);

    if(argc < 2)
    {
	    printf("\n Usage: mpirun <args> --sync=2/3 mapping_file_name.conf (optional --nkp) ");
	    MPI_Finalize();
	    return 0;
    }
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nprocs);
  
    configuration_load(argv[2], MPI_COMM_WORLD, &config);
    svr_add_lp_type();
    model_net_register();
    
    codes_mapping_setup();

    net_ids = model_net_configure(&num_nets);
    assert(num_nets==1);
    net_id = *net_ids;
    free(net_ids);
    
    num_servers = codes_mapping_get_lp_count("MODELNET_GRP", 0, "server",
            NULL, 1);
    assert(num_servers == 3);

    if(lp_io_prepare("modelnet-test", LP_IO_UNIQ_SUFFIX, &handle, MPI_COMM_WORLD) < 0)
    {
        return(-1);
    }

    tw_run();
    model_net_report_stats(net_id);

    if(lp_io_flush(handle, MPI_COMM_WORLD) < 0)
    {
        return(-1);
    }

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
    
    memset(ns, 0, sizeof(*ns));

    /* each server sends a dummy event to itself that will kick off the real
     * simulation
     */

    //printf("\n Initializing servers %d ", (int)lp->gid);
    /* skew each kickoff event slightly to help avoid event ties later on */
    kickoff_time = g_tw_lookahead + tw_rand_unif(lp->rng); 

    e = codes_event_new(lp->gid, kickoff_time, lp);
    m = tw_event_data(e);
    m->svr_event_type = KICKOFF;
    tw_event_send(e);

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
        case REQ:
            handle_req_event(ns, b, m, lp);
            break;
        case ACK:
            handle_ack_event(ns, b, m, lp);
            break;
        case KICKOFF:
            handle_kickoff_event(ns, b, m, lp);
            break;
	case LOCAL:
	   handle_local_event(ns, b, m, lp); 
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
        case REQ:
            handle_req_rev_event(ns, b, m, lp);
            break;
        case ACK:
            handle_ack_rev_event(ns, b, m, lp);
            break;
        case KICKOFF:
            handle_kickoff_rev_event(ns, b, m, lp);
            break;
	case LOCAL:
	    handle_local_rev_event(ns, b, m, lp);    
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
    double t = ns_to_s(tw_now(lp) - ns->start_ts);
    printf("server %llu recvd %d bytes in %f seconds, %f MiB/s sent_count %d recvd_count %d local_count %d \n", (unsigned long long)lp->gid, PAYLOAD_SZ*ns->msg_recvd_count, t, 
        ((double)(PAYLOAD_SZ*NUM_REQS)/(double)(1024*1024)/t), ns->msg_sent_count, ns->msg_recvd_count, ns->local_recvd_count);
    return;
}

/* convert ns to seconds */
static tw_stime ns_to_s(tw_stime ns)
{
    return(ns / (1000.0 * 1000.0 * 1000.0));
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
    svr_msg m_local, m_remote;

//    m_local.svr_event_type = REQ;
    m_local.svr_event_type = LOCAL;
    m_local.src = lp->gid;

    memcpy(&m_remote, &m_local, sizeof(svr_msg));
    m_remote.svr_event_type = REQ;
    //printf("handle_kickoff_event(), lp %llu.\n", (unsigned long long)lp->gid);

    /* record when transfers started on this server */
    ns->start_ts = tw_now(lp);

    /* each server sends a request to the next highest server */
    int dest_id;
    switch (lp->gid / 2){
        case 0: dest_id = 4; break;
        case 1: dest_id = 4; break;
        case 2: return; /* LP 4 doesn't send messages */ 
    }
    model_net_event(net_id, "test", dest_id, PAYLOAD_SZ, 0.0, sizeof(svr_msg), &m_remote, sizeof(svr_msg), &m_local, lp);
    ns->msg_sent_count++;
}

static void handle_local_event(
		svr_state * ns,
		tw_bf * b,
		svr_msg * m,
		tw_lp * lp)
{
    ns->local_recvd_count++;
}

static void handle_local_rev_event(
	       svr_state * ns,
	       tw_bf * b,
	       svr_msg * m,
	       tw_lp * lp)
{
   ns->local_recvd_count--;
}
/* reverse handler for req event */
static void handle_req_rev_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp)
{
    ns->msg_recvd_count--;
    model_net_event_rc(net_id, lp, PAYLOAD_SZ);

    return;
}


/* reverse handler for kickoff */
static void handle_kickoff_rev_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp)
{
    ns->msg_sent_count--;
    model_net_event_rc(net_id, lp, PAYLOAD_SZ);

    return;
}

/* reverse handler for ack*/
static void handle_ack_rev_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp)
{
    if(m->incremented_flag)
    {
        model_net_event_rc(net_id, lp, PAYLOAD_SZ);
        ns->msg_sent_count--;
    }
    return;
}

/* handle recving ack */
static void handle_ack_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp)
{
    svr_msg m_local;
    svr_msg m_remote;

    m_local.svr_event_type = LOCAL;
    m_local.src = lp->gid;

    memcpy(&m_remote, &m_local, sizeof(svr_msg));
    m_remote.svr_event_type = REQ;

    if(ns->msg_sent_count < NUM_REQS)
    {
	model_net_event(net_id, "test", m->src, PAYLOAD_SZ, 0.0, sizeof(svr_msg), &m_remote, sizeof(svr_msg), &m_local, lp);
        ns->msg_sent_count++;
        m->incremented_flag = 1;
    }
    else
    {
        m->incremented_flag = 0;
    }

    return;
}

/* handle receiving request */
static void handle_req_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp)
{
    svr_msg m_local;
    svr_msg m_remote;

    m_local.svr_event_type = LOCAL;
    m_local.src = lp->gid;

    memcpy(&m_remote, &m_local, sizeof(svr_msg));
    m_remote.svr_event_type = ACK;

    ns->msg_recvd_count++;

   // mm Q: What should be the size of an ack message? may be a few bytes? or larger..? 
    model_net_event(net_id, "test", m->src, PAYLOAD_SZ, 0.0, sizeof(svr_msg), &m_remote, sizeof(svr_msg), &m_local, lp);
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
