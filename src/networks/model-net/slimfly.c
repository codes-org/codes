/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

// Local router ID: 0 --- total_router-1
// Router LP ID
// Terminal LP ID

#include <ross.h>

#include "codes/jenkins-hash.h"
#include "codes/codes_mapping.h"
#include "codes/codes.h"
#include "codes/model-net.h"
#include "codes/model-net-method.h"
#include "codes/model-net-lp.h"
#include "codes/net/slimfly.h"
#include "sys/file.h"
#include "codes/quickhash.h"
#include "codes/rc-stack.h"

#define CREDIT_SIZE 8
#define MEAN_PROCESS 1.0

/* collective specific parameters */
#define DFLY_HASH_TABLE_SIZE 65536

// debugging parameters
#define TRACK 4
//#define TRACK 100001
#define TRACK_MSG 0
#define TRACK_OUTPUT 1
#define DEBUG 0
#define DEBUG_ROUTING 0
#define USE_DIRECT_SCHEME 1
#define LOAD_FROM_FILE 0

#define LP_CONFIG_NM (model_net_lp_config_names[SLIMFLY])
#define LP_METHOD_NM (model_net_method_names[SLIMFLY])

/* Begin Visualization Piece */
#define TERMINAL_SENDS_RECVS_LOG 0
#define ROUTER_SENDS_RECVS_LOG 0
#define TERMINAL_OCCUPANCY_LOG 0
#define ROUTER_OCCUPANCY_LOG 0
#define PARAMS_LOG 1
#define N_COLLECT_POINTS 100

/*unsigned long terminal_sends[TEMP_NUM_TERMINALS][N_COLLECT_POINTS];
  unsigned long terminal_recvs[TEMP_NUM_TERMINALS][N_COLLECT_POINTS];
  unsigned long router_sends[TEMP_NUM_ROUTERS][N_COLLECT_POINTS];
  unsigned long router_recvs[TEMP_NUM_ROUTERS][N_COLLECT_POINTS];
  int vc_occupancy_storage_router[TEMP_NUM_ROUTERS][TEMP_RADIX][TEMP_NUM_VC][N_COLLECT_POINTS];
  int vc_occupancy_storage_terminal[TEMP_NUM_TERMINALS][TEMP_NUM_VC][N_COLLECT_POINTS];
  */FILE * slimfly_terminal_sends_recvs_log = NULL;
FILE * slimfly_router_sends_recvs_log = NULL;
FILE * slimfly_router_occupancy_log=NULL;
FILE * slimfly_terminal_occupancy_log=NULL;
FILE * slimfly_results_log=NULL;
int TEMP_RADIX;
int TEMP_NUM_VC;
int slim_total_routers_noah;
int slim_total_terminals_noah;
/* End Visualization Piece */

/*Begin Misc*/
FILE * MMS_input_file=NULL;
int csf_ratio = 1;						//Constant selected to balance the ratio between minimal and indirect routes
int num_indirect_routes = 4;			//Number of indirect (Valiant) routes to use in Adaptive routing methods
float adaptive_threshold = 0.1;
float pe_throughput_percent = 0.0;
float pe_throughput = 0.0;

int *X;
int *X_prime;
int X_size;
/*End Misc*/

static double maxd(double a, double b) { return a < b ? b : a; }

/* minimal and non-minimal packet counts for adaptive routing*/
static int minimal_count=0, nonmin_count=0;

typedef struct slimfly_param slimfly_param;
/* annotation-specific parameters (unannotated entry occurs at the
 * last index) */
static uint64_t                  num_params = 0;
static slimfly_param         * all_params = NULL;
static const config_anno_map_t * anno_map   = NULL;

/* global variables for codes mapping */
static char lp_group_name[MAX_NAME_LENGTH];
static int mapping_grp_id, mapping_type_id, mapping_rep_id, mapping_offset;

/* router magic number */
int slim_router_magic_num = 0;

/* terminal magic number */
int slim_terminal_magic_num = 0;

typedef struct slim_terminal_message_list slim_terminal_message_list;
struct slim_terminal_message_list {
    slim_terminal_message msg;
    char* event_data;
    slim_terminal_message_list *next;
    slim_terminal_message_list *prev;
};

void slim_init_terminal_message_list(slim_terminal_message_list *this,
        slim_terminal_message *inmsg) {
    this->msg = *inmsg;
    this->event_data = NULL;
    this->next = NULL;
    this->prev = NULL;
}

void slim_delete_terminal_message_list(slim_terminal_message_list *this) {
    if(this->event_data != NULL) free(this->event_data);
    free(this);
}

struct slimfly_param
{
    // configuration parameters
    int num_routers; 		/*NUM_ROUTER Number of routers in a group*/
    double local_bandwidth;	/*LOCAL_BANDWIDTH bandwidth of the router-router channels within a group */
    double global_bandwidth;/*GLOBAL_BANDWIDTH bandwidth of the inter-group router connections */
    double cn_bandwidth;	/*NODE_BANDWIDTH bandwidth of the compute node channels connected to routers */
    int num_vcs; 			/*NUM_VC number of virtual channels */
    int local_vc_size; 		/*LOCAL_VC_SIZE buffer size of the router-router channels */
    int global_vc_size; 	/*GLOBAL_VC_SIZE buffer size of the global channels */
    int cn_vc_size; 		/*TERMINAL_VC_SIZE buffer size of the compute node channels */
    int chunk_size; 		/*CHUNK_SIZE full-sized packets are broken into smaller chunks.*/
    // derived parameters
    int num_cn; 			/*NUM_TERMINALS*/
    int num_groups; 		/*No relation to slim fly*/
    int radix;				/*RADIX*/
    int slim_total_routers;
    int slim_total_terminals;
    int num_global_channels;
    double cn_delay;
    double local_delay;
    double global_delay;
    double credit_delay;
    //slimfly added
    double router_delay;	/*Router processing delay moving packet from input port to output port*/
    double link_delay;		/*Network link latency. Currently encorporated into the arrival time*/
    int num_local_channels;
};

struct sfly_hash_key
{
    uint64_t message_id;
    tw_lpid sender_id;
};

struct sfly_qhash_entry
{
    struct sfly_hash_key key;
    char * remote_event_data;
    int num_chunks;
    int remote_event_size;
    struct qhash_head hash_link;
};

/* handles terminal and router events like packet generate/send/receive/buffer */
typedef enum event_t event_t;
typedef struct terminal_state terminal_state;
typedef struct router_state router_state;

/* slimfly compute node data structure */
struct terminal_state
{
    uint64_t packet_counter;

    // Dragonfly specific parameters
    int router_id;
    int terminal_id;

    // Each terminal will have an input and output channel with the router
    int* vc_occupancy; // NUM_VC
    int num_vcs;
    tw_stime terminal_available_time;
    slim_terminal_message_list **terminal_msgs;
    slim_terminal_message_list **terminal_msgs_tail;
    int in_send_loop;
    // Terminal generate, sends and arrival T_SEND, T_ARRIVAL, T_GENERATE
    // Router-Router Intra-group sends and receives RR_LSEND, RR_LARRIVE
    // Router-Router Inter-group sends and receives RR_GSEND, RR_GARRIVE
    struct mn_stats slimfly_stats_array[CATEGORY_MAX];

    struct rc_stack * st;
    int issueIdle;
    int terminal_length;

    const char * anno;
    const slimfly_param *params;

    struct qhash_table *rank_tbl;
    uint64_t rank_tbl_pop;

    tw_stime   total_time;
    long total_msg_size;
    long total_hops;
    long finished_msgs;
    long finished_chunks;
    long finished_packets;

    tw_stime last_buf_full;
    tw_stime busy_time;

    char output_buf[512];
};

/* terminal event type (1-4) */
enum event_t
{
    T_GENERATE=1,
    T_ARRIVE,
    T_SEND,
    T_BUFFER,
    R_SEND,
    R_ARRIVE,
    R_BUFFER
};
/* status of a virtual channel can be idle, active, allocated or wait for credit */
enum vc_status
{
    VC_IDLE,
    VC_ACTIVE,
    VC_ALLOC,
    VC_CREDIT
};

/* whether the last hop of a packet was global, local or a terminal */
enum last_hop
{
    GLOBAL,
    LOCAL,
    TERMINAL
};

/* three forms of routing algorithms available, adaptive routing is not
 * accurate and fully functional in the current version as the formulas
 * for detecting load on global channels are not very accurate */
enum ROUTING_ALGO
{
    MINIMAL = 0,
    NON_MINIMAL,
    ADAPTIVE
};

struct router_state
{
    int router_id;
    int group_id;

    int* global_channel;
    int* local_channel;

    tw_stime* next_output_available_time;
    slim_terminal_message_list ***pending_msgs;
    slim_terminal_message_list ***pending_msgs_tail;
    slim_terminal_message_list ***queued_msgs;
    slim_terminal_message_list ***queued_msgs_tail;
    int *in_send_loop;
    struct rc_stack * st;

    tw_stime* last_buf_full;
    tw_stime* busy_time;
    tw_stime* busy_time_sample;

    char output_buf[4096];
    char output_buf2[4096];

    int** vc_occupancy;
    int* link_traffic;	//Aren't used

    const char * anno;
    const slimfly_param *params;

    int* prev_hist_num;	//Aren't used
    int* cur_hist_num;	//Aren't used
};

static short routing = MINIMAL;

static tw_stime         slimfly_total_time = 0;
static tw_stime         slimfly_max_latency = 0;

static long long       total_hops = 0;
static long long       N_finished_packets = 0;
static long long       total_msg_sz = 0;
static long long       N_finished_msgs = 0;
static long long       N_finished_chunks = 0;

