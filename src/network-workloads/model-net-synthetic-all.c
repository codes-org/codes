/*
 * This test program generates some synthetic traffic patterns for the slim
 * fly, dragonfly, and fat-tree network models. Currently it supports
 * uniform random, worst-case (slim fly only), 1D nearest neighbor, 2D nearest
 * neighbor, 3D nearest neighbor, gather, scatter, and bisection traffic
 * patterns.
 */

#include "codes/model-net.h"
#include "codes/lp-io.h"
#include "codes/codes.h"
#include "codes/codes_mapping.h"
#include "codes/configuration.h"
#include "codes/lp-type-lookup.h"

#define PRINT_WORST_CASE_MATCH 0

FILE * slimfly_results_log_2=NULL;
FILE * slimfly_ross_csv_log=NULL;

static int net_id = 0;
static int offset = 2;
static int traffic = 1;
static double warm_up_time = 0.0;
static double arrival_time = 0.0;
static double load = 0.0;				//percent utilization of each terminal's uplink when sending messages
static double MEAN_INTERVAL = 0.0;
static int payload_size = 0;
static double link_bandwidth = 0.0;
static int *worst_dest;						//Array mapping worst case destination for each router
static int total_routers;

static char lp_io_dir[356] = {'\0'};
static lp_io_handle io_handle;
static unsigned int lp_io_use_suffix = 0;
static int do_lp_io = 0;
static int num_msgs = 20;

static int num_servers_per_rep = 0;
static int num_routers_per_grp = 0;

static int num_nodes = 0;   // Number of terminals/compute nodes
static int num_servers = 0; // Number of servers/MPI processes

typedef struct svr_msg svr_msg;
typedef struct svr_state svr_state;

/* global variables for codes mapping */
static char group_name[MAX_NAME_LENGTH];
static char lp_type_name[MAX_NAME_LENGTH];
static int group_index, lp_type_index, rep_id, offset;

/* 2D rank communication heat map */
// static int **comm_map;

/* Function for computing local and global connections for a given router id */
static void get_router_connections(int src_router_id, int* local_channels, int* global_channels);

/* Global variables for worst-case workload pairing */
static int num_local_channels;
static int num_global_channels;
int *X;
int *X_prime;
int X_size;

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
    UNIFORM             = 1, /* sends message to a randomly selected node */
    WORST_CASE          = 2, /* sends messages in pairs configured to utilize same worst-case hop router */
    NEAREST_NEIGHBOR_1D = 3, /* sends messages to next and previous nodes */
    NEAREST_NEIGHBOR_2D = 4, /* sends messages to nearest neighbor nodes following 2D node mapping */
    NEAREST_NEIGHBOR_3D = 5, /* sends messages to nearest neighbor nodes following 3D node mapping */
    GATHER              = 6, /* sends messages from all ranks to rank 0 */
    SCATTER             = 7, /* sends messages from rank 0 to all other ranks */
    BISECTION           = 8, /* sends messages between paired nodes in opposing bisection partitions */
    ALL2ALL             = 9, /* sends messages from each rank to all other ranks */
    PING                = 10 /* sends messages from rank 0 to rank 1 */
};

