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
#include "codes/codes-jobmap.h"


static int net_id = 0;
static int traffic = 1;
static int PAYLOAD_SZ = 2048;
static double arrival_time = 1000.0;

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

/* for multi synthetic workloads */ 
#define max_app 5               // maximum number of workload, increase if more is required
#define max_msg 1000000         // maximum number of message can be recorded for latency

typedef tw_stime twoDarray[max_app][max_msg];

struct codes_jobmap_ctx *jobmap_ctx;
struct codes_jobmap_params_list jobmap_p;

static int multi_syn_job = 0;
static int num_apps=0;
static int num_syn_clients=0;

static char workloads_file[8192];              
static char alloc_file[8192];
static char app_job_name[max_app][8192]; //should all be synthetic
static int app_job_nodes[max_app];
static int app_traffic[max_app];
static double app_arrival_time[max_app]; 
static int app_payload_sz[max_app];
static int app_num_msgs[max_app];
static int app_msg_chunk_ratio[max_app];


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
    RANDOM_OTHER_GROUP = 5,

    //for multi-workload with job placement config
    UNIFORM_1 = 6,
    BROADCAST = 7, //Rank 0 sends msg to all other ranks
    TORNADO=9, //each MPI rank communicate with fixed offset away from it
    ALL_TO_ALL=10,
    STENCIL_3D=11,
    ND_BROADCAST = 12, /*Non delayed BC, difference from 7 is that the first broadcasting is immediately issued at time 0*/

    //backgroud noise between three contigous groups: group 0,1,2, bi-directional
    BACKGROUND_NOISE3 = 15,
    ND_BACKGROUND_NOISE3 = 18,
};

struct svr_state
{
    int msg_sent_count;   /* requests sent */
    int msg_recvd_count;  /* requests recvd */
    int local_recvd_count; /* number of local messages received */
    tw_stime start_ts;    /* time that we started sending requests */
    tw_stime end_ts;      /* time that we ended sending requests */
    int dest_id;

    //for multi workload and data accouting
    tw_lpid svr_id;
    int group_id;
    int app_id;
    int local_rank; //rank relative to job
    twoDarray *comm_time;   // comm_time[app_id][msg_id]
    int *num_msg_recvd;     // num_msg_recvd[app_id]
    char output_buf0[512];
    char output_buf1[512];
};

struct svr_msg
{
    enum svr_event svr_event_type;
    tw_lpid src;          /* source of this request or ack */
    int incremented_flag; /* helper for reverse computation */
    model_net_event_return event_rc;

