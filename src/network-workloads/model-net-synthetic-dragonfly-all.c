/*
 * Copyright (C) 2019 Neil McGlohon
 * See LICENSE notice in top-level directory
 */

#include "codes/model-net.h"
#include "codes/lp-io.h"
#include "codes/codes.h"
#include "codes/codes_mapping.h"
#include "codes/configuration.h"
#include "codes/lp-type-lookup.h"
#include "codes/congestion-controller-core.h"

#define G_TW_END_OPT_OVERRIDE 1

static int net_id = 0;
static int traffic = 1;
static double warm_up_time = 0.0;
static double arrival_time = 0.0;
static double load = 0.0;
static int PAYLOAD_SZ = 2048;
static double mean_interval = 0.0;
static double link_bandwidth = 0.0;

static double pe_total_offered_load, pe_total_observed_load = 0.0;
static double pe_max_end_ts = 0.0;

static int num_servers_per_rep = 0;
static int num_routers_per_grp = 0;
static int num_nodes_per_grp = 0;
static int num_nodes_per_router = 0;
static int num_groups = 0;
static unsigned long long num_nodes = 0;
static int total_terminals = 0;
static int num_servers = 0;
static int num_servers_per_terminal = 0;

//Dragonfly Custom Specific values
int num_router_rows, num_router_cols;

//Dragonfly Plus Specific Values
int num_router_leaf, num_router_spine;

//Dragonfly Dally Specific Values
int num_routers; //also used by original Dragonfly

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

/* statistic values for final output */
static tw_stime max_global_server_latency = 0.0;
static tw_stime sum_global_server_latency = 0.0;
static long long sum_global_messages_received = 0;
static tw_stime mean_global_server_latency = 0.0;

/* type of events */
enum svr_event
{
    KICKOFF,	   /* kickoff event */
    REMOTE,        /* remote event */
    LOCAL,      /* local event */
    ACK /*pdes acknowledgement of received message*/
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
    int warm_msg_sent_count;
    int msg_recvd_count;  /* requests recvd */
    int local_recvd_count; /* number of local messages received */
    tw_stime start_ts;    /* time that we started sending requests */
    tw_stime end_ts;      /* time that we ended sending requests */
    int svr_id;
    int dest_id;
    int msg_complete_count; //number of messages that successfully made it to their dest svr

    tw_stime max_server_latency; /* maximum measured packet latency observed by server */
    tw_stime sum_server_latency; /* running sum of measured latencies observed by server for calc of mean */
};

