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
#include "codes/local-storage-model.h"

static int num_reqs = 0;/* number of requests sent by each server (read from config) */
static int payload_sz = 0; /* size of simulated data payload, bytes (read from config) */

/* model-net ID, can be either simple-net, dragonfly or torus (more may be
 * added) */
static int net_id = 0;
static int num_servers = 0;
static int offset = 2;

/* expected LP group name in configure files for this program */
static char *group_name = "SERVERS";
/* expected parameter group name for rounds of communication */
static char *param_group_nm = "server_pings";
static char *num_reqs_key = "num_reqs";
static char *payload_sz_key = "payload_sz";

typedef struct svr_msg svr_msg;
typedef struct svr_state svr_state;

/* types of events that will constitute server activities */
enum svr_event
{
    KICKOFF,    /* initial event */
    REQ,        /* request event */
    ACK,        /* ack event */
    LOCAL      /* local event */
};

/* this struct serves as the ***persistent*** state of the LP representing the 
 * server in question. This struct is setup when the LP initialization function
 * ptr is called */
struct svr_state
{
    int msg_sent_count;   /* requests sent */
    int msg_recvd_count;  /* requests recvd */
    int local_recvd_count; /* number of local messages received */
    tw_stime start_ts;    /* time that we started sending requests */
    tw_stime end_ts;      /* time that last request finished */
};

/* this struct serves as the ***temporary*** event data, which can be thought
 * of as a message between two LPs. */
struct svr_msg
{
    enum svr_event svr_event_type;
    tw_lpid src;          /* source of this request or ack */

    int incremented_flag; /* helper for reverse computation */
};

/* ROSS expects four functions per LP:
 * - an LP initialization function, called for each LP
 * - an event processing function
 * - a *reverse* event processing function (rollback), and
 * - a finalization/cleanup function when the simulation ends
 */
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

/* set up the function pointers for ROSS, as well as the size of the LP state
 * structure (NOTE: ROSS is in charge of event and state (de-)allocation) */
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

/* as we only have a single event processing entry point and multiple event
 * types, for clarity we define "handlers" for each (reverse) event type */
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

/* for this simulation, each server contacts its neighboring server in an id.
 * this function shows how to use the codes_mapping API to calculate IDs when
 * having to contend with multiple LP types and counts. Note that in this simple
 * example codes_mapping is overkill. */
static tw_lpid get_next_server(tw_lpid sender_id);

/* arguments to be handled by ROSS - strings passed in are expected to be
 * pre-allocated */
static char conf_file_name[256] = {0};
/* this struct contains default parameters used by ROSS, as well as
 * user-specific arguments to be handled by the ROSS config sys. Pass it in
 * prior to calling tw_init */
const tw_optdef app_opt [] =
{
	TWOPT_GROUP("Model net test case" ),
        TWOPT_CHAR("codes-config", conf_file_name, "name of codes configuration file"),
	TWOPT_END()
};