static int slimfly_rank_hash_compare(
        void *key, struct qhash_head *link)
{
    struct sfly_hash_key *message_key = (struct sfly_hash_key *)key;
    struct sfly_qhash_entry *tmp = NULL;

    tmp = qhash_entry(link, struct sfly_qhash_entry, hash_link);

    if (tmp->key.message_id == message_key->message_id
            && tmp->key.sender_id == message_key->sender_id)
        return 1;

    return 0;
}
static int slimfly_hash_func(void *k, int table_size)
{
    struct sfly_hash_key *tmp = (struct sfly_hash_key *)k;
    uint64_t key = (~tmp->message_id) + (tmp->message_id << 18);
    key = key * 21;
    key = ~key ^ (tmp->sender_id >> 4);
    key = key * tmp->sender_id;
    return (int)(key & (table_size - 1));
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

/* returns the slimfly message size */
static int slimfly_get_msg_sz(void)
{
    return sizeof(slim_terminal_message);
}

static void free_tmp(void * ptr)
{
    struct sfly_qhash_entry * sfly = ptr;
    if(sfly->remote_event_data)
        free(sfly->remote_event_data);
    if(sfly)
        free(sfly);
}

static void append_to_terminal_message_list(
        slim_terminal_message_list ** thisq,
        slim_terminal_message_list ** thistail,
        int index,
        slim_terminal_message_list *msg) {
    if(thisq[index] == NULL) {
        thisq[index] = msg;
    } else {
        thistail[index]->next = msg;
        msg->prev = thistail[index];
    }
    thistail[index] = msg;
}

static void prepend_to_terminal_message_list(
        slim_terminal_message_list ** thisq,
        slim_terminal_message_list ** thistail,
        int index,
        slim_terminal_message_list *msg) {
    if(thisq[index] == NULL) {
        thistail[index] = msg;
    } else {
        thisq[index]->prev = msg;
        msg->next = thisq[index];
    }
    thisq[index] = msg;
}

static slim_terminal_message_list* return_head(
        slim_terminal_message_list ** thisq,
        slim_terminal_message_list ** thistail,
        int index) {
    slim_terminal_message_list *head = thisq[index];
    if(head != NULL) {
        thisq[index] = head->next;
        if(head->next != NULL) {
            head->next->prev = NULL;
            head->next = NULL;
        } else {
            thistail[index] = NULL;
        }
    }
    return head;
}

static slim_terminal_message_list* return_tail(
        slim_terminal_message_list ** thisq,
        slim_terminal_message_list ** thistail,
        int index) {
    slim_terminal_message_list *tail = thistail[index];
    assert(tail);
    if(tail->prev != NULL) {
        tail->prev->next = NULL;
        thistail[index] = tail->prev;
        tail->prev = NULL;
    } else {
        thistail[index] = NULL;
        thisq[index] = NULL;
    }
    return tail;
}

static void slimfly_read_config(const char * anno, slimfly_param *params){
    // shorthand
    slimfly_param *p = params;

    configuration_get_value_int(&config, "PARAMS", "num_routers", anno,
            &p->num_routers);
    if(p->num_routers <= 0) {
        p->num_routers = 4;
        fprintf(stderr, "Number of dimensions not specified, setting to %d\n",
                p->num_routers);
    }

    configuration_get_value_int(&config, "PARAMS", "num_vcs", anno, &p->num_vcs);
    if(!p->num_vcs) {
        p->num_vcs = 4;
        fprintf(stderr, "Virtual channel size not specified, setting to %d\n", p->num_vcs);
    }

    configuration_get_value_int(&config, "PARAMS", "local_vc_size", anno, &p->local_vc_size);
    if(!p->local_vc_size) {
        p->local_vc_size = 1024;
        fprintf(stderr, "Buffer size of local channels not specified, setting to %d\n", p->local_vc_size);
    }

    configuration_get_value_int(&config, "PARAMS", "global_vc_size", anno, &p->global_vc_size);
    if(!p->global_vc_size) {
        p->global_vc_size = 2048;
        fprintf(stderr, "Buffer size of global channels not specified, setting to %d\n", p->global_vc_size);
    }

    configuration_get_value_int(&config, "PARAMS", "cn_vc_size", anno, &p->cn_vc_size);
    if(!p->cn_vc_size) {
        p->cn_vc_size = 1024;
        fprintf(stderr, "Buffer size of compute node channels not specified, setting to %d\n", p->cn_vc_size);
    }

    configuration_get_value_int(&config, "PARAMS", "chunk_size", anno, &p->chunk_size);
    if(!p->chunk_size) {
        p->chunk_size = 512;
        fprintf(stderr, "Chunk size for packets is specified, setting to %d\n", p->chunk_size);
    }

    configuration_get_value_double(&config, "PARAMS", "local_bandwidth", anno, &p->local_bandwidth);
    if(!p->local_bandwidth) {
        p->local_bandwidth = 5.25;
        fprintf(stderr, "Bandwidth of local channels not specified, setting to %lf\n", p->local_bandwidth);
    }

    configuration_get_value_double(&config, "PARAMS", "global_bandwidth", anno, &p->global_bandwidth);
    if(!p->global_bandwidth) {
        p->global_bandwidth = 4.7;
        fprintf(stderr, "Bandwidth of global channels not specified, setting to %lf\n", p->global_bandwidth);
    }

    configuration_get_value_double(&config, "PARAMS", "cn_bandwidth", anno, &p->cn_bandwidth);
    if(!p->cn_bandwidth) {
        p->cn_bandwidth = 5.25;
        fprintf(stderr, "Bandwidth of compute node channels not specified, setting to %lf\n", p->cn_bandwidth);
    }

    configuration_get_value_int(&config, "PARAMS", "local_channels", anno, &p->num_local_channels);
    if(!p->num_local_channels) {
        p->num_local_channels = 2;
        fprintf(stderr, "Number of Local channels not specified, setting to %d\n", p->num_local_channels);
    }

    configuration_get_value_int(&config, "PARAMS", "global_channels", anno, &p->num_global_channels);
    if(!p->num_global_channels) {
        p->num_global_channels = 2;
        fprintf(stderr, "Number of Global channels not specified, setting to %d\n", p->num_global_channels);
    }

    configuration_get_value_int(&config, "PARAMS", "num_terminals", anno, &p->num_cn);
    if(!p->num_cn) {
        p->num_cn = 2;
        fprintf(stderr, "Number of terminals not specified, setting to %d\n", p->num_cn);
    }

    p->router_delay = -1;
    configuration_get_value_double(&config, "PARAMS", "router_delay", anno, &p->router_delay);
    if(p->router_delay < 0) {
        p->router_delay = 0;
        fprintf(stderr, "Router delay not specified, setting to %lf\n", p->router_delay);
    }

    char       **values;
    size_t       length;
    int ret = configuration_get_multivalue(&config, "PARAMS", "generator_set_X", anno, &values, &length);
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

    ret = configuration_get_multivalue(&config, "PARAMS", "generator_set_X_prime", anno, &values, &length);
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

    char routing_str[MAX_NAME_LENGTH];
    configuration_get_value(&config, "PARAMS", "routing", anno, routing_str,
            MAX_NAME_LENGTH);
    if(strcmp(routing_str, "minimal") == 0)
        routing = MINIMAL;
    else if(strcmp(routing_str, "nonminimal")==0 ||
            strcmp(routing_str,"non-minimal")==0)
        routing = NON_MINIMAL;
    else if (strcmp(routing_str, "adaptive") == 0)
        routing = ADAPTIVE;
    else
    {
        fprintf(stderr,
                "No routing protocol specified, setting to minimal routing\n");
        routing = -1;
    }

    TEMP_RADIX = p->num_local_channels + p->num_global_channels + p->num_cn;
    TEMP_NUM_VC = p->num_vcs;

    // set the derived parameters
    p->num_groups = p->num_routers * 2;
    p->radix = (p->num_cn + p->num_global_channels + p->num_local_channels);
    p->slim_total_routers = p->num_groups * p->num_routers;
    p->slim_total_terminals = p->slim_total_routers * p->num_cn;
    slim_total_routers_noah = p->num_groups * p->num_routers;
    slim_total_terminals_noah = p->slim_total_routers * p->num_cn;
    int rank;
    MPI_Comm_rank(MPI_COMM_CODES, &rank);
    if(!rank) {
        printf("\n Total nodes %d total routers %d total groups %d num_terminals %d num_routers %d radix %d local_channels %d global_channels %d \n",
                p->num_cn * p->slim_total_routers, p->slim_total_routers, p->num_groups, p->num_cn, p->num_routers,
                p->radix, p->num_local_channels, p->num_global_channels);
    }

    p->cn_delay = bytes_to_ns(p->chunk_size, p->cn_bandwidth);
    p->local_delay = bytes_to_ns(p->chunk_size, p->local_bandwidth);
    p->global_delay = bytes_to_ns(p->chunk_size, p->global_bandwidth);
    p->credit_delay = bytes_to_ns(8.0, p->local_bandwidth); //assume 8 bytes packet

}

static void slimfly_configure(){
    anno_map = codes_mapping_get_lp_anno_map(LP_CONFIG_NM);
    assert(anno_map);
    num_params = anno_map->num_annos + (anno_map->has_unanno_lp > 0);
    all_params = malloc(num_params * sizeof(*all_params));

    for (uint64_t i = 0; i < (uint64_t)anno_map->num_annos; i++){
        const char * anno = anno_map->annotations[i].ptr;
        slimfly_read_config(anno, &all_params[i]);
    }
    if (anno_map->has_unanno_lp > 0){
        slimfly_read_config(NULL, &all_params[anno_map->num_annos]);
    }
}

/* report slimfly statistics like average and maximum packet latency, average number of hops traversed */
static void slimfly_report_stats()
{
    long long avg_hops, total_finished_packets, total_finished_chunks;
    long long total_finished_msgs, final_msg_sz;
    tw_stime avg_time, max_time;
    int total_minimal_packets, total_nonmin_packets;
    float throughput_avg = 0.0;
    float throughput_avg2 = 0.0;
    char log[300];

    MPI_Reduce( &total_hops, &avg_hops, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce( &N_finished_packets, &total_finished_packets, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce( &N_finished_msgs, &total_finished_msgs, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce( &N_finished_chunks, &total_finished_chunks, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce( &total_msg_sz, &final_msg_sz, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce( &slimfly_total_time, &avg_time, 1,MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce( &slimfly_max_latency, &max_time, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);

    MPI_Reduce(&pe_throughput_percent, &throughput_avg, 1, MPI_FLOAT, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce(&pe_throughput, &throughput_avg2, 1, MPI_FLOAT, MPI_SUM, 0, MPI_COMM_WORLD);

    MPI_Reduce(&minimal_count, &total_minimal_packets, 1, MPI_INT, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce(&nonmin_count, &total_nonmin_packets, 1, MPI_INT, MPI_SUM, 0, MPI_COMM_WORLD);

    /* print statistics */
    if(!g_tw_mynode)
    {
        printf(" Average number of hops traversed %f average chunk latency %lf us maximum chunk latency %lf us avg message size %lf bytes finished messages %lld \n", (float)avg_hops/total_finished_chunks, avg_time/(total_finished_chunks*1000), max_time/1000, (float)final_msg_sz/total_finished_msgs, total_finished_msgs);

#if PARAMS_LOG
        throughput_avg = throughput_avg / (float)slim_total_terminals_noah;
        throughput_avg2 = throughput_avg2 / (float)slim_total_terminals_noah;

        //Open file to append simulation results
        sprintf( log, "slimfly-results-log.txt");
        slimfly_results_log=fopen(log, "a");
        if(slimfly_results_log == NULL)
            tw_error(TW_LOC, "\n Failed to open slimfly results log file \n");
        printf("Printing Simulation Parameters/Results Log File\n");
        fprintf(slimfly_results_log,"%10.3lf, %15.3lf, %11.3lf, %13.3d, %16.3d, %16.3lld, %25.5f, %14.5f, ", (float)avg_hops/total_finished_packets, avg_time/(total_finished_packets),max_time,total_minimal_packets,total_nonmin_packets,total_finished_chunks,throughput_avg*100,throughput_avg2);
        fclose(slimfly_results_log);
#endif
    }

#if ROUTER_OCCUPANCY_LOG
    if(tw_ismaster())
    {
        printf("Printing Realtime Router Occupancy Log Files\n");
        for(k=0; k<slim_total_routers_noah; k++)
        {
            sprintf( log, "vc-occupancy/routers/slimfly_router_occupancy_log.%d.txt", k );
            slimfly_router_occupancy_log=fopen(log, "w+");
            if(slimfly_router_occupancy_log == NULL)
                tw_error(TW_LOC, "\n Failed to open slimfly router occupancy log file \n");

            for( t=0; t<N_COLLECT_POINTS; t++ )
            {
                if(t == 0)
                {
                    fprintf(slimfly_router_occupancy_log, "%d, ",t*100/N_COLLECT_POINTS);
                }
                else
                {
                    fprintf(slimfly_router_occupancy_log, "\n%d, ",t*100/N_COLLECT_POINTS);
                }
                for( j=0; j<TEMP_RADIX; j++)
                {
                    for(i=0; i<TEMP_NUM_VC; i++)
                    {
                        if( k==0 && t==0)
                            printf("k:%d, j:%d, i:%d, t:%d\n",k,j,i,t);
                        fprintf(slimfly_router_occupancy_log, "%d, ", vc_occupancy_storage_router[k][j][i][t]);
                    }
                }
            }
        }
    }
    fclose(slimfly_router_occupancy_log);
#endif

#if TERMINAL_OCCUPANCY_LOG
    if(tw_ismaster())
    {
        printf("Printing Realtime Terminal Occupancy Log Files\n");
        for(k=0; k<slim_total_terminals_noah; k++)
        {
            sprintf( log, "vc-occupancy/terminals/slimfly_terminal_occupancy_log.%d.txt", k );
            slimfly_terminal_occupancy_log=fopen(log, "w+");
            if(slimfly_terminal_occupancy_log == NULL)
            {
                printf("Failed to open slimfly terminal occupancy log file vc-occupancy/terminals/slimfly_terminal_occupancy_log.%d.txt\n",k);
                tw_error(TW_LOC, "\n Failed to open slimfly terminal occupancy log file \n");
            }

            for( i=0; i<N_COLLECT_POINTS; i++ )
            {
                if(i == 0)
                {
                    fprintf(slimfly_terminal_occupancy_log, "%d, ",i*100/N_COLLECT_POINTS);
                }
                else
                {
                    fprintf(slimfly_terminal_occupancy_log, "\n%d, ",i*100/N_COLLECT_POINTS);
                }
                for( j=0; j<1; j++)
                {
                    fprintf(slimfly_terminal_occupancy_log, "%d, ", vc_occupancy_storage_terminal[k][j][i]);
                }
            }
        }
    }
    fclose(slimfly_terminal_occupancy_log);
#endif

#if TERMINAL_SENDS_RECVS_LOG
    if(tw_ismaster())
    {
        printf("Printing Realtime Terminal sends & recvs Log Files\n");
        sprintf( log, "slimfly_terminal_sends_recvs_log.txt");
        slimfly_terminal_sends_recvs_log=fopen(log, "w+");
        if(slimfly_terminal_sends_recvs_log == NULL)
            tw_error(TW_LOC, "\n Failed to open slimfly terminal sends & recvs log file \n");
        for( i=0; i<N_COLLECT_POINTS; i++ )
        {
            fprintf(slimfly_terminal_sends_recvs_log, "%d",i*100/N_COLLECT_POINTS);
            for(j=0; j<slim_total_terminals_noah; j++)
            {
                fprintf(slimfly_terminal_sends_recvs_log, ", %lu", terminal_sends[j][i]);
                fprintf(slimfly_terminal_sends_recvs_log, ", %lu", terminal_recvs[j][i]);
            }
            fprintf(slimfly_terminal_sends_recvs_log, "\n");
        }
    }
    fclose(slimfly_terminal_sends_recvs_log);
#endif

#if ROUTER_SENDS_RECVS_LOG
    if(tw_ismaster())
    {
        printf("Printing Realtime Router ROUTER sends & recvs Log Files\n");
        sprintf( log, "slimfly_router_sends_recvs_log.txt");
        slimfly_router_sends_recvs_log=fopen(log, "w+");
        if(slimfly_router_sends_recvs_log == NULL)
            tw_error(TW_LOC, "\n Failed to open slimfly router sends & recvs log file \n");
        for( i=0; i<N_COLLECT_POINTS; i++ )
        {
            fprintf(slimfly_router_sends_recvs_log, "%d",i*100/N_COLLECT_POINTS);
            for(j=0; j<slim_total_routers_noah; j++)
            {
                fprintf(slimfly_router_sends_recvs_log, ", %lu", router_sends[j][i]);
                fprintf(slimfly_router_sends_recvs_log, ", %lu", router_recvs[j][i]);
            }
            fprintf(slimfly_router_sends_recvs_log, "\n");
        }
    }
    fclose(slimfly_router_sends_recvs_log);
#endif

    return;
}

/* initialize a slimfly compute node terminal */
void slim_terminal_init( terminal_state * s,
        tw_lp * lp )
{
    uint32_t h1 = 0, h2 = 0;
    bj_hashlittle2(LP_METHOD_NM, strlen(LP_METHOD_NM), &h1, &h2);
    slim_terminal_magic_num = h1 + h2;

    int i;
    char anno[MAX_NAME_LENGTH];

    // Assign the global router ID
    // TODO: be annotation-aware
    codes_mapping_get_lp_info(lp->gid, lp_group_name, &mapping_grp_id, NULL,
            &mapping_type_id, anno, &mapping_rep_id, &mapping_offset);
    if (anno[0] == '\0'){
        s->anno = NULL;
        s->params = &all_params[num_params-1];
    }
    else{
        s->anno = strdup(anno);
        int id = configuration_get_annotation_index(anno, anno_map);
        s->params = &all_params[id];
    }

    int num_lps = codes_mapping_get_lp_count(lp_group_name, 1, LP_CONFIG_NM,
            s->anno, 0);

    s->terminal_id = (mapping_rep_id * num_lps) + mapping_offset;
    s->router_id=(int)s->terminal_id / (num_lps);
    s->terminal_available_time = 0.0;
    s->packet_counter = 0;

    s->finished_msgs = 0;
    s->finished_chunks = 0;
    s->finished_packets = 0;
    s->total_time = 0.0;
    s->total_msg_size = 0;

    s->last_buf_full = 0;
    s->busy_time = 0;

    rc_stack_create(&s->st);
    s->num_vcs = 1;
    s->vc_occupancy = (int*)malloc(s->num_vcs * sizeof(int));

    for( i = 0; i < s->num_vcs; i++ )
    {
        s->vc_occupancy[i]=0;
    }

    s->rank_tbl = qhash_init(slimfly_rank_hash_compare, slimfly_hash_func, DFLY_HASH_TABLE_SIZE);

    if(!s->rank_tbl)
        tw_error(TW_LOC, "\n Hash table not initialized! ");

    s->terminal_msgs =
        (slim_terminal_message_list**)malloc(1*sizeof(slim_terminal_message_list*));
    s->terminal_msgs_tail =
        (slim_terminal_message_list**)malloc(1*sizeof(slim_terminal_message_list*));
    s->terminal_msgs[0] = NULL;
    s->terminal_msgs_tail[0] = NULL;
    s->terminal_length = 0;
    s->in_send_loop = 0;
    s->issueIdle = 0;

    return;
}


/* sets up the router virtual channels, global channels,
 * local channels, compute node channels */
void slim_router_setup(router_state * r, tw_lp * lp)
{
    uint32_t h1 = 0, h2 = 0;
    bj_hashlittle2(LP_METHOD_NM, strlen(LP_METHOD_NM), &h1, &h2);
    slim_router_magic_num = h1 + h2;

    char anno[MAX_NAME_LENGTH];
    codes_mapping_get_lp_info(lp->gid, lp_group_name, &mapping_grp_id, NULL,
            &mapping_type_id, anno, &mapping_rep_id, &mapping_offset);

    if (anno[0] == '\0'){
        r->anno = NULL;
        r->params = &all_params[num_params-1];
    } else{
        r->anno = strdup(anno);
        int id = configuration_get_annotation_index(anno, anno_map);
        r->params = &all_params[id];
    }

    // shorthand
    const slimfly_param *p = r->params;

    r->router_id=mapping_rep_id + mapping_offset;
    r->group_id=r->router_id/p->num_routers;

    r->global_channel = (int*)malloc(p->num_global_channels * sizeof(int));
    r->local_channel = (int*)malloc(p->num_local_channels * sizeof(int));
    r->next_output_available_time = (tw_stime*)malloc(p->radix * sizeof(tw_stime));
    r->link_traffic = (int*)malloc(p->radix * sizeof(int));
    r->cur_hist_num = (int*)malloc(p->radix * sizeof(int));
    r->prev_hist_num = (int*)malloc(p->radix * sizeof(int));

    r->vc_occupancy = (int**)malloc(p->radix * sizeof(int*));
    r->in_send_loop = (int*)malloc(p->radix * sizeof(int));
    r->pending_msgs =
        (slim_terminal_message_list***)malloc(p->radix * sizeof(slim_terminal_message_list**));
    r->pending_msgs_tail =
        (slim_terminal_message_list***)malloc(p->radix * sizeof(slim_terminal_message_list**));
    r->queued_msgs =
        (slim_terminal_message_list***)malloc(p->radix * sizeof(slim_terminal_message_list**));
    r->queued_msgs_tail =
        (slim_terminal_message_list***)malloc(p->radix * sizeof(slim_terminal_message_list**));

    r->last_buf_full = (tw_stime*)malloc(p->radix * sizeof(tw_stime));
    r->busy_time = (tw_stime*)malloc(p->radix * sizeof(tw_stime));

    rc_stack_create(&r->st);

    for(int i=0; i < p->radix; i++)
    {
        // Set credit & router occupancy
        r->next_output_available_time[i]=0;
        r->link_traffic[i]=0;
        r->cur_hist_num[i] = 0;
        r->prev_hist_num[i] = 0;

        r->last_buf_full[i] = 0.0;
        r->busy_time[i] = 0.0;

        r->in_send_loop[i] = 0;
        r->vc_occupancy[i] = (int*)malloc(p->num_vcs * sizeof(int));
        r->pending_msgs[i] = (slim_terminal_message_list**)malloc(p->num_vcs *
                sizeof(slim_terminal_message_list*));
        r->pending_msgs_tail[i] = (slim_terminal_message_list**)malloc(p->num_vcs *
                sizeof(slim_terminal_message_list*));
        r->queued_msgs[i] = (slim_terminal_message_list**)malloc(p->num_vcs *
                sizeof(slim_terminal_message_list*));
        r->queued_msgs_tail[i] = (slim_terminal_message_list**)malloc(p->num_vcs *
                sizeof(slim_terminal_message_list*));
        for(int j = 0; j < p->num_vcs; j++) {
            r->vc_occupancy[i][j] = 0;
            r->pending_msgs[i][j] = NULL;
            r->pending_msgs_tail[i][j] = NULL;
            r->queued_msgs[i][j] = NULL;
            r->queued_msgs_tail[i][j] = NULL;
        }
    }

#if LOAD_FROM_FILE
    //Load input MMS router and node layout/connection graph from file
    char log[500];
    sprintf( log, "simulation-input-files/MMS.%d/MMS.%d.%d.bsconf", p->num_global_channels+p->num_local_channels, p->num_global_channels+p->num_local_channels, p->num_cn);
    MMS_input_file = fopen( log, "r");
    if( MMS_input_file == NULL )
        tw_error( TW_LOC, "Failed to Open Slim_fly input MMS layout file: %s The reason *may* have been %s\n",log,strerror(errno));

    int i,j;
    rewind(MMS_input_file);
    char one_word[16];
    int one_number = 0;
    int temp;
    //Skip over preceeding lines
    for(i=0;i<r->router_id;i++)
    {
        for(j=0;j<=p->num_local_channels+p->num_global_channels+p->num_cn;j++)
        {
            temp = fscanf(MMS_input_file,"%s",one_word); // got one word from the file
            temp = fscanf(MMS_input_file,"%d",&one_number);
        }
    }
    //Skip over self in MMS file
    temp = fscanf(MMS_input_file,"%s",one_word); // got one word from the file /
    temp = fscanf(MMS_input_file,"%d",&one_number);
    //Assign Local router connections
    for(i=0;i<p->num_local_channels;i++)
    {
        temp = fscanf(MMS_input_file,"%s",one_word); // got one word from the file /
        temp = fscanf(MMS_input_file,"%d",&one_number);
        r->local_channel[i] = one_number;
    }
    //Set global router connections according to input MMS file
    for(i=0;i<p->num_global_channels;i++)
    {
        temp = fscanf(MMS_input_file,"%s",one_word); // got one word from the file
        temp = fscanf(MMS_input_file,"%d",&one_number);
        r->global_channel[i] = one_number;
    }
    fclose(MMS_input_file);

#else
    //Compute MMS router layout/connection graph
    int rid_s = (int)r->router_id;	// ID for source router
    int rid_d;						// ID for dest. router
    int s_s,s_d;					// subgraph location for source and destination routers
    int i_s,i_d;					// x or m coordinates for source and destination routers
    int j_s,j_d; 					// y or c coordinates for source and destination routers
    int k;
    int local_idx = 0;
    int global_idx = 0;
    int generator_size = X_size;

    for(rid_d=0;rid_d<r->params->slim_total_routers;rid_d++)
    {
        // Decompose source and destination Router IDs into 3D subgraph coordinates (subgraph,i,j)
        if(rid_d >= r->params->slim_total_routers/2)
        {
            s_d = 1;
            i_d = (rid_d - r->params->slim_total_routers/2) /  r->params->num_global_channels;
            j_d = (rid_d - r->params->slim_total_routers/2) %  r->params->num_global_channels;
        }
        else
        {
            s_d = 0;
            i_d = rid_d /  r->params->num_global_channels;
            j_d = rid_d %  r->params->num_global_channels;
        }
        if(rid_s >= r->params->slim_total_routers/2)
        {
            s_s = 1;
            i_s = (rid_s - r->params->slim_total_routers/2) /  r->params->num_global_channels;
            j_s = (rid_s - r->params->slim_total_routers/2) %  r->params->num_global_channels;
        }
        else
        {
            s_s = 0;
            i_s = rid_s /  r->params->num_global_channels;
            j_s = rid_s %  r->params->num_global_channels;
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
                        r->local_channel[local_idx++] = rid_d;
                    }
                }
            }
        }
        // Check if global connections
        if(s_s==0 && s_d==1)
        {
            if(j_s == (i_d*i_s + j_d) % r->params->num_routers)							// equation (3) y=mx+c
            {
                r->global_channel[global_idx++] = rid_d;
            }
        }
    }

    // Loop over second subgraph source routers
    for(rid_d=r->params->slim_total_routers-1;rid_d>=0;rid_d--)
    {
        // Decompose source and destination Router IDs into 3D subgraph coordinates (subgraph,i,j)
        if(rid_d >= r->params->slim_total_routers/2)
        {
            s_d = 1;
            i_d = (rid_d -  r->params->slim_total_routers/2) /  r->params->num_global_channels;
            j_d = (rid_d -  r->params->slim_total_routers/2) %  r->params->num_global_channels;
        }
        else
        {
            s_d = 0;
            i_d = rid_d /  r->params->num_global_channels;
            j_d = rid_d %  r->params->num_global_channels;
        }
        if(rid_s >= r->params->slim_total_routers/2)
        {
            s_s = 1;
            i_s = (rid_s -  r->params->slim_total_routers/2) /  r->params->num_global_channels;
            j_s = (rid_s -  r->params->slim_total_routers/2) %  r->params->num_global_channels;
        }
        else
        {
            s_s = 0;
            i_s = rid_s /  r->params->num_global_channels;
            j_s = rid_s %  r->params->num_global_channels;
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
                        r->local_channel[local_idx++] = rid_d;
                    }
                }
            }
        }
        // Check if global connections
        if(s_s==1 && s_d==0)
        {
            if(j_d == (i_s*i_d + j_s) %  r->params->num_routers)							// equation (3) y=mx+c
            {
                r->global_channel[global_idx++] = rid_d;
            }
        }
    }
#endif

    return;
}


/* slimfly packet event , generates a slimfly packet on the compute node */
static tw_stime slimfly_packet_event(
        model_net_request const * req,
        uint64_t message_offset,
        uint64_t packet_size,
        tw_stime offset,
        mn_sched_params const * sched_params,
        void const * remote_event,
        void const * self_event,
        tw_lp *sender,
        int is_last_pckt)
{
    (void)message_offset;
    (void)sched_params;

    tw_event * e_new;
    tw_stime xfer_to_nic_time;
    slim_terminal_message * msg;
    char* tmp_ptr;

    xfer_to_nic_time = codes_local_latency(sender);
    //e_new = tw_event_new(sender->gid, xfer_to_nic_time+offset, sender);
    //msg = tw_event_data(e_new);
    e_new = model_net_method_event_new(sender->gid, xfer_to_nic_time+offset,
            sender, SLIMFLY, (void**)&msg, (void**)&tmp_ptr);
    strcpy(msg->category, req->category);
    msg->final_dest_gid = req->final_dest_lp;
    msg->total_size = req->msg_size;
    msg->sender_lp=req->src_lp;
    msg->sender_mn_lp = sender->gid;
    msg->packet_size = packet_size;
    msg->travel_start_time = tw_now(sender);
    msg->remote_event_size_bytes = 0;
    msg->local_event_size_bytes = 0;
    msg->type = T_GENERATE;
    msg->dest_terminal_id = req->dest_mn_lp;
    msg->message_id = req->msg_id;
    msg->is_pull = req->is_pull;
    msg->pull_size = req->pull_size;
    msg->magic = slim_terminal_magic_num;
    msg->msg_start_time = req->msg_start_time;

    if(is_last_pckt) /* Its the last packet so pass in remote and local event information*/
    {
        if(req->remote_event_size > 0)
        {
            msg->remote_event_size_bytes = req->remote_event_size;
            memcpy(tmp_ptr, remote_event, req->remote_event_size);
            tmp_ptr += req->remote_event_size;
        }
        if(req->self_event_size > 0)
        {
            msg->local_event_size_bytes = req->self_event_size;
            memcpy(tmp_ptr, self_event, req->self_event_size);
            tmp_ptr += req->self_event_size;
        }
    }
    //printf("\n slimfly remote event %d local event %d last packet %d %lf ", msg->remote_event_size_bytes, msg->local_event_size_bytes, is_last_pckt, xfer_to_nic_time);
    tw_event_send(e_new);
    return xfer_to_nic_time;
}

/* slimfly packet event reverse handler */
static void slimfly_packet_event_rc(tw_lp *sender)
{
    codes_local_latency_reverse(sender);
    return;
}

/*When a packet is sent from the current router and a buffer slot becomes available, a credit is sent back to schedule another packet event*/
void slim_router_credit_send(router_state * s, slim_terminal_message * msg, tw_lp * lp, int sq)
{
    tw_event * buf_e;
    tw_stime ts;
    slim_terminal_message * buf_msg;

    int dest = 0,  type = R_BUFFER;
    int is_terminal = 0;

    const slimfly_param *p = s->params;

    // Notify sender terminal about available buffer space
    if(msg->last_hop == TERMINAL) {
        dest = msg->src_terminal_id;
        type = T_BUFFER;
        is_terminal = 1;
    } else if(msg->last_hop == GLOBAL) {
        dest = msg->intm_lp_id;
    } else if(msg->last_hop == LOCAL) {
        dest = msg->intm_lp_id;
    } else
        printf("\n Invalid message type");

#if TRACK_OUTPUT
    if( msg->packet_ID == TRACK )
    {
        printf("sending credit to gid:%d\n",dest);
    }
#endif

    ts = g_tw_lookahead + p->credit_delay +  tw_rand_unif(lp->rng);

    if (is_terminal) {
        buf_e = model_net_method_event_new(dest, ts, lp, SLIMFLY,
                (void**)&buf_msg, NULL);
        buf_msg->magic = slim_terminal_magic_num;
    } else {
        buf_e = tw_event_new(dest, ts , lp);
        buf_msg = tw_event_data(buf_e);
        buf_msg->magic = slim_router_magic_num;
    }

    if(sq == -1) {
        buf_msg->vc_index = msg->vc_index;
        buf_msg->output_chan = msg->output_chan;
    } else {
        buf_msg->vc_index = msg->saved_vc;
        buf_msg->output_chan = msg->saved_channel;
    }

    buf_msg->type = type;

    tw_event_send(buf_e);
    return;
}

void slim_packet_generate_rc(terminal_state * s, tw_bf * bf, slim_terminal_message * msg, tw_lp * lp)
{
    tw_rand_reverse_unif(lp->rng);

    int num_chunks = msg->packet_size/s->params->chunk_size;
    if(msg->packet_size % s->params->chunk_size)
        num_chunks++;

    if(!num_chunks)
        num_chunks = 1;

    int i;
    for(i = 0; i < num_chunks; i++)
    {
        slim_delete_terminal_message_list(return_tail(s->terminal_msgs,
                    s->terminal_msgs_tail, 0));
        s->terminal_length -= s->params->chunk_size;
    }
    if(bf->c5)
    {
        codes_local_latency_reverse(lp);
        s->in_send_loop = 0;
    }
    if(bf->c11)
    {
        s->issueIdle = 0;
        s->last_buf_full = msg->saved_busy_time;
    }
    struct mn_stats* stat;
    stat = model_net_find_stats(msg->category, s->slimfly_stats_array);
    stat->send_count--;
    stat->send_bytes -= msg->packet_size;
    stat->send_time -= (1/s->params->cn_bandwidth) * msg->packet_size;
}

/* generates packet at the current slimfly compute node */
void slim_packet_generate(terminal_state * s, tw_bf * bf, slim_terminal_message * msg, tw_lp * lp)
{
    tw_stime ts, nic_ts;

    assert(lp->gid != msg->dest_terminal_id);
    const slimfly_param *p = s->params;

    int total_event_size;
    uint64_t num_chunks = msg->packet_size / p->chunk_size;
    if (msg->packet_size % s->params->chunk_size)
        num_chunks++;

    if(!num_chunks)
        num_chunks = 1;

    nic_ts = g_tw_lookahead + (s->params->cn_delay * num_chunks) + tw_rand_unif(lp->rng);

    msg->packet_ID = lp->gid + g_tw_nlp * s->packet_counter;
    msg->my_N_hop = 0;
    msg->my_l_hop = 0;
    msg->my_g_hop = 0;
    msg->intm_group_id = -1;
    msg->intm_router_id = -1;

    if(msg->packet_ID == TRACK)
        printf("\x1B[34m-->Packet generated at terminal %d sending to router %d \x1b[0m\n", (int)lp->gid, s->router_id);

    for(uint64_t i = 0; i < num_chunks; i++)
    {
        slim_terminal_message_list *cur_chunk = (slim_terminal_message_list*)malloc(
                sizeof(slim_terminal_message_list));
        slim_init_terminal_message_list(cur_chunk, msg);

        if(msg->remote_event_size_bytes + msg->local_event_size_bytes > 0)
        {
            cur_chunk->event_data = (char*)malloc(
                    msg->remote_event_size_bytes + msg->local_event_size_bytes);
        }

        void * m_data_src = model_net_method_get_edata(SLIMFLY, msg);
        if (msg->remote_event_size_bytes)
        {
            memcpy(cur_chunk->event_data, m_data_src, msg->remote_event_size_bytes);
        }
        if (msg->local_event_size_bytes)
        {
            m_data_src = (char*)m_data_src + msg->remote_event_size_bytes;
            memcpy((char*)cur_chunk->event_data + msg->remote_event_size_bytes,
                    m_data_src, msg->local_event_size_bytes);
        }

        cur_chunk->msg.chunk_id = i;
        append_to_terminal_message_list(s->terminal_msgs, s->terminal_msgs_tail,
                0, cur_chunk);
        s->terminal_length += s->params->chunk_size;
    }

    if(s->terminal_length < 2 * s->params->cn_vc_size)
    {
        model_net_method_idle_event(nic_ts, 0, lp);
    }
    else
    {
        bf->c11 = 1;
        s->issueIdle = 1;
        msg->saved_busy_time = s->last_buf_full;
        s->last_buf_full = tw_now(lp);
    }

    if(s->in_send_loop == 0)
    {
        bf->c5 = 1;
        ts = codes_local_latency(lp);
        slim_terminal_message *m;
        tw_event* e = model_net_method_event_new(lp->gid, ts, lp, SLIMFLY,
                (void**)&m, NULL);
        m->type = T_SEND;
        m->magic = slim_terminal_magic_num;
        s->in_send_loop = 1;
        tw_event_send(e);
    }

    total_event_size = model_net_get_msg_sz(SLIMFLY) +
        msg->remote_event_size_bytes + msg->local_event_size_bytes;
    mn_stats* stat;
    stat = model_net_find_stats(msg->category, s->slimfly_stats_array);
    stat->send_count++;
    stat->send_bytes += msg->packet_size;
    stat->send_time += (1/p->cn_bandwidth) * msg->packet_size;
    if(stat->max_event_size < total_event_size)
        stat->max_event_size = total_event_size;

    return;
}

void slim_packet_send_rc(terminal_state * s, tw_bf * bf, slim_terminal_message * msg, tw_lp * lp)
{
    if(bf->c1) {
        s->in_send_loop = 1;
        s->last_buf_full = msg->saved_busy_time;
        return;
    }

    tw_rand_reverse_unif(lp->rng);
    s->terminal_available_time = msg->saved_available_time;
    if(bf->c2) {
        codes_local_latency_reverse(lp);
    }

    s->terminal_length += s->params->chunk_size;
    s->packet_counter--;
    s->vc_occupancy[0] -= s->params->chunk_size;

    slim_terminal_message_list* cur_entry = rc_stack_pop(s->st);

    prepend_to_terminal_message_list(s->terminal_msgs,
            s->terminal_msgs_tail, 0, cur_entry);
    if(bf->c3) {
        tw_rand_reverse_unif(lp->rng);
    }
    if(bf->c4) {
        s->in_send_loop = 1;
    }
    if(bf->c5)
    {
        tw_rand_reverse_unif(lp->rng);
        s->issueIdle = 1;
        if(bf->c6)
        {
            s->busy_time = msg->saved_total_time;
            s->last_buf_full = msg->saved_busy_time;
        }
    }
    return;
}

/* sends the packet from the current slimfly compute node to the attached router */
void slim_packet_send(terminal_state * s, tw_bf * bf, slim_terminal_message * msg,
        tw_lp * lp)
{
    tw_stime ts;
    tw_event *e;
    slim_terminal_message *m;
    tw_lpid router_id;

    slim_terminal_message_list* cur_entry = s->terminal_msgs[0];

    if(s->vc_occupancy[0] + s->params->chunk_size > s->params->cn_vc_size
            || cur_entry == NULL)
    {
        bf->c1 = 1;
        s->in_send_loop = 0;
        msg->saved_busy_time = s->last_buf_full;
        s->last_buf_full = tw_now(lp);
        return;
    }

    uint64_t num_chunks = cur_entry->msg.packet_size/s->params->chunk_size;
    if(cur_entry->msg.packet_size % s->params->chunk_size)
        num_chunks++;

    if(!num_chunks)
        num_chunks = 1;

    tw_stime delay = s->params->cn_delay;
    if((cur_entry->msg.packet_size % s->params->chunk_size) && (cur_entry->msg.chunk_id == num_chunks - 1))
        delay = bytes_to_ns(cur_entry->msg.packet_size % s->params->chunk_size, s->params->cn_bandwidth); 

    msg->saved_available_time = s->terminal_available_time;
    ts = g_tw_lookahead + delay + tw_rand_unif(lp->rng);
    s->terminal_available_time = maxd(s->terminal_available_time, tw_now(lp));
    s->terminal_available_time += ts;

    ts = s->terminal_available_time - tw_now(lp);
    //TODO: be annotation-aware
    codes_mapping_get_lp_info(lp->gid, lp_group_name, &mapping_grp_id, NULL,
            &mapping_type_id, NULL, &mapping_rep_id, &mapping_offset);
    codes_mapping_get_lp_id(lp_group_name, "slimfly_router", NULL, 1,
            s->router_id, 0, &router_id);
    // we are sending an event to the router, so no method_event here
    e = tw_event_new(router_id, s->terminal_available_time - tw_now(lp), lp);
    m = tw_event_data(e);
    memcpy(m, &cur_entry->msg, sizeof(slim_terminal_message));
    if (m->remote_event_size_bytes)
    {
        memcpy(model_net_method_get_edata(SLIMFLY, m), cur_entry->event_data,
                m->remote_event_size_bytes);
    }

    m->origin_router_id = s->router_id;
    m->type = R_ARRIVE;
    m->src_terminal_id = lp->gid;
    m->vc_index = 0;
    m->last_hop = TERMINAL;
    m->intm_group_id = -1;
    m->intm_router_id = -1;
    m->magic = slim_router_magic_num;
    m->path_type = -1;
    m->local_event_size_bytes = 0;
    m->local_id = s->terminal_id;
    tw_event_send(e);

#if DEBUG
    if( m->packet_ID == TRACK)
    {
        printf( "(%lf) [Terminal %d] packet_ID %lld message_ID %d is sending. ",tw_now(lp), (int)lp->gid, m->packet_ID, (int)m->message_id);
        printf("travel start time is %f. ", m->travel_start_time);
        printf("My hop now is %d\n",m->my_N_hop);
    }
#endif

#if TERMINAL_SENDS_RECVS_LOG || TERMINAL_OCCUPANCY_LOG
    int index = floor(N_COLLECT_POINTS*(tw_now(lp)/g_tw_ts_end));
#endif

#if TERMINAL_SENDS_RECVS_LOG
    terminal_sends[s->terminal_id][index]++;
#endif

    if(cur_entry->msg.chunk_id == num_chunks - 1 && (cur_entry->msg.local_event_size_bytes > 0))
    {
        bf->c2 = 1;
        tw_stime local_ts = codes_local_latency(lp);
        tw_event *e_new = tw_event_new(cur_entry->msg.sender_lp, local_ts, lp);
        slim_terminal_message* m_new = tw_event_data(e_new);
        void *local_event = (char*)cur_entry->event_data +
            cur_entry->msg.remote_event_size_bytes;
        memcpy(m_new, local_event, cur_entry->msg.local_event_size_bytes);
        tw_event_send(e_new);
    }
    s->packet_counter++;
    s->vc_occupancy[0] += s->params->chunk_size;
#if TERMINAL_OCCUPANCY_LOG
    vc_occupancy_storage_terminal[s->terminal_id][0][index] = s->vc_occupancy[0]/s->params->chunk_size;
#endif
    cur_entry = return_head(s->terminal_msgs, s->terminal_msgs_tail, 0);
    rc_stack_push(lp, cur_entry, (void*)slim_delete_terminal_message_list, s->st);
    s->terminal_length -= s->params->chunk_size;

    cur_entry = s->terminal_msgs[0];

    if(cur_entry != NULL && s->vc_occupancy[0] + s->params->chunk_size <= s->params->cn_vc_size)
    {
        bf->c3 = 1;
        slim_terminal_message *m_new;
        ts += tw_rand_unif(lp->rng);
        tw_event* e_new = model_net_method_event_new(lp->gid, ts, lp, SLIMFLY,
                (void**)&m_new, NULL);
        m_new->type = T_SEND;
        m_new->magic = slim_terminal_magic_num;
        tw_event_send(e_new);
    }
    else
    {
        bf->c4 = 1;
        s->in_send_loop = 0;
    }
    if(s->issueIdle)
    {
        bf->c5 = 1;
        s->issueIdle = 0;
        ts += tw_rand_unif(lp->rng);
        model_net_method_idle_event(ts, 0, lp);

        if(s->last_buf_full > 0.0)
        {
            bf->c6 = 1;
            msg->saved_total_time = s->busy_time;
            msg->saved_busy_time = s->last_buf_full;

            s->busy_time += (tw_now(lp) - s->last_buf_full);
            s->last_buf_full = 0.0;
        }
    }
    return;
}

void slim_packet_arrive_rc(terminal_state * s, tw_bf * bf, slim_terminal_message * msg, tw_lp * lp)
{
    tw_rand_reverse_unif(lp->rng);
    if(msg->path_type == MINIMAL)
        minimal_count--;
    if(msg->path_type == NON_MINIMAL)
        nonmin_count--;

    N_finished_chunks--;
    s->finished_chunks--;

    total_hops -= msg->my_N_hop;
    s->total_hops -= msg->my_N_hop;
    slimfly_total_time -= (tw_now(lp) - msg->travel_start_time);
    s->total_time = msg->saved_avg_time;

    struct qhash_head * hash_link = NULL;
    struct sfly_qhash_entry * tmp = NULL;

    struct sfly_hash_key key;
    key.message_id = msg->message_id;
    key.sender_id = msg->sender_lp;

    hash_link = qhash_search(s->rank_tbl, &key);
    tmp = qhash_entry(hash_link, struct sfly_qhash_entry, hash_link);

    mn_stats* stat;
    stat = model_net_find_stats(msg->category, s->slimfly_stats_array);
    stat->recv_time -= (tw_now(lp) - msg->travel_start_time);

    if(bf->c1)
    {
        stat->recv_count--;
        stat->recv_bytes -= msg->packet_size;
        N_finished_packets--;
        s->finished_packets--;
    }
    if(bf->c3)
        slimfly_max_latency = msg->saved_available_time;

    if(bf->c7)
    {
        if(bf->c8) 
            tw_rand_reverse_unif(lp->rng);

        s->finished_msgs--;
        total_msg_sz -= msg->total_size;
        N_finished_msgs--;
        s->total_msg_size -= msg->total_size;

        struct sfly_qhash_entry * d_entry_pop = rc_stack_pop(s->st);
        qhash_add(s->rank_tbl, &key, &(d_entry_pop->hash_link));
        s->rank_tbl_pop++;

        hash_link = &(d_entry_pop->hash_link);
        tmp = d_entry_pop;


        if(bf->c4)
            model_net_event_rc2(lp, &msg->event_rc);
    }

    assert(tmp);
    tmp->num_chunks--;

    return;
}
void slim_send_remote_event(terminal_state * s, slim_terminal_message * msg, tw_lp * lp, tw_bf * bf, char * event_data, int remote_event_size)
{
    (void)s;
    void * tmp_ptr = model_net_method_get_edata(SLIMFLY, msg);
    tw_stime ts = g_tw_lookahead + tw_rand_unif(lp->rng);

    if (msg->is_pull){
        bf->c4 = 1;
        struct codes_mctx mc_dst =
            codes_mctx_set_global_direct(msg->sender_mn_lp);
        struct codes_mctx mc_src =
            codes_mctx_set_global_direct(lp->gid);
        int net_id = model_net_get_id(LP_METHOD_NM);

        model_net_set_msg_param(MN_MSG_PARAM_START_TIME, MN_MSG_PARAM_START_TIME_VAL, &(msg->msg_start_time));

        msg->event_rc = model_net_event_mctx(net_id, &mc_src, &mc_dst, msg->category,
                msg->sender_lp, msg->pull_size, ts,
                remote_event_size, tmp_ptr, 0, NULL, lp);
    }
    else{
        tw_event * e = tw_event_new(msg->final_dest_gid, ts, lp);
        void * m_remote = tw_event_data(e);
        memcpy(m_remote, event_data, remote_event_size);
        tw_event_send(e);
    }
    return;
}
/* packet arrives at the destination terminal */
void slim_packet_arrive(terminal_state * s, tw_bf * bf, slim_terminal_message * msg,
        tw_lp * lp) {

    // NIC aggregation - should this be a separate function?
    // Trigger an event on receiving server

    struct sfly_hash_key key;
    key.message_id = msg->message_id;
    key.sender_id = msg->sender_lp;

    struct qhash_head *hash_link = NULL;
    struct sfly_qhash_entry * tmp = NULL;

    hash_link = qhash_search(s->rank_tbl, &key);

    if(hash_link)
        tmp = qhash_entry(hash_link, struct sfly_qhash_entry, hash_link);

    uint64_t total_chunks = msg->total_size / s->params->chunk_size;

    if(msg->total_size % s->params->chunk_size)
        total_chunks++;

    if(!total_chunks)
        total_chunks = 1;

    tw_stime ts = g_tw_lookahead + s->params->credit_delay + tw_rand_unif(lp->rng);

    // no method_event here - message going to router
    tw_event * buf_e;
    slim_terminal_message * buf_msg;
    buf_e = tw_event_new(msg->intm_lp_id, ts, lp);
    buf_msg = tw_event_data(buf_e);
    buf_msg->magic = slim_router_magic_num;
    buf_msg->vc_index = msg->vc_index;
    buf_msg->output_chan = msg->output_chan;
    buf_msg->type = R_BUFFER;
    tw_event_send(buf_e);

    bf->c1 = 0;
    bf->c3 = 0;
    bf->c4 = 0;
    bf->c7 = 0;

    N_finished_chunks++;
    s->finished_chunks++;

    /* WE do not allow self messages through slimfly */
    assert(lp->gid != msg->src_terminal_id);

    uint64_t num_chunks = msg->packet_size / s->params->chunk_size;
    if (msg->packet_size % s->params->chunk_size)
        num_chunks++;

    if(!num_chunks)
        num_chunks = 1;

    if(msg->path_type == MINIMAL)
        minimal_count++;

    if(msg->path_type == NON_MINIMAL)
        nonmin_count++;

    if(msg->path_type != MINIMAL && msg->path_type != NON_MINIMAL)
        printf("\n Wrong message path type %d ", msg->path_type);

    msg->saved_avg_time = s->total_time;
    s->total_time += (tw_now(lp) - msg->travel_start_time);
    slimfly_total_time += tw_now( lp ) - msg->travel_start_time;
    total_hops += msg->my_N_hop;
    s->total_hops += msg->my_N_hop;

    mn_stats* stat = model_net_find_stats(msg->category, s->slimfly_stats_array);
    stat->recv_time += (tw_now(lp) - msg->travel_start_time);

#if DEBUG
    if( msg->packet_ID == TRACK /*
                                   && msg->message_id == TRACK_MSG*/)
    {
        printf( "(%lf) [Terminal %d] packet %lld has arrived. ", tw_now(lp), (int)lp->gid, msg->packet_ID);

        printf("travel start time is %f. ", msg->travel_start_time);

        printf("My hop now is %d\n\n",msg->my_N_hop);
    }
#endif

    /* Now retreieve the number of chunks completed from the hash and update
     * them */
    void *m_data_src = model_net_method_get_edata(SLIMFLY, msg);


    /* If an entry does not exist then create one */
    if(!tmp)
    {
        bf->c5 = 1;
        struct sfly_qhash_entry * d_entry = malloc(sizeof (struct sfly_qhash_entry));
        d_entry->num_chunks = 0;
        d_entry->key = key;
        d_entry->remote_event_data = NULL;
        d_entry->remote_event_size = 0;
        qhash_add(s->rank_tbl, &key, &(d_entry->hash_link));
        s->rank_tbl_pop++;

        hash_link = &(d_entry->hash_link);
        tmp = d_entry;
    }

    assert(tmp);
    tmp->num_chunks++;

    /* if its the last chunk of the packet then handle the remote event data */
    if(msg->chunk_id == num_chunks - 1)
    {
        bf->c1 = 1;
        stat->recv_count++;
        stat->recv_bytes += msg->packet_size;

        N_finished_packets++;
        s->finished_packets++;
    }
    if(msg->remote_event_size_bytes > 0 && !tmp->remote_event_data)
    {
        /* Retreive the remote event entry */
        tmp->remote_event_data = (void*)malloc(msg->remote_event_size_bytes);
        assert(tmp->remote_event_data);
        tmp->remote_event_size = msg->remote_event_size_bytes;
        memcpy(tmp->remote_event_data, m_data_src, msg->remote_event_size_bytes);
    }
    if (slimfly_max_latency < tw_now( lp ) - msg->travel_start_time) {
        bf->c3 = 1;
        msg->saved_available_time = slimfly_max_latency;
        slimfly_max_latency = tw_now( lp ) - msg->travel_start_time;
    }
    /* If all chunks of a message have arrived then send a remote event to the
     * callee*/
    if((uint64_t)tmp->num_chunks >= total_chunks)
    {
        bf->c7 = 1;

        N_finished_msgs++;
        total_msg_sz += msg->total_size;
        s->total_msg_size += msg->total_size;
        s->finished_msgs++;

        if(tmp->remote_event_data && tmp->remote_event_size > 0) {
            bf->c8 = 1;
            slim_send_remote_event(s, msg, lp, bf, tmp->remote_event_data, tmp->remote_event_size);
        }
        /* Remove the hash entry */
        qhash_del(hash_link);
        rc_stack_push(lp, tmp, free_tmp, s->st);
        s->rank_tbl_pop--;
    }
#if TERMINAL_SENDS_RECVS_LOG
    int index = floor(N_COLLECT_POINTS*(tw_now(lp)/g_tw_ts_end));
    terminal_recvs[s->terminal_id][index]++;
#endif
    return;
}

void slim_terminal_buf_update_rc(terminal_state * s,
        tw_bf * bf,
        slim_terminal_message * msg,
        tw_lp * lp)
{
    (void)msg;

    s->vc_occupancy[0] += s->params->chunk_size;
    codes_local_latency_reverse(lp);
    if(bf->c1) {
        s->in_send_loop = 0;
    }

    return;
}
/* update the compute node-router channel buffer */
void slim_terminal_buf_update(terminal_state * s,
        tw_bf * bf,
        slim_terminal_message * msg,
        tw_lp * lp)
{
    (void)msg;
    bf->c1 = 0;

    tw_stime ts = codes_local_latency(lp);
    s->vc_occupancy[0] -= s->params->chunk_size;

#if TERMINAL_OCCUPANCY_LOG
    int index = floor(N_COLLECT_POINTS*(tw_now(lp)/g_tw_ts_end));
    vc_occupancy_storage_terminal[s->terminal_id][0][index] = s->vc_occupancy[0]/s->params->chunk_size;
#endif

    if(s->in_send_loop == 0 && s->terminal_msgs[0] != NULL) {
        slim_terminal_message *m;
        bf->c1 = 1;
        tw_event* e = model_net_method_event_new(lp->gid, ts, lp, SLIMFLY, (void**)&m, NULL);
        m->type = T_SEND;
        m->magic = slim_terminal_magic_num;
        s->in_send_loop = 1;
        tw_event_send(e);
    }

    return;
}

void slim_terminal_event( terminal_state * s,
        tw_bf * bf,
        slim_terminal_message * msg,
        tw_lp * lp )
{
    *(int *)bf = (int)0;
    assert(msg->magic == slim_terminal_magic_num);

    rc_stack_gc(lp, s->st);
    switch(msg->type)
    {
        case T_GENERATE:
            slim_packet_generate(s,bf,msg,lp);
            break;

        case T_ARRIVE:
            slim_packet_arrive(s,bf,msg,lp);
            break;

        case T_SEND:
            slim_packet_send(s,bf,msg,lp);
            break;

        case T_BUFFER:
            slim_terminal_buf_update(s, bf, msg, lp);
            break;

        default:
            printf("\n LP %d Terminal message type not supported %d ", (int)lp->gid, msg->type);
            tw_error(TW_LOC, "Msg type not supported");
    }
}

void slimfly_terminal_final( terminal_state * s,
        tw_lp * lp )
{
    model_net_print_stats(lp->gid, s->slimfly_stats_array);

#if PARAMS_LOG
    float link_throughput;
    float throughput_percentage = 0.0;
    tw_stime bandwidth;
    tw_stime interval;
    bandwidth = s->total_msg_size / (1024.0 *1024.0 *1024.0);
    interval = ((1000000000.0)/g_tw_ts_end);
    bandwidth = bandwidth * interval;
    link_throughput = bandwidth;
    if(s->params->num_cn == 9 || s->params->num_cn == 3)
    {
        throughput_percentage = link_throughput / (0.71 * 12.5);
    }
    else if(s->params->num_cn == 10 || s->params->num_cn == 11)
    {
        throughput_percentage = link_throughput / (0.67 * 12.5);
    }
    else
    {
        throughput_percentage = link_throughput / (0.71 * 12.5);
    }

    pe_throughput_percent = pe_throughput_percent + throughput_percentage;
    pe_throughput = pe_throughput + link_throughput;

#endif

    int written = 0;
    if(!s->terminal_id)
        written = sprintf(s->output_buf, "# Format <LP id> <Terminal ID> <Total Data Size> <Total Packet Latency> <# Flits/Packets finished> <Avg hops> <Busy Time>");

    written += sprintf(s->output_buf + written, "%llu %u %ld %lf %ld %lf %lf\n",
            LLU(lp->gid), s->terminal_id, s->total_msg_size, s->total_time,
            s->finished_packets, (double)s->total_hops/s->finished_chunks,
            s->busy_time);

    lp_io_write(lp->gid, "slimfly-msg-stats", written, s->output_buf);

    if(s->terminal_msgs[0] != NULL)
        //      printf("[%lu] leftover terminal messages \n", lp->gid);

        if(!s->terminal_id)
        {
#if PARAMS_LOG
            //Insert output for general and slimfly specific stats/params
            //Open file to append simulation results
            char log[200];
            sprintf( log, "slimfly-results-log.txt");
            slimfly_results_log=fopen(log, "a");
            if(slimfly_results_log == NULL)
                tw_error(TW_LOC, "\n Failed to open slimfly results log file \n");
            printf("Printing Simulation Parameters/Results Log File\n");
            fprintf(slimfly_results_log," %9d.%d, %10.3d, %9.3d, %11.3d, %5.3d, %12.3d, %10.3d, %12.3d, %7.3d, %10.3d, %17.3d, %9.3d, %19.3d, %12.3f, %8.3d, ",s->params->num_global_channels+s->params->num_local_channels, s->params->num_cn,g_tw_synchronization_protocol, tw_nnodes(),(int)g_tw_ts_end,(int)g_tw_mblock,(int)g_tw_gvt_interval, (int)g_tw_nkp, s->params->slim_total_terminals,s->params->slim_total_terminals,s->params->slim_total_routers, routing, csf_ratio, num_indirect_routes, adaptive_threshold, s->params->num_vcs);
            fclose(slimfly_results_log);
#endif
        }

    qhash_finalize(s->rank_tbl);
    rc_stack_destroy(s->st);
    free(s->vc_occupancy);
    free(s->terminal_msgs);
    free(s->terminal_msgs_tail);
}

void slimfly_router_final(router_state * s,
        tw_lp * lp)
{
    (void)lp;

    free(s->global_channel);
    /*char *stats_file = getenv("TRACER_LINK_FILE");
      if(stats_file != NULL) {
      FILE *fout = fopen(stats_file, "a");
      const slimfly_param *p = s->params;
      int result = flock(fileno(fout), LOCK_EX);
      assert(result);
      fprintf(fout, "%d %d ", s->router_id / p->num_routers,
      s->router_id % p->num_routers);
      for(int d = 0; d < p->num_routers + p->num_global_channels; d++) {
      fprintf(fout, "%d ", s->link_traffic[d]);
      }
      fprintf(fout, "\n");
      result = flock(fileno(fout), LOCK_UN);
      fclose(fout);
      }*/
    int i, j;
    for(i = 0; i < s->params->radix; i++) {
        for(j = 0; j < s->params->num_vcs; j++) {
            if(s->queued_msgs[i][j] != NULL) {
                //          printf("[%lu] leftover queued messages %d %d %d\n", lp->gid, i, j,
                //          s->vc_occupancy[i][j]);
            }
            if(s->pending_msgs[i][j] != NULL) {
                //          printf("[%lu] lefover pending messages %d %d\n", lp->gid, i, j);
            }
        }
    }
    int written = 0;
    if(s->router_id == 0)
    {
        /* write metadata file */
        /*        char meta_fname[64];
                  sprintf(meta_fname, "slimfly-msg-stats.meta");

                  FILE * fp = fopen(meta_fname, "w");
                  fprintf(fp, "# Format <LP ID> <Group ID> <Router ID> <Busy time per router port(s)>");
                  fprintf(fp, "# Router ports in the order: %d local channels, %d global channels", 
                  p->num_routers, p->num_global_channels);
                  fclose(fp);
                  */        written = sprintf(s->output_buf,"# Format <LP ID> <Group ID> <Router ID> <Busy time per router port(s)>");
    }
    written += sprintf(s->output_buf + written, "\n %llu %d %d", 
            LLU(lp->gid),
            s->group_id,
            s->router_id);
    for(int d = 0; d < s->params->num_local_channels + s->params->num_global_channels; d++) 
        written += sprintf(s->output_buf + written, " %lf", s->busy_time[d]);

    lp_io_write(lp->gid, "slimfly-router-stats", written, s->output_buf);

    written = 0;
    if(!s->router_id)
    {
        written = sprintf(s->output_buf2, "# Format <LP ID> <Group ID> <Router ID> <Link traffic per router port(s)>");
        written += sprintf(s->output_buf2 + written, "# Router ports in the order: %d local channels, %d global channels",
                s->params->num_local_channels, s->params->num_global_channels);
    }
    written += sprintf(s->output_buf2 + written, "\n %llu %d %d",
            LLU(lp->gid),
            s->group_id,
            s->router_id);

    for(int d = 0; d < s->params->num_local_channels + s->params->num_global_channels; d++) 
        written += sprintf(s->output_buf2 + written, " %lld", LLD(s->link_traffic[d]));

    assert(written < 4096);
    lp_io_write(lp->gid, "slimfly-router-traffic", written, s->output_buf2);
}

/** Get the length (number of hops) in the route/path starting with a local src router to ending dest router
 *  @param[in] 	dest 		The ID for the destination router
 *  @param[in] 	src   		The state for the current local router
 *  @param[out] num_hops 	number of hops in the minimal path/route
 */
int get_path_length_local(router_state * src, int dest)
{
    int i, num_hops=2;
    for(i=0;i<src->params->num_global_channels;i++)
    {
        if(src->global_channel[i] == dest)
        {
            num_hops = 1;
        }
    }
    for(i=0;i<src->params->num_local_channels;i++)
    {
        if(src->local_channel[i] == dest)
        {
            num_hops = 1;
        }
    }
    return num_hops;
}

void get3DCoordinates(int router_id, int *s, int *i, int *j, router_state * r)
{
    // Decompose source and destination Router IDs into 3D subgraph coordinates (subgraph,i,j)
    if(router_id >= r->params->slim_total_routers/2)
    {
        *s = 1;
        *i = (router_id - r->params->slim_total_routers/2) / r->params->num_global_channels;
        *j = (router_id - r->params->slim_total_routers/2) % r->params->num_global_channels;
    }
    else
    {
        *s = 0;
        *i = router_id / r->params->num_global_channels;
        *j = router_id % r->params->num_global_channels;
    }
}

/** Get the length (number of hops) in the route/path starting with a global src router to ending dest router
 *  @param[in] 	dest 		The ID for the destination router
 *  @param[in] 	src   		The state for the global router
 *  @param[out] num_hops 	number of hops in the minimal path/route
 */
int get_path_length_global(int src, int dest, router_state * r)
{
    int j,num_hops=2;
    int s_s,s_d,i_s,i_d,j_s,j_d;
    // Get corresponding graph coordinates for source and destination routers
    get3DCoordinates(src,&s_s,&i_s,&j_s,r);
    get3DCoordinates(dest,&s_d,&i_d,&j_d,r);

    if(s_s == s_d)
    {
        if(s_s == 0)
        {
            //Case 1a local connection same subgroup 0
            for(j=0;j<X_size;j++)
            {
                if(abs(j_s-j_d) == X[j])
                {
                    num_hops = 1;
                    break;
                }
            }
        }
        else
        {
            //Case 1b local connection same subgroup 1
            for(j=0;j<X_size;j++)
            {
                if(abs(j_s-j_d) == X_prime[j])
                {
                    num_hops = 1;
                    break;
                }
            }
        }
    }
    else
    {
        if(s_s == 0)
        {
            //Case 1c global connection s_s=0, s_d=1
            if(j_s == (i_d*i_s+j_d) % r->params->num_routers)
            {
                num_hops = 1;
            }
        }
        else
        {
            //Case 1d global connection s_s=1, s_d=0
            if(j_d == (i_s*i_d+j_s) % r->params->num_routers)
            {
                num_hops = 1;
            }
        }
    }
    return num_hops;
}

/** Get the next router along the minimal path to the destination using the 3 paper connection equations
 *  Cases:
 *     1. Rs is directly connected to Rd.
 *       a. Subgraph 0 local connection
 *       b. Subgraph 1 local connection
 *       c. Subgraph 0 global connection to subgraph 1
 *       d. Subgraph 1 global connection to subgraph 0
 *     2. Rs is indirectly connected to Rd in separate subgraphs. (global and local path)
 *       a. Rs local to Rm and global to Rd. Rs and Rm in subgraph 0. Rd in subgraph 1.
 *       b. Rs global to Rm and local to Rd. Rs in subgraph 0. Rd and Rm in subgraph 1.
 *       c. Rs local to Rm and global to Rd. Rs and Rm in subgraph 1. Rd in subgraph 0.
 *       d. Rs global to Rm and local to Rd. Rs in subgraph 1. Rd and Rs in subgraph 0.
 *     3. Rs and Rd are in same group connected through Rm also in same group. (2 local paths)
 *       a. All in subgraph 0.
 *       b. All in subgraph 1.
 *     4. Rs and Rd in same subgraph globally connected to intermediate router in other subgraph (2 global paths)
 *       a. Rs and Rd in subgraph 0. Rm in subgraph 1.
 *       b. Rs and Rd in subgraph 1. Rm in subgraph 0.
 *  @param[in] rid The ID for the destination router
 *  @param[in] r   The state for the current router
 *  @param[out] router_id The ID for a router in the destination router group
 */
//////////////////////////////////////// Get router in the group which has a global channel to group id gid /////////////////////////////////
tw_lpid getMinimalRouterFromEquations(slim_terminal_message * msg, int rid, router_state * r)
{
    int s_s,s_d,i_s,i_d,j_s,j_d;
    int i,j;
    int match = 0;
    tw_lpid router_id = 0;
    // Get corresponding graph coordinates for source and destination routers
    get3DCoordinates((int)r->router_id,&s_s,&i_s,&j_s,r);
    get3DCoordinates(rid,&s_d,&i_d,&j_d,r);
#if DEBUG_ROUTING
    if( msg->packet_ID == TRACK && msg->chunk_id == num_chunks-1)
    {
        printf("source:%d, s_s:%d, i_s:%d, j_s:%d\n",(int)r->router_id,s_s, i_s, j_s);
        printf("destination:%d s_d:%d, i_d:%d, j_d:%d\n",rid,s_d, i_d, j_d);
    }
#endif

    // Check for case 1
    for(i=0;i<r->params->num_local_channels;i++)
    {
#if DEBUG_ROUTING
        if( msg->packet_ID == TRACK && msg->chunk_id == num_chunks-1)
        {
            printf("comparing local:%d with dest:%d\n",r->local_channel[i],rid);
        }
#endif
        if(r->local_channel[i] == rid)
        {
            router_id = rid;
            match = 1; 				// Case 1a and 1b
#if DEBUG_ROUTING
            if( msg->packet_ID == TRACK && msg->chunk_id == num_chunks-1)
            {
                printf("match:%d case:1a/1b\n",(int)router_id);
            }
#endif
            break;
        }
    }
    if(match == 0)
    {
        for(i=0;i<r->params->num_global_channels;i++)
        {
#if DEBUG_ROUTING
            if( msg->packet_ID == TRACK && msg->chunk_id == num_chunks-1)
            {
                printf("comparing global:%d with dest:%d\n",r->global_channel[i],rid);
            }
#endif
            if(r->global_channel[i] == rid)
            {
                router_id = rid;
                match = 1;			// Case 1c
#if DEBUG_ROUTING
                if( msg->packet_ID == TRACK && msg->chunk_id == num_chunks-1)
                {
                    printf("match case:1c\n");
                }
#endif
                break;
            }
        }
    }
    if(match == 0)
    {
        //Loop over all global and local connections
        for(i=0;i<r->params->num_global_channels+r->params->num_local_channels;i++)
        {
            int s_m,i_m,j_m;
            if(i<r->params->num_global_channels)
            {
                get3DCoordinates(r->global_channel[i],&s_m,&i_m,&j_m,r);
            }
            else
            {
                get3DCoordinates(r->local_channel[i-r->params->num_global_channels],&s_m,&i_m,&j_m,r);
            }
#if DEBUG_ROUTING
            if( msg->packet_ID == TRACK && msg->chunk_id == num_chunks-1)
            {
                printf("possible case 2,3,4\n");
                if(i<r->params->num_global_channels)
                {
                    printf("global_id:%d, s_m:%d, i_m:%d, j_m:%d\n",(int)r->global_channel[i%GLOBAL_CHANNELS],s_m, i_m, j_m);
                    printf("case 4b,2a result comparing global%d and dest%d: %d=%d\n",r->global_channel[i%GLOBAL_CHANNELS],rid,j_m,(i_d*i_m+j_d) % NUM_ROUTER);
                    printf("case 4a,2c result comparing global%d and dest%d: %d=%d\n",r->global_channel[i%GLOBAL_CHANNELS],rid,j_d,(i_m*i_d+j_m) % NUM_ROUTER);
                }
                else
                {
                    printf("local_id:%d, s_m:%d, i_m:%d, j_m:%d\n",(int)r->local_channel[i%GLOBAL_CHANNELS],s_m, i_m, j_m);
                    printf("case 4b,2a result comparing local%d and dest%d: %d=%d\n",r->local_channel[i%GLOBAL_CHANNELS],rid,j_m,(i_d*i_m+j_d) % NUM_ROUTER);
                    printf("case 4a,2c result comparing local%d and dest%d: %d=%d\n",r->local_channel[i%GLOBAL_CHANNELS],rid,j_d,(i_m*i_d+j_m) % NUM_ROUTER);
                }
            }
#endif
            if(s_s == s_d)
            {
                if(s_s == s_m)
                {
                    if(s_s == 0)
                    {
                        if(i_m == i_d && i_m == i_s)
                        {
#if DEBUG_ROUTING
                            if( msg->packet_ID == TRACK && msg->chunk_id == num_chunks-1)
                            {
                                printf("possible case:3a\n");
                            }
#endif
                            for(j=0;j<X_size;j++)
                            {
                                if(abs(j_m-j_d) == X[j])
                                {
                                    if(i<r->params->num_global_channels)
                                    {
                                        router_id = r->global_channel[i];
                                    }
                                    else
                                    {
                                        router_id = r->local_channel[i-r->params->num_global_channels];
                                    }
                                    match = 1;			// Case 3a
#if DEBUG_ROUTING
                                    if( msg->packet_ID == TRACK && msg->chunk_id == num_chunks-1)
                                    {
                                        printf("match case:3a\n");
                                    }
#endif
                                    break;
                                }
                            }
                        }
                    }
                    else
                    {
                        if(i_m == i_d && i_m == i_s)
                        {
#if DEBUG_ROUTING
                            if( msg->packet_ID == TRACK && msg->chunk_id == num_chunks-1)
                            {
                                printf("possible case:3b\n");
                            }
#endif
                            for(j=0;j<X_size;j++)
                            {
                                if(abs(j_m-j_d) == X_prime[j])
                                {
                                    if(i<r->params->num_global_channels)
                                    {
                                        router_id = r->global_channel[i];
                                    }
                                    else
                                    {
                                        router_id = r->local_channel[i-r->params->num_global_channels];
                                    }
                                    match = 1;			// Case 3b
#if DEBUG_ROUTING
                                    if( msg->packet_ID == TRACK && msg->chunk_id == num_chunks-1)
                                    {
                                        printf("match case:3b\n");
                                    }
#endif
                                }
                            }
                        }
                    }
                }
                else
                {
                    if(s_s == 0)
                    {
#if DEBUG_ROUTING
                        if( msg->packet_ID == TRACK && msg->chunk_id == num_chunks-1)
                        {
                            printf("possible case:4a\n");
                        }
#endif
                        if(j_d == (i_m*i_d+j_m) % r->params->num_routers)
                        {
                            if(i<r->params->num_global_channels)
                            {
                                router_id = r->global_channel[i];
                            }
                            else
                            {
                                router_id = r->local_channel[i-r->params->num_global_channels];
                            }
                            match = 1;				// Case 4a
#if DEBUG_ROUTING
                            if( msg->packet_ID == TRACK && msg->chunk_id == num_chunks-1)
                            {
                                printf("match case:4a\n");
                            }
#endif
                            break;
                        }

                    }
                    else
                    {
#if DEBUG_ROUTING
                        if( msg->packet_ID == TRACK && msg->chunk_id == num_chunks-1)
                        {
                            printf("possible case:4b\n");
                        }
#endif
                        if(j_m == (i_d*i_m+j_d) % r->params->num_routers)
                        {
                            if(i<r->params->num_global_channels)
                            {
                                router_id = r->global_channel[i];
                            }
                            else
                            {
                                router_id = r->local_channel[i-r->params->num_global_channels];
                            }
                            match = 1;				// Case 4b
#if DEBUG_ROUTING
                            if( msg->packet_ID == TRACK && msg->chunk_id == num_chunks-1)
                            {
                                printf("match case:4b\n");
                            }
#endif
                            break;
                        }
                    }
                }
            }
            else
            {
                if(s_s == s_m)
                {
                    if(s_s == 0)
                    {
                        if(i_m == i_s)
                        {
#if DEBUG_ROUTING
                            if( msg->packet_ID == TRACK && msg->chunk_id == num_chunks-1)
                            {
                                printf("possible case:2a\n");
                            }
#endif
                            if(j_m == (i_d*i_m+j_d) % r->params->num_routers)
                            {
                                if(i<r->params->num_global_channels)
                                {
                                    router_id = r->global_channel[i];
                                }
                                else
                                {
                                    router_id = r->local_channel[i-r->params->num_global_channels];
                                }
                                match = 1;				// Case 2a
#if DEBUG_ROUTING
                                if( msg->packet_ID == TRACK && msg->chunk_id == num_chunks-1)
                                {
                                    printf("match case:2a\n");
                                }
#endif
                                break;
                            }
                        }
                    }
                    else
                    {
                        if(i_m == i_s)
                        {
#if DEBUG_ROUTING
                            if( msg->packet_ID == TRACK && msg->chunk_id == num_chunks-1)
                            {
                                printf("possible case:2c\n");
                            }
#endif
                            if(j_d == (i_m*i_d+j_m) % r->params->num_routers)
                            {
                                if(i<r->params->num_global_channels)
                                {
                                    router_id = r->global_channel[i];
                                }
                                else
                                {
                                    router_id = r->local_channel[i-r->params->num_global_channels];
                                }
                                match = 1;				// Case 2c
#if DEBUG_ROUTING
                                if( msg->packet_ID == TRACK && msg->chunk_id == num_chunks-1)
                                {
                                    printf("match case:2c\n");
                                }
#endif
                                break;
                            }
                        }
                    }
                }
                else
                {
                    if(s_s == 0)
                    {
                        if(i_m == i_d)
                        {
#if DEBUG_ROUTING
                            if( msg->packet_ID == TRACK && msg->chunk_id == num_chunks-1)
                            {
                                printf("possible case:2b\n");
                            }
#endif
                            for(j=0;j<X_size;j++)
                            {
                                if(abs(j_d-j_m) == X_prime[j])
                                {
                                    if(i<r->params->num_global_channels)
                                    {
                                        router_id = r->global_channel[i];
                                    }
                                    else
                                    {
                                        router_id = r->local_channel[i-r->params->num_global_channels];
                                    }
                                    match = 1;			// Case 2b
#if DEBUG_ROUTING
                                    if( msg->packet_ID == TRACK && msg->chunk_id == num_chunks-1)
                                    {
                                        printf("match case:2b\n");
                                    }
#endif
                                }
                            }
                        }
                    }
                    else
                    {
                        if(i_m == i_d)
                        {
#if DEBUG_ROUTING
                            if( msg->packet_ID == TRACK && msg->chunk_id == num_chunks-1)
                            {
                                printf("possible case:2d\n");
                            }
#endif
                            for(j=0;j<X_size;j++)
                            {
                                if(abs(j_m-j_d) == X[j])
                                {
                                    if(i<r->params->num_global_channels)
                                    {
                                        router_id = r->global_channel[i];
                                    }
                                    else
                                    {
                                        router_id = r->local_channel[i-r->params->num_global_channels];
                                    }
                                    match = 1;			// Case 2d
#if DEBUG_ROUTING
                                    if( msg->packet_ID == TRACK && msg->chunk_id == num_chunks-1)
                                    {
                                        printf("match case:2d\n");
                                    }
#endif
                                    break;
                                }
                            }
                        }
                    }
                }
            }
            if(match==1)
                break;
        }
    }
#if DEBUG_ROUTING
    if( msg->packet_ID == TRACK && msg->chunk_id == num_chunks-1)
    {
        if(match == 1)
        {
            printf("source:%d destination:%d match:%d\n",(int)r->router_id,rid,(int)router_id);
        }
        else
        {
            printf("source:%d destination:%d no match so defaulting to router:%d\n",(int)r->router_id,rid,(int)router_id);
        }
    }
#endif
    if(match == 0 && (int)r->router_id != 0)
    {
        printf("packet_ID:%d source:%d destination:%d no match so defaulting to router:%d\n",(int)msg->packet_ID,(int)r->router_id,rid,(int)router_id);
    }
    return router_id;
}

/* get the next stop for the current packet
 * determines if it is a router within a group, a router in another group
 * or the destination terminal */
tw_lpid slim_get_next_stop(router_state * s,
        tw_bf * bf,
        slim_terminal_message * msg,
        tw_lp * lp,
        int path,
        int dest_router_id,
        int intm_id)
{
    (void)msg;
    (void)bf;

    int dest_lp = -1;
    tw_lpid router_dest_id = -1;

    codes_mapping_get_lp_info(lp->gid, lp_group_name, &mapping_grp_id, NULL,
            &mapping_type_id, NULL, &mapping_rep_id, &mapping_offset);
    int local_router_id = (mapping_offset + mapping_rep_id);

    /* If the packet has arrived at the destination router */
    if(dest_router_id == local_router_id)
    {
        dest_lp = msg->dest_terminal_id;
#if TRACK_OUTPUT
        if( msg->packet_ID == TRACK )
        {
            printf( "\x1b[32m-->Finishing Routing at current router:%d (gid:%d) to dest_terminal(globalID):%d through randm intm router:%d next stop:%d (globalid:%d)\x1b[0m\n",s->router_id,(int)lp->gid,(int)msg->dest_terminal_id,msg->intm_router_id,dest_lp,(int)router_dest_id);
        }
#endif
        return dest_lp;
    }
    /* Assign inter-mediate destination for non-minimal routing (selecting a random router) */
    if(msg->last_hop == TERMINAL && path == NON_MINIMAL)
    {
        msg->intm_router_id = intm_id;
        dest_lp=getMinimalRouterFromEquations(msg, msg->intm_router_id, s);
        codes_mapping_get_lp_id(lp_group_name, "slimfly_router", s->anno, 0, dest_lp, 0, &router_dest_id);
#if TRACK_OUTPUT
        if( msg->packet_ID == TRACK )
        {
            printf( "\x1b[32m-->Starting First leg NonMinimal from current router:%d (gid:%d) to dest_router:%d through randm intm router:%d next stop:%d (globalid:%d)\x1b[0m\n",s->router_id,(int)lp->gid,dest_router_id,msg->intm_router_id,dest_lp,(int)router_dest_id);
        }
#endif
        return router_dest_id;
    }
    /* If the packet has arrived at the inter-mediate router for non-minimal routing. Reset the intm router now. */
    if(path == NON_MINIMAL && msg->intm_router_id == s->router_id)
    {
#if TRACK_OUTPUT
        if( msg->packet_ID == TRACK )
        {
            printf( "\x1b[32m-->Arrived at NonMinimal intm_router %d s->router_id:%d local_router_id:%d (gid:%d) dest_router:%d next stop:%d (globalid:%d)\x1b[0m\n",msg->intm_router_id,s->router_id,local_router_id,(int)lp->gid,dest_router_id,dest_lp,(int)router_dest_id);
        }
#endif
        msg->intm_router_id = -1;//no inter-mediate router
    }
    /* If intermediate router is set, route minimally to intermediate router*/
    if(path == NON_MINIMAL && msg->intm_router_id >= 0 && (msg->intm_router_id != local_router_id))
    {
        dest_lp=getMinimalRouterFromEquations(msg, msg->intm_router_id, s);
        codes_mapping_get_lp_id(lp_group_name, "slimfly_router", s->anno, 0, dest_lp, 0, &router_dest_id);
#if TRACK_OUTPUT
        if( msg->packet_ID == TRACK )
        {
            printf( "\x1b[32m-->Continuing First leg Non-Minimal Routing from current router:%d (gid:%d) to dest_router:%d through randm intm router:%d next stop:%d (globalid:%d)\x1b[0m\n",s->router_id,(int)lp->gid,dest_router_id,msg->intm_router_id,dest_lp,(int)router_dest_id);
        }
#endif
        return router_dest_id;
    }
    /* No intermediate router set, then route to destination*/
    if(path == NON_MINIMAL && msg->intm_router_id < 0)
    {
        dest_lp=getMinimalRouterFromEquations(msg, dest_router_id, s);
        codes_mapping_get_lp_id(lp_group_name, "slimfly_router", s->anno, 0, dest_lp, 0, &router_dest_id);
#if TRACK_OUTPUT
        if( msg->packet_ID == TRACK )
        {
            printf( "\x1b[32m-->Second leg Non-Minimal Routing from current router:%d (gid:%d) to dest_router:%d through randm intm router:%d next stop:%d (globalid:%d)\x1b[0m\n",s->router_id,(int)lp->gid,dest_router_id,msg->intm_router_id,dest_lp,(int)router_dest_id);
        }
#endif
        return router_dest_id;
    }
    if(path == MINIMAL)
    {
        dest_lp=getMinimalRouterFromEquations(msg, dest_router_id, s);
    }

    codes_mapping_get_lp_id(lp_group_name, "slimfly_router", s->anno, 0, dest_lp, 0, &router_dest_id);

#if TRACK_OUTPUT
    if( msg->packet_ID == TRACK && msg->last_hop == TERMINAL)
    {
        printf( "\x1b[32m-->Starting Minimal Routing from current router:%d (gid:%d) to dest_router:%d next stop:%d (globalid:%d)\x1b[0m\n",s->router_id,(int)lp->gid,dest_router_id,dest_lp,(int)router_dest_id);
    }
    if( msg->packet_ID == TRACK && msg->last_hop != TERMINAL)
    {
        printf( "\x1b[32m-->Continuing Minimal Routing from current router:%d (gid:%d) to dest_router:%d \x1b[0m\n",s->router_id,(int)lp->gid,dest_router_id);
    }
#endif

    return router_dest_id;
}
/* gets the output port corresponding to the next stop of the message */
int slim_get_output_port( router_state * s,
        slim_terminal_message * msg,
        tw_lp * lp,
        int next_stop )
{
    (void)lp;

    int output_port = -1, terminal_id;
    codes_mapping_get_lp_info(msg->dest_terminal_id, lp_group_name,
            &mapping_grp_id, NULL, &mapping_type_id, NULL, &mapping_rep_id,
            &mapping_offset);
    int num_lps = codes_mapping_get_lp_count(lp_group_name,1,LP_CONFIG_NM,s->anno,0);
    terminal_id = (mapping_rep_id * num_lps) + mapping_offset;

    if((tw_lpid)next_stop == msg->dest_terminal_id)
    {
        output_port = s->params->num_local_channels + s->params->num_global_channels +
            ( terminal_id % s->params->num_cn);
    }
    else
    {
        codes_mapping_get_lp_info(next_stop, lp_group_name, &mapping_grp_id,
                NULL, &mapping_type_id, NULL, &mapping_rep_id, &mapping_offset);
        int local_router_id = mapping_rep_id + mapping_offset;

        int i;

        for(i=0; i<s->params->num_global_channels; i++)
        {
            if(s->global_channel[i] == local_router_id)
            {
                output_port = s->params->num_local_channels + i;
                break;
            }
            else if(i<s->params->num_local_channels && s->local_channel[i] == local_router_id)
            {
                output_port = i;
                break;
            }
        }
        if(output_port == -1)
        {
            printf("\n output port not found %d next stop lp_id %d next_stop router id %d \n", output_port, next_stop, local_router_id);
        }
    }

    return output_port;
}


/* UGAL (first condition is from booksim), output port equality check comes from Dally slimfly'09*/
static int do_adaptive_routing( router_state * s,
        slim_terminal_message * msg,
        tw_lp * lp,
        int dest_router_id,
        int * intm_id)
{
    int i;
    int *nonmin_out_port = (int *)malloc(num_indirect_routes * sizeof(int));
    int *num_nonmin_hops = (int *)malloc(num_indirect_routes * sizeof(int));
    int *nonmin_port_count = (int *)malloc(num_indirect_routes * sizeof(int));
    int next_stop;
    int minimal_out_port = -1;
    float *cost_nonminimal = (float *)(int *)malloc(num_indirect_routes * sizeof(float));
    float cost_minimal;
    tw_lpid *nonmin_next_stop = (tw_lpid *)malloc(num_indirect_routes * sizeof(tw_lpid));
    tw_lpid minimal_next_stop;

    //Compute the next stop on the minimal path and get port number
    int temp_minimal_next_stop = getMinimalRouterFromEquations(msg, dest_router_id, s);
    codes_mapping_get_lp_id(lp_group_name, "slimfly_router", s->anno, 0, temp_minimal_next_stop, 0, &minimal_next_stop);
    minimal_out_port = slim_get_output_port(s, msg, lp, minimal_next_stop);

    //Compute the next stop on the non-minimal paths and get port number
    int temp_nonmin_next_stop;
    for(i=0;i<num_indirect_routes;i++)
    {
        temp_nonmin_next_stop = getMinimalRouterFromEquations(msg, intm_id[i], s);
        codes_mapping_get_lp_id(lp_group_name, "slimfly_router", s->anno, 0, temp_nonmin_next_stop, 0, &nonmin_next_stop[i]);
        nonmin_out_port[i] = slim_get_output_port(s, msg, lp, nonmin_next_stop[i]);
    }

    //Only computed on first hop so vc will always be 0 for both min and nonmin
    int nomin_vc = 0;
    int min_vc = 0;

    //Calculate number of hops in random indirect routes above
    for(i=0;i<num_indirect_routes;i++)
    {
        //Compute # of hops from src to intm router
        num_nonmin_hops[i] = get_path_length_local(s, intm_id[i]);
        //Compute # of hops from intm to dest router
        num_nonmin_hops[i] += get_path_length_global(intm_id[i], dest_router_id, s);
    }
    //Calculate number of hops in minimal path
    int num_min_hops = get_path_length_local(s, dest_router_id);

    //Determine port occupancy for all minimal and nonminimal paths
    int min_port_count = s->vc_occupancy[minimal_out_port][min_vc];
    for(i=0;i<num_indirect_routes;i++)
    {
        nonmin_port_count[i] = s->vc_occupancy[nonmin_out_port[i]][nomin_vc];
    }

    //Calculate cost of all paths/routes
    cost_minimal = min_port_count;
    for(i=0;i<num_indirect_routes;i++)
    {
        cost_nonminimal[i] = ((float)num_nonmin_hops[i]/(float)num_min_hops) * (float)nonmin_port_count[i] * csf_ratio;
#if DEBUG
        if( msg->packet_ID == TRACK)
        {
            printf( "\x1b[31mnonmin_next_stop[%d]:%d num_hops_nonmin[%d]:%d, output_port_nonmin[%d]:%d port_occupancy_non_min[%d]:%d cost_indirect[%d]:%f\n      min_next_stop:%d       num_hops_min:%d,       output_port_min:%d        port_occupancy_min:%d     cost_minimal:%f\x1b[0m\n",i,(int)nonmin_next_stop[i],i,num_nonmin_hops[i],i,nonmin_out_port[i],i,nonmin_port_count[i],i,cost_nonminimal[i],(int)minimal_next_stop,num_min_hops,minimal_out_port,min_port_count,cost_minimal);
        }
#endif
    }

    //If minCost=="Minimal", route minimally; Else route Valiantly according to corresponding path
    msg->path_type = MINIMAL;
    next_stop = minimal_next_stop;
    float min_cost = cost_minimal;
    msg->intm_router_id = -1;		//So far no intermediate router
    for(i=0;i<num_indirect_routes;i++)
    {

        if(cost_nonminimal[i] < min_cost)
        {
            min_cost = (int)cost_nonminimal[i];
            msg->path_type = NON_MINIMAL;
            next_stop = nonmin_next_stop[i];
            msg->intm_router_id = intm_id[i];
        }
    }

    if(msg->path_type == MINIMAL)
    {
        if(msg->packet_ID == TRACK && msg->message_id == TRACK_MSG)
            printf("\n (%lf) [Router %d] Packet %d routing minimally \n", tw_now(lp), (int)lp->gid, (int)msg->packet_ID);
    }
    else
    {
        if(msg->packet_ID == TRACK && msg->message_id == TRACK_MSG)
            printf("\n (%lf) [Router %d] Packet %d routing non-minimally \n", tw_now(lp), (int)lp->gid, (int)msg->packet_ID);
    }
    return next_stop;
}

void slim_router_packet_receive_rc(router_state * s,
        tw_bf * bf,
        slim_terminal_message * msg,
        tw_lp * lp)
{
    int output_port = msg->saved_vc;
    int output_chan = msg->saved_channel;

    if(bf->c1)
        tw_rand_reverse_unif(lp->rng);

    if(bf->c5){
        for(int i=0;i<num_indirect_routes;i++){
            tw_rand_reverse_unif(lp->rng);
        }
    }

    if(bf->c2) {
        tw_rand_reverse_unif(lp->rng);
        slim_terminal_message_list * tail = return_tail(s->pending_msgs[output_port],
                    s->pending_msgs_tail[output_port], output_chan);
        slim_delete_terminal_message_list(tail);
        s->vc_occupancy[output_port][output_chan] -= s->params->chunk_size;
        if(bf->c3) {
            codes_local_latency_reverse(lp);
            s->in_send_loop[output_port] = 0;
        }
    }
    if(bf->c4) {
        s->last_buf_full[output_port] = msg->saved_busy_time;
        slim_delete_terminal_message_list(return_tail(s->queued_msgs[output_port],
                    s->queued_msgs_tail[output_port], output_chan));
    }
}

/* Packet arrives at the router and a credit is sent back to the sending terminal/router */
    void
slim_router_packet_receive( router_state * s,
        tw_bf * bf,
        slim_terminal_message * msg,
        tw_lp * lp )
{
    bf->c1 = 0;
    bf->c2 = 0;
    bf->c3 = 0;
    bf->c4 = 0;
    bf->c5 = 0;
    bf->c6 = 0;
    bf->c7 = 0;
    bf->c8 = 0;

    tw_stime ts;
    int i;
    int next_stop = -1, output_port = -1, output_chan = -1;

    codes_mapping_get_lp_info(msg->dest_terminal_id, lp_group_name,
            &mapping_grp_id, NULL, &mapping_type_id, NULL, &mapping_rep_id,
            &mapping_offset);
    int num_lps = codes_mapping_get_lp_count(lp_group_name, 1, LP_CONFIG_NM,
            s->anno, 0);
    int dest_router_id = (mapping_offset + (mapping_rep_id * num_lps)) /
        s->params->num_cn;

    int intm_id = -1;
    int *intm_router;		//Array version of intm_id for use in Adaptive routing
    int local_grp_id = s->router_id / s->params->num_routers;

    if(routing == NON_MINIMAL)
    {
        bf->c1 = 1;
        intm_id = tw_rand_integer(lp->rng, 0, s->params->slim_total_routers - 1);
        if(intm_id == local_grp_id)
        {
            //        intm_id = (local_grp_id + 2) % s->params->num_groups;
            intm_id = (local_grp_id + 1) % (s->params->slim_total_routers - 1);
        }
    }
    if(routing == ADAPTIVE)
    {
        intm_router = (int *)malloc(num_indirect_routes * sizeof(int)); 	//indirect == nonMinimal == valiant
        //Generate n_I many indirect routes through intermediate random routers
        bf->c5 = 1;
        for(i=0;i<num_indirect_routes;i++)
        {
            intm_router[i] = tw_rand_integer(lp->rng, 0, s->params->slim_total_routers-1);
            if(intm_router[i] == dest_router_id)	//Check if same as dest_router
            {
                intm_router[i] = (intm_router[i]+1) % (s->params->slim_total_routers-1);
            }
            if(intm_router[i] == (int)lp->gid)			//Check if same as source router
            {
                intm_router[i] = (intm_router[i]+1) % (s->params->slim_total_routers-1);
            }
        }
    }

    slim_terminal_message_list * cur_chunk = (slim_terminal_message_list *)malloc(
            sizeof(slim_terminal_message_list));
    slim_init_terminal_message_list(cur_chunk, msg);

    if(msg->last_hop == TERMINAL && routing == ADAPTIVE)
    {
        next_stop = do_adaptive_routing(s, &(cur_chunk->msg), lp, dest_router_id, intm_router);
    }
    else
    {
        if(routing == MINIMAL || routing == NON_MINIMAL)
            cur_chunk->msg.path_type = routing; /*defaults to the routing algorithm if we don't have adaptive routing here*/

        next_stop = slim_get_next_stop(s, bf, &(cur_chunk->msg), lp, cur_chunk->msg.path_type, dest_router_id, intm_id);
    }
    assert(cur_chunk->msg.path_type == MINIMAL || cur_chunk->msg.path_type == NON_MINIMAL);
    if(msg->remote_event_size_bytes > 0)
    {
        void *m_data_src = model_net_method_get_edata(SLIMFLY, msg);
        cur_chunk->event_data = (char*)malloc(msg->remote_event_size_bytes);
        memcpy(cur_chunk->event_data, m_data_src, msg->remote_event_size_bytes);
    }

    codes_mapping_get_lp_info(lp->gid, lp_group_name, &mapping_grp_id, NULL,
            &mapping_type_id, NULL, &mapping_rep_id, &mapping_offset);
    int local_router_id = (mapping_offset + mapping_rep_id);

    output_port = slim_get_output_port(s, &(cur_chunk->msg), lp, next_stop);
    assert(output_port >= 0);
    output_chan = 0;
    int max_vc_size = s->params->cn_vc_size;

#if TRACK_OUTPUT
    if( msg->packet_ID == TRACK )
    {
        printf( "\x1b[32m-->next stop:%d \x1b[0m\n",next_stop);
    }
#endif

    cur_chunk->msg.vc_index = output_port;
    cur_chunk->msg.next_stop = next_stop;

    if(output_port < s->params->num_local_channels)
    {
        max_vc_size = s->params->local_vc_size;
        output_chan = msg->my_N_hop;
//        if(msg->my_N_hop >3)
//            output_chan = 3;
    }
    else if(output_port < (s->params->num_local_channels + s->params->num_global_channels))
    {
        max_vc_size = s->params->global_vc_size;
        output_chan = msg->my_N_hop;
//        if(msg->my_N_hop >3)
//            output_chan = 3;
    }

#if TRACK_OUTPUT
    if( msg->packet_ID == TRACK )
    {
        printf("current router:%d dest_router:%d next stop lp_id:%d output port:%d output chan:%d\n",local_router_id,dest_router_id,next_stop,output_port,output_chan);
    }
#endif

    cur_chunk->msg.output_chan = output_chan;
    cur_chunk->msg.my_N_hop++;

    assert(output_chan < 4);	//Minimal routing needs 2 vcs and non-minimal needs 4 vcs
    assert(output_port < s->params->radix);

    if(s->vc_occupancy[output_port][output_chan] + s->params->chunk_size <= max_vc_size)
    {
#if TRACK_OUTPUT
        if( msg->packet_ID == TRACK )
        {
            printf("vc occupancy available\n");
        }
#endif
        bf->c2 = 1;
        slim_router_credit_send(s, msg, lp, -1);
        append_to_terminal_message_list( s->pending_msgs[output_port], s->pending_msgs_tail[output_port], output_chan, cur_chunk);
        s->vc_occupancy[output_port][output_chan] += s->params->chunk_size;

#if ROUTER_OCCUPANCY_LOG
        int index = floor(N_COLLECT_POINTS*(tw_now(lp)/g_tw_ts_end));
        vc_occupancy_storage_router[s->router_id][output_port][output_chan][index] = s->vc_occupancy[output_port][output_chan]/s->params->chunk_size;
#endif

        if(s->in_send_loop[output_port] == 0) {
            bf->c3 = 1;
            slim_terminal_message *m;
            ts = codes_local_latency(lp);
            tw_event *e = tw_event_new(lp->gid, ts, lp);
            m = tw_event_data(e);
            m->type = R_SEND;
            m->magic = slim_router_magic_num;
            m->vc_index = output_port;

            tw_event_send(e);
            s->in_send_loop[output_port] = 1;
        }
    }
    else
    {
#if TRACK_OUTPUT
        if( msg->packet_ID == TRACK )
        {
            printf("vc occupancy not available\n");
        }
#endif
        bf->c4 = 1;
        cur_chunk->msg.saved_vc = msg->vc_index;
        cur_chunk->msg.saved_channel = msg->output_chan;
        append_to_terminal_message_list( s->queued_msgs[output_port], s->queued_msgs_tail[output_port], output_chan, cur_chunk);
        msg->saved_busy_time = s->last_buf_full[output_port];
        s->last_buf_full[output_port] = tw_now(lp);
    }

    msg->saved_vc = output_port;
    msg->saved_channel = output_chan;

#if ROUTER_SENDS_RECVS_LOG
    router_recvs[s->router_id][index]++;
#endif

    return;
}

void slim_router_packet_send_rc(router_state * s,
        tw_bf * bf,
        slim_terminal_message * msg, tw_lp * lp)
{
    int output_port = msg->saved_vc;
    int output_chan = msg->saved_channel;
    if(bf->c1) {
        s->in_send_loop[output_port] = 1;
        return;
    }

    tw_rand_reverse_unif(lp->rng);
    slim_terminal_message_list * cur_entry = rc_stack_pop(s->st);
    assert(cur_entry);

    if(bf->c11)
    {
        s->link_traffic[output_port] -= cur_entry->msg.packet_size % s->params->chunk_size;
    }
    if(bf->c12)
    {
        s->link_traffic[output_port] -= s->params->chunk_size;
    }
    s->next_output_available_time[output_port] = msg->saved_available_time;

    prepend_to_terminal_message_list(s->pending_msgs[output_port],
            s->pending_msgs_tail[output_port], output_chan, cur_entry);

    if(bf->c3) {
        tw_rand_reverse_unif(lp->rng);
    }

    if(bf->c4) {
        s->in_send_loop[output_port] = 1;
    }

}
/* routes the current packet to the next stop */
    void
slim_router_packet_send( router_state * s,
        tw_bf * bf,
        slim_terminal_message * msg, tw_lp * lp)
{
    tw_stime ts;
    tw_event *e;
    slim_terminal_message *m;
    int output_port = msg->vc_index;
    int output_chan = 3;

    slim_terminal_message_list *cur_entry = s->pending_msgs[output_port][3];
    if(cur_entry == NULL)
    {
        cur_entry = s->pending_msgs[output_port][2];
        output_chan = 2;
        if(cur_entry == NULL)
        {
            cur_entry = s->pending_msgs[output_port][1];
            output_chan = 1;
            if(cur_entry == NULL)
            {
                cur_entry = s->pending_msgs[output_port][0];
                output_chan = 0;
            }
        }
    }
    msg->saved_vc = output_port;
    msg->saved_channel = output_chan;

    if(cur_entry == NULL)
    {
        bf->c1 = 1;
        s->in_send_loop[output_port] = 0;
        //printf("[%d] Router skipping send at begin %d \n", lp->gid, output_port);
        return;
    }

    int to_terminal = 1, global = 0;
    double delay = s->params->cn_delay;
    double bandwidth = s->params->cn_bandwidth;

    if(output_port < s->params->num_local_channels)
    {
        to_terminal = 0;
        delay = s->params->local_delay;
        bandwidth = s->params->local_bandwidth;
    }
    else if(output_port < s->params->num_local_channels + s->params->num_global_channels)
    {
        to_terminal = 0;
        global = 1;
        delay = s->params->global_delay;
        bandwidth = s->params->global_bandwidth;
    }

    uint64_t num_chunks = cur_entry->msg.packet_size / s->params->chunk_size;
    if(msg->packet_size % s->params->chunk_size)
        num_chunks++;
    if(!num_chunks)
        num_chunks = 1;

    double bytetime = delay;

    if((cur_entry->msg.packet_size % s->params->chunk_size) && (cur_entry->msg.chunk_id == num_chunks - 1))
        bytetime = bytes_to_ns(cur_entry->msg.packet_size % s->params->chunk_size, bandwidth); 

    ts = g_tw_lookahead + tw_rand_unif(lp->rng) + bytetime + s->params->router_delay;

    msg->saved_available_time = s->next_output_available_time[output_port];
    s->next_output_available_time[output_port] =
        maxd(s->next_output_available_time[output_port], tw_now(lp));
    s->next_output_available_time[output_port] += ts;

    ts = s->next_output_available_time[output_port] - tw_now(lp);
    // dest can be a router or a terminal, so we must check
    void * m_data;
    if (to_terminal)
    {
        assert(cur_entry->msg.next_stop == cur_entry->msg.dest_terminal_id);
        e = model_net_method_event_new(cur_entry->msg.next_stop,
                s->next_output_available_time[output_port] - tw_now(lp), lp,
                SLIMFLY, (void**)&m, &m_data);
    }
    else
    {
        e = tw_event_new(cur_entry->msg.next_stop,
                s->next_output_available_time[output_port] - tw_now(lp), lp);
        m = tw_event_data(e);
        m_data = model_net_method_get_edata(SLIMFLY, m);
    }
    memcpy(m, &cur_entry->msg, sizeof(slim_terminal_message));
    if (m->remote_event_size_bytes){
        memcpy(m_data, cur_entry->event_data, m->remote_event_size_bytes);
    }

    if(global)
        m->last_hop = GLOBAL;
    else
        m->last_hop = LOCAL;

    m->local_id = s->router_id;
    m->intm_lp_id = lp->gid;
    m->magic = slim_router_magic_num;

    if((cur_entry->msg.packet_size % s->params->chunk_size) && (cur_entry->msg.chunk_id == num_chunks - 1)) {
        bf->c11 = 1;
        s->link_traffic[output_port] +=  (cur_entry->msg.packet_size %
            s->params->chunk_size); 
    } else {
        bf->c12 = 1;
        s->link_traffic[output_port] += s->params->chunk_size;
    }

    /* Determine the event type. If the packet has arrived at the final
     * destination router then it should arrive at the destination terminal
     * next.*/
    if(to_terminal)
    {
        m->type = T_ARRIVE;
        m->magic = slim_terminal_magic_num;
    }
    else
    {
        /* The packet has to be sent to another router */
        m->magic = slim_router_magic_num;
        m->type = R_ARRIVE;
    }
    tw_event_send(e);

    cur_entry = return_head(s->pending_msgs[output_port],
            s->pending_msgs_tail[output_port], output_chan);
    rc_stack_push(lp, cur_entry, (void*)slim_delete_terminal_message_list, s->st);

    cur_entry = s->pending_msgs[output_port][3];

    s->next_output_available_time[output_port] -= s->params->router_delay;
    ts -= s->params->router_delay;

    if(cur_entry == NULL) cur_entry = s->pending_msgs[output_port][2];
    if(cur_entry == NULL) cur_entry = s->pending_msgs[output_port][1];
    if(cur_entry == NULL) cur_entry = s->pending_msgs[output_port][0];
    if(cur_entry != NULL)
    {
        bf->c3 = 1;
        slim_terminal_message *m_new;
        ts = ts + g_tw_lookahead * tw_rand_unif(lp->rng);
        tw_event *e_new = tw_event_new(lp->gid, ts, lp);
        m_new = tw_event_data(e_new);
        m_new->type = R_SEND;
        m_new->magic = slim_router_magic_num;
        m_new->vc_index = output_port;
        tw_event_send(e_new);
    }
    else
    {
        bf->c4 = 1;
        s->in_send_loop[output_port] = 0;
    }

#if TRACK_OUTPUT
    if( m->packet_ID == TRACK )
    {
        printf("router packet sending \n");
    }
#endif

#if ROUTER_SENDS_RECVS_LOG
    int index = floor(N_COLLECT_POINTS*(tw_now(lp)/g_tw_ts_end));
    router_sends[s->router_id][index]++;
#endif

    return;
}

void slim_router_buf_update_rc(router_state * s,
        tw_bf * bf,
        slim_terminal_message * msg,
        tw_lp * lp)
{
    int indx = msg->vc_index;
    int output_chan = msg->output_chan;
    s->vc_occupancy[indx][output_chan] += s->params->chunk_size;
    if(bf->c3)
    {
        s->busy_time[indx] = msg->saved_rcv_time;
        s->last_buf_full[indx] = msg->saved_busy_time;
    }
    if(bf->c1) {
        slim_terminal_message_list* head = return_tail(s->pending_msgs[indx],
                s->pending_msgs_tail[indx], output_chan);
        tw_rand_reverse_unif(lp->rng);
        prepend_to_terminal_message_list(s->queued_msgs[indx],
                s->queued_msgs_tail[indx], output_chan, head);
        s->vc_occupancy[indx][output_chan] += s->params->chunk_size;
    }
    if(bf->c2) {
        codes_local_latency_reverse(lp);
        s->in_send_loop[indx] = 0;
    }
}
/* Update the buffer space associated with this router LP */
void slim_router_buf_update(router_state * s, tw_bf * bf, slim_terminal_message * msg, tw_lp * lp)
{
    int indx = msg->vc_index;
    int output_chan = msg->output_chan;
    s->vc_occupancy[indx][output_chan] -= s->params->chunk_size;

    if(s->last_buf_full[indx])
    {
        bf->c3 = 1;
        msg->saved_rcv_time = s->busy_time[indx];
        msg->saved_busy_time = s->last_buf_full[indx];
        s->busy_time[indx] += (tw_now(lp) - s->last_buf_full[indx]);
        s->last_buf_full[indx] = 0.0;
    }

#if ROUTER_OCCUPANCY_LOG
    int index = floor(N_COLLECT_POINTS*(tw_now(lp)/g_tw_ts_end));
    vc_occupancy_storage_router[s->router_id][indx][output_chan][index] = s->vc_occupancy[indx][output_chan]/s->params->chunk_size;
#endif
    if(s->queued_msgs[indx][output_chan] != NULL) {
        bf->c1 = 1;
        slim_terminal_message_list *head = return_head(s->queued_msgs[indx],
                s->queued_msgs_tail[indx], output_chan);
        slim_router_credit_send(s, &head->msg, lp, 1);
        append_to_terminal_message_list(s->pending_msgs[indx],
                s->pending_msgs_tail[indx], output_chan, head);
        s->vc_occupancy[indx][output_chan] -= s->params->chunk_size;
#if ROUTER_OCCUPANCY_LOG
        vc_occupancy_storage_router[s->router_id][indx][output_chan][index] = s->vc_occupancy[indx][output_chan]/s->params->chunk_size;
#endif
    }
    if(s->in_send_loop[indx] == 0 && s->pending_msgs[indx][output_chan] != NULL) {
        bf->c2 = 1;
        slim_terminal_message *m;
        tw_stime ts = codes_local_latency(lp);
        tw_event *e = tw_event_new(lp->gid, ts, lp);
        m = tw_event_data(e);
        m->type = R_SEND;
        m->vc_index = indx;
        m->magic = slim_router_magic_num;
        s->in_send_loop[indx] = 1;
        tw_event_send(e);
    }

    return;
}

void slim_router_event(router_state * s, tw_bf * bf, slim_terminal_message * msg,
        tw_lp * lp) {
    assert(msg->magic == slim_router_magic_num);
    switch(msg->type)
    {
        case R_SEND: // Router has sent a packet to an intra-group router (local channel)
            slim_router_packet_send(s, bf, msg, lp);
            break;

        case R_ARRIVE: // Router has received a packet from an intra-group router (local channel)
            slim_router_packet_receive(s, bf, msg, lp);
            break;

        case R_BUFFER:
            slim_router_buf_update(s, bf, msg, lp);
            break;

        default:
            printf("\n (%lf) [Router %d] Router Message type not supported %d dest "
                    "terminal id %d packet ID %d ", tw_now(lp), (int)lp->gid, msg->type,
                    (int)msg->dest_terminal_id, (int)msg->packet_ID);
            tw_error(TW_LOC, "Msg type not supported");
            break;
    }
}

/* Reverse computation handler for a terminal event */
void slim_terminal_rc_event_handler(terminal_state * s, tw_bf * bf,
        slim_terminal_message * msg, tw_lp * lp) {

    switch(msg->type)
    {
        case T_GENERATE:
            slim_packet_generate_rc(s, bf, msg, lp);
            break;

        case T_SEND:
            slim_packet_send_rc(s, bf, msg, lp);
            break;

        case T_ARRIVE:
            slim_packet_arrive_rc(s, bf, msg, lp);
            break;

        case T_BUFFER:
            slim_terminal_buf_update_rc(s, bf, msg, lp);
            break;
    }
}

/* Reverse computation handler for a router event */
void slim_router_rc_event_handler(router_state * s, tw_bf * bf,
        slim_terminal_message * msg, tw_lp * lp) {
    switch(msg->type) {
        case R_SEND:
            slim_router_packet_send_rc(s, bf, msg, lp);
            break;
        case R_ARRIVE:
            slim_router_packet_receive_rc(s, bf, msg, lp);
            break;

        case R_BUFFER:
            slim_router_buf_update_rc(s, bf, msg, lp);
            break;
    }
}

/* slimfly compute node and router LP types */
tw_lptype slimfly_lps[] =
{
    // Terminal handling functions
    {
        (init_f)slim_terminal_init,
        (pre_run_f) NULL,
        (event_f) slim_terminal_event,
        (revent_f) slim_terminal_rc_event_handler,
        (commit_f) NULL,
        (final_f) slimfly_terminal_final,
        (map_f) codes_mapping,
        sizeof(terminal_state)
    },
    {
        (init_f) slim_router_setup,
        (pre_run_f) NULL,
        (event_f) slim_router_event,
        (revent_f) slim_router_rc_event_handler,
        (commit_f) NULL,
        (final_f) slimfly_router_final,
        (map_f) codes_mapping,
        sizeof(router_state),
    },
    {NULL, NULL, NULL, NULL, NULL, NULL, NULL, 0},
};

/* returns the slimfly lp type for lp registration */
static const tw_lptype* slimfly_get_cn_lp_type(void)
{
    return(&slimfly_lps[0]);
}

static void slimfly_register(tw_lptype *base_type) {
    lp_type_register(LP_CONFIG_NM, base_type);
    lp_type_register("slimfly_router", &slimfly_lps[1]);
}

/* data structure for slimfly statistics */
struct model_net_method slimfly_method =
{
    .mn_configure = slimfly_configure,
    .mn_register = slimfly_register,
    .model_net_method_packet_event = slimfly_packet_event,
    .model_net_method_packet_event_rc = slimfly_packet_event_rc,
    .model_net_method_recv_msg_event = NULL,
    .model_net_method_recv_msg_event_rc = NULL,
    .mn_get_lp_type = slimfly_get_cn_lp_type,
    .mn_get_msg_sz = slimfly_get_msg_sz,
    .mn_report_stats = slimfly_report_stats
};