struct svr_msg
{
    enum svr_event svr_event_type;
    tw_lpid src;          /* source of this request or ack */
    tw_stime msg_start_time;
    int completed_sends; /* helper for reverse computation */
    tw_stime saved_time; /* helper for reverse computation */
    tw_stime saved_end_time;
    tw_stime saved_max_end_time;
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

void dragonfly_svr_event_collect(svr_msg *m, tw_lp *lp, char *buffer, int *collect_flag)
{
    (void)lp;
    (void)collect_flag;
    int type = (int) m->svr_event_type;
    memcpy(buffer, &type, sizeof(type));
}

/* can add in any model level data to be collected along with simulation engine data
 * in the ROSS instrumentation.  Will need to update the last field in 
 * svr_model_types[0] for the size of the data to save in each function call
 */
void dragonfly_svr_model_stat_collect(svr_state *s, tw_lp *lp, char *buffer)
{
    (void)s;
    (void)lp;
    (void)buffer;
    return;
}

st_model_types dragonfly_svr_model_types[] = {
    {(ev_trace_f) dragonfly_svr_event_collect,
     sizeof(int),
     (model_stat_f) dragonfly_svr_model_stat_collect,
     0,
     NULL,
     NULL,
     0},
    {NULL, 0, NULL, 0, NULL, NULL, 0}
};

static const st_model_types  *dragonfly_svr_get_model_stat_types(void)
{
    return(&dragonfly_svr_model_types[0]);
}

void dragonfly_svr_register_model_types()
{
    st_model_type_register("nw-lp", dragonfly_svr_get_model_stat_types());
}

const tw_optdef app_opt [] =
{
        TWOPT_GROUP("Model net synthetic traffic " ),
    	TWOPT_UINT("traffic", traffic, "UNIFORM RANDOM=1, NEAREST NEIGHBOR=2 "),
    	TWOPT_UINT("num_messages", num_msgs, "Number of messages to be generated per terminal "),
    	TWOPT_UINT("payload_sz",PAYLOAD_SZ, "size of the message being sent "),
    	TWOPT_STIME("sampling-interval", sampling_interval, "the sampling interval "),
    	TWOPT_STIME("sampling-end-time", sampling_end_time, "sampling end time "),
	    TWOPT_STIME("arrival_time", arrival_time, "INTER-ARRIVAL TIME"),
        TWOPT_STIME("warm_up_time", warm_up_time, "Time delay before starting stats colleciton. For generating accurate observed bandwidth calculations"),
        TWOPT_STIME("load_per_svr", load, "percentage of packet inter-arrival rate to simulate per server"),
        TWOPT_CHAR("lp-io-dir", lp_io_dir, "Where to place io output (unspecified -> no output"),
        TWOPT_UINT("lp-io-use-suffix", lp_io_use_suffix, "Whether to append uniq suffix to lp-io directory (default 0)"),
        TWOPT_CHAR("link_failure_file", g_nm_link_failure_filepath, "filepath for override of link failure file from configuration for supporting models"),
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

/* convert GiB/s and bytes to ns */
static tw_stime bytes_to_ns(uint64_t bytes, double GB_p_s)
{
    tw_stime time;

    /* bytes to GB */
    time = ((double)bytes)/(1024.0*1024.0*1024.0);
    /* MB to s */
    time = time / GB_p_s;
    /* s to ns */
    time = time * 1000.0 * 1000.0 * 1000.0;

    return(time);
}

static uint64_t ns_to_bytes(tw_stime ns, double GB_p_s)
{
    uint64_t bytes;

    bytes = ((ns / (1000.0*1000.0*1000.0))*GB_p_s)*(1024.0*1024.0*1024.0);
    return bytes;
}

static double ns_to_GBps(tw_stime ns, uint64_t bytes)
{
    double GB_p_s;
    GB_p_s = (bytes/(1024.0*1024.0*1024.0))/(ns/(1000.0*1000.0*1000.0));
    return GB_p_s;
}

static tw_stime issue_event(
    svr_state * ns,
    tw_lp * lp)
{
    (void)ns;
    tw_event *e;
    svr_msg *m;
    tw_stime time_offset;

    configuration_get_value_double(&config, "PARAMS", "cn_bandwidth", NULL, &link_bandwidth);
    if(!link_bandwidth) {
        link_bandwidth = 4.7;
        fprintf(stderr, "Bandwidth of channels not specified, setting to %lf\n", link_bandwidth);
    }

    if(mean_interval == 0.0)
    {
        if(arrival_time != 0.0)
        {
            mean_interval = arrival_time;
            load = ns_to_GBps(mean_interval, PAYLOAD_SZ);
        }
        else if (load != 0.0)
        {
            mean_interval = bytes_to_ns(PAYLOAD_SZ, load*link_bandwidth);
            // printf("load=%.2f\n",load);
        }
        else
        {
            mean_interval = 1000.0;
            load = ns_to_GBps(mean_interval, PAYLOAD_SZ);
        }
    }

    /* each server sends a dummy event to itself that will kick off the real
     * simulation
     */

    /* skew each kickoff event slightly to help avoid event ties later on */
    time_offset = g_tw_lookahead + tw_rand_exponential(lp->rng, mean_interval) + tw_rand_unif(lp->rng)*.00001;
    // time_offset = tw_rand_exponential(lp->rng, mean_interval);

    e = tw_event_new(lp->gid, time_offset, lp);
    m = tw_event_data(e);
    m->svr_event_type = KICKOFF;
    tw_event_send(e);

    return time_offset;
}

// static void notify_workload_complete(svr_state *ns, tw_bf *bf, tw_lp *lp)
// {
//     if (g_congestion_control_enabled) {
//         tw_event *e;
//         congestion_control_message *m;
//         tw_stime noise = tw_rand_unif(lp->rng) *.001;
//         bf->c10 = 1;
//         e = tw_event_new(g_cc_supervisory_controller_gid, noise, lp);
//         m = tw_event_data(e);
//         m->type = CC_WORKLOAD_RANK_COMPLETE;
//         tw_event_send(e);
//     }
// }

static void svr_init(
    svr_state * ns,
    tw_lp * lp)
{
    ns->dest_id = -1;
    ns->svr_id = codes_mapping_get_lp_relative_id(lp->gid, 0, 0);
    ns->max_server_latency = 0.0;
    ns->sum_server_latency = 0.0;
    ns->warm_msg_sent_count = 0;

    ns->start_ts = issue_event(ns, lp);
    return;
}

static void handle_kickoff_rev_event(
            svr_state * ns,
            tw_bf * b,
            svr_msg * m,
            tw_lp * lp)
{
    if(m->completed_sends) {
        return;
    }

    if(b->c1)
        tw_rand_reverse_unif(lp->rng);

    if(b->c8)
        tw_rand_reverse_unif(lp->rng);
    if(traffic == RANDOM_OTHER_GROUP) {
        tw_rand_reverse_unif(lp->rng);
        tw_rand_reverse_unif(lp->rng);
    }
    if(b->c5)
        ns->warm_msg_sent_count--; 

    model_net_event_rc2(lp, &m->event_rc);
	ns->msg_sent_count--;
    tw_rand_reverse_unif(lp->rng);
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
        m->completed_sends = 1;
        return;
    }

    m->completed_sends = 0;

    char anno[MAX_NAME_LENGTH];
    tw_lpid local_dest = -1, global_dest = -1;

    svr_msg * m_local = malloc(sizeof(svr_msg));
    svr_msg * m_remote = malloc(sizeof(svr_msg));

    m_local->svr_event_type = LOCAL;
    m_local->src = lp->gid;
    m_local->msg_start_time = tw_now(lp);

    memcpy(m_remote, m_local, sizeof(svr_msg));
    m_remote->svr_event_type = REMOTE;

    codes_mapping_get_lp_info(lp->gid, group_name, &group_index, lp_type_name, &lp_type_index, anno, &rep_id, &offset);
    int local_id = codes_mapping_get_lp_relative_id(lp->gid, 0, 0);

   /* in case of uniform random traffic, send to a random destination. */
   if(traffic == UNIFORM)
   {
    b->c1 = 1;
    local_dest = tw_rand_integer(lp->rng, 1, num_nodes - 2);
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
        printf("\n LP %d sending to %llu num nodes %llu ", local_id, LLU(local_dest), num_nodes);

   }

    if (tw_now(lp) >= warm_up_time) {
        b->c5 = 1;
        ns->warm_msg_sent_count++;
    }

   assert(local_dest < num_nodes);
//   codes_mapping_get_lp_id(group_name, lp_type_name, anno, 1, local_dest / num_servers_per_rep, local_dest % num_servers_per_rep, &global_dest);
   global_dest = codes_mapping_get_lpid_from_relative(local_dest, group_name, lp_type_name, NULL, 0);

   ns->msg_sent_count++;
   m->event_rc = model_net_event(net_id, "test", global_dest, PAYLOAD_SZ, 0.0, sizeof(svr_msg), (const void*)m_remote, sizeof(svr_msg), (const void*)m_local, lp);
   issue_event(ns, lp);
   return;
}

static void handle_ack_event(svr_state * ns, tw_bf *bf, svr_msg *m, tw_lp *lp)
{
    ns->msg_complete_count++;
    if (ns->msg_complete_count >= num_msgs)
    {
        bf->c11 = 1;

        m->saved_end_time = ns->end_ts;
        ns->end_ts = tw_now(lp);

        m->saved_max_end_time = pe_max_end_ts;
        if (ns->end_ts > pe_max_end_ts)
            pe_max_end_ts = ns->end_ts;
    }
    
}

static void handle_ack_event_rc(svr_state * ns, tw_bf *bf, svr_msg *m, tw_lp *lp)
{
    ns->msg_complete_count--;
    if (bf->c11) {
        ns->end_ts = m->saved_end_time;
        
        pe_max_end_ts = m->saved_max_end_time;

        if (bf->c10)
            tw_rand_reverse_unif(lp->rng);
    }

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
    
    if (b->c3) {
        // ns->end_ts = m->saved_end_time;

        ns->msg_recvd_count--;

        tw_stime packet_latency = tw_now(lp) - m->msg_start_time;
        ns->sum_server_latency -= packet_latency;
        if (b->c2)
            ns->max_server_latency = m->saved_time;

        tw_rand_reverse_unif(lp->rng);
    }
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
    
    if (tw_now(lp) >= warm_up_time) {
        b->c3 = 1;
        ns->msg_recvd_count++;

        tw_stime packet_latency = tw_now(lp) - m->msg_start_time;
        ns->sum_server_latency += packet_latency;
        if (packet_latency > ns->max_server_latency) {
            b->c2 = 1;
            m->saved_time = ns->max_server_latency;
            ns->max_server_latency = packet_latency;
        }

        // m->saved_end_time = ns->end_ts;
        // ns->end_ts = tw_now(lp);


        tw_stime noise = tw_rand_unif(lp->rng) * .001;

        tw_event *e;
        svr_msg *new_msg;
        e = tw_event_new(m->src, noise, lp);
        new_msg = tw_event_data(e);
        new_msg->svr_event_type = ACK;
        tw_event_send(e);
    }
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
    if (b->c4)
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
    if (tw_now(lp) >= warm_up_time) {
        b->c4 = 1;
        ns->local_recvd_count++;
    }
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
    tw_stime now = tw_now(lp);
    //add to the global running sums
    sum_global_server_latency += ns->sum_server_latency;
    sum_global_messages_received += ns->msg_recvd_count;

    //compare to global maximum
    if (ns->max_server_latency > max_global_server_latency)
        max_global_server_latency = ns->max_server_latency;

    //this server's mean
    // tw_stime mean_packet_latency = ns->sum_server_latency/ns->msg_recvd_count;


    //add to the global running sums
    sum_global_server_latency += ns->sum_server_latency;
    sum_global_messages_received += ns->msg_recvd_count;

    //compare to global maximum
    if (ns->max_server_latency > max_global_server_latency)
        max_global_server_latency = ns->max_server_latency;

    //this server's mean
    // tw_stime mean_packet_latency = ns->sum_server_latency/ns->msg_recvd_count;

    //printf("server %llu recvd %d bytes in %f seconds, %f MiB/s sent_count %d recvd_count %d local_count %d \n", (unsigned long long)lp->gid, PAYLOAD_SZ*ns->msg_recvd_count, ns_to_s(ns->end_ts-ns->start_ts),
    //    ((double)(PAYLOAD_SZ*ns->msg_sent_count)/(double)(1024*1024)/ns_to_s(ns->end_ts-ns->start_ts)), ns->msg_sent_count, ns->msg_recvd_count, ns->local_recvd_count);
    
    char output_buf[1024];

    double observed_load_time = ((double)ns->end_ts-warm_up_time);
    double observed_load = ((double)PAYLOAD_SZ*(double)ns->msg_recvd_count)/observed_load_time;
    observed_load = observed_load * (double)(1000*1000*1000);
    observed_load = observed_load / (double)(1024*1024*1024);

    // double offered_load = (double)(load*link_bandwidth);

    double offered_load_time = ((double)ns->end_ts-warm_up_time);
    double offered_load = ((double)PAYLOAD_SZ*(double)ns->warm_msg_sent_count)/offered_load_time;
    offered_load = offered_load * (double)(1000*1000*1000);
    offered_load = offered_load / (double)(1024*1024*1024);

    int written = 0;
    int written2 = 0;

    // printf("%.2f Offered | %.2f Observed locally\n",offered_load,observed_load);

    pe_total_offered_load+= offered_load;
    pe_total_observed_load+= observed_load;

    if(lp->gid == 0){
        written = sprintf(output_buf, "# Format <LP id> <Msgs Sent> <Msgs Recvd> <Bytes Sent> <Bytes Recvd> <Offered Load [GBps]> <Observed Load [GBps]> <End Time [ns]>\n");
    }

    written += sprintf(output_buf + written, "%llu %d %d %d %d %f %f %f %f\n",LLU(lp->gid), ns->msg_sent_count, ns->msg_recvd_count,
            PAYLOAD_SZ*ns->msg_sent_count, PAYLOAD_SZ*ns->msg_recvd_count, load*link_bandwidth, observed_load, ns->end_ts, observed_load_time);

    lp_io_write(lp->gid, "synthetic-stats", written, output_buf);
    
    
    
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
    case ACK:
        handle_ack_event_rc(ns, b, m, lp);
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
        case ACK:
            handle_ack_event(ns, b, m, lp);
            break;
        default:
            printf("\n Invalid message type %d ", m->svr_event_type);
            assert(0);
        break;
    }
}