struct svr_state
{
    int msg_sent_count;   /* requests sent */
    int msg_recvd_count;  /* requests recvd */
    int* msg_send_times;  /* send time of all messages */
    int* msg_recvd_times; /* recv time of all messages */
    int local_recvd_count; /* number of local messages received */
    tw_stime start_ts;    /* time that we started sending requests */
    tw_stime end_ts;      /* time that we ended sending requests */
    char output_buf[512]; /* buffer space for lp-io output data */
    char output_buf2[65536]; /* buffer space for lp-io output data */
    char output_buf3[65536]; /* buffer space for msg timing output data */
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

/* setup for the ROSS event tracing
 * can have a different function for  rbev_trace_f and ev_trace_f
 * but right now it is set to the same function for both
 */
void svr_event_collect(svr_msg *m, tw_lp *lp, char *buffer, int *collect_flag)
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
void svr_model_stat_collect(svr_state *s, tw_lp *lp, char *buffer)
{
    (void)s;
    (void)lp;
    (void)buffer;
    return;
}

st_model_types svr_model_types[] = {
        {(ev_trace_f) svr_event_collect,
        sizeof(int),
        (model_stat_f) svr_model_stat_collect,
        0,
        NULL,
        NULL,
        0},
    {NULL, 0, NULL, 0, NULL, NULL, 0}
};

static const st_model_types  *svr_get_model_stat_types(void)
{
    return(&svr_model_types[0]);
}

void svr_register_model_types()
{
    st_model_type_register("nw-lp", svr_get_model_stat_types());
}

const tw_optdef app_opt [] =
{
    TWOPT_GROUP("Model net synthetic traffic " ),
    TWOPT_UINT("traffic", traffic, "1: Uniform Random, 2: Worst-Case, 3: 1D Nearest Neighbor, 4: 2D Nearest Neighbor, 5: 3D Nearest Neighbor, 6: Gather, 7: Scatter, 8: Bisection, 9: All-To-All, 1: Ping"),
    TWOPT_UINT("num_messages", num_msgs, "Number of messages to be generated per terminal "),
    TWOPT_UINT("payload_size", payload_size, "Size of data to be sent per terminal "),
    TWOPT_STIME("arrival_time", arrival_time, "INTER-ARRIVAL TIME"),
    TWOPT_STIME("warm_up_time", warm_up_time, "Time delay before starting stats collection. For generating accurate observed bandwidth calculations"),
    TWOPT_STIME("load", load, "percentage of packet inter-arrival rate to simulate"), 
    TWOPT_CHAR("lp-io-dir", lp_io_dir, "Where to place io output (unspecified -> no output"),
    TWOPT_UINT("lp-io-use-suffix", lp_io_use_suffix, "Whether to append uniq suffix to lp-io directory (default 0)"),
    TWOPT_END(),
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

/** Function checks to see if the provided src and dst routers are two hops away from one another and if they are, sets
 *  the intm input variable to the id of the intermediate router.
 *  @param[in] dest         Local/relative ID of the destination router
 *  @param[in] src          Local/relative ID of the source terminal
 *  @param[in/out] intm     Local/relative ID of the intm router if it exists
 */
bool is_two_hops_away(int src, int dst, int *intm, int *count)
{
    if(src == dst)
        return false;
    int *local_channel_src = (int*) malloc(num_local_channels*sizeof(int));
    int *global_channel_src = (int*) malloc(num_global_channels*sizeof(int));
    int *local_channel_dst = (int*) malloc(num_local_channels*sizeof(int));
    int *global_channel_dst = (int*) malloc(num_global_channels*sizeof(int));
    get_router_connections(src, local_channel_src, global_channel_src);
    get_router_connections(dst, local_channel_dst, global_channel_dst);
    int i,j;
    int intm_count = 0;
    bool return_val = false;
    int temp_vals[6];

    for(i=0;i<num_local_channels;i++)
    {
        for(j=0; j<num_global_channels;j++){
            if(local_channel_src[i] == global_channel_dst[j])
            {
                    temp_vals[intm_count] = local_channel_src[i];
                    intm_count++;
                    return_val = true;
            }
        }
    }
    for(i=0;i<num_global_channels;i++)
    {
        for(j=0; j<num_local_channels;j++){
            if(global_channel_src[i] == local_channel_dst[j])
            {
                    temp_vals[intm_count] = global_channel_src[i];
                    intm_count++;
                    return_val = true;
            }
        }
    }
    for(i=0;i<num_global_channels;i++)
    {
        for(j=0; j<num_global_channels;j++){
            if(global_channel_src[i] == global_channel_dst[j])
            {
                    temp_vals[intm_count] = global_channel_src[i];
                    intm_count++;
                    return_val = true;
            }
        }
    }
    for(i=0;i<num_local_channels;i++)
    {
        for(j=0; j<num_local_channels;j++){
            if(local_channel_src[i] == local_channel_dst[j])
            {
                    temp_vals[intm_count] = local_channel_src[i];
                    intm_count++;
                    return_val = true;
            }
        }
    }
    for(i=0;i<intm_count;i++){
        intm[i] = temp_vals[i];
        //printf("src:%d dst:%d intm router%d: %d\n", src, dst, i, intm[i]);
    }

    *count = intm_count;
    return return_val;
}

/** Latest implementation of function to return an array mapping each router with it's corresponding worst-case router pair
 *  @param[in/out] 	*worst_dest   		Router ID to send all messages to
 */
void init_worst_case_mapping()
{
    int pairings = 0;
    int srcID = -1;
    int* middled;
    //for(int start=0; start<total_routers; start++){
    //    printf("start:%d\n",start);
    int start = 0;
    // variable to track if a router is an intermediate router for another pairing
    middled = (int*)calloc(total_routers,sizeof(int));
    // Loop over all routers
    for(int src_idx=start; src_idx<total_routers+start; src_idx++){
        int src = src_idx % total_routers;
        if(src == srcID)
            printf("src:%d total_routers:%d\n",src, total_routers);
        // If no pairing yet
        if(worst_dest[src] < 0){
            // Check remaining routers
            for(int dst_idx=src+total_routers/2; dst_idx<total_routers+src+total_routers/2; dst_idx++){
                int dst = dst_idx % total_routers;
                if(src == srcID)
                    printf("potential dst:%d\n",dst);
                // Check if destination already has a pairing
                if(worst_dest[dst] < 0 && dst != src){
                    int intm[6];
                    int len_intm = 0;
                    // Check if the destination is two hops from the source
                    if(is_two_hops_away(src, dst, intm, &len_intm)){
                        for(int intm_indx=0;intm_indx<len_intm;intm_indx++){
                            assert(intm[intm_indx]>=0);
                            // Check if the intm router already has a pairing
                            if(worst_dest[intm[intm_indx]] < 0){
                                // Check remaining routers for worst case pairing for intm router
                                for(int intm_dst_idx=intm[intm_indx]+total_routers/2; intm_dst_idx<total_routers+intm[intm_indx]+total_routers/2; intm_dst_idx++){
                                    int intm_dst = intm_dst_idx % total_routers;
                                    // Make sure intm_dst is not equal to dst or src
                                    if(intm_dst != dst && intm_dst != src){
                                        // Check if destination already has a pairing
                                        if(worst_dest[intm_dst] < 0){
                                            int intm2[6];
                                            int len_intm2 = 0;
                                            // Check if the destination is two hops from the source
                                            if(is_two_hops_away(intm[intm_indx], intm_dst, intm2, &len_intm2)){
                                                for(int intm2_indx=0; intm2_indx<len_intm2; intm2_indx++){
                                                    assert(intm2[intm2_indx]>=0);
                                                    // Check if the intm2 router already has been set as an intermediate router
                                                    if(middled[intm2[intm2_indx]] < 1){
                                                        // Check if the destination is either the src or dst router
                                                        if(intm2[intm2_indx] == src){
                                                            worst_dest[src] = dst;
                                                            worst_dest[dst] = src;
                                                            worst_dest[intm[intm_indx]] = intm_dst;
                                                            worst_dest[intm_dst] = intm[intm_indx];
                                                            middled[src]++;
                                                            middled[intm[intm_indx]]++;
                                                            pairings+=4;
                                                            //printf("T0 found pairings: %d<->%d<->%d & %d<->%d<->%d. Total pairings:%d\n",intm_dst, src, intm[intm_indx], src, intm[intm_indx], dst, pairings);
                                                            break;
                                                        }
                                                        if(intm2[intm2_indx] == dst){
                                                            worst_dest[src] = dst;
                                                            worst_dest[dst] = src;
                                                            worst_dest[intm[intm_indx]] = intm_dst;
                                                            worst_dest[intm_dst] = intm[intm_indx];
                                                            middled[dst]++;
                                                            middled[intm[intm_indx]]++;
                                                            pairings+=4;
                                                            //printf("T1 found pairings: %d<->%d<->%d & %d<->%d<->%d. Total pairings:%d\n",src, intm[intm_indx], dst, intm[intm_indx], dst, intm_dst, pairings);
                                                            break;
                                                        }
                                                    }else if(src == srcID)
                                                        printf("intm2:%d is already paired with %d\n",intm2[intm2_indx], worst_dest[intm2[intm2_indx]]);
                                                }if(worst_dest[intm[intm_indx]] < 0){
                                                    if(src == srcID)
                                                        printf("No worst-case pairing found for the intermediate router:%d with src:%d and dst:%d\n",intm[intm_indx], src, dst);
                                                }else{
                                                    break;
                                                }
                                            }else if(src == srcID)
                                                printf("intm_dst:%d is not two hops away from intm:%d\n",intm_dst, intm[intm_indx]);
                                        }else if(src == srcID)
                                            printf("intm_dst:%d is already paired with %d\n",intm_dst, worst_dest[intm_dst]);
                                    }else if(src == srcID)
                                        printf("intm_dst:%d is equal to dst:%d\n", intm_dst, dst);
                                }if(worst_dest[intm[intm_indx]] < 0){
                                    if(src == srcID){
                                        printf("No worst-case pairing found for the intermediate router:%d with src:%d and dst:%d\n",intm[intm_indx], src, dst);
                                    }
                                }else{
                                    break;
                                }
                            }else{
                                if(src == srcID)
                                    printf("dst:%d intm:%d has a pairing\n",dst, intm[intm_indx]);
                            }
                        }if(worst_dest[src] < 0){
                            if(src == srcID){
                                printf("No worst-case pairing found for src:%d and dst:%d\n", src, dst);
                            }
                        }else{
                            if(src == srcID){
                                printf("Found worst-case pairing found for src:%d and dst:%d\n", src, dst);
                            }
                            break;
                        }
                    }else{
                        if(src == srcID)
                            printf("dst:%d isn't two hops away\n",dst);
                    }
                }else{
                    if(src == srcID)
                        printf("dst:%d has connection to %d\n",dst, worst_dest[dst]);
                }
            }
            // Verify a destination was found
            if(worst_dest[src] < 0){
                printf("No worst-case pairing found for source router:%d\n",src);
                //free(middled);
                //break;
                //exit(0);
            }
        }
    }
    //}
    int no_pairing_count = 0;
    int no_reverse_pairing_count = 0;
    int *number_of_pairings = (int*)calloc(total_routers,sizeof(int));
    int *number_of_reverse_pairings = (int*)calloc(total_routers,sizeof(int));
    int *reverse_pairings = (int*)calloc(total_routers,sizeof(int));
    for(int i=0; i<total_routers; i++){
        reverse_pairings[i] = -1;
    }
    for(int i=0; i<total_routers; i++){
        if (worst_dest[i] < 0){
            no_pairing_count++;
            //printf("no mapping for %d\n",i);
        }
        else{
            reverse_pairings[worst_dest[i]]= i;
            number_of_pairings[i]++;
            if(number_of_pairings[i] > 1)
                printf("router:%d has more than one pairing (%d pairings)\n",i,number_of_pairings[i]);
        }
    }
    for(int i=0; i<total_routers; i++){
        if(reverse_pairings[i] < 0){
            no_reverse_pairing_count++;
            //printf("no reverse mapping for %d\n",i);
        }
        else{
            number_of_reverse_pairings[i]++;
            if(number_of_reverse_pairings[i] > 1)
                printf("router:%d has more than one reverse pairing (%d pairings)\n",i,number_of_reverse_pairings[i]);
        }
    }
    printf("number of unpaired routers:%d\n",no_pairing_count);
    printf("number of unpaired reverse routers:%d\n",no_reverse_pairing_count);
}

/** Older implementation of function to return an array mapping each router with it's corresponding worst-case router pair
 *  @param[in/out] 	*worst_dest   		Router ID to send all messages to
 */
void old_init_worst_case_mapping()
{
    int i,j,k;
    int r1,r2;		//Routers to be paired
    for(k=0;k<2;k++)
    {
        for(j=0;j<num_routers_per_grp-1;j+=2)
        {
            for(i=0;i<num_routers_per_grp;i++)
            {
                r1 = i + j*num_routers_per_grp + k*num_routers_per_grp*num_routers_per_grp;
                r2 = i + (j+1)*num_routers_per_grp + k*num_routers_per_grp*num_routers_per_grp;
                worst_dest[r1] = r2;
                worst_dest[r2] = r1;
                //				printf("%d->%d, %d->%d\n",r1,worst_dest[r1],r2,worst_dest[r2]);
            }
        }
    }
    j = num_routers_per_grp-1;
    for(i=0;i<num_routers_per_grp;i++)
    {
        r1 = i + j*num_routers_per_grp + 0*num_routers_per_grp*num_routers_per_grp;
        r2 = i + j*num_routers_per_grp + 1*num_routers_per_grp*num_routers_per_grp;
        worst_dest[r1] = r2;
        worst_dest[r2] = r1;
        //		printf("%d->%d, %d->%d\n",r1,worst_dest[r1],r2,worst_dest[r2]);
    }
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

    configuration_get_value_double(&config, "PARAMS", "cn_bandwidth", NULL, &link_bandwidth);
    if(!link_bandwidth) {
        link_bandwidth = 4.7;
        fprintf(stderr, "Bandwidth of channels not specified, setting to %lf\n", link_bandwidth);
    }

    if(arrival_time!=0)
    {
        MEAN_INTERVAL = arrival_time;
    }
    if(load != 0)
    {
        MEAN_INTERVAL = bytes_to_ns(payload_size, load*link_bandwidth);
        if(traffic == NEAREST_NEIGHBOR_1D){
            MEAN_INTERVAL = MEAN_INTERVAL * 2; //Because we are injecting two messages each interval
        }else if(traffic == NEAREST_NEIGHBOR_2D){
            MEAN_INTERVAL = MEAN_INTERVAL * 4;
        }else if(traffic == NEAREST_NEIGHBOR_3D){
            MEAN_INTERVAL = MEAN_INTERVAL * 6;
        }else if(traffic == SCATTER){
            MEAN_INTERVAL = MEAN_INTERVAL * (num_nodes - 1);
        }else if(traffic == GATHER){
            MEAN_INTERVAL = MEAN_INTERVAL * (num_nodes - 1);
        }else if(traffic == ALL2ALL){
            MEAN_INTERVAL = MEAN_INTERVAL * (num_nodes - 1);
        }

    }

    unsigned int rng_calls = 0;
    //kickoff_time = g_tw_lookahead + tw_rand_normal_sd(lp->rng, MEAN_INTERVAL, MEAN_INTERVAL*0.05, &rng_calls);
    //kickoff_time = g_tw_lookahead + tw_rand_normal_sd(lp->rng, MEAN_INTERVAL, MEAN_INTERVAL*0.00001, &rng_calls);
    kickoff_time = g_tw_lookahead + MEAN_INTERVAL;

    /*if(lp->gid == 0){
        printf("MEAN_INTERVAL:%f\n",MEAN_INTERVAL);
        printf("kickoff_time:%f\n",kickoff_time);
    }*/
    e = tw_event_new(lp->gid, kickoff_time, lp);
    m = tw_event_data(e);
    m->svr_event_type = KICKOFF;
    tw_event_send(e);
}

static void svr_init(
        svr_state * ns,
        tw_lp * lp)
{
    int myRank;
    MPI_Comm_rank(MPI_COMM_CODES, &myRank);

    // Initiailize comm_map 2D array
    // if(!lp->gid){
    //     comm_map = (int**)malloc(num_nodes*sizeof(int*));
    //     for(int i=0; i<num_nodes; i++){
    //         comm_map[i] = (int*)calloc(num_nodes,sizeof(int));
    //     }
    // }

    ns->msg_send_times = (int*)calloc(num_msgs*2,sizeof(int));
    ns->msg_recvd_times = (int*)calloc(num_msgs*2,sizeof(int));

    issue_event(ns, lp);
    return;
}

static void handle_kickoff_rev_event(
        svr_state * ns,
        tw_bf * b,
        svr_msg * m,
        tw_lp * lp)
{
    (void)b;
    (void)m;
    (void)lp;
    if(m->incremented_flag)

        if(b->c1)
            tw_rand_reverse_unif(lp->rng);

    ns->msg_sent_count--;
    model_net_event_rc2(lp, &m->event_rc);

    tw_rand_reverse_unif(lp->rng);
}	
static void handle_kickoff_event(
        svr_state * ns,
        tw_bf * b,
        svr_msg * m,
        tw_lp * lp)
{
    (void)m;
    if(traffic == SCATTER){
        if(ns->msg_sent_count/(num_nodes-1) >= num_msgs){
            m->incremented_flag = 1;
            return;
        }
    }else{
        if(ns->msg_sent_count >= num_msgs)
        {
            m->incremented_flag = 1;
            return;
        }
    }
    m->incremented_flag = 0;

    char anno[MAX_NAME_LENGTH];

    svr_msg * m_local = malloc(sizeof(svr_msg));
    svr_msg * m_remote = malloc(sizeof(svr_msg));

    m_local->svr_event_type = LOCAL;
    m_local->src = lp->gid;

    memcpy(m_remote, m_local, sizeof(svr_msg));
    m_remote->svr_event_type = REMOTE;

    ns->start_ts = tw_now(lp);

    codes_mapping_get_lp_info(lp->gid, group_name, &group_index, lp_type_name, &lp_type_index, anno, &rep_id, &offset);

    int num_server_lps = codes_mapping_get_lp_count(group_name, 1, "nw-lp", NULL, 0);

    int src_terminal_id, src_router_id, dst_router_id;

    int num_transfers = 0;
    int *local_dest;
    tw_lpid global_dest = -1;

    // Compute current server's local/relative ID
    int server_id = rep_id * num_server_lps + offset;

    /* in case of uniform random traffic, send to a random destination. */
    if(traffic == UNIFORM)
    {
        b->c1 = 1;
        num_transfers = 1;
        local_dest = (int*)malloc(num_transfers*sizeof(int));
        local_dest[0] = tw_rand_integer(lp->rng, 0, num_nodes - 1);
    }
    else if(traffic == WORST_CASE)
    {
        num_transfers = 1;
        local_dest = (int*)malloc(num_transfers*sizeof(int));

        // Compute local id of source terminal (same local id as current server, assuming one server per terminal)
        src_terminal_id = (rep_id * num_server_lps) + offset;  
        // Compute loacl id of source router
        src_router_id = src_terminal_id / (num_server_lps);
        // Only send messages if we have a worst-case pairing
        if(worst_dest[src_router_id] < 0)
            return;
        // Get local id of destination router from precomputed worst-case mapping
        dst_router_id = worst_dest[src_router_id];
        // Get local id of destination terminal (same offset connection from dest router as src terminal from src router)
        local_dest[0] = (dst_router_id * num_server_lps) + offset;
    }
    else if(traffic == NEAREST_NEIGHBOR_1D)
    {
        num_transfers = 2;
        local_dest = (int*)malloc(num_transfers*sizeof(int));

        // Neighbors in the first dimension
        local_dest[0] =  (server_id - 1);
        if(local_dest[0] < 0)
            local_dest[0] = num_nodes - 1;
        local_dest[1] =  (server_id + 1) % num_nodes;
    }
    else if(traffic == NEAREST_NEIGHBOR_2D)
    {
        num_transfers = 4;
        local_dest = (int*)malloc(num_transfers*sizeof(int));

        // Neighbors in the first dimension
        local_dest[0] =  (server_id - 1);
        if(local_dest[0] < 0)
            local_dest[0] = num_nodes - 1;
        local_dest[1] =  (server_id + 1) % num_nodes;
        // Neighbors in the second dimension
        local_dest[2] = (server_id + num_nodes/6) % num_nodes;
        local_dest[3] = (server_id - num_nodes/6) % num_nodes;
        if(local_dest[3] < 0)
            local_dest[3] = num_nodes + local_dest[3];
    }
    else if(traffic == NEAREST_NEIGHBOR_3D)
    {
        num_transfers = 6;
        if(net_id == DRAGONFLY_CUSTOM){
            // For Dragonfly the division doesn't work out evenly so we can't use the last 3 nodes
            if(server_id >= num_nodes-3){
                num_transfers = 0;
            }
        }
        num_transfers = 6;
        local_dest = (int*)malloc(num_transfers*sizeof(int));

        // Neighbors in the first dimension
        local_dest[0] =  (server_id - 1);
        if(local_dest[0] < 0)
            local_dest[0] = num_nodes - 1;
        local_dest[1] =  (server_id + 1) % num_nodes;
        // Neighbors in the second dimension
        local_dest[2] = (server_id + num_nodes/3) % num_nodes;
        local_dest[3] = (server_id - num_nodes/3) % num_nodes;
        if(local_dest[3] < 0)
            local_dest[3] = num_nodes + local_dest[3];
        // Neighbors in the third dimension
        local_dest[4] = (server_id + num_nodes/3) % num_nodes;
        local_dest[5] = (server_id - num_nodes/3) % num_nodes;
        if(local_dest[5] < 0)
            local_dest[5] = num_nodes + local_dest[5];
    }
    else if(traffic == GATHER)
    {
        num_transfers = 0;
        if(server_id != 1234){
            num_transfers = 1;
            local_dest = (int*)malloc(num_transfers*sizeof(int));
            local_dest[0] = 1234;
        }else{
            m->incremented_flag = 1;
            return;
        }
    }
    else if(traffic == SCATTER)
    {
        num_transfers = 0;
        if(server_id == 1234){
            num_transfers = num_nodes - 1;
            local_dest = (int*)malloc(num_transfers*sizeof(int));
            for(int i=0; i<1234; i++)
                local_dest[i] = i;
            for(int i=1235; i<num_nodes; i++)
                local_dest[i-1] = i;
        }else{
            m->incremented_flag = 1;
            return;
        }
    }
    else if(traffic == BISECTION)
    {
        num_transfers = 0;
        if(server_id <num_nodes){
            num_transfers = 1;
            local_dest = (int*)malloc(num_transfers*sizeof(int));
            local_dest[0] = (server_id + (int)(num_nodes/2)) % num_nodes;
        }
    }
    else if(traffic == ALL2ALL)
    {
        num_transfers = num_nodes - 1;
        local_dest = (int*)malloc(num_transfers*sizeof(int));
        for(int i=0; i<server_id; i++)
            local_dest[i] = i;
        for(int i=server_id; i<num_nodes; i++)
            local_dest[i-1] = i;
    }
    else if(traffic == PING)
    {
        num_transfers = 0;
        if(server_id == 0){
            num_transfers = 1;
            local_dest = (int*)malloc(num_transfers*sizeof(int));
            local_dest[0] = 1;
        }else{
            m->incremented_flag = 1;
            return;
        }
    }

    for(int i=0; i<num_transfers; i++){
        // Verify local/relative ID of the destination is a valid option
        assert(local_dest[i] < num_nodes);
        // Get global/lp ID of the destination
        codes_mapping_get_lp_id(group_name, lp_type_name, anno, 1, local_dest[i] / num_servers_per_rep, local_dest[i] % num_servers_per_rep, &global_dest);
        // Increment send count in communication heat map
        // comm_map[server_id][local_dest[i]]++;
        // Increment send count
        ns->msg_sent_count++;
        // Issue event
        if( traffic == PING ){
            ns->msg_send_times[ns->msg_sent_count-1] = (int)(tw_now(lp));
            printf("\x1b[35m(%lf) Sending Message %d from server\x1b[0m\n",tw_now(lp),ns->msg_sent_count-1);
        }
        m->event_rc = model_net_event(net_id, "test", global_dest, payload_size, i*0.2, sizeof(svr_msg), (const void*)m_remote, sizeof(svr_msg), (const void*)m_local, lp);
        //printf("%llu kickoff_event() with ts offset: %f\n",LLU(tw_now(lp)),i*0.2);
    }
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
    if( tw_now(lp) >= warm_up_time )
        ns->msg_recvd_count++;
    if( traffic == PING ){
        ns->msg_recvd_times[ns->msg_recvd_count-1] = (int)(tw_now(lp));
        //printf("\x1b[33m(%lf) Received Message %d at server\x1b[0m\n",tw_now(lp),ns->msg_recvd_count);
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

int index_mine = 0;

static void svr_finalize(
        svr_state * ns,
        tw_lp * lp)
{
    ns->end_ts = tw_now(lp);

    double observed_load_time = ((double)ns->end_ts-warm_up_time);
    double observed_load = ((double)payload_size*(double)ns->msg_recvd_count)/((double)ns->end_ts-warm_up_time);
    observed_load = observed_load * (double)(1000*1000*1000);
    observed_load = observed_load / (double)(1024*1024*1024);
    int written = 0;
    int written2 =0;
    int written3 =0;

    if(lp->gid == 0){
        written = sprintf(ns->output_buf, "# Format <LP id> <Msgs Sent> <Msgs Recvd> <Bytes Sent> <Bytes Recvd> <Offered Load [GBps]> <Observed Load [GBps]> <End Time [ns]> <Observed Load Time [ns]>\n");
        // written2 = sprintf(ns->output_buf2, "# Format <server ID> <sends to svr 0> <sends to svr 1> ... <sends to svr N>\n");
        written3 = sprintf(ns->output_buf3, "# Format <Server ID> <Msg ID> <Send Time> <Recv Time>\n");
    }

    written += sprintf(ns->output_buf + written, "%llu %d %d %d %d %f %f %f %f\n",LLU(lp->gid), ns->msg_sent_count, ns->msg_recvd_count,
            payload_size*ns->msg_sent_count, payload_size*ns->msg_recvd_count, load*link_bandwidth, observed_load, ns->end_ts, observed_load_time);

    lp_io_write(lp->gid, "synthetic-stats", written, ns->output_buf);

    // Get server mapping info
    char anno[MAX_NAME_LENGTH];
    int num_server_lps = codes_mapping_get_lp_count(group_name, 1, "nw-lp", NULL, 0);
    codes_mapping_get_lp_info(lp->gid, group_name, &group_index, lp_type_name, &lp_type_index, anno, &rep_id, &offset);
    // Compute current server's local/relative ID
    int server_id = rep_id * num_server_lps + offset;
    // written2 += sprintf(ns->output_buf2 + written2, "%d ", server_id);
    // for(int j=0; j<num_nodes; j++){
    //     written2 += sprintf(ns->output_buf2 + written2, "%d ", comm_map[server_id][j]);
    // }
    // written2 += sprintf(ns->output_buf2 + written2, "\n");
    // lp_io_write(lp->gid, "communication-map", written2, ns->output_buf2);

    if(traffic == PING){
        for(int j=0; j<num_msgs*2; j++)
            written3 += sprintf(ns->output_buf3 + written3, "%d %d %d %d\n", server_id, j, ns->msg_send_times[j], ns->msg_recvd_times[j]);
        lp_io_write(lp->gid, "server-msg-times", written3, ns->output_buf3);
    }

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

    tw_opt_add(app_opt);
    tw_init(&argc, &argv);

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

    if (g_st_ev_trace || g_st_model_stats || g_st_use_analysis_lps)
        svr_register_model_types();

    codes_mapping_setup();
    net_ids = model_net_configure(&num_nets);
    net_id = *net_ids;
    free(net_ids);

    /*if(num_nets != 1)
    {
        printf("\n The test works with only one network model configured");
        MPI_Finalize();
        return 0;
    }*/

    if(net_id == SLIMFLY){
        num_nodes = codes_mapping_get_lp_count("MODELNET_GRP", 0, "modelnet_slimfly", NULL, 1);
        configuration_get_value_int(&config, "PARAMS", "num_routers", NULL, &num_routers_per_grp);
        total_routers = num_routers_per_grp * num_routers_per_grp * 2;
    }
    else if(net_id == DRAGONFLY_CUSTOM)
        num_nodes = codes_mapping_get_lp_count("MODELNET_GRP", 0, "modelnet_dragonfly_custom", NULL, 1);
    else if(net_id == FATTREE)
        num_nodes = codes_mapping_get_lp_count("MODELNET_GRP", 0, "modelnet_fattree", NULL, 1);

    num_servers_per_rep = codes_mapping_get_lp_count("MODELNET_GRP", 1, "nw-lp", NULL, 1);
    num_servers = codes_mapping_get_lp_count("MODELNET_GRP", 0, "nw-lp", NULL, 1);

    if(lp_io_dir[0])
    {
        do_lp_io = 1;
        int flags = lp_io_use_suffix ? LP_IO_UNIQ_SUFFIX : 0;
        int ret = lp_io_prepare(lp_io_dir, flags, &io_handle, MPI_COMM_CODES);
        assert(ret == 0 || !"lp_io_prepare failure");
    }

    //WORST_CASE Initialization array
    if(traffic == WORST_CASE)
    {
        if(net_id != SLIMFLY)
        {
            printf("\n The worst-case workload is currently only supported for the slim fly model");
            MPI_Finalize();
            return 0;
        }
        configuration_get_value_int(&config, "PARAMS", "local_channels", NULL, &num_local_channels);
        if(!num_local_channels) {
            num_local_channels = 2;
            fprintf(stderr, "Number of Local channels not specified, setting to %d\n", num_local_channels);
        }

        configuration_get_value_int(&config, "PARAMS", "global_channels", NULL, &num_global_channels);
        if(!num_global_channels) {
            num_global_channels = 2;
            fprintf(stderr, "Number of Global channels not specified, setting to %d\n", num_global_channels);
        }

        char       **values;
        size_t       length;
        int ret = configuration_get_multivalue(&config, "PARAMS", "generator_set_X", NULL, &values, &length);
        if (ret != 1)
            tw_error(TW_LOC, "unable to read PARAMS:generator_set_X\n");
        if (length < 2)
            fprintf(stderr, "generator set X less than 2 elements\n");

        X = (int*)malloc(sizeof(int)*length);
        for (size_t i = 0; i < length; i++)
        {
            X[i] = atoi(values[i]);
        }
        free(values);

        ret = configuration_get_multivalue(&config, "PARAMS", "generator_set_X_prime", NULL, &values, &length);
        if (ret != 1)
            tw_error(TW_LOC, "unable to read PARAMS:generator_set_X_prime\n");
        if (length < 2)
            fprintf(stderr, "generator set  X_prime less than 2 elements\n");

        X_size = length;
        X_prime = (int*)malloc(sizeof(int)*length);
        for (size_t i = 0; i < length; i++)
        {
            X_prime[i] = atoi(values[i]);
        }
        free(values);

        worst_dest = (int*)malloc(total_routers*sizeof(int));
        for(int i=0; i<total_routers; i++){
            worst_dest[i] = -1;
        }
        init_worst_case_mapping();
#if PRINT_WORST_CASE_MATCH
        int l;
        for(l=0; l<total_routers; l++)
        {
            printf("match %d->%d\n",l,worst_dest[l]);
        }
#endif
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

/** Get the local and global router connections for the given source router
 *  @param[in] src_router_id            The local/relative ID for the source router
 *  @param[in] num_global_channels      Number of global connections
 *  @param[in] num_local_channels       Number of local connections
 *  @param[in] total_routers            The total number of routers in one rail
 *  @param[in] *local_channels          Integer array pointer for storing the local connections
 *  @param[in] *global_channels         Integer array pointer for storing the global connections
 */
static void get_router_connections(int src_router_id, int* local_channels, int* global_channels){
    //Compute MMS router layout/connection graph
    int rid_s = src_router_id;	// ID for source router

    int rid_d;						// ID for dest. router
    int s_s,s_d;					// subgraph location for source and destination routers
    int i_s,i_d;					// x or m coordinates for source and destination routers
    int j_s,j_d; 					// y or c coordinates for source and destination routers
    int k;
    int local_idx = 0;
    int global_idx = 0;
    int generator_size = X_size;

    for(rid_d=0;rid_d<total_routers;rid_d++)
    {
        // Decompose source and destination Router IDs into 3D subgraph coordinates (subgraph,i,j)
        if(rid_d >= total_routers/2)
        {
            s_d = 1;
            i_d = (rid_d - total_routers/2) /  num_global_channels;
            j_d = (rid_d - total_routers/2) %  num_global_channels;
        }
        else
        {
            s_d = 0;
            i_d = rid_d /  num_global_channels;
            j_d = rid_d %  num_global_channels;
        }
        if(rid_s >= total_routers/2)
        {
            s_s = 1;
            i_s = (rid_s - total_routers/2) /  num_global_channels;
            j_s = (rid_s - total_routers/2) %  num_global_channels;
        }
        else
        {
            s_s = 0;
            i_s = rid_s /  num_global_channels;
            j_s = rid_s %  num_global_channels;
        }
        // Check for subgraph 0 local connections
        if(s_s==0 && s_d==0)
        {
            if(i_s==i_d)							// equation (2) y-y' is in X'
            {
                for(k=0;k<generator_size;k++)
                {
                    if(abs(j_s-j_d)==X[k])
                    {
                        if(src_router_id >= total_routers)
                            local_channels[local_idx++] = rid_d + total_routers;
                        else
                            local_channels[local_idx++] = rid_d;
                    }
                }
            }
        }
        // Check if global connections
        if(s_s==0 && s_d==1)
        {
            if(j_s == (i_d*i_s + j_d) % num_global_channels)							// equation (3) y=mx+c
            {
                if(src_router_id >= total_routers)
                    global_channels[global_idx++] = rid_d + total_routers;
                else
                    global_channels[global_idx++] = rid_d;
            }
        }
    }

    // Loop over second subgraph source routers
    for(rid_d=total_routers-1;rid_d>=0;rid_d--)
    {
        // Decompose source and destination Router IDs into 3D subgraph coordinates (subgraph,i,j)
        if(rid_d >= total_routers/2)
        {
            s_d = 1;
            i_d = (rid_d -  total_routers/2) /  num_global_channels;
            j_d = (rid_d -  total_routers/2) %  num_global_channels;
        }
        else
        {
            s_d = 0;
            i_d = rid_d /  num_global_channels;
            j_d = rid_d %  num_global_channels;
        }
        if(rid_s >= total_routers/2)
        {
            s_s = 1;
            i_s = (rid_s -  total_routers/2) /  num_global_channels;
            j_s = (rid_s -  total_routers/2) %  num_global_channels;
        }
        else
        {
            s_s = 0;
            i_s = rid_s /  num_global_channels;
            j_s = rid_s %  num_global_channels;
        }
        // Check for subgraph 1 local connections
        if(s_s==1 && s_d==1)
        {
            if(i_s==i_d)							// equation (2) c-c' is in X'
            {
                for(k=0;k<generator_size;k++)
                {
                    if(abs(j_s-j_d)==X_prime[k])
                    {
                        if(src_router_id >= total_routers)
                            local_channels[local_idx++] = rid_d + total_routers;
                        else
                            local_channels[local_idx++] = rid_d;
                    }
                }
            }
        }
        // Check if global connections
        if(s_s==1 && s_d==0)
        {
            if(j_d == (i_s*i_d + j_s) %  num_global_channels)							// equation (3) y=mx+c
            {
                if(src_router_id >= total_routers)
                    global_channels[global_idx++] = rid_d + total_routers;
                else
                    global_channels[global_idx++] = rid_d;
            }
        }
    }

    assert(local_idx == num_local_channels);
    assert(global_idx == num_global_channels);
}
/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */