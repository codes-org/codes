/*
 * Copyright (C) 2015 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

/*
* The test program generates some synthetic traffic patterns for the model-net network models.
* currently it only support the dragonfly network model uniform random and nearest neighbor traffic patterns.
*/

#include "codes/model-net.h"
#include "codes/lp-io.h"
#include "codes/codes.h"
#include "codes/codes_mapping.h"
#include "codes/configuration.h"
#include "codes/lp-type-lookup.h"

#define PAYLOAD_SZ 512

static int net_id = 0;
static int num_routers = 0;
static int num_servers = 0;
static int offset = 2;
static int traffic = 1;
static double arrival_time = 1000.0;

/* whether to pull instead of push */
static int do_pull = 0;

static int num_servers_per_rep = 0;
static int num_routers_per_grp = 0;
static int num_nodes_per_grp = 0;

static int num_reps = 0;
static int num_groups = 0;
static int num_nodes = 0;

typedef struct svr_msg svr_msg;
typedef struct svr_state svr_state;

/* global variables for codes mapping */
static char group_name[MAX_NAME_LENGTH];
static char lp_type_name[MAX_NAME_LENGTH];
static int group_index, lp_type_index, rep_id, offset;

/* type of events */
enum svr_event
{
    KICKOFF,	   /* kickoff event */
    REMOTE,        /* remote event */
    LOCAL      /* local event */
};

