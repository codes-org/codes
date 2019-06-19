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


static int net_id = 0;
static int traffic = 1;
static double arrival_time = 1000.0;
static int PAYLOAD_SZ = 2048;

/* whether to pull instead of push */
static int num_servers_per_rep = 0;
static int num_routers_per_grp = 0;
static int num_nodes_per_grp = 0;
static int num_nodes_per_cn = 0;
static int num_groups = 0;
static unsigned long long num_nodes = 0;

static char lp_io_dir[256] = {'\0'};
static lp_io_handle io_handle;
static unsigned int lp_io_use_suffix = 0;
static int do_lp_io = 0;
static int num_msgs = 20;
static tw_stime sampling_interval = 800000;
static tw_stime sampling_end_time = 1600000;

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
    RAND_PERM = 2, 
	NEAREST_GROUP = 3, /* sends message to the node connected to the neighboring router */
	NEAREST_NEIGHBOR = 4, /* sends message to the next node (potentially connected to the same router) */
    RANDOM_OTHER_GROUP = 5

};

struct svr_state
{
    int msg_sent_count;   /* requests sent */
    int msg_recvd_count;  /* requests recvd */
    int local_recvd_count; /* number of local messages received */
    tw_stime start_ts;    /* time that we started sending requests */
    tw_stime end_ts;      /* time that we ended sending requests */
    int svr_id;
    int dest_id;
};

struct svr_msg
{
    enum svr_event svr_event_type;
    tw_lpid src;          /* source of this request or ack */
    int incremented_flag; /* helper for reverse computation */
    model_net_event_return event_rc;
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
    (commit_f) NULL,
    (final_f)  svr_finalize,
    (map_f) codes_mapping,
    sizeof(svr_state),
};

// /* setup for the ROSS event tracing
//  */
// void dally_svr_event_collect(svr_msg *m, tw_lp *lp, char *buffer, int *collect_flag)
// {
//     (void)lp;
//     (void)collect_flag;
//     int type = (int) m->svr_event_type;
//     memcpy(buffer, &type, sizeof(type));
// }

// /* can add in any model level data to be collected along with simulation engine data
//  * in the ROSS instrumentation.  Will need to update the last field in 
//  * svr_model_types[0] for the size of the data to save in each function call
//  */
// void dally_svr_model_stat_collect(svr_state *s, tw_lp *lp, char *buffer)
// {
//     (void)s;
//     (void)lp;
//     (void)buffer;
//     return;
// }

// st_model_types dally_svr_model_types[] = {
//     {(ev_trace_f) dally_svr_event_collect,
//      sizeof(int),
//      (model_stat_f) dally_svr_model_stat_collect,
//      0,
//      NULL,
//      NULL,
//      0},
//     {NULL, 0, NULL, 0, NULL, NULL, 0}
// };

// static const st_model_types  *dally_svr_get_model_stat_types(void)
// {
//     return(&dally_svr_model_types[0]);
// }

// void dally_svr_register_model_types()
// {
//     st_model_type_register("nw-lp", dally_svr_get_model_stat_types());
// }

const tw_optdef app_opt [] =
{
        TWOPT_GROUP("Model net synthetic traffic " ),
    	TWOPT_UINT("traffic", traffic, "UNIFORM RANDOM=1, NEAREST NEIGHBOR=2 "),
    	TWOPT_UINT("num_messages", num_msgs, "Number of messages to be generated per terminal "),
    	TWOPT_UINT("payload_sz",PAYLOAD_SZ, "size of the message being sent "),
    	TWOPT_STIME("sampling-interval", sampling_interval, "the sampling interval "),
    	TWOPT_STIME("sampling-end-time", sampling_end_time, "sampling end time "),
	    TWOPT_STIME("arrival_time", arrival_time, "INTER-ARRIVAL TIME"),
        TWOPT_CHAR("lp-io-dir", lp_io_dir, "Where to place io output (unspecified -> no output"),
        TWOPT_UINT("lp-io-use-suffix", lp_io_use_suffix, "Whether to append uniq suffix to lp-io directory (default 0)"),
        TWOPT_END()
};

const tw_lptype* svr_get_lp_type()
{
            return(&svr_lp);
}

static void svr_add_lp_type()
{
  lp_type_register("nw-lp", svr_get_lp_type());
}