// does MPI reduces across PEs to generate stats based on the global static variables in this file
static void svr_report_stats()
{
    long long total_received_messages;
    tw_stime total_sum_latency, max_latency, mean_latency;
    tw_stime max_end_time;

    MPI_Reduce( &sum_global_messages_received, &total_received_messages, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_CODES);
    MPI_Reduce( &sum_global_server_latency, &total_sum_latency, 1,MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_CODES);
    MPI_Reduce( &max_global_server_latency, &max_latency, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_CODES);
    MPI_Reduce( &pe_max_end_ts, &max_end_time, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_CODES);

    mean_latency = total_sum_latency / total_received_messages;

    if(!g_tw_mynode)
    {	
        printf("\nSynthetic Workload LP Stats: Mean Message Latency: %lf us,  Maximum Message Latency: %lf us, Total Messages Received: %lld\n",
                (float)mean_latency / 1000, (float)max_latency / 1000, total_received_messages);
        printf("\tMaximum Workload End Time %.2f\n",max_end_time);
    }
}



static void aggregate_svr_stats(int myrank)
{

    double agg_offered_load, agg_observed_load;
    MPI_Reduce(&pe_total_offered_load, &agg_offered_load, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_CODES);
    MPI_Reduce(&pe_total_observed_load, &agg_observed_load, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_CODES);

    double avg_offered_load = agg_offered_load / num_servers * num_servers_per_terminal;
    double avg_observed_load = agg_observed_load / num_servers * num_servers_per_terminal;

    if (myrank == 0) {
        printf("\nSynthetic-All Stats ---\n");
        printf("AVG OFFERED LOAD = %.3f     |     AVG OBSERVED LOAD = %.3f\n",avg_offered_load, avg_observed_load);
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

    tw_opt_add(app_opt);
    tw_init(&argc, &argv);

#ifdef USE_RDAMARIS
    if(g_st_ross_rank)
    { // keep damaris ranks from running code between here up until tw_end()
#endif
    codes_comm_update();


    if(argc < 2)
    {
            printf("\n Usage: mpirun <args> --sync=1/2/3 -- <config_file.conf> ");
            MPI_Finalize();
            return 0;
    }

    MPI_Comm_rank(MPI_COMM_CODES, &rank);
    MPI_Comm_size(MPI_COMM_CODES, &nprocs);

    configuration_load(argv[2], MPI_COMM_CODES, &config);

    model_net_register();
    svr_add_lp_type();

    if (g_st_ev_trace || g_st_model_stats || g_st_use_analysis_lps)
        dragonfly_svr_register_model_types();

    codes_mapping_setup();

    net_ids = model_net_configure(&num_nets);
    //assert(num_nets==1);
    net_id = *net_ids;
    free(net_ids);

    if(G_TW_END_OPT_OVERRIDE) {
        /* 5 days of simulation time */
        g_tw_ts_end = s_to_ns(5 * 24 * 60 * 60);
    }
    model_net_enable_sampling(sampling_interval, sampling_end_time);

    if(!(net_id == DRAGONFLY_DALLY || net_id == DRAGONFLY_PLUS || net_id == DRAGONFLY_CUSTOM || net_id == DRAGONFLY))
    {
	printf("\n The workload generator is designed to only work with Dragonfly based model (Dally, Plus, Custom, Original) configuration only! %d %d ", DRAGONFLY_DALLY, net_id);
        MPI_Finalize();
        return 0;
    }
    num_servers_per_rep = codes_mapping_get_lp_count("MODELNET_GRP", 1, "nw-lp",
            NULL, 1);

    int num_routers_with_cns_per_group;

    if (net_id == DRAGONFLY_DALLY) {
        if (!rank)
            printf("Synthetic Generator: Detected Dragonfly Dally\n");
        configuration_get_value_int(&config, "PARAMS", "num_routers", NULL, &num_routers);
        configuration_get_value_int(&config, "PARAMS", "num_groups", NULL, &num_groups);
        configuration_get_value_int(&config, "PARAMS", "num_cns_per_router", NULL, &num_nodes_per_router);
        total_terminals = codes_mapping_get_lp_count("MODELNET_GRP", 0, "modelnet_dragonfly_dally", NULL, 1);
        num_routers_with_cns_per_group = num_routers;
    }
    else if (net_id == DRAGONFLY_PLUS) {
        if (!rank)
            printf("Synthetic Generator: Detected Dragonfly Plus\n");
        configuration_get_value_int(&config, "PARAMS", "num_router_leaf", NULL, &num_router_leaf);
        configuration_get_value_int(&config, "PARAMS", "num_router_spine", NULL, &num_router_spine);
        configuration_get_value_int(&config, "PARAMS", "num_routers", NULL, &num_routers);
        configuration_get_value_int(&config, "PARAMS", "num_groups", NULL, &num_groups);
        configuration_get_value_int(&config, "PARAMS", "num_cns_per_router", NULL, &num_nodes_per_router);
        total_terminals = codes_mapping_get_lp_count("MODELNET_GRP", 0, "modelnet_dragonfly_plus", NULL, 1);
        num_routers_with_cns_per_group = num_router_leaf;

    }
    else if (net_id == DRAGONFLY_CUSTOM) {
        if (!rank)
            printf("Synthetic Generator: Detected Dragonfly Custom\n");
        configuration_get_value_int(&config, "PARAMS", "num_router_rows", NULL, &num_router_rows);
        configuration_get_value_int(&config, "PARAMS", "num_router_cols", NULL, &num_router_cols);
        configuration_get_value_int(&config, "PARAMS", "num_groups", NULL, &num_groups);
        configuration_get_value_int(&config, "PARAMS", "num_cns_per_router", NULL, &num_nodes_per_router);
        total_terminals = codes_mapping_get_lp_count("MODELNET_GRP", 0, "modelnet_dragonfly_custom", NULL, 1);
        num_routers_with_cns_per_group = num_router_rows * num_router_cols;
    }
    else if (net_id == DRAGONFLY) {
        if (!rank)
            printf("Synthetic Generator: Detected Dragonfly Original 1D\n");
        configuration_get_value_int(&config, "PARAMS", "num_routers", NULL, &num_routers);
        num_nodes_per_router = num_routers/2;
        num_routers_with_cns_per_group = num_routers;
        num_groups = num_routers * num_nodes_per_router + 1;
    }

    num_servers = codes_mapping_get_lp_count("MODELNET_GRP", 0, "nw-lp",
            NULL, 1);
    num_nodes = num_servers;
    num_servers_per_terminal = num_servers / total_terminals;
    // num_nodes = num_groups * num_routers_with_cns_per_group * num_nodes_per_router;
    num_nodes_per_grp = (num_nodes / num_groups);

    assert(num_nodes);

    struct codes_jobmap_params_identity jobmap_ident_p;
    jobmap_ident_p.num_ranks = num_servers;
    struct codes_jobmap_ctx * jobmap_ctx = codes_jobmap_configure(CODES_JOBMAP_IDENTITY, &jobmap_ident_p);
    // congestion_control_set_jobmap(jobmap_ctx, net_id); //must be placed after codes_mapping_setup - where g_congestion_control_enabled is set

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
    svr_report_stats();
    aggregate_svr_stats(rank);

#ifdef USE_RDAMARIS
    } // end if(g_st_ross_rank)
#endif
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