int main(
    int argc,
    char **argv)
{
    int nprocs;
    int rank;
    int num_nets, *net_ids;

    /* TODO: explain why we need this (ROSS has cutoff??) */
    g_tw_ts_end = s_to_ns(60*60*24*365); /* one year, in nsecs */

    /* ROSS initialization function calls */
    tw_opt_add(app_opt); /* add user-defined args */
    /* initialize ROSS and parse args. NOTE: tw_init calls MPI_Init */
    tw_init(&argc, &argv); 

    if (!conf_file_name[0]) 
    {
        fprintf(stderr, "Expected \"codes-config\" option, please see --help.\n");
        MPI_Finalize();
        return 1;
    }

    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nprocs);
  
    /* loading the config file into the codes-mapping utility, giving us the
     * parsed config object in return. 
     * "config" is a global var defined by codes-mapping */
    if (configuration_load(conf_file_name, MPI_COMM_WORLD, &config)){
        fprintf(stderr, "Error loading config file %s.\n", conf_file_name);
        MPI_Finalize();
        return 1;
    }

    /* register model-net LPs with ROSS */
    model_net_register();

    /* register the server LP type with ROSS */
    svr_add_lp_type();

    /* Setup takes the global config object, the registered LPs, and
     * generates/places the LPs as specified in the configuration file.
     * This should only be called after ALL LP types have been registered in 
     * codes */
    codes_mapping_setup();

    /* Setup the model-net parameters specified in the global config object,
     * returned are the identifier(s) for the network type. In this example, we
     * only expect one*/
    net_ids = model_net_configure(&num_nets);
    assert(num_nets==1);
    net_id = *net_ids;
    free(net_ids);
    /* in this example, we are using simplenet, which simulates point to point 
     * communication between any two entities (other networks are trickier to
     * setup). Hence: */
    if(net_id != SIMPLENET)
    {
	    printf("\n The test works with simple-net configuration only! ");
	    MPI_Finalize();
	    return 0;
    }
    
    /* calculate the number of servers in this simulation,
     * ignoring annotations */
    num_servers = codes_mapping_get_lp_count(group_name, 0, "server", NULL, 1);

    /* for this example, we read from a separate configuration group for
     * server message parameters. Since they are constant for all LPs,
     * go ahead and read them prior to running */
    configuration_get_value_int(&config, param_group_nm, num_reqs_key, NULL, &num_reqs);
    configuration_get_value_int(&config, param_group_nm, payload_sz_key, NULL, &payload_sz);

    /* begin simulation */ 
    tw_run();

    /* model-net has the capability of outputting network transmission stats */
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
    /* lp_type_register should be called exactly once per process per 
     * LP type */
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

    /* first create the event (time arg is an offset, not absolute time) */
    e = codes_event_new(lp->gid, kickoff_time, lp);
    /* after event is created, grab the allocated message and set msg-specific
     * data */ 
    m = tw_event_data(e);
    m->svr_event_type = KICKOFF;
    /* event is ready to be processed, send it off */
    tw_event_send(e);

    return;
}

/* event processing entry point
 * - simply forward the message to the appropriate handler */
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

/* reverse event processing entry point
 * - simply forward the message to the appropriate handler */
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

/* once the simulation is over, do some output */
static void svr_finalize(
    svr_state * ns,
    tw_lp * lp)
{
    printf("server %llu recvd %d bytes in %lf seconds, %lf MiB/s sent_count %d recvd_count %d local_count %d \n", 
            (unsigned long long)(lp->gid/2),
            payload_sz*ns->msg_recvd_count,
            ns_to_s(ns->end_ts-ns->start_ts),
            ((double)(payload_sz*num_reqs)/(double)(1024*1024)/ns_to_s(ns->end_ts-ns->start_ts)),
            ns->msg_sent_count,
            ns->msg_recvd_count,
            ns->local_recvd_count);
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

/* see declaration for more general info */
tw_lpid get_next_server(tw_lpid sender_id)
{
    tw_lpid rtn_id;
    /* first, get callers LP and group info from codes-mapping. Caching this 
     * info in the LP struct isn't a bad idea for preventing a huge number of
     * lookups */
    char grp_name[MAX_NAME_LENGTH], lp_type_name[MAX_NAME_LENGTH],
         annotation[MAX_NAME_LENGTH];
    int  lp_type_id, grp_id, grp_rep_id, offset, num_reps;
    int dest_rep_id;
    codes_mapping_get_lp_info(sender_id, grp_name, &grp_id, lp_type_name,
            &lp_type_id, annotation, &grp_rep_id, &offset);
    /* in this example, we assume that, for our group of servers, each 
     * "repetition" consists of a single server/NIC pair. Hence, we grab the 
     * server ID for the next repetition, looping around if necessary */
    num_reps = codes_mapping_get_group_reps(grp_name);
    dest_rep_id = (grp_rep_id+1) % num_reps;
    /* finally, get the server (exactly 1 server per rep -> offset w/in rep = 0 */
    codes_mapping_get_lp_id(grp_name, lp_type_name, NULL, 1, dest_rep_id,
            0, &rtn_id);
    return rtn_id;
}

/* handle initial event */
static void handle_kickoff_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp)
{
    int dest_id;
    int use_brute_force_map = 0;
    /* normally, when using ROSS, events are allocated as a result of the event
     * creation process. However, since we are now asking model-net to
     * communicate with an entity on our behalf, we need to generate both the
     * message to the recipient and an optional callback message 
     * - thankfully, memory need not persist past the model_net_event call - it
     *   copies the messages */
    svr_msg m_local;
    svr_msg m_remote;

    m_local.svr_event_type = LOCAL;
    m_local.src = lp->gid;
    m_remote.svr_event_type = REQ;
    m_remote.src = lp->gid;

    /* record when transfers started on this server */
    ns->start_ts = tw_now(lp);

    /* each server sends a request to the next highest server 
     * In this simulation, LP determination is simple: LPs are assigned
     * round robin as in serv_1, net_1, serv_2, net_2, etc. 
     * However, that may not always be the case, so we also show a more
     * complicated way to map through codes_mapping */
    if (use_brute_force_map)
        dest_id = (lp->gid + offset)%(num_servers*2);
    else
    {
        dest_id = get_next_server(lp->gid);
    }

    /* model-net needs to know about (1) higher-level destination LP which is a neighboring server in this case
     * (2) struct and size of remote message and (3) struct and size of local message (a local message can be null) */
    model_net_event(net_id, "test", dest_id, payload_sz, 0.0, sizeof(svr_msg), 
            (const void*)&m_remote, sizeof(svr_msg), (const void*)&m_local, lp);
    ns->msg_sent_count++;
}