static void issue_event(
    svr_state * ns,
    tw_lp * lp)
{
    (void)ns;
    tw_event *e;
    svr_msg *m;
    tw_stime kickoff_time;

    /* each server sends a dummy event to itself that will kick off the real
     * simulation
     */

    /* skew each kickoff event slightly to help avoid event ties later on */
    kickoff_time = 1.1 * g_tw_lookahead + tw_rand_exponential(lp->rng, arrival_time);

    e = tw_event_new(lp->gid, kickoff_time, lp);
    m = tw_event_data(e);
    m->svr_event_type = KICKOFF;
    tw_event_send(e);
}

static void svr_init(
    svr_state * ns,
    tw_lp * lp)
{
    ns->start_ts = 0.0;
    ns->dest_id = -1;
    ns->svr_id = codes_mapping_get_lp_relative_id(lp->gid, 0, 0);

    issue_event(ns, lp);
    return;
}

static void handle_kickoff_rev_event(
            svr_state * ns,
            tw_bf * b,
            svr_msg * m,
            tw_lp * lp)
{
    if(m->incremented_flag)
        return;

    if(b->c1)
        tw_rand_reverse_unif(lp->rng);

    if(b->c8)
        tw_rand_reverse_unif(lp->rng);
    if(traffic == RANDOM_OTHER_GROUP) {
        tw_rand_reverse_unif(lp->rng);
        tw_rand_reverse_unif(lp->rng);
    }

    model_net_event_rc2(lp, &m->event_rc);
	ns->msg_sent_count--;
    tw_rand_reverse_unif(lp->rng);
}
static void handle_kickoff_event(
	    svr_state * ns,
	    tw_bf * b,
	    svr_msg * m,
	    tw_lp * lp)
{
    if(ns->msg_sent_count >= num_msgs)
    {
        m->incremented_flag = 1;
        return;
    }

    m->incremented_flag = 0;

    char anno[MAX_NAME_LENGTH];
    tw_lpid local_dest = -1, global_dest = -1;

    svr_msg * m_local = malloc(sizeof(svr_msg));
    svr_msg * m_remote = malloc(sizeof(svr_msg));

    m_local->svr_event_type = LOCAL;
    m_local->src = lp->gid;

    memcpy(m_remote, m_local, sizeof(svr_msg));
    m_remote->svr_event_type = REMOTE;

    assert(net_id == DRAGONFLY || net_id == DRAGONFLY_DALLY); /* only supported for dragonfly model right now. */
    ns->start_ts = tw_now(lp);
    codes_mapping_get_lp_info(lp->gid, group_name, &group_index, lp_type_name, &lp_type_index, anno, &rep_id, &offset);
    int local_id = codes_mapping_get_lp_relative_id(lp->gid, 0, 0);

   /* in case of uniform random traffic, send to a random destination. */
   if(traffic == UNIFORM)
   {
    b->c1 = 1;
   	local_dest = tw_rand_integer(lp->rng, 0, num_nodes - 1);
    local_dest = (ns->svr_id + local_dest) % num_nodes;
   }
   else if(traffic == NEAREST_GROUP)
   {
	local_dest = (local_id + num_nodes_per_grp) % num_nodes;
	//printf("\n LP %ld sending to %ld num nodes %d ", local_id, local_dest, num_nodes);
   }
   else if(traffic == NEAREST_NEIGHBOR)
   {
	local_dest =  (local_id + 1) % num_nodes;
//	 printf("\n LP %ld sending to %ld num nodes %d ", rep_id * 2 + offset, local_dest, num_nodes);
   }
   else if(traffic == RAND_PERM)
   {
       if(ns->dest_id == -1)
       {
            b->c8 = 1;
            ns->dest_id = tw_rand_integer(lp->rng, 0, num_nodes - 1); 
            local_dest = ns->dest_id;
       }
       else
       {
        local_dest = ns->dest_id; 
       }
   }
   else if(traffic == RANDOM_OTHER_GROUP)
   {
       int my_group_id = local_id / num_nodes_per_grp;

       int other_groups[num_groups-1];
       int added =0;
       for(int i = 0; i < num_groups; i++)
       {
           if(i != my_group_id) {
               other_groups[added] = i;
               added++;
           }
       }
        int rand_group = other_groups[tw_rand_integer(lp->rng,0,added -1)];
        int rand_node_intra_id = tw_rand_integer(lp->rng, 0, num_nodes_per_grp-1);

        local_dest = (rand_group * num_nodes_per_grp) + rand_node_intra_id;
        printf("\n LP %ld sending to %ld num nodes %d ", local_id, local_dest, num_nodes);

   }
   assert(local_dest < num_nodes);
//   codes_mapping_get_lp_id(group_name, lp_type_name, anno, 1, local_dest / num_servers_per_rep, local_dest % num_servers_per_rep, &global_dest);
   global_dest = codes_mapping_get_lpid_from_relative(local_dest, group_name, lp_type_name, NULL, 0);
   ns->msg_sent_count++;
   m->event_rc = model_net_event(net_id, "test", global_dest, PAYLOAD_SZ, 0.0, sizeof(svr_msg), (const void*)m_remote, sizeof(svr_msg), (const void*)m_local, lp);

   issue_event(ns, lp);
   return;
}