    int app_id;
    tw_lpid dest;
    tw_stime issued_time;
    tw_stime arrive_time;
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

const tw_optdef app_opt [] =
{
        TWOPT_GROUP("Model net synthetic traffic " ),
    	TWOPT_UINT("traffic", traffic, "UNIFORM RANDOM=1, NEAREST NEIGHBOR=2 "),
    	TWOPT_UINT("payload_sz",PAYLOAD_SZ, "size of the message being sent "),
    	TWOPT_UINT("num_messages", num_msgs, "Number of messages to be generated per terminal "),
    	TWOPT_STIME("sampling-interval", sampling_interval, "the sampling interval "),
    	TWOPT_STIME("sampling-end-time", sampling_end_time, "sampling end time "),
	    TWOPT_STIME("arrival_time", arrival_time, "INTER-ARRIVAL TIME"),
        TWOPT_CHAR("lp-io-dir", lp_io_dir, "Where to place io output (unspecified -> no output"),
        TWOPT_UINT("lp-io-use-suffix", lp_io_use_suffix, "Whether to append uniq suffix to lp-io directory (default 0)"),

        TWOPT_CHAR("alloc_file", alloc_file, "allocation file name"),
        TWOPT_CHAR("workload_conf_file", workloads_file, "workload config file name"),

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

    tw_stime init;
    init = tw_now(lp);
    double noise = 1.0;
    int pattern = -1; 

    /* each server sends a dummy event to itself that will kick off the real
     * simulation
     */

    /* skew each kickoff event slightly to help avoid event ties later on */

    if(multi_syn_job){
        kickoff_time =  app_arrival_time[ns->app_id] + tw_rand_exponential(lp->rng, noise); 
    }
    else
        kickoff_time =  arrival_time + tw_rand_exponential(lp->rng, noise); 

    //kickoff_time = 1.1 * g_tw_lookahead + tw_rand_exponential(lp->rng, arrival_time);

    e = tw_event_new(lp->gid, kickoff_time, lp);
    m = tw_event_data(e);
    m->svr_event_type = KICKOFF;
    tw_event_send(e);
}

//no waiting after init svr
static void issue_event_non_delay(
    svr_state * ns,
    tw_lp * lp)
{
    (void)ns;
    tw_event *e;
    svr_msg *m;

    e = tw_event_new(lp->gid, tw_rand_unif(lp->rng)*0.0001, lp);
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

    //message latency recording
    ns->comm_time = calloc(max_app*max_msg , sizeof(tw_stime));
    ns->num_msg_recvd = calloc(max_app, sizeof(int));

    if(!ns->comm_time || !ns->num_msg_recvd){
        printf("calloc failed for svr %llu\n", ns->svr_id);
        assert(0);
    }

    ns->svr_id = codes_mapping_get_lp_relative_id(lp->gid, 0, 0);
    ns->group_id = ns->svr_id/num_nodes_per_grp;

    if(multi_syn_job)
    {
        struct codes_jobmap_id lid;
        lid = codes_jobmap_to_local_id(ns->svr_id, jobmap_ctx);

        if(lid.job == -1)
        {
            ns->app_id = -1;
            ns->local_rank = -1;
            return;
        } else {
            ns->app_id=lid.job;
            ns->local_rank=lid.rank;
        }
    } else {
        ns->app_id=0;
        ns->local_rank=ns->svr_id;
    }

    if(app_traffic[ns->app_id] == ND_BROADCAST || app_traffic[ns->app_id] == ND_BACKGROUND_NOISE3)
        issue_event_non_delay(ns, lp);    
    else
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

    if(multi_syn_job) {
         if(app_traffic[ns->app_id] == RANDOM_OTHER_GROUP) {
            tw_rand_reverse_unif(lp->rng);
            tw_rand_reverse_unif(lp->rng);
        }
    } else {
        if(traffic == RANDOM_OTHER_GROUP) {
            tw_rand_reverse_unif(lp->rng);
            tw_rand_reverse_unif(lp->rng);
        }
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
    int pattern = -1;

    if(multi_syn_job) {
        pattern = app_traffic[ns->app_id];
        if(ns->msg_sent_count >= app_num_msgs[ns->app_id])
        {
            m->incremented_flag = 1;
            return;
        }
    } else {
        pattern = traffic; 
        if(ns->msg_sent_count >= num_msgs)
        {
            m->incremented_flag = 1;
            return;
        }
    }

    m->incremented_flag = 0;

    char anno[MAX_NAME_LENGTH];
    tw_lpid local_dest = -1, global_dest = -1;

    assert(net_id == DRAGONFLY || net_id == DRAGONFLY_PLUS || net_id == DRAGONFLY_CUSTOM); /* only supported for dragonfly model right now. */
    ns->start_ts = tw_now(lp);
    codes_mapping_get_lp_info(lp->gid, group_name, &group_index, lp_type_name, &lp_type_index, anno, &rep_id, &offset);
    int local_id = codes_mapping_get_lp_relative_id(lp->gid, 0, 0);

    //fot multi workload
    tw_lpid intm_dest_id=-1;
    int i, dest_num=0, sender_receiver_from_alloc=0;
    tw_lpid* local_dest_s;  //rank number in the alloc file. aka index in alloc file
    struct codes_jobmap_id jid;
    jid = codes_jobmap_to_local_id(ns->svr_id, jobmap_ctx);
    int num_clients = codes_jobmap_get_num_ranks(jid.job, jobmap_ctx);

    /* in case of uniform random traffic, send to a random destination. */
    if(traffic == UNIFORM)
    {
        b->c1 = 1;
        local_dest = tw_rand_integer(lp->rng, 0, num_nodes - 1);
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
            if(i != my_group_id){
                other_groups[added] = i;
                added++;
            }
        }
        int rand_group = other_groups[tw_rand_integer(lp->rng,0,added -1)];
        int rand_node_intra_id = tw_rand_integer(lp->rng, 0, num_nodes_per_grp-1);

        local_dest = (rand_group * num_nodes_per_grp) + rand_node_intra_id;

        int dest_group_calc = local_dest / num_nodes_per_grp;
        assert(rand_group == dest_group_calc);
        assert(rand_group != my_group_id);
        assert(dest_group_calc != my_group_id);
        
        // printf("\n LP %ld sending to %ld num nodes %d ", local_id, local_dest, num_nodes);
    }

    //synthetic pattern read from workload file
    else if(pattern == UNIFORM_1) {
        assert(multi_syn_job);
        sender_receiver_from_alloc = 1;
        b->c1 = 1;
        dest_num = 1;
        local_dest_s = calloc(dest_num, sizeof(tw_lpid));
        if(!local_dest_s)
            tw_error(TW_LOC,"Error: UNIFORM_1 calloc failed\n");

        local_dest_s[0] = tw_rand_integer(lp->rng, 0, num_clients - 1);
        //printf("UNIFORM_1: num_clients %d, local_dest %llu, ns->local_rank %llu\n", num_clients, LLU(local_dest), LLU(ns->local_rank));
            if(local_dest_s[0] == ns->local_rank){
                local_dest_s[0] = (ns->local_rank + 1) % num_clients;
            }
    }
    else if(pattern == BROADCAST || pattern == ND_BROADCAST) {
        assert(multi_syn_job);
        sender_receiver_from_alloc = 1;
        int i;
        
        if(ns->local_rank != 0)
            return;

        dest_num = num_clients-1;
        local_dest_s = calloc(dest_num, sizeof(tw_lpid));  //store rank number
        if(!local_dest_s)
            tw_error(TW_LOC,"Error: BROADCAST calloc failed\n");

        for (i = 0; i < dest_num; ++i)
            local_dest_s[i] = (ns->local_rank + i + 1) % num_clients;
    }
    else if (pattern == TORNADO) {

        int offset = 1 * num_nodes_per_grp;   //offset is number of multiple group sizes 

        assert(multi_syn_job);
        sender_receiver_from_alloc = 1;
        dest_num = 1;

        local_dest_s = calloc(dest_num, sizeof(tw_lpid));
        if(!local_dest_s)
            tw_error(TW_LOC,"Error: UNIFORM_1 calloc failed\n");

        local_dest_s[0] = (ns->local_rank + offset) % num_clients;
    }

    // 3D Stencil for 2304=12*12*16 job size
    else if(pattern == STENCIL_3D) {
        int x_size=12, y_size=12, z_size=16, x, y, z;

        assert(multi_syn_job);
        sender_receiver_from_alloc = 1;
        dest_num = 6;

        if ( (x_size*y_size*z_size) != num_clients) {
            tw_error(TW_LOC,"Error: Job size %d, not equal to cube size %d\n", num_clients, x_size*y_size*z_size);
        }

        local_dest_s = calloc(dest_num, sizeof(tw_lpid));

        int layer_size = x_size*y_size;

        z = ns->local_rank / layer_size;
        y = (ns->local_rank - z * layer_size) / x_size;
        x = (ns->local_rank - z * layer_size) - y * x_size;

        assert(x<x_size && y<y_size && z<z_size);

        /* x-axis neighbors */
        local_dest_s[0] = layer_size * z + x_size * y + (x+1) % x_size;   
        local_dest_s[1] = layer_size * z + x_size * y + (x-1+x_size) % x_size;     
        /* y-axis neighbor */
        local_dest_s[2] = layer_size * z + x_size * ((y+1) % y_size) + x;
        local_dest_s[3] = layer_size * z + x_size * ((y-1+y_size) % y_size) + x;   
        /* z-axis neighbor */
        local_dest_s[4] = layer_size * ((z + 1) % z_size) + x_size * y + x;
        local_dest_s[5] = layer_size * ((z - 1 + z_size) % z_size) + x_size * y + x; 
    }

    //background noise3
    else if (pattern == BACKGROUND_NOISE3 || pattern == ND_BACKGROUND_NOISE3 ) {
        assert(multi_syn_job);
        sender_receiver_from_alloc = 1;
        dest_num = 2;
        local_dest_s = calloc(dest_num, sizeof(tw_lpid));
        if(!local_dest_s)
            tw_error(TW_LOC,"Error: BACKGROUND3 calloc failed\n");

        switch(ns->group_id){
            case 0:
                local_dest_s[0] = (ns->local_rank + 1) % num_clients;
                local_dest_s[1] = (ns->local_rank + 2) % num_clients;
            break;

            case 1:
                local_dest_s[0] = (ns->local_rank - 1) % num_clients;
                local_dest_s[1] = (ns->local_rank + 1) % num_clients;
            break;

            case 2:
                local_dest_s[0] = (ns->local_rank - 2) % num_clients;
                local_dest_s[1] = (ns->local_rank - 1) % num_clients;
            break;

            default:
                tw_error(TW_LOC, "Node %llu in group %d should not generate BACKGROUND_NOISE3", LLU(ns->svr_id), ns->group_id);
        }
    }

    else {
        tw_error(TW_LOC, "\n Traffic pattern not recoginzed %d \n", pattern);
    }

    if(!sender_receiver_from_alloc){
        assert(local_dest < num_nodes);
        global_dest = codes_mapping_get_lpid_from_relative(local_dest, group_name, lp_type_name, NULL, 0);
        //printf("KICKOFF: intm_dest_id is %llu, global_id is %llu\n", LLU(intm_dest_id), LLU(global_dest));

        ns->msg_sent_count++;
        svr_msg * m_local = malloc(sizeof(svr_msg));
        svr_msg * m_remote = malloc(sizeof(svr_msg));

        m_local->svr_event_type = LOCAL;
        m_local->src = lp->gid;
        m_local->app_id = ns->app_id;
        m_local->dest = local_dest;
        m_local->issued_time = tw_now(lp);

        memcpy(m_remote, m_local, sizeof(svr_msg));
        m_remote->svr_event_type = REMOTE;

        if(multi_syn_job)
            m->event_rc = model_net_event(net_id, "test", global_dest, app_payload_sz[ns->app_id], 0.0, sizeof(svr_msg), (const void*)m_remote, sizeof(svr_msg), (const void*)m_local, lp);
        else
            m->event_rc = model_net_event(net_id, "test", global_dest, PAYLOAD_SZ, 0.0, sizeof(svr_msg), (const void*)m_remote, sizeof(svr_msg), (const void*)m_local, lp);

        issue_event(ns, lp);

        free(m_local);
        free(m_remote);
    }
    else {

        //newly added synthetic pattern all follows the same format: use local_dest_s, dest_num=1 if only 1 destination 
        assert(dest_num);
        assert(multi_syn_job);
        ns->msg_sent_count++;

        int count_tmp = 0;

        for (i = 0; i < dest_num; ++i)
        {

            count_tmp++;

            jid.rank = local_dest_s[i]; //rank id
            intm_dest_id = codes_jobmap_to_global_id(jid, jobmap_ctx); //node id
            assert(intm_dest_id < num_nodes);
            global_dest = codes_mapping_get_lpid_from_relative(intm_dest_id, group_name, lp_type_name, NULL, 0); //node lpid
            
            svr_msg * m_local = malloc(sizeof(svr_msg));
            svr_msg * m_remote = malloc(sizeof(svr_msg));

            m_local->svr_event_type = LOCAL;
            m_local->src = lp->gid;
            m_local->app_id = ns->app_id;
            m_local->dest = intm_dest_id;
            m_local->issued_time = tw_now(lp);

            memcpy(m_remote, m_local, sizeof(svr_msg));
            m_remote->svr_event_type = REMOTE;

            m->event_rc = model_net_event(net_id, "test", global_dest, app_payload_sz[ns->app_id], 0.0, sizeof(svr_msg), (const void*)m_remote, sizeof(svr_msg), (const void*)m_local, lp);

            //if(ns->svr_id ==0) printf("KICKOFF: msg geneerated at svr_id %llu,to intm_dest_id %llu at time %lf\n", LLU(ns->svr_id), LLU(intm_dest_id), m_local->issued_time);
            //printf("KICKOFF: msg geneerated at svr_id %llu,to intm_dest_id %llu at time %lf\n", LLU(ns->svr_id), LLU(intm_dest_id), m_local->issued_time);
            //if(ns->local_rank) printf("KICKOFF: App %d : msg geneerated at svr_id %llu,to intm_dest_id %llu at time %lf\n", ns->app_id, LLU(ns->svr_id), LLU(intm_dest_id), m_local->issued_time);

            free(m_local);
            free(m_remote);
        }
        issue_event(ns, lp);
        free(local_dest_s);
    }
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

        int index = -- ns->num_msg_recvd[m->app_id];

        if(app_traffic[m->app_id] < BACKGROUND_NOISE3) {
            if((*ns->comm_time)[m->app_id][index] ==  (m->arrive_time - m->issued_time) ){
                (*ns->comm_time)[m->app_id][index] = 0.0;
            } else {
                tw_error(TW_LOC, "Error:remote reverse calculatioin failed \n svr %llu found travel time %lf but record %lf at index %d\n", ns->svr_id, m->arrive_time - m->issued_time, (*ns->comm_time)[m->app_id][index], index);
            }
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
	ns->msg_recvd_count++;

    if(m->dest != ns->svr_id)
        tw_error(TW_LOC, "Error: msg issued by %llu at time %lfns should arrive at %llu but reached %llu\n", (unsigned long long)m->src, m->issued_time , (unsigned long long) m->dest, (unsigned long long) ns->svr_id);

    m->arrive_time = tw_now(lp);

    int index = ns->num_msg_recvd[m->app_id]++;

    //Non-noise application
    if(app_traffic[m->app_id] < BACKGROUND_NOISE3) {
        if(index > max_msg) 
            tw_error(TW_LOC, "\n More than %d msgs received. Increase 'max_msg' value\n ", max_msg);

        (*ns->comm_time)[m->app_id][index] = m->arrive_time - m->issued_time;
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

    int i,j;
    int total_receive_verify = 0;
    int written = 0; 
    int index = 0; 
    int result;

    if (multi_syn_job){
        for (i = 0; i < num_apps; ++i)
        {

            int index = ns->num_msg_recvd[i];
            total_receive_verify += index;

            if(app_traffic[i] >= BACKGROUND_NOISE3)
                continue;

            char file[512];
            sprintf(file, "application_%d", i);
            written = 0;

            if (ns->svr_id ==0) {
                written = sprintf(ns->output_buf0, "### Application %d, Synthetic Traffic: %d, comm. time (ns)\n <msg comm. time>, <Avg. chunck comm.time>", i, app_traffic[i]);
                result=lp_io_write(lp->gid, file, written, ns->output_buf0);
                if(result!=0)
                    tw_error(TW_LOC, "\nERROR: lpio result %d, svr %llu, lpgid %llu, writting %s failed, written %d, buf %s\n", result, LLU(ns->svr_id), LLU(lp->gid), file, written, ns->output_buf0);
            }

            for (j=0; j<index; j++) {

                written = sprintf(ns->output_buf0, "\n%lf %lf",  (*ns->comm_time)[i][j], (*ns->comm_time)[i][j]/app_msg_chunk_ratio[i]);
                //printf("app[%d][%d]comm.time %lf, buf is %s\n", i, j, (*ns->comm_time)[i][j], ns->output_buf0);
                result=lp_io_write(lp->gid, file, written, ns->output_buf0);
                
                if(result!=0)
                    tw_error(TW_LOC, "\nERROR: lpio result %d, svr %llu, lpgid %llu, writting %s failed, written %d, buf %s, index %d\n", result, LLU(ns->svr_id), LLU(lp->gid), file, written, ns->output_buf0, j);
            } 
        }
    }

    written=0;
    if(ns->svr_id == 0)
        written = sprintf(ns->output_buf1, "#<LP ID> <Terminal ID> <Job ID> <Local Rank> <Total sends> <Total Recvs> <Local count> <lp running time>");

    written += sprintf(ns->output_buf1 + written, "\n %llu %llu %d %d %d %d %d %lf %lf %lf", 
        LLU(lp->gid), LLU(ns->svr_id), ns->app_id, ns->local_rank, ns->msg_sent_count, ns->msg_recvd_count, ns->local_recvd_count, ns->end_ts - ns->start_ts);

    result = lp_io_write(lp->gid, (char*)"mpi-synthetic-stats", written, ns->output_buf1);
    if( result != 0)
        tw_error(TW_LOC, "\nERROR: lpio result %d, svr %llu, lpgid %llu, writting %s failed, written %d, buf %s\n", result, LLU(ns->svr_id), LLU(lp->gid), (char*)"mpi-synthetic-stats", written, ns->output_buf1);
    
//    printf("server %llu recvd %d bytes in %f seconds, %f MiB/s sent_count %d recvd_count %d local_count %d \n", (unsigned long long)lp->gid, PAYLOAD_SZ*ns->msg_recvd_count, ns_to_s(ns->end_ts-ns->start_ts),
//        ((double)(PAYLOAD_SZ*ns->msg_sent_count)/(double)(1024*1024)/ns_to_s(ns->end_ts-ns->start_ts)), ns->msg_sent_count, ns->msg_recvd_count, ns->local_recvd_count);
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
    int num_routers;
    int num_router_leaf, num_router_spine;
    int chunk_size, packet_size;

    tw_opt_add(app_opt);
    tw_init(&argc, &argv);
#ifdef USE_RDAMARIS
    if(g_st_ross_rank)
    { // keep damaris ranks from running code between here up until tw_end()
#endif
    codes_comm_update();

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

    codes_mapping_setup();

    net_ids = model_net_configure(&num_nets);
    //assert(num_nets==1);
    net_id = *net_ids;
    free(net_ids);

    /* 5 days of simulation time */
    g_tw_ts_end = s_to_ns(5 * 24 * 60 * 60);
    model_net_enable_sampling(sampling_interval, sampling_end_time);

    if(!(net_id == DRAGONFLY || net_id == DRAGONFLY_PLUS || net_id == DRAGONFLY_CUSTOM))
    {
	printf("\n The test works with dragonfly model configuration only! %d %d ", DRAGONFLY_PLUS, net_id);
        MPI_Finalize();
        return 0;
    }
    num_servers_per_rep = codes_mapping_get_lp_count("MODELNET_GRP", 1, "nw-lp",
            NULL, 1);
    configuration_get_value_int(&config, "PARAMS", "num_router_leaf", NULL, &num_router_leaf);
    configuration_get_value_int(&config, "PARAMS", "num_router_spine", NULL, &num_router_spine);
    configuration_get_value_int(&config, "PARAMS", "num_routers", NULL, &num_routers);
    configuration_get_value_int(&config, "PARAMS", "num_groups", NULL, &num_groups);
    configuration_get_value_int(&config, "PARAMS", "num_cns_per_router", NULL, &num_nodes_per_cn);

    //num_routers_per_grp = num_routers;
    num_routers_per_grp = num_router_leaf + num_router_spine;

    num_nodes = num_groups * num_router_leaf * num_nodes_per_cn;
    num_nodes_per_grp = num_router_leaf * num_nodes_per_cn;

    assert(num_nodes);

    configuration_get_value_int(&config, "PARAMS", "packet_size", NULL, &packet_size);
    configuration_get_value_int(&config, "PARAMS", "chunk_size", NULL, &chunk_size);

    jobmap_ctx = NULL; 
    num_apps = 0;

    if(strlen(workloads_file) > 0 && strlen(alloc_file)) {
        multi_syn_job = 1;

        FILE *name_file = fopen(workloads_file, "r");
        if(!name_file)
            tw_error(TW_LOC, "\n Could not open file %s ", workloads_file);

        int i = 0;
        char ref = '\n';
        while(!feof(name_file))
        {
            if(i>max_app)
                tw_error(TW_LOC, "\n Support no more than %d jobs, increase 'max_app' value\n", max_app);

            ref = fscanf(name_file, "%d %s %d %lf %d %d", &app_job_nodes[i], app_job_name[i], &app_traffic[i], &app_arrival_time[i], &app_payload_sz[i], &app_num_msgs[i]);

            if(ref != EOF && strncmp(app_job_name[i], "synthetic", 9) == 0)
            {   
                num_syn_clients += app_job_nodes[i];
                num_apps++; 

                app_msg_chunk_ratio[i] = (app_payload_sz[i] + chunk_size - 1) / chunk_size;

                if(rank==0) 
                    printf("\nApp id %d, #nodes %d, traffic#: %d, arrival-time %lf, payload size %d, num msgs %d, msg chunk ratio %d\n", i, app_job_nodes[i], app_traffic[i], app_arrival_time[i], app_payload_sz[i], app_num_msgs[i], app_msg_chunk_ratio[i]);

            } else if(ref!=EOF) {
                if (strncmp(app_job_name[i], "synthetic", 9) != 0)
                   tw_error(TW_LOC, "\nOnly supprot synthetic traffic\n");

                tw_error(TW_LOC, "\nUndefined behavior in %s\n", workloads_file);
 
            }
            i++;
        }
        fclose(name_file);
        jobmap_p.alloc_file = alloc_file;
        jobmap_ctx = codes_jobmap_configure(CODES_JOBMAP_LIST, &jobmap_p);

    } else {
        multi_syn_job = 0;
        num_apps = 1;
        num_syn_clients = num_nodes;
        if(!rank)
            printf("Synthetic Job %d for whole system \n", traffic);

    }

    if(lp_io_dir[0])
    {
        do_lp_io = 1;
        int flags = lp_io_use_suffix ? LP_IO_UNIQ_SUFFIX : 0;
        int ret = lp_io_prepare(lp_io_dir, flags, &io_handle, MPI_COMM_CODES);
        assert(ret == 0 || !"lp_io_prepare failure");
    }

    assert(num_apps <= max_app);
    if(rank==0) 
        printf("\nIntotal of %d applications, begin simulation\n", num_apps);

    tw_run();
    if (do_lp_io){
        int ret = lp_io_flush(io_handle, MPI_COMM_CODES);
        assert(ret == 0 || !"lp_io_flush failure");
    }
    model_net_report_stats(net_id);

    if(multi_syn_job)
       codes_jobmap_destroy(jobmap_ctx);

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