/* at the moment, no need for local callbacks from model-net, so we maintain a
 * count for debugging purposes */ 
static void handle_local_event(
		svr_state * ns,
		tw_bf * b,
		svr_msg * m,
		tw_lp * lp)
{
    ns->local_recvd_count++;
}

/* handle recving ack
 * for this simulation, we repeatedly ping the destination server until num_reqs
 * of size payload_sz have been satisfied - we begin the next req when we
 * receive an ACK from the destination server */
static void handle_ack_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp)
{
    /* the ACK actually doesn't come from the NIC on the other server -
     * model-net "hides" the NIC LP from us so we only see the original
     * destination server */

    /* safety check that this request got to the right server, both with our
     * brute-force lp calculation and our more generic codes-mapping 
     * calculation */
    assert(m->src == (lp->gid + offset)%(num_servers*2) &&
           m->src == get_next_server(lp->gid));

    if(ns->msg_sent_count < num_reqs)
    {
        /* again, allocate our own msgs so model-net can transmit on our behalf */
        svr_msg m_local;
        svr_msg m_remote;

        m_local.svr_event_type = LOCAL;
        m_local.src = lp->gid;
        m_remote.svr_event_type = REQ;
        m_remote.src = lp->gid;

        /* send another request */
	model_net_event(net_id, "test", m->src, payload_sz, 0.0, sizeof(svr_msg), 
                (const void*)&m_remote, sizeof(svr_msg), (const void*)&m_local, lp);
        ns->msg_sent_count++;
        m->incremented_flag = 1;
        
    }
    else
    {
	/* threshold count reached, stop sending messages */
        m->incremented_flag = 0;
        ns->end_ts = tw_now(lp);
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
    m_remote.svr_event_type = ACK;
    m_remote.src = lp->gid;

    /* safety check that this request got to the right server */
    
    assert(lp->gid == (m->src + offset)%(num_servers*2) &&
           lp->gid == get_next_server(m->src));
    ns->msg_recvd_count++;

    /* send ack back */
    /* simulated payload of 1 MiB */
    /* also trigger a local event for completion of payload msg */
    /* remote host will get an ack event */
   
    model_net_event(net_id, "test", m->src, payload_sz, 0.0, sizeof(svr_msg), 
            (const void*)&m_remote, sizeof(svr_msg), (const void*)&m_local, lp);
    return;
}

/* for us, reverse events are very easy, the only LP state that needs to be
 * rolled back are the counts.
 * for more complex simulations, this will not be the case (e.g., state
 * containing queues) */

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
    /* model-net has its own reverse computation support */ 
    model_net_event_rc(net_id, lp, payload_sz);

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
    model_net_event_rc(net_id, lp, payload_sz);

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
        model_net_event_rc(net_id, lp, payload_sz);
        ns->msg_sent_count--;
    }
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