/* type of synthetic traffic */
enum TRAFFIC
{
	UNIFORM = 1, /* sends message to a randomly selected node */
	NEAREST_GROUP = 2, /* sends message to the node connected to the neighboring router */
	NEAREST_NEIGHBOR = 3 /* sends message to the next node (potentially connected to the same router) */
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

const tw_optdef app_opt [] =
{
        TWOPT_GROUP("Model net synthetic traffic " ),
	TWOPT_UINT("traffic", traffic, "UNIFORM RANDOM=1, NEAREST NEIGHBOR=2 "),
	TWOPT_STIME("arrival_time", arrival_time, "INTER-ARRIVAL TIME"),
        TWOPT_END()
};

const tw_lptype* svr_get_lp_type()
{
            return(&svr_lp);
}

static void svr_add_lp_type()
{
  lp_type_register("server", svr_get_lp_type());
}

static void issue_event(
    svr_state * ns,
    tw_lp * lp)
{
    tw_event *e;
    svr_msg *m;
    tw_stime kickoff_time;

    /* each server sends a dummy event to itself that will kick off the real
     * simulation
     */

    /* skew each kickoff event slightly to help avoid event ties later on */
    kickoff_time = g_tw_lookahead + arrival_time + tw_rand_exponential(lp->rng, (double)arrival_time/100);

    e = tw_event_new(lp->gid, kickoff_time, lp);
    m = tw_event_data(e);
    m->svr_event_type = KICKOFF;
    tw_event_send(e);
}

static void svr_init(
    svr_state * ns,
    tw_lp * lp)
{
    issue_event(ns, lp);
    return;
}

static void handle_kickoff_rev_event(
            svr_state * ns,
            tw_bf * b,
            svr_msg * m,
            tw_lp * lp)
{
	ns->msg_sent_count--;
	model_net_event_rc(net_id, lp, PAYLOAD_SZ);
}	
static void handle_kickoff_event(
	    svr_state * ns,
	    tw_bf * b,
	    svr_msg * m,
	    tw_lp * lp)
{
    char* anno;
    tw_lpid local_dest = -1, global_dest = -1;
   
    svr_msg * m_local = malloc(sizeof(svr_msg));
    svr_msg * m_remote = malloc(sizeof(svr_msg));

    m_local->svr_event_type = LOCAL;
    m_local->src = lp->gid;

    memcpy(m_remote, m_local, sizeof(svr_msg));
    m_remote->svr_event_type = REMOTE;

    assert(net_id == DRAGONFLY); /* only supported for dragonfly model right now. */

    ns->start_ts = tw_now(lp);
    
   codes_mapping_get_lp_info(lp->gid, group_name, &group_index, lp_type_name, &lp_type_index, anno, &rep_id, &offset);
   /* in case of uniform random traffic, send to a random destination. */
   if(traffic == UNIFORM)
   {
   	local_dest = tw_rand_integer(lp->rng, 0, num_nodes - 1);
//	printf("\n LP %ld sending to %d ", lp->gid, local_dest);
   }
   else if(traffic == NEAREST_GROUP)
   {
	local_dest = (rep_id * 2 + offset + num_nodes_per_grp) % num_nodes;
//	printf("\n LP %ld sending to %ld num nodes %d ", rep_id * 2 + offset, local_dest, num_nodes);
   }	
   else if(traffic == NEAREST_NEIGHBOR)
   {
	local_dest =  (rep_id * 2 + offset + 2) % num_nodes;
//	 printf("\n LP %ld sending to %ld num nodes %d ", rep_id * 2 + offset, local_dest, num_nodes);
   }
   assert(local_dest < num_nodes);
   codes_mapping_get_lp_id(group_name, lp_type_name, anno, 1, local_dest / num_servers_per_rep, local_dest % num_servers_per_rep, &global_dest);
  
   ns->msg_sent_count++;
   model_net_event(net_id, "test", global_dest, PAYLOAD_SZ, 0.0, sizeof(svr_msg), (const void*)m_remote, sizeof(svr_msg), (const void*)m_local, lp);
   issue_event(ns, lp);
   return;
}

static void handle_remote_rev_event(
            svr_state * ns,     
            tw_bf * b,
            svr_msg * m,
            tw_lp * lp)
{
        ns->msg_recvd_count--;
}

static void handle_remote_event(
	    svr_state * ns,
	    tw_bf * b,
	    svr_msg * m,
	    tw_lp * lp)
{
	ns->msg_recvd_count++;
}

static void handle_local_rev_event(
                svr_state * ns,
                tw_bf * b,
                svr_msg * m,
                tw_lp * lp)
{
	ns->local_recvd_count--;
}

static void handle_local_event(
                svr_state * ns,
                tw_bf * b,
                svr_msg * m,
                tw_lp * lp)
{
    ns->local_recvd_count++;
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

static void svr_finalize(
    svr_state * ns,
    tw_lp * lp)
{
    ns->end_ts = tw_now(lp);

    printf("server %llu recvd %d bytes in %f seconds, %f MiB/s sent_count %d recvd_count %d local_count %d \n", (unsigned long long)lp->gid, PAYLOAD_SZ*ns->msg_recvd_count, ns_to_s(ns->end_ts-ns->start_ts),
        ((double)(PAYLOAD_SZ*ns->msg_sent_count)/(double)(1024*1024)/ns_to_s(ns->end_ts-ns->start_ts)), ns->msg_sent_count, ns->msg_recvd_count, ns->local_recvd_count);
    return;
}

static void svr_rev_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp)
{
    switch (m->svr_event_type)
    {
	case REMOTE:
		handle_remote_rev_event(ns, b, m, lp);
		break;
	case LOCAL:
		handle_local_rev_event(ns, b, m, lp);
		break;
	case KICKOFF:
		handle_kickoff_rev_event(ns, b, m, lp);
		break;
	default:
		assert(0);
		break;
    }
}

static void svr_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp)
{
   switch (m->svr_event_type)
    {
        case REMOTE:
            handle_remote_event(ns, b, m, lp);
            break;
        case LOCAL:
            handle_local_event(ns, b, m, lp);
            break;
	case KICKOFF:
	    handle_kickoff_event(ns, b, m, lp);
	    break;
        default:
            printf("\n Invalid message type %d ", m->svr_event_type);
            assert(0);
        break;
    }
}

int main(
    int argc,
    char **argv)
{
    int nprocs;
    int rank;
    int num_nets;
    int *net_ids;
    char* anno;

    lp_io_handle handle;

    tw_opt_add(app_opt);
    tw_init(&argc, &argv);
    offset = 1;

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

    if(net_id != DRAGONFLY)
    {
	printf("\n The test works with dragonfly model configuration only! ");
        MPI_Finalize();
        return 0;
    }
    num_servers_per_rep = codes_mapping_get_lp_count("MODELNET_GRP", 1, "server",
            NULL, 1);
    configuration_get_value_int(&config, "PARAMS", "num_routers", anno, &num_routers_per_grp);
    
    num_groups = (num_routers_per_grp * (num_routers_per_grp/2) + 1);
    num_nodes = num_groups * num_routers_per_grp * (num_routers_per_grp / 2);
    num_nodes_per_grp = num_routers_per_grp * (num_routers_per_grp / 2);

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

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
