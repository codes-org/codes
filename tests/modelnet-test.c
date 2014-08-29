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

#define NUM_REQS 1  /* number of requests sent by each server */
#define PAYLOAD_SZ 2048 /* size of simulated data payload, bytes  */

static int net_id = 0;
static int num_routers = 0;
static int num_servers = 0;
static int offset = 2;

/* whether to pull instead of push */ 
static int do_pull = 0;

static int num_routers_per_rep = 0;
static int num_servers_per_rep = 0;
static int lps_per_rep = 0;

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
    tw_stime end_ts;      /* time that we ended sending requests */
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
    int num_nets;
    int *net_ids;
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

    model_net_register();
    svr_add_lp_type();
    
    codes_mapping_setup();
    
    net_ids = model_net_configure(&num_nets);
    assert(num_nets==1);
    net_id = *net_ids;
    free(net_ids);

    num_servers = codes_mapping_get_lp_count("MODELNET_GRP", 0, "server",
            NULL, 1);
    if(net_id == DRAGONFLY)
    {
	  num_routers = codes_mapping_get_lp_count("MODELNET_GRP", 0,
                  "dragonfly_router", NULL, 1); 
	  offset = 1;
    }

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
    printf("server %llu recvd %d bytes in %f seconds, %f MiB/s sent_count %d recvd_count %d local_count %d \n", (unsigned long long)lp->gid, PAYLOAD_SZ*ns->msg_recvd_count, ns_to_s(ns->end_ts-ns->start_ts), 
        ((double)(PAYLOAD_SZ*NUM_REQS)/(double)(1024*1024)/ns_to_s(ns->end_ts-ns->start_ts)), ns->msg_sent_count, ns->msg_recvd_count, ns->local_recvd_count);
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

//    m_local->svr_event_type = REQ;
    m_local->svr_event_type = LOCAL;
    m_local->src = lp->gid;

    memcpy(m_remote, m_local, sizeof(svr_msg));
    m_remote->svr_event_type = (do_pull) ? ACK : REQ;
    //printf("handle_kickoff_event(), lp %llu.\n", (unsigned long long)lp->gid);

    /* record when transfers started on this server */
    ns->start_ts = tw_now(lp);

    num_servers_per_rep = codes_mapping_get_lp_count("MODELNET_GRP", 1,
            "server", NULL, 1);
    num_routers_per_rep = codes_mapping_get_lp_count("MODELNET_GRP", 1,
            "dragonfly_router", NULL, 1);

    lps_per_rep = num_servers_per_rep * 2 + num_routers_per_rep;

    int opt_offset = 0;
    int total_lps = num_servers * 2 + num_routers;

    if(net_id == DRAGONFLY && (lp->gid % lps_per_rep == num_servers_per_rep - 1))
          opt_offset = num_servers_per_rep + num_routers_per_rep; /* optional offset due to dragonfly mapping */
    
    /* each server sends a request to the next highest server */
    int dest_id = (lp->gid + offset + opt_offset)%total_lps;
    if (do_pull){
        model_net_pull_event(net_id, "test", dest_id, PAYLOAD_SZ, 0.0,
                sizeof(svr_msg), (const void*)m_remote, lp);
    }
    else{
        model_net_event(net_id, "test", dest_id, PAYLOAD_SZ, 0.0, sizeof(svr_msg), (const void*)m_remote, sizeof(svr_msg), (const void*)m_local, lp);
    }
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
    if (do_pull){
        model_net_pull_event_rc(net_id, lp);
    }
    else{
        model_net_event_rc(net_id, lp, PAYLOAD_SZ);
    }

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
    if (do_pull){
        model_net_pull_event_rc(net_id, lp);
    }
    else{
        model_net_event_rc(net_id, lp, PAYLOAD_SZ);
    }

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
    // don't worry about resetting end_ts - just let the ack 
    // event bulldoze it
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
    m_remote->svr_event_type = (do_pull) ? ACK : REQ;

//    printf("handle_ack_event(), lp %llu.\n", (unsigned long long)lp->gid);

    /* safety check that this request got to the right server */
//    printf("\n m->src %d lp->gid %d ", m->src, lp->gid);
    int opt_offset = 0;
    
   if(net_id == DRAGONFLY && (lp->gid % lps_per_rep == num_servers_per_rep - 1))
      opt_offset = num_servers_per_rep + num_routers_per_rep; /* optional offset due to dragonfly mapping */    	

    tw_lpid dest_id = (lp->gid + offset + opt_offset)%(num_servers*2 + num_routers);

    /* in the "pull" case, src should actually be self */
    if (do_pull){
        assert(m->src == lp->gid);
    }
    else{
        assert(m->src == dest_id);
    }

    if(ns->msg_sent_count < NUM_REQS)
    {
        /* send another request */
        if (do_pull){
            model_net_pull_event(net_id, "test", dest_id, PAYLOAD_SZ, 0.0,
                    sizeof(svr_msg), (const void*)m_remote, lp);
        }
        else{
            model_net_event(net_id, "test", dest_id, PAYLOAD_SZ, 0.0, sizeof(svr_msg), (const void*)m_remote, sizeof(svr_msg), (const void*)m_local, lp);
        }
        ns->msg_sent_count++;
        m->incremented_flag = 1;
    }
    else
    {
        ns->end_ts = tw_now(lp);
        m->incremented_flag = 0;
    }

    return;
}

/* handle receiving request 
 * (note: this should never be called when doing the "pulling" version of
 * the program) */
static void handle_req_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp)
{
    assert(!do_pull);
    svr_msg * m_local = malloc(sizeof(svr_msg));
    svr_msg * m_remote = malloc(sizeof(svr_msg));

    m_local->svr_event_type = LOCAL;
    m_local->src = lp->gid;

    memcpy(m_remote, m_local, sizeof(svr_msg));
    m_remote->svr_event_type = ACK;
    //printf("handle_req_event(), lp %llu src %llu .\n", (unsigned long long)lp->gid, (unsigned long long) m->src);

    /* safety check that this request got to the right server */
//    printf("\n m->src %d lp->gid %d ", m->src, lp->gid);
    int opt_offset = 0;
    if(net_id == DRAGONFLY && (m->src % lps_per_rep == num_servers_per_rep - 1))
          opt_offset = num_servers_per_rep + num_routers_per_rep; /* optional offset due to dragonfly mapping */       
 
    assert(lp->gid == (m->src + offset + opt_offset)%(num_servers*2 + num_routers));
    ns->msg_recvd_count++;

    /* send ack back */
    /* simulated payload of 1 MiB */
    /* also trigger a local event for completion of payload msg */
    /* remote host will get an ack event */
   
   // mm Q: What should be the size of an ack message? may be a few bytes? or larger..? 
    model_net_event(net_id, "test", m->src, PAYLOAD_SZ, 0.0, sizeof(svr_msg), (const void*)m_remote, sizeof(svr_msg), (const void*)m_local, lp);
//    printf("\n Sending ack to LP %d %d ", m->src, m_remote->src);
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
