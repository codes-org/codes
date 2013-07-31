/*
 * Copyright (C) 2011, University of Chicago
 *
 * See COPYRIGHT notice in top-level directory.
 */

/* SUMMARY:
 *
 * This is a test harness for the simplenet module.  It sets up a number of
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

#define NUM_SERVERS 16  /* number of servers */
#define NUM_REQS 1000  /* number of requests sent by each server */
#define PAYLOAD_SZ 2048 /* size of simulated data payload, bytes  */

static int net_id = 0;

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

const tw_optdef app_opt[] = {
    TWOPT_GROUP("Simple Network Test Model"),
    TWOPT_END()
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
/*static tw_peid svr_node_mapping(
    tw_lpid gid);
*/

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
int main(
    int argc,
    char **argv)
{
    int nprocs;
    int rank;
    int ret;
    lp_io_handle handle;
    //printf("\n Config count %d ",(int) config.lpgroups_count);
    g_tw_ts_end = s_to_ns(60*60*24*365); /* one year, in nsecs */

    tw_opt_add(app_opt);
    tw_init(&argc, &argv);

    if(argc != 2)
    {
	    printf("\n Usage: mpirun <args> --sync=2/3 mapping_file_name.conf");
	    MPI_Finalize();
	    return 0;
    }
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nprocs);
  
    simplenet_param net_params;
    net_params.net_startup_ns = 1.5;
    net_params.net_bw_mbps = 20000;
    net_params.num_nics = NUM_SERVERS;

    net_id = model_net_setup("simplenet", 512, (const void*)&net_params); /* Sets the network as simplenet and packet size 512 */
    model_net_add_lp_type(net_id);
    svr_add_lp_type();

     configuration_load(argv[2], MPI_COMM_WORLD, &config);

     codes_mapping_setup();
     g_tw_mapping=CUSTOM;
     g_tw_custom_initial_mapping=&codes_mapping_init;
     g_tw_custom_lp_global_to_local_map=&codes_mapping_to_lp;

     //printf("\n Initializing %d lps on %d ", get_lps_for_pe(), g_tw_mynode);
     tw_define_lps(codes_mapping_get_lps_for_pe(), 2* sizeof(struct svr_msg) +model_net_get_msg_sz(0) , 0 );
    /* NOTE: the message size defined here has to be able to handle two
     * svr_msg structs and a simplenet message joined together.  This allows
     * the model to send a single simplenet even that will handle a)
     * simplenet routing b) remote event delivery and c) local send
     * completion event.
     */
    ret = lp_io_prepare("simplenet-test", LP_IO_UNIQ_SUFFIX, &handle, MPI_COMM_WORLD);
    if(ret < 0)
    {
       return(-1); 
    }

    tw_run();
    
    ret = lp_io_flush(handle, MPI_COMM_WORLD);
    assert(ret == 0);

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

    printf("\n Initializing servers %d ", (int)lp->gid);
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
    printf("server %llu recvd %d bytes in %f seconds, %f MiB/s sent_count %d recvd_count %d local_count %d \n", (unsigned long long)lp->gid/2, PAYLOAD_SZ*ns->msg_recvd_count, ns_to_s((tw_now(lp)-ns->start_ts)), 
        ((double)(PAYLOAD_SZ*NUM_REQS)/(double)(1024*1024)/ns_to_s(tw_now(lp)-ns->start_ts)), ns->msg_sent_count, ns->msg_recvd_count, ns->local_recvd_count);
    return;
}

/*static tw_peid svr_node_mapping(
    tw_lpid gid)
{
    return (tw_peid) gid / g_tw_nlp;
}*/

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
    svr_msg * m_local = malloc(sizeof(svr_msg));
    svr_msg * m_remote = malloc(sizeof(svr_msg));

//    m_local->svr_event_type = REQ;
    m_local->svr_event_type = LOCAL;
    m_local->src = lp->gid;

    memcpy(m_remote, m_local, sizeof(svr_msg));
    m_remote->svr_event_type = REQ;
    //printf("handle_kickoff_event(), lp %llu.\n", (unsigned long long)lp->gid);

    /* record when transfers started on this server */
    ns->start_ts = tw_now(lp);

    /* each server sends a request to the next highest server */
    model_net_event(net_id, "test", (lp->gid + 2)%(NUM_SERVERS*2), PAYLOAD_SZ, sizeof(svr_msg), (const void*)m_remote, sizeof(svr_msg), (const void*)m_local, lp);
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
    svr_msg * m_local = malloc(sizeof(svr_msg));
    svr_msg * m_remote = malloc(sizeof(svr_msg));

//    m_local->svr_event_type = REQ;
    m_local->svr_event_type = LOCAL;
    m_local->src = lp->gid;

    memcpy(m_remote, m_local, sizeof(svr_msg));
    m_remote->svr_event_type = REQ;

//    printf("handle_ack_event(), lp %llu.\n", (unsigned long long)lp->gid);

    /* safety check that this request got to the right server */
//    printf("\n m->src %d lp->gid %d ", m->src, lp->gid);
    assert(m->src == (lp->gid + 2)%(NUM_SERVERS*2));

    if(ns->msg_sent_count < NUM_REQS)
    {
        /* send another request */
	model_net_event(net_id, "test", m->src, PAYLOAD_SZ, sizeof(svr_msg), (const void*)m_remote, sizeof(svr_msg), (const void*)m_local, lp);
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
    svr_msg * m_local = malloc(sizeof(svr_msg));
    svr_msg * m_remote = malloc(sizeof(svr_msg));

    m_local->svr_event_type = LOCAL;
    m_local->src = lp->gid;

    memcpy(m_remote, m_local, sizeof(svr_msg));
    m_remote->svr_event_type = ACK;
    //printf("handle_req_event(), lp %llu src %llu .\n", (unsigned long long)lp->gid, (unsigned long long) m->src);

    /* safety check that this request got to the right server */
    assert(lp->gid == (m->src + 2)%(NUM_SERVERS*2));

    ns->msg_recvd_count++;

    /* send ack back */
    /* simulated payload of 1 MiB */
    /* also trigger a local event for completion of payload msg */
    /* remote host will get an ack event */
   
   // mm Q: What should be the size of an ack message? may be a few bytes? or larger..? 
    model_net_event(net_id, "test", m->src, PAYLOAD_SZ, sizeof(svr_msg), (const void*)m_remote, sizeof(svr_msg), (const void*)m_local, lp);
//    printf("\n Sending ack to LP %d %d ", m->src, m_remote->src);
    return;
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 
 m_remote->src = lp->gid;*
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