static void handle_remote_rev_event(
            svr_state * ns,
            tw_bf * b,
            svr_msg * m,
            tw_lp * lp)
{
        (void)b;
        (void)m;
        (void)lp;
        ns->msg_recvd_count--;
}

static void handle_remote_event(
	    svr_state * ns,
	    tw_bf * b,
	    svr_msg * m,
	    tw_lp * lp)
{
        (void)b;
        (void)m;
        (void)lp;
	ns->msg_recvd_count++;
}

static void handle_local_rev_event(
                svr_state * ns,
                tw_bf * b,
                svr_msg * m,
                tw_lp * lp)
{
        (void)b;
        (void)m;
        (void)lp;
	ns->local_recvd_count--;
}

static void handle_local_event(
                svr_state * ns,
                tw_bf * b,
                svr_msg * m,
                tw_lp * lp)
{
        (void)b;
        (void)m;
        (void)lp;
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

    //printf("server %llu recvd %d bytes in %f seconds, %f MiB/s sent_count %d recvd_count %d local_count %d \n", (unsigned long long)lp->gid, PAYLOAD_SZ*ns->msg_recvd_count, ns_to_s(ns->end_ts-ns->start_ts),
    //    ((double)(PAYLOAD_SZ*ns->msg_sent_count)/(double)(1024*1024)/ns_to_s(ns->end_ts-ns->start_ts)), ns->msg_sent_count, ns->msg_recvd_count, ns->local_recvd_count);
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
    int num_router_rows, num_router_cols;

    tw_opt_add(app_opt);
    tw_init(&argc, &argv);

    if(argc < 2)
    {
            printf("\n Usage: mpirun <args> --sync=2/3 mapping_file_name.conf (optional --nkp) ");
            MPI_Finalize();
            return 0;
    }

    MPI_Comm_rank(MPI_COMM_CODES, &rank);
    MPI_Comm_size(MPI_COMM_CODES, &nprocs);

    configuration_load(argv[2], MPI_COMM_CODES, &config);

    model_net_register();
    svr_add_lp_type();

    // if (g_st_ev_trace || g_st_model_stats || g_st_use_analysis_lps)
    //     dally_svr_register_model_types();

    codes_mapping_setup();

    net_ids = model_net_configure(&num_nets);
    //assert(num_nets==1);
    net_id = *net_ids;
    free(net_ids);

    /* 5 days of simulation time */
    g_tw_ts_end = s_to_ns(5 * 24 * 60 * 60);
    model_net_enable_sampling(sampling_interval, sampling_end_time);

    if(net_id != DRAGONFLY && net_id != DRAGONFLY_DALLY)
    {
	printf("\n The test works with dragonfly model configuration only! %d %d ", DRAGONFLY_DALLY, net_id);
        MPI_Finalize();
        return 0;
    }
    num_servers_per_rep = codes_mapping_get_lp_count("MODELNET_GRP", 1, "nw-lp",
            NULL, 1);

    int num_routers;

    configuration_get_value_int(&config, "PARAMS", "num_routers", NULL, &num_routers);

    configuration_get_value_int(&config, "PARAMS", "num_groups", NULL, &num_groups);
    configuration_get_value_int(&config, "PARAMS", "num_cns_per_router", NULL, &num_nodes_per_cn);

    num_routers_per_grp = num_routers;

    num_nodes = num_groups * num_routers_per_grp * num_nodes_per_cn;
    num_nodes_per_grp = num_routers_per_grp * num_nodes_per_cn;

    assert(num_nodes);

    if(lp_io_dir[0])
    {
        do_lp_io = 1;
        int flags = lp_io_use_suffix ? LP_IO_UNIQ_SUFFIX : 0;
        int ret = lp_io_prepare(lp_io_dir, flags, &io_handle, MPI_COMM_CODES);
        assert(ret == 0 || !"lp_io_prepare failure");
    }
    tw_run();
    if (do_lp_io){
        int ret = lp_io_flush(io_handle, MPI_COMM_CODES);
        assert(ret == 0 || !"lp_io_flush failure");
    }
    model_net_report_stats(net_id);
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
