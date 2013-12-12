/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

/* SUMMARY:
 *
 * This is a sample code to demonstrate CODES usage and best practices.  It
 * sets up a number of servers, each of which is paired up with a simplenet LP
 * to serve as the NIC.  Each server exchanges a sequence of requests and acks
 * with one peer and measures the throughput in terms of payload bytes (ack
 * size) moved per second.
 */

#include <string.h>
#include <assert.h>
#include <ross.h>

#include "codes/lp-io.h"
#include "codes/codes.h"
#include "codes/codes_mapping.h"
#include "codes/configuration.h"
#include "codes/model-net.h"
#include "codes/lp-type-lookup.h"

#define NUM_REQS 500  /* number of requests sent by each server */
#define PAYLOAD_SZ 2048 /* size of simulated data payload, bytes  */

/* model-net ID, can be either simple-net, dragonfly or torus */
static int net_id = 0;
static int num_servers = 0;
static int offset = 2;

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
    g_tw_ts_end = s_to_ns(60*60*24*365); /* one year, in nsecs */

    /* ROSS initialization function calls */
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
  
    /* loading the config file of codes-mapping */
    configuration_load(argv[2], MPI_COMM_WORLD, &config);

    /* Setup the model-net parameters specified in the config file */
    net_id=model_net_set_params();

    /* register the server LP type (model-net LP type is registered internally in model_net_set_params() */
    svr_add_lp_type();
    
    /*Now setup codes mapping */
    codes_mapping_setup();
    
    /*query codes mapping API*/
    num_servers = codes_mapping_get_group_reps("MODELNET_GRP") * codes_mapping_get_lp_count("MODELNET_GRP", "server");
    if(net_id != SIMPLENET)
    {
	    printf("\n The test works with simple-net configuration only! ");
	    MPI_Finalize();
	    return 0;
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
    
    memset(ns, 0, sizeof(*ns));

    /* each server sends a dummy event to itself that will kick off the real
     * simulation
     */

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
    printf("server %llu recvd %d bytes in %f seconds, %f MiB/s sent_count %d recvd_count %d local_count %d \n", (unsigned long long)lp->gid, PAYLOAD_SZ*ns->msg_recvd_count, ns_to_s((tw_now(lp)-ns->start_ts)), 
        ((double)(PAYLOAD_SZ*NUM_REQS)/(double)(1024*1024)/ns_to_s(tw_now(lp)-ns->start_ts)), ns->msg_sent_count, ns->msg_recvd_count, ns->local_recvd_count);
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
    svr_msg * m_local = malloc(sizeof(svr_msg));
    svr_msg * m_remote = malloc(sizeof(svr_msg));

    /* we allocate a local message and a remote message both */
    m_local->svr_event_type = LOCAL;
    m_local->src = lp->gid;

    memcpy(m_remote, m_local, sizeof(svr_msg));
    m_remote->svr_event_type = REQ;

    /* record when transfers started on this server */
    ns->start_ts = tw_now(lp);

    /* each server sends a request to the next highest server */
    int dest_id = (lp->gid + offset)%(num_servers*2 + num_routers);

    /*model-net needs to know about (1) higher-level destination LP which is a neighboring server in this case
     * (2) struct and size of remote message and (3) struct and size of local message (a local message can be null) */
    model_net_event(net_id, "test", dest_id, PAYLOAD_SZ, sizeof(svr_msg), (const void*)m_remote, sizeof(svr_msg), (const void*)m_local, lp);
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

    m_local->svr_event_type = LOCAL;
    m_local->src = lp->gid;

    memcpy(m_remote, m_local, sizeof(svr_msg));
    m_remote->svr_event_type = REQ;

    /* safety check that this request got to the right server */
    assert(m->src == (lp->gid + offset)%(num_servers*2));

    if(ns->msg_sent_count < NUM_REQS)
    {
        /* send another request */
	model_net_event(net_id, "test", m->src, PAYLOAD_SZ, sizeof(svr_msg), (const void*)m_remote, sizeof(svr_msg), (const void*)m_local, lp);
        ns->msg_sent_count++;
        m->incremented_flag = 1;
    }
    else
    {
	/* threshold count reached, stop sending messages */
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

    /* safety check that this request got to the right server */
    
    assert(lp->gid == (m->src + offset)%(num_servers*2));
    ns->msg_recvd_count++;

    /* send ack back */
    /* simulated payload of 1 MiB */
    /* also trigger a local event for completion of payload msg */
    /* remote host will get an ack event */
   
    model_net_event(net_id, "test", m->src, PAYLOAD_SZ, sizeof(svr_msg), (const void*)m_remote, sizeof(svr_msg), (const void*)m_local, lp);
    return;
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
