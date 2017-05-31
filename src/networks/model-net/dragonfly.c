/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

// Local router ID: 0 --- total_router-1
// Router LP ID 
// Terminal LP ID

#include <ross.h>

#define DEBUG_LP 892
#include "codes/jenkins-hash.h"
#include "codes/codes_mapping.h"
#include "codes/codes.h"
#include "codes/model-net.h"
#include "codes/model-net-method.h"
#include "codes/model-net-lp.h"
#include "codes/net/dragonfly.h"
#include "sys/file.h"
#include "codes/quickhash.h"
#include "codes/rc-stack.h"

#ifdef ENABLE_CORTEX
#include <cortex/cortex.h>
#include <cortex/topology.h>
#endif

#define CREDIT_SZ 8
#define MEAN_PROCESS 1.0

/* collective specific parameters */
#define TREE_DEGREE 4
#define LEVEL_DELAY 1000
#define DRAGONFLY_COLLECTIVE_DEBUG 0
#define NUM_COLLECTIVES  1
#define COLLECTIVE_COMPUTATION_DELAY 5700
#define DRAGONFLY_FAN_OUT_DELAY 20.0
#define WINDOW_LENGTH 0
#define DFLY_HASH_TABLE_SIZE 4999

// debugging parameters
#define TRACK -1
#define TRACK_PKT -1
#define TRACK_MSG -1
#define PRINT_ROUTER_TABLE 1
#define DEBUG 0
#define USE_DIRECT_SCHEME 1
#define MAX_STATS 65536

#define LP_CONFIG_NM_TERM (model_net_lp_config_names[DRAGONFLY])
#define LP_METHOD_NM_TERM (model_net_method_names[DRAGONFLY])
#define LP_CONFIG_NM_ROUT (model_net_lp_config_names[DRAGONFLY_ROUTER])
#define LP_METHOD_NM_ROUT (model_net_method_names[DRAGONFLY_ROUTER])

#ifdef ENABLE_CORTEX
/* This structure is defined at the end of the file */
extern cortex_topology dragonfly_cortex_topology;
#endif

int debug_slot_count = 0;
long term_ecount, router_ecount, term_rev_ecount, router_rev_ecount;
long packet_gen = 0, packet_fin = 0;

static double maxd(double a, double b) { return a < b ? b : a; }

/* minimal and non-minimal packet counts for adaptive routing*/
static int minimal_count=0, nonmin_count=0;
static int num_routers_per_mgrp = 0;

typedef struct dragonfly_param dragonfly_param;
/* annotation-specific parameters (unannotated entry occurs at the 
 * last index) */
static uint64_t                  num_params = 0;
static dragonfly_param         * all_params = NULL;
static const config_anno_map_t * anno_map   = NULL;

/* global variables for codes mapping */
static char lp_group_name[MAX_NAME_LENGTH];
static int mapping_grp_id, mapping_type_id, mapping_rep_id, mapping_offset;

/* router magic number */
int router_magic_num = 0;

/* terminal magic number */
int terminal_magic_num = 0;

FILE * dragonfly_log = NULL;

int sample_bytes_written = 0;
int sample_rtr_bytes_written = 0;

char cn_sample_file[MAX_NAME_LENGTH];
char router_sample_file[MAX_NAME_LENGTH];

typedef struct terminal_message_list terminal_message_list;
struct terminal_message_list {
    terminal_message msg;
    char* event_data;
    terminal_message_list *next;
    terminal_message_list *prev;
};

static void init_terminal_message_list(terminal_message_list *this, 
    terminal_message *inmsg) {
    this->msg = *inmsg;
    this->event_data = NULL;
    this->next = NULL;
    this->prev = NULL;
}

static void delete_terminal_message_list(terminal_message_list *this) {
    if(this->event_data != NULL) free(this->event_data);
    free(this);
}

struct dragonfly_param
{
    // configuration parameters
    int num_routers; /*Number of routers in a group*/
    double local_bandwidth;/* bandwidth of the router-router channels within a group */
    double global_bandwidth;/* bandwidth of the inter-group router connections */
    double cn_bandwidth;/* bandwidth of the compute node channels connected to routers */
    int num_vcs; /* number of virtual channels */
    int local_vc_size; /* buffer size of the router-router channels */
    int global_vc_size; /* buffer size of the global channels */
    int cn_vc_size; /* buffer size of the compute node channels */
    int chunk_size; /* full-sized packets are broken into smaller chunks.*/
    // derived parameters
    int num_cn;
    int num_groups;
    int num_real_groups;
    int radix;
    int total_routers;
    int total_terminals;
    int num_global_channels;
    double cn_delay;
    double local_delay;
    double global_delay;
    double credit_delay;
    double router_delay;
};

struct dfly_hash_key
{
    uint64_t message_id;
    tw_lpid sender_id;
};

struct dfly_router_sample
{
    tw_lpid router_id;
    tw_stime* busy_time;
    int64_t* link_traffic_sample;
    tw_stime end_time;
    long fwd_events;
    long rev_events;
};

struct dfly_cn_sample
{
   tw_lpid terminal_id;
   long fin_chunks_sample;
   long data_size_sample;
   double fin_hops_sample;
   tw_stime fin_chunks_time;
   tw_stime busy_time_sample;
   tw_stime end_time;
   long fwd_events;
   long rev_events;
};

struct dfly_qhash_entry
{
   struct dfly_hash_key key;
   char * remote_event_data;
   uint64_t num_chunks;
   int remote_event_size;
   struct qhash_head hash_link;
};

/* handles terminal and router events like packet generate/send/receive/buffer */
typedef enum event_t event_t;
typedef struct terminal_state terminal_state;
typedef struct router_state router_state;

/* dragonfly compute node data structure */
struct terminal_state
{
   uint64_t packet_counter;

   int packet_gen;
   int packet_fin;

   // Dragonfly specific parameters
   unsigned int router_id;
   unsigned int terminal_id;

   // Each terminal will have an input and output channel with the router
   int* vc_occupancy; // NUM_VC
   int num_vcs;
   tw_stime terminal_available_time;
   terminal_message_list **terminal_msgs;
   terminal_message_list **terminal_msgs_tail;
   int in_send_loop;
// Terminal generate, sends and arrival T_SEND, T_ARRIVAL, T_GENERATE
// Router-Router Intra-group sends and receives RR_LSEND, RR_LARRIVE
// Router-Router Inter-group sends and receives RR_GSEND, RR_GARRIVE
   struct mn_stats dragonfly_stats_array[CATEGORY_MAX];
  /* collective init time */
  tw_stime collective_init_time;

  /* node ID in the tree */ 
   tw_lpid node_id;

   /* messages sent & received in collectives may get interchanged several times so we have to save the 
     origin server information in the node's state */
   tw_lpid origin_svr; 
  
  /* parent node ID of the current node */
   tw_lpid parent_node_id;
   /* array of children to be allocated in terminal_init*/
   tw_lpid* children;

   /* children of a node can be less than or equal to the tree degree */
   int num_children;

   short is_root;
   short is_leaf;

   struct rc_stack * st;
   int issueIdle;
   int terminal_length;

   /* to maintain a count of child nodes that have fanned in at the parent during the collective
      fan-in phase*/
   int num_fan_nodes;

   const char * anno;
   dragonfly_param *params;

   struct qhash_table *rank_tbl;
   uint64_t rank_tbl_pop;

   tw_stime   total_time;
   uint64_t total_msg_size;
   double total_hops;
   long finished_msgs;
   long finished_chunks;
   long finished_packets;

   tw_stime last_buf_full;
   tw_stime busy_time;
   char output_buf[4096];
   /* For LP suspend functionality */
   int error_ct;

   /* For sampling */
   long fin_chunks_sample;
   long data_size_sample;
   double fin_hops_sample;
   tw_stime fin_chunks_time;
   tw_stime busy_time_sample;

   char sample_buf[4096];
   struct dfly_cn_sample * sample_stat;
   int op_arr_size;
   int max_arr_size;
   
   /* for logging forward and reverse events */
   long fwd_events;
   long rev_events;

   /* following used for ROSS model-level stats collection */
   long fin_chunks_ross_sample;
   long data_size_ross_sample;
   long fin_hops_ross_sample;
   tw_stime fin_chunks_time_ross_sample;
   tw_stime busy_time_ross_sample;
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
  R_BUFFER,
  D_COLLECTIVE_INIT,
  D_COLLECTIVE_FAN_IN,
  D_COLLECTIVE_FAN_OUT
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
    ADAPTIVE,
    PROG_ADAPTIVE
};

struct router_state
{
   unsigned int router_id;
   int group_id;
   int op_arr_size;
   int max_arr_size;

   int* global_channel; 
   
   tw_stime* next_output_available_time;
   tw_stime* cur_hist_start_time;
   tw_stime* last_buf_full;

   tw_stime* busy_time;
   tw_stime* busy_time_sample;

   terminal_message_list ***pending_msgs;
   terminal_message_list ***pending_msgs_tail;
   terminal_message_list ***queued_msgs;
   terminal_message_list ***queued_msgs_tail;
   int *in_send_loop;
   int *queued_count;
   struct rc_stack * st;
   
   int** vc_occupancy;
   int64_t* link_traffic;
   int64_t * link_traffic_sample;

   const char * anno;
   dragonfly_param *params;

   int* prev_hist_num;
   int* cur_hist_num;
   
   char output_buf[4096];
   char output_buf2[4096];

   struct dfly_router_sample * rsamples;
   
   long fwd_events;
   long rev_events;

   /* following used for ROSS model-level stats collection */
   tw_stime* busy_time_ross_sample;
   int64_t * link_traffic_ross_sample;
};

/* had to pull some of the ROSS model stats collection stuff up here */
void dragonfly_event_collect(terminal_message *m, tw_lp *lp, char *buffer, int *collect_flag);
void dragonfly_model_stat_collect(terminal_state *s, tw_lp *lp, char *buffer);
void dfly_router_model_stat_collect(router_state *s, tw_lp *lp, char *buffer);

st_model_types dragonfly_model_types[] = {
    {(rbev_trace_f) dragonfly_event_collect,
     sizeof(int),
     (ev_trace_f) dragonfly_event_collect,
     sizeof(int),
     (model_stat_f) dragonfly_model_stat_collect,
     sizeof(tw_lpid) + sizeof(long) * 2 + sizeof(double) + sizeof(tw_stime) *2}, 
    {(rbev_trace_f) dragonfly_event_collect,
     sizeof(int),
     (ev_trace_f) dragonfly_event_collect,
     sizeof(int),
     (model_stat_f) dfly_router_model_stat_collect,
     0}, //updated in router_setup() since it's based on the radix
    {NULL, 0, NULL, 0, NULL, 0}
};
/* End of ROSS model stats collection */

static short routing = MINIMAL;

static tw_stime         dragonfly_total_time = 0;
static tw_stime         dragonfly_max_latency = 0;
static tw_stime         max_collective = 0;


static long long       total_hops = 0;
static long long       N_finished_packets = 0;
static long long       total_msg_sz = 0;
static long long       N_finished_msgs = 0;
static long long       N_finished_chunks = 0;

static int dragonfly_rank_hash_compare(
        void *key, struct qhash_head *link)
{
    struct dfly_hash_key *message_key = (struct dfly_hash_key *)key;
    struct dfly_qhash_entry *tmp = NULL;

    tmp = qhash_entry(link, struct dfly_qhash_entry, hash_link);
    
    if (tmp->key.message_id == message_key->message_id
            && tmp->key.sender_id == message_key->sender_id)
        return 1;

    return 0;
}
static int dragonfly_hash_func(void *k, int table_size)
{
    struct dfly_hash_key *tmp = (struct dfly_hash_key *)k;
    //uint32_t pc = 0, pb = 0;	
    //bj_hashlittle2(tmp, sizeof(*tmp), &pc, &pb);
    uint64_t key = (~tmp->message_id) + (tmp->message_id << 18);
    key = key * 21;
    key = ~key ^ (tmp->sender_id >> 4);
    key = key * tmp->sender_id; 
    return (int)(key & (table_size - 1));
    //return (int)(pc % (table_size - 1));
}

/* convert GiB/s and bytes to ns */
static tw_stime bytes_to_ns(uint64_t bytes, double GB_p_s)
{
    tw_stime time;

    /* bytes to GB */
    time = ((double)bytes)/(1024.0*1024.0*1024.0);
    /* GiB to s */
    time = time / GB_p_s;
    /* s to ns */
    time = time * 1000.0 * 1000.0 * 1000.0;

    return(time);
}

/* returns the dragonfly message size */
static int dragonfly_get_msg_sz(void)
{
	   return sizeof(terminal_message);
}

static void free_tmp(void * ptr)
{
    struct dfly_qhash_entry * dfly = ptr; 
    
    if(dfly->remote_event_data)
        free(dfly->remote_event_data);
   
    if(dfly)
        free(dfly);
}
static void append_to_terminal_message_list(  
        terminal_message_list ** thisq,
        terminal_message_list ** thistail,
        int index, 
        terminal_message_list *msg) {
    if(thisq[index] == NULL) {
        thisq[index] = msg;
    } else {
        thistail[index]->next = msg;
        msg->prev = thistail[index];
    } 
    thistail[index] = msg;
}

static void prepend_to_terminal_message_list(  
        terminal_message_list ** thisq,
        terminal_message_list ** thistail,
        int index, 
        terminal_message_list *msg) {
    if(thisq[index] == NULL) {
        thistail[index] = msg;
    } else {
        thisq[index]->prev = msg;
        msg->next = thisq[index];
    } 
    thisq[index] = msg;
}

static terminal_message_list* return_head(
        terminal_message_list ** thisq,
        terminal_message_list ** thistail,
        int index) {
    terminal_message_list *head = thisq[index];
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

static terminal_message_list* return_tail(
        terminal_message_list ** thisq,
        terminal_message_list ** thistail,
        int index) {
    terminal_message_list *tail = thistail[index];
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

static void dragonfly_read_config(const char * anno, dragonfly_param *params){
    // shorthand
    dragonfly_param *p = params;

    int rc = configuration_get_value_int(&config, "PARAMS", "num_routers", anno,
            &p->num_routers);
    if(rc) {
        p->num_routers = 4;
        fprintf(stderr, "Number of dimensions not specified, setting to %d\n",
                p->num_routers);
    }

    p->num_vcs = 3;

    rc = configuration_get_value_int(&config, "PARAMS", "local_vc_size", anno, &p->local_vc_size);
    if(rc) {
        p->local_vc_size = 1024;
        fprintf(stderr, "Buffer size of local channels not specified, setting to %d\n", p->local_vc_size);
    }

    rc = configuration_get_value_int(&config, "PARAMS", "global_vc_size", anno, &p->global_vc_size);
    if(rc) {
        p->global_vc_size = 2048;
        fprintf(stderr, "Buffer size of global channels not specified, setting to %d\n", p->global_vc_size);
    }

    rc = configuration_get_value_int(&config, "PARAMS", "cn_vc_size", anno, &p->cn_vc_size);
    if(rc) {
        p->cn_vc_size = 1024;
        fprintf(stderr, "Buffer size of compute node channels not specified, setting to %d\n", p->cn_vc_size);
    }

    rc = configuration_get_value_int(&config, "PARAMS", "chunk_size", anno, &p->chunk_size);
    if(rc) {
        p->chunk_size = 512;
        fprintf(stderr, "Chunk size for packets is specified, setting to %d\n", p->chunk_size);
    }

    rc = configuration_get_value_double(&config, "PARAMS", "local_bandwidth", anno, &p->local_bandwidth);
    if(rc) {
        p->local_bandwidth = 5.25;
        fprintf(stderr, "Bandwidth of local channels not specified, setting to %lf\n", p->local_bandwidth);
    }

    rc = configuration_get_value_double(&config, "PARAMS", "global_bandwidth", anno, &p->global_bandwidth);
    if(rc) {
        p->global_bandwidth = 4.7;
        fprintf(stderr, "Bandwidth of global channels not specified, setting to %lf\n", p->global_bandwidth);
    }

    rc = configuration_get_value_double(&config, "PARAMS", "cn_bandwidth", anno, &p->cn_bandwidth);
    if(rc) {
        p->cn_bandwidth = 5.25;
        fprintf(stderr, "Bandwidth of compute node channels not specified, setting to %lf\n", p->cn_bandwidth);
    }

    p->router_delay = 50;
    configuration_get_value_double(&config, "PARAMS", "router_delay", anno,
            &p->router_delay);

    configuration_get_value(&config, "PARAMS", "cn_sample_file", anno, cn_sample_file,
            MAX_NAME_LENGTH);
    configuration_get_value(&config, "PARAMS", "rt_sample_file", anno, router_sample_file,
            MAX_NAME_LENGTH);
    
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
    else if (strcmp(routing_str, "prog-adaptive") == 0)
	routing = PROG_ADAPTIVE;
    else
    {
        fprintf(stderr, 
                "No routing protocol specified, setting to minimal routing\n");
        routing = -1;
    }

    // set the derived parameters
    p->num_cn = p->num_routers/2;
    p->num_global_channels = p->num_routers/2;
    p->num_groups = p->num_routers * p->num_cn + 1;
    p->radix = (p->num_routers + p->num_global_channels + p->num_cn);
    p->total_routers = p->num_groups * p->num_routers;
    p->total_terminals = p->total_routers * p->num_cn;
    int rank;
    MPI_Comm_rank(MPI_COMM_CODES, &rank);
    if(!rank) {
        printf("\n Total nodes %d routers %d groups %d radix %d \n",
                p->num_cn * p->total_routers, p->total_routers, p->num_groups,
                p->radix);
    }
    
    p->cn_delay = bytes_to_ns(p->chunk_size, p->cn_bandwidth);
    p->local_delay = bytes_to_ns(p->chunk_size, p->local_bandwidth);
    p->global_delay = bytes_to_ns(p->chunk_size, p->global_bandwidth);
    p->credit_delay = bytes_to_ns(CREDIT_SZ, p->local_bandwidth); //assume 8 bytes packet
}

static void dragonfly_configure(){
    anno_map = codes_mapping_get_lp_anno_map(LP_CONFIG_NM_TERM);
    assert(anno_map);
    num_params = anno_map->num_annos + (anno_map->has_unanno_lp > 0);
    all_params = malloc(num_params * sizeof(*all_params));

    for (int i = 0; i < anno_map->num_annos; i++){
        const char * anno = anno_map->annotations[i].ptr;
        dragonfly_read_config(anno, &all_params[i]);
    }
    if (anno_map->has_unanno_lp > 0){
        dragonfly_read_config(NULL, &all_params[anno_map->num_annos]);
    }
#ifdef ENABLE_CORTEX
	model_net_topology = dragonfly_cortex_topology;
#endif
}

/* report dragonfly statistics like average and maximum packet latency, average number of hops traversed */
static void dragonfly_report_stats()
{
   long long avg_hops, total_finished_packets, total_finished_chunks;
   long long total_finished_msgs, final_msg_sz;
   tw_stime avg_time, max_time;
   int total_minimal_packets, total_nonmin_packets;
   long total_gen, total_fin;

   MPI_Reduce( &total_hops, &avg_hops, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_CODES);
   MPI_Reduce( &N_finished_packets, &total_finished_packets, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_CODES);
   MPI_Reduce( &N_finished_msgs, &total_finished_msgs, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_CODES);
   MPI_Reduce( &N_finished_chunks, &total_finished_chunks, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_CODES);
   MPI_Reduce( &total_msg_sz, &final_msg_sz, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_CODES);
   MPI_Reduce( &dragonfly_total_time, &avg_time, 1,MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_CODES);
   MPI_Reduce( &dragonfly_max_latency, &max_time, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_CODES);
   
   MPI_Reduce( &packet_gen, &total_gen, 1, MPI_LONG, MPI_SUM, 0, MPI_COMM_CODES);
   MPI_Reduce( &packet_fin, &total_fin, 1, MPI_LONG, MPI_SUM, 0, MPI_COMM_CODES);
   if(routing == ADAPTIVE || routing == PROG_ADAPTIVE)
    {
	MPI_Reduce(&minimal_count, &total_minimal_packets, 1, MPI_INT, MPI_SUM, 0, MPI_COMM_CODES);
 	MPI_Reduce(&nonmin_count, &total_nonmin_packets, 1, MPI_INT, MPI_SUM, 0, MPI_COMM_CODES);
    }

   /* print statistics */
   if(!g_tw_mynode)
   {	
      printf(" Average number of hops traversed %f average chunk latency %lf us maximum chunk latency %lf us avg message size %lf bytes finished messages %lld finished chunks %lld \n", 
              (float)avg_hops/total_finished_chunks, avg_time/(total_finished_chunks*1000), max_time/1000, (float)final_msg_sz/total_finished_msgs, total_finished_msgs, total_finished_chunks);
     if(routing == ADAPTIVE || routing == PROG_ADAPTIVE)
              printf("\n ADAPTIVE ROUTING STATS: %d chunks routed minimally %d chunks routed non-minimally completed packets %lld \n", 
                      total_minimal_packets, total_nonmin_packets, total_finished_chunks);
 
      printf("\n Total packets generated %ld finished %ld \n", total_gen, total_fin);
   }
   return;
}

static void dragonfly_collective_init(terminal_state * s,
           		   tw_lp * lp)
{
    // TODO: be annotation-aware
    codes_mapping_get_lp_info(lp->gid, lp_group_name, &mapping_grp_id, NULL,
            &mapping_type_id, NULL, &mapping_rep_id, &mapping_offset);
    int num_lps = codes_mapping_get_lp_count(lp_group_name, 1, LP_CONFIG_NM_TERM,
            NULL, 1);
    int num_reps = codes_mapping_get_group_reps(lp_group_name);
    s->node_id = (mapping_rep_id * num_lps) + mapping_offset;

    int i;
   /* handle collective operations by forming a tree of all the LPs */
   /* special condition for root of the tree */
   if( s->node_id == 0)
    {
        s->parent_node_id = -1;
        s->is_root = 1;
   }
   else
   {
       s->parent_node_id = (s->node_id - ((s->node_id - 1) % TREE_DEGREE)) / TREE_DEGREE;
       s->is_root = 0;
   }
   s->children = (tw_lpid*)malloc(TREE_DEGREE * sizeof(tw_lpid));

   /* set the isleaf to zero by default */
   s->is_leaf = 1;
   s->num_children = 0;

   /* calculate the children of the current node. If its a leaf, no need to set children,
      only set isleaf and break the loop*/

   for( i = 0; i < TREE_DEGREE; i++ )
    {
        tw_lpid next_child = (TREE_DEGREE * s->node_id) + i + 1;
        if(next_child < ((tw_lpid)num_lps * (tw_lpid)num_reps))
        {
            s->num_children++;
            s->is_leaf = 0;
            s->children[i] = next_child;
        }
        else
           s->children[i] = -1;
    }

#if DRAGONFLY_COLLECTIVE_DEBUG == 1
   printf("\n LP %ld parent node id ", s->node_id);

   for( i = 0; i < TREE_DEGREE; i++ )
        printf(" child node ID %ld ", s->children[i]);
   printf("\n");

   if(s->is_leaf)
        printf("\n LP %ld is leaf ", s->node_id);
#endif
}

/* initialize a dragonfly compute node terminal */
static void 
terminal_init( terminal_state * s, 
	       tw_lp * lp )
{
    s->packet_gen = 0;
    s->packet_fin = 0;

    uint32_t h1 = 0, h2 = 0; 
    bj_hashlittle2(LP_METHOD_NM_TERM, strlen(LP_METHOD_NM_TERM), &h1, &h2);
    terminal_magic_num = h1 + h2;
    
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

   //int num_lps = codes_mapping_get_lp_count(lp_group_name, 1, LP_CONFIG_NM_TERM,
   //        s->anno, 0);

   s->terminal_id = codes_mapping_get_lp_relative_id(lp->gid, 0, 0);  
   
   s->router_id=(int)s->terminal_id / s->params->num_cn;
   s->terminal_available_time = 0.0;
   s->packet_counter = 0;
   
   s->finished_msgs = 0;
   s->finished_chunks = 0;
   s->finished_packets = 0;
   s->total_time = 0.0;
   s->total_msg_size = 0;

   s->last_buf_full = 0.0;
   s->busy_time = 0.0;

   s->fwd_events = 0;
   s->rev_events = 0;

   rc_stack_create(&s->st);
   s->num_vcs = 1;
   s->vc_occupancy = (int*)malloc(s->num_vcs * sizeof(int));

   for( i = 0; i < s->num_vcs; i++ )
    {
      s->vc_occupancy[i]=0;
    }


   s->rank_tbl = NULL;
   s->terminal_msgs = 
       (terminal_message_list**)malloc(1*sizeof(terminal_message_list*));
   s->terminal_msgs_tail = 
       (terminal_message_list**)malloc(1*sizeof(terminal_message_list*));
   s->terminal_msgs[0] = NULL;
   s->terminal_msgs_tail[0] = NULL;
   s->terminal_length = 0;
   s->in_send_loop = 0;
   s->issueIdle = 0;

   /* set up for ROSS stats sampling */
   s->fin_chunks_ross_sample = 0;
   s->data_size_ross_sample = 0;
   s->fin_hops_ross_sample = 0;
   s->fin_chunks_time_ross_sample = 0.0;
   s->busy_time_ross_sample = 0.0;

   dragonfly_collective_init(s, lp);
   return;
}

/* sets up the router virtual channels, global channels, 
 * local channels, compute node channels */
static void router_setup(router_state * r, tw_lp * lp)
{
    uint32_t h1 = 0, h2 = 0; 
    bj_hashlittle2(LP_METHOD_NM_ROUT, strlen(LP_METHOD_NM_ROUT), &h1, &h2);
    router_magic_num = h1 + h2;
    
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

    dragonfly_param *p = r->params;
    p->num_real_groups = codes_mapping_get_lp_count(lp_group_name, 0, LP_CONFIG_NM_ROUT, NULL, 1);
    assert(p->num_real_groups > 0);
    if(p->num_real_groups % p->num_routers)
    {
        tw_error(TW_LOC, "\n Config error: num_routers specified %d "
                "does not divide num_router per group %d  ",
                p->num_real_groups , p->num_routers);
    }
    p->num_real_groups = p->num_real_groups/p->num_routers;
    
    num_routers_per_mgrp = codes_mapping_get_lp_count (lp_group_name, 1, LP_METHOD_NM_ROUT,
            NULL, 0);
    /*int num_grp_reps = codes_mapping_get_group_reps(lp_group_name);
    if(p->total_routers != num_grp_reps * num_routers_per_mgrp)
        tw_error(TW_LOC, "\n Config error: num_routers specified %d total routers computed in the network %d "
                "does not match with repetitions * dragonfly_router %d  ",
                p->num_routers, p->total_routers, num_grp_reps * num_routers_per_mgrp);
    */
   r->router_id=mapping_rep_id + mapping_offset;
   r->group_id=r->router_id/p->num_routers;

   r->fwd_events = 0;
   r->rev_events = 0;

   r->global_channel = (int*)malloc(p->num_global_channels * sizeof(int));
   r->next_output_available_time = (tw_stime*)malloc(p->radix * sizeof(tw_stime));
   r->cur_hist_start_time = (tw_stime*)malloc(p->radix * sizeof(tw_stime));
   r->link_traffic = (int64_t*)malloc(p->radix * sizeof(int64_t));
   r->link_traffic_sample = (int64_t*)malloc(p->radix * sizeof(int64_t));
   r->cur_hist_num = (int*)malloc(p->radix * sizeof(int));
   r->prev_hist_num = (int*)malloc(p->radix * sizeof(int));
   
   r->vc_occupancy = (int**)malloc(p->radix * sizeof(int*));
   r->in_send_loop = (int*)malloc(p->radix * sizeof(int));
   r->pending_msgs = 
    (terminal_message_list***)malloc(p->radix * sizeof(terminal_message_list**));
   r->pending_msgs_tail = 
    (terminal_message_list***)malloc(p->radix * sizeof(terminal_message_list**));
   r->queued_msgs = 
    (terminal_message_list***)malloc(p->radix * sizeof(terminal_message_list**));
   r->queued_msgs_tail = 
    (terminal_message_list***)malloc(p->radix * sizeof(terminal_message_list**));
   r->queued_count = (int*)malloc(p->radix * sizeof(int));
   r->last_buf_full = (tw_stime*)malloc(p->radix * sizeof(tw_stime));
   r->busy_time = (tw_stime*)malloc(p->radix * sizeof(tw_stime));
   r->busy_time_sample = (tw_stime*)malloc(p->radix * sizeof(tw_stime));

   /* set up for ROSS stats sampling */
   r->link_traffic_ross_sample = (int64_t*)calloc(p->radix, sizeof(int64_t));
   r->busy_time_ross_sample = (tw_stime*)calloc(p->radix, sizeof(tw_stime));
   if (g_st_model_stats)
       lp->model_types->mstat_sz = sizeof(tw_lpid) + (sizeof(int64_t) + sizeof(tw_stime)) * p->radix;

   rc_stack_create(&r->st);
   for(int i=0; i < p->radix; i++)
    {
       // Set credit & router occupancy
    r->last_buf_full[i] = 0.0;
    r->busy_time[i] = 0.0;
    r->busy_time_sample[i] = 0.0;
	r->next_output_available_time[i]=0;
	r->cur_hist_start_time[i] = 0;
    r->link_traffic[i]=0;
    r->link_traffic_sample[i] = 0;
	r->cur_hist_num[i] = 0;
	r->prev_hist_num[i] = 0;
    r->queued_count[i] = 0;    
    r->in_send_loop[i] = 0;
    r->vc_occupancy[i] = (int*)malloc(p->num_vcs * sizeof(int));
    r->pending_msgs[i] = (terminal_message_list**)malloc(p->num_vcs * 
        sizeof(terminal_message_list*));
    r->pending_msgs_tail[i] = (terminal_message_list**)malloc(p->num_vcs * 
        sizeof(terminal_message_list*));
    r->queued_msgs[i] = (terminal_message_list**)malloc(p->num_vcs * 
        sizeof(terminal_message_list*));
    r->queued_msgs_tail[i] = (terminal_message_list**)malloc(p->num_vcs * 
        sizeof(terminal_message_list*));
        for(int j = 0; j < p->num_vcs; j++) {
            r->vc_occupancy[i][j] = 0;
            r->pending_msgs[i][j] = NULL;
            r->pending_msgs_tail[i][j] = NULL;
            r->queued_msgs[i][j] = NULL;
            r->queued_msgs_tail[i][j] = NULL;
        }
    }

#if DEBUG == 1
//   printf("\n LP ID %d VC occupancy radix %d Router %d is connected to ", lp->gid, p->radix, r->router_id);
#endif 
   //round the number of global channels to the nearest even number
#if USE_DIRECT_SCHEME
       int first = r->router_id % p->num_routers;
       for(int i=0; i < p->num_global_channels; i++)
        {
            int target_grp = first;
            if(target_grp == r->group_id) {
                target_grp = p->num_groups - 1;
            }
            int my_pos = r->group_id % p->num_routers;
            if(r->group_id == p->num_groups - 1) {
                my_pos = target_grp % p->num_routers;
            }
            r->global_channel[i] = target_grp * p->num_routers + my_pos;
            first += p->num_routers;
        }
#else
   int router_offset = (r->router_id % p->num_routers) * 
    (p->num_global_channels / 2) + 1;
   for(int i=0; i < p->num_global_channels; i++)
    {
      if(i % 2 != 0)
          {
             r->global_channel[i]=(r->router_id + (router_offset * p->num_routers))%p->total_routers;
             router_offset++;
          }
          else
           {
             r->global_channel[i]=r->router_id - ((router_offset) * p->num_routers);
           }
        if(r->global_channel[i]<0)
         {
           r->global_channel[i]=p->total_routers+r->global_channel[i]; 
	 }
#if DEBUG == 1
    printf("\n channel %d ", r->global_channel[i]);
#endif 
    }
#endif

#if DEBUG == 1
   printf("\n");
#endif
   return;
}	


/* dragonfly packet event , generates a dragonfly packet on the compute node */
static tw_stime dragonfly_packet_event(
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
    terminal_message * msg;
    char* tmp_ptr;

    xfer_to_nic_time = codes_local_latency(sender); 
    //e_new = tw_event_new(sender->gid, xfer_to_nic_time+offset, sender);
    //msg = tw_event_data(e_new);
    e_new = model_net_method_event_new(sender->gid, xfer_to_nic_time+offset,
            sender, DRAGONFLY, (void**)&msg, (void**)&tmp_ptr);
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
    msg->magic = terminal_magic_num; 
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
	   //printf("\n dragonfly remote event %d local event %d last packet %d %lf ", msg->remote_event_size_bytes, msg->local_event_size_bytes, is_last_pckt, xfer_to_nic_time);
    tw_event_send(e_new);
    return xfer_to_nic_time;
}

/* dragonfly packet event reverse handler */
static void dragonfly_packet_event_rc(tw_lp *sender)
{
	  codes_local_latency_reverse(sender);
	    return;
}

/* given two group IDs, find the router of the src_gid that connects to the dest_gid*/
tw_lpid getRouterFromGroupID(int dest_gid, 
		    int src_gid,
		    int num_routers,
            int total_groups)
{
#if USE_DIRECT_SCHEME
  int dest = dest_gid;
  if(dest == total_groups - 1) {
      dest = src_gid;
  }
  return src_gid * num_routers + (dest % num_routers);
#else
  int group_begin = src_gid * num_routers;
  int group_end = (src_gid * num_routers) + num_routers-1;
  int offset = (dest_gid * num_routers - group_begin) / num_routers;
  
  if((dest_gid * num_routers) < group_begin)
    offset = (group_begin - dest_gid * num_routers) / num_routers; // take absolute value
  
  int half_channel = num_routers / 4;
  int index = (offset - 1)/(half_channel * num_routers);
  
  offset=(offset - 1) % (half_channel * num_routers);

  // If the destination router is in the same group
  tw_lpid router_id;

  if(index % 2 != 0)
    router_id = group_end - (offset / half_channel); // start from the end
  else
    router_id = group_begin + (offset / half_channel);

  return router_id;
#endif
}	

/*When a packet is sent from the current router and a buffer slot becomes available, a credit is sent back to schedule another packet event*/
static void router_credit_send(router_state * s, terminal_message * msg, 
  tw_lp * lp, int sq) {
  tw_event * buf_e;
  tw_stime ts;
  terminal_message * buf_msg;

  int dest = 0,  type = R_BUFFER;
  int is_terminal = 0;

  const dragonfly_param *p = s->params;
 
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

  ts = g_tw_lookahead + p->credit_delay +  tw_rand_unif(lp->rng);
	
  if (is_terminal) {
    buf_e = model_net_method_event_new(dest, ts, lp, DRAGONFLY, 
      (void**)&buf_msg, NULL);
    buf_msg->magic = terminal_magic_num;
  } else {
    buf_e = model_net_method_event_new(dest, ts, lp, DRAGONFLY_ROUTER,
            (void**)&buf_msg, NULL);
    buf_msg->magic = router_magic_num;
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

static void packet_generate_rc(terminal_state * s, tw_bf * bf, terminal_message * msg, tw_lp * lp)
{
   s->packet_gen--;
   packet_gen--;
   
   tw_rand_reverse_unif(lp->rng);

   int num_chunks = msg->packet_size/s->params->chunk_size;
   if(msg->packet_size % s->params->chunk_size)
       num_chunks++;

   if(!num_chunks)
       num_chunks = 1;

   int i;
   for(i = 0; i < num_chunks; i++) {
        delete_terminal_message_list(return_tail(s->terminal_msgs, 
          s->terminal_msgs_tail, 0));
        s->terminal_length -= s->params->chunk_size;
   }
    if(bf->c5) {
        codes_local_latency_reverse(lp);
        s->in_send_loop = 0;
    }
      if(bf->c11) {
        s->issueIdle = 0;
        s->last_buf_full = msg->saved_busy_time;
      }
     struct mn_stats* stat;
     stat = model_net_find_stats(msg->category, s->dragonfly_stats_array);
     stat->send_count--;
     stat->send_bytes -= msg->packet_size;
     stat->send_time -= (1/s->params->cn_bandwidth) * msg->packet_size;
}

/* generates packet at the current dragonfly compute node */
static void packet_generate(terminal_state * s, tw_bf * bf, terminal_message * msg, 
  tw_lp * lp) {
  packet_gen++;
  s->packet_gen++;

  tw_stime ts, nic_ts;

  assert(lp->gid != msg->dest_terminal_id);
  const dragonfly_param *p = s->params;

  int total_event_size;
  uint64_t num_chunks = msg->packet_size / p->chunk_size;
  if (msg->packet_size % s->params->chunk_size) 
      num_chunks++;

  if(!num_chunks)
    num_chunks = 1;

  nic_ts = g_tw_lookahead + (num_chunks * s->params->cn_delay) + tw_rand_unif(lp->rng);
  
  msg->packet_ID = lp->gid + g_tw_nlp * s->packet_counter;
  msg->my_N_hop = 0;
  msg->my_l_hop = 0;
  msg->my_g_hop = 0;
  msg->intm_group_id = -1;

  //if(msg->dest_terminal_id == TRACK)
  if(msg->packet_ID == LLU(TRACK_PKT))
    printf("\n Packet %llu generated at terminal %d dest %llu size %llu num chunks %llu ", 
            msg->packet_ID, s->terminal_id, LLU(msg->dest_terminal_id),
            LLU(msg->packet_size), LLU(num_chunks));

  for(uint64_t i = 0; i < num_chunks; i++)
  {
    terminal_message_list *cur_chunk = (terminal_message_list*)malloc(
      sizeof(terminal_message_list));
    msg->origin_router_id = s->router_id;
    init_terminal_message_list(cur_chunk, msg);
  

    if(msg->remote_event_size_bytes + msg->local_event_size_bytes > 0) {
      cur_chunk->event_data = (char*)malloc(
          msg->remote_event_size_bytes + msg->local_event_size_bytes);
    }
    
    void * m_data_src = model_net_method_get_edata(DRAGONFLY, msg);
    if (msg->remote_event_size_bytes){
      memcpy(cur_chunk->event_data, m_data_src, msg->remote_event_size_bytes);
    }
    if (msg->local_event_size_bytes){ 
      m_data_src = (char*)m_data_src + msg->remote_event_size_bytes;
      memcpy((char*)cur_chunk->event_data + msg->remote_event_size_bytes, 
          m_data_src, msg->local_event_size_bytes);
    }

    cur_chunk->msg.chunk_id = i;
    cur_chunk->msg.origin_router_id = s->router_id;
    append_to_terminal_message_list(s->terminal_msgs, s->terminal_msgs_tail,
      0, cur_chunk);
    s->terminal_length += s->params->chunk_size;
  }

  if(s->terminal_length < 2 * s->params->cn_vc_size) {
    model_net_method_idle_event(nic_ts, 0, lp);
  } else {
    bf->c11 = 1;
    s->issueIdle = 1;
    msg->saved_busy_time = s->last_buf_full;
    s->last_buf_full = tw_now(lp);
  }
  
  if(s->in_send_loop == 0) {
    bf->c5 = 1;
    ts = codes_local_latency(lp);
    terminal_message *m;
    tw_event* e = model_net_method_event_new(lp->gid, ts, lp, DRAGONFLY, 
      (void**)&m, NULL);
    m->type = T_SEND;
    m->magic = terminal_magic_num;
    s->in_send_loop = 1;
    tw_event_send(e);
  }

  total_event_size = model_net_get_msg_sz(DRAGONFLY) + 
      msg->remote_event_size_bytes + msg->local_event_size_bytes;
  mn_stats* stat;
  stat = model_net_find_stats(msg->category, s->dragonfly_stats_array);
  stat->send_count++;
  stat->send_bytes += msg->packet_size;
  stat->send_time += (1/p->cn_bandwidth) * msg->packet_size;
  if(stat->max_event_size < total_event_size)
	  stat->max_event_size = total_event_size;

  return;
}

static void packet_send_rc(terminal_state * s, tw_bf * bf, terminal_message * msg,
        tw_lp * lp)
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

      terminal_message_list* cur_entry = rc_stack_pop(s->st);

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
            s->busy_time_sample = msg->saved_sample_time;
            s->busy_time_ross_sample = msg->saved_busy_time_ross;
          }
      }
      return;
}
/* sends the packet from the current dragonfly compute node to the attached router */
static void packet_send(terminal_state * s, tw_bf * bf, terminal_message * msg, 
  tw_lp * lp) {
  
  tw_stime ts;
  tw_event *e;
  terminal_message *m;
  tw_lpid router_id;

  terminal_message_list* cur_entry = s->terminal_msgs[0];

  if(s->vc_occupancy[0] + s->params->chunk_size > s->params->cn_vc_size 
      || cur_entry == NULL) {
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
  codes_mapping_get_lp_id(lp_group_name, LP_CONFIG_NM_ROUT, NULL, 1,
      s->router_id, 0, &router_id);
  // we are sending an event to the router, so no method_event here
  void * remote_event;
  e = model_net_method_event_new(router_id, ts, lp,
          DRAGONFLY_ROUTER, (void**)&m, &remote_event);
  memcpy(m, &cur_entry->msg, sizeof(terminal_message));
  if (m->remote_event_size_bytes){
    memcpy(remote_event, cur_entry->event_data, m->remote_event_size_bytes);
  }

  m->type = R_ARRIVE;
  m->src_terminal_id = lp->gid;
  m->vc_index = 0;
  m->last_hop = TERMINAL;
  m->intm_group_id = -1;
  m->magic = router_magic_num;
  m->path_type = -1;
  m->local_event_size_bytes = 0;
  tw_event_send(e);


  if(cur_entry->msg.chunk_id == num_chunks - 1 && 
      (cur_entry->msg.local_event_size_bytes > 0)) {
    bf->c2 = 1;
    tw_stime local_ts = codes_local_latency(lp);
    tw_event *e_new = tw_event_new(cur_entry->msg.sender_lp, local_ts, lp);
    void * m_new = tw_event_data(e_new);
    void *local_event = (char*)cur_entry->event_data + 
      cur_entry->msg.remote_event_size_bytes;
    memcpy(m_new, local_event, cur_entry->msg.local_event_size_bytes);
    tw_event_send(e_new);
  }
  s->packet_counter++;
  s->vc_occupancy[0] += s->params->chunk_size;
  cur_entry = return_head(s->terminal_msgs, s->terminal_msgs_tail, 0); 
  rc_stack_push(lp, cur_entry, (void*)delete_terminal_message_list, s->st);
  s->terminal_length -= s->params->chunk_size;

  cur_entry = s->terminal_msgs[0];

  /* if there is another packet inline then schedule another send event */
  if(cur_entry != NULL &&
    s->vc_occupancy[0] + s->params->chunk_size <= s->params->cn_vc_size) {
    bf->c3 = 1;
    terminal_message *m_new;
    ts += tw_rand_unif(lp->rng);
    e = model_net_method_event_new(lp->gid, ts, lp, DRAGONFLY, 
      (void**)&m_new, NULL);
    m_new->type = T_SEND;
    m_new->magic = terminal_magic_num;
    tw_event_send(e);
  } else {
      /* If not then the LP will wait for another credit or packet generation */
    bf->c4 = 1;
    s->in_send_loop = 0;
  }
  if(s->issueIdle) {
    bf->c5 = 1;
    s->issueIdle = 0;
    ts += tw_rand_unif(lp->rng);
    model_net_method_idle_event(ts, 0, lp);
   
    if(s->last_buf_full > 0.0)
    {
        bf->c6 = 1;
        msg->saved_total_time = s->busy_time;
        msg->saved_busy_time = s->last_buf_full;
        msg->saved_sample_time = s->busy_time_sample;
        msg->saved_busy_time_ross = s->busy_time_ross_sample;

        s->busy_time += (tw_now(lp) - s->last_buf_full);
        s->busy_time_sample += (tw_now(lp) - s->last_buf_full);
        s->busy_time_ross_sample += (tw_now(lp) - s->last_buf_full);
        s->last_buf_full = 0.0;
    }
  }
  return;
}

static void packet_arrive_rc(terminal_state * s, tw_bf * bf, terminal_message * msg, tw_lp * lp)
{
    if(bf->c31)
    {
      s->packet_fin--;
      packet_fin--;
    }
      tw_rand_reverse_unif(lp->rng);
      if(msg->path_type == MINIMAL)
        minimal_count--;
      if(msg->path_type == NON_MINIMAL)
        nonmin_count--;

      N_finished_chunks--;
      s->finished_chunks--;
      s->fin_chunks_sample--;
      s->fin_chunks_ross_sample--;

      total_hops -= msg->my_N_hop;
       s->total_hops -= msg->my_N_hop;
       s->fin_hops_sample -= msg->my_N_hop;
       s->fin_hops_ross_sample -= msg->my_N_hop;
       dragonfly_total_time  = msg->saved_total_time;
       s->fin_chunks_time = msg->saved_sample_time;
       s->fin_chunks_time_ross_sample = msg->saved_fin_chunks_ross;
       s->total_time = msg->saved_avg_time;
      
      struct qhash_head * hash_link = NULL;
      struct dfly_qhash_entry * tmp = NULL; 
      
      struct dfly_hash_key key;
      key.message_id = msg->message_id;
      key.sender_id = msg->sender_lp;
      
      hash_link = qhash_search(s->rank_tbl, &key);
      tmp = qhash_entry(hash_link, struct dfly_qhash_entry, hash_link);
      
      mn_stats* stat;
      stat = model_net_find_stats(msg->category, s->dragonfly_stats_array);
      stat->recv_time = msg->saved_rcv_time;

      if(bf->c1)
      {
        stat->recv_count--;
        stat->recv_bytes -= msg->packet_size;
        N_finished_packets--;
        s->finished_packets--;
      }
       if(bf->c3)
          dragonfly_max_latency = msg->saved_available_time;
       
       if(bf->c7)
        {
            //assert(!hash_link);
            if(bf->c8) 
              tw_rand_reverse_unif(lp->rng);
            N_finished_msgs--;
            s->finished_msgs--;
            total_msg_sz -= msg->total_size;
            s->total_msg_size -= msg->total_size;
            s->data_size_sample -= msg->total_size;
            s->data_size_ross_sample -= msg->total_size;

	        struct dfly_qhash_entry * d_entry_pop = rc_stack_pop(s->st);
            qhash_add(s->rank_tbl, &key, &(d_entry_pop->hash_link));
            s->rank_tbl_pop++; 

            if(s->rank_tbl_pop >= DFLY_HASH_TABLE_SIZE)
                tw_error(TW_LOC, "\n Exceeded allocated qhash size, increase hash size in dragonfly model");

            hash_link = &(d_entry_pop->hash_link);
            tmp = d_entry_pop; 

            if(bf->c4)
                model_net_event_rc2(lp, &msg->event_rc);
        }
      
       assert(tmp);
       tmp->num_chunks--;

   if(bf->c5)
	{
	   qhash_del(hash_link);
	   free_tmp(tmp);	
       s->rank_tbl_pop--;
	}
       return;
}
static void send_remote_event(terminal_state * s, terminal_message * msg, tw_lp * lp, tw_bf * bf, char * event_data, int remote_event_size)
{
        (void)s;
        void * tmp_ptr = model_net_method_get_edata(DRAGONFLY, msg);
        //tw_stime ts = g_tw_lookahead + bytes_to_ns(msg->remote_event_size_bytes, (1/s->params->cn_bandwidth));
        tw_stime ts = g_tw_lookahead + tw_rand_unif(lp->rng);
        if (msg->is_pull){
            bf->c4 = 1;
            struct codes_mctx mc_dst =
                codes_mctx_set_global_direct(msg->sender_mn_lp);
            struct codes_mctx mc_src =
                codes_mctx_set_global_direct(lp->gid);
            int net_id = model_net_get_id(LP_METHOD_NM_TERM);

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
static void packet_arrive(terminal_state * s, tw_bf * bf, terminal_message * msg, 
  tw_lp * lp) {

    // NIC aggregation - should this be a separate function?
    // Trigger an event on receiving server

    if(!s->rank_tbl)
        s->rank_tbl = qhash_init(dragonfly_rank_hash_compare, dragonfly_hash_func, DFLY_HASH_TABLE_SIZE);
    
    struct dfly_hash_key key;
    key.message_id = msg->message_id; 
    key.sender_id = msg->sender_lp;
    
    struct qhash_head *hash_link = NULL;
    struct dfly_qhash_entry * tmp = NULL;
      
    hash_link = qhash_search(s->rank_tbl, &key);
    
    if(hash_link)
        tmp = qhash_entry(hash_link, struct dfly_qhash_entry, hash_link);

    uint64_t total_chunks = msg->total_size / s->params->chunk_size;

    if(msg->total_size % s->params->chunk_size)
          total_chunks++;

    if(!total_chunks)
          total_chunks = 1;

    /*if(tmp)
    {
        if(tmp->num_chunks >= total_chunks || tmp->num_chunks < 0)
        {
           //tw_output(lp, "\n invalid number of chunks %d for LP %ld ", tmp->num_chunks, lp->gid);
           tw_lp_suspend(lp, 0, 0);
           return;
        }
    }*/
    assert(lp->gid == msg->dest_terminal_id);

    if(msg->packet_ID == LLU(TRACK_PKT))
        printf("\n Packet %llu arrived at lp %llu hops %d", msg->packet_ID, LLU(lp->gid), msg->my_N_hop);
  
  tw_stime ts = g_tw_lookahead + s->params->credit_delay + tw_rand_unif(lp->rng);

  // no method_event here - message going to router
  tw_event * buf_e;
  terminal_message * buf_msg;
  buf_e = model_net_method_event_new(msg->intm_lp_id, ts, lp,
          DRAGONFLY_ROUTER, (void**)&buf_msg, NULL);
  buf_msg->magic = router_magic_num;
  buf_msg->vc_index = msg->vc_index;
  buf_msg->output_chan = msg->output_chan;
  buf_msg->type = R_BUFFER;
  tw_event_send(buf_e);

  bf->c1 = 0;
  bf->c3 = 0;
  bf->c4 = 0;
  bf->c7 = 0;

  /* Total overall finished chunks in simulation */
  N_finished_chunks++;
  /* Finished chunks on a LP basis */
  s->finished_chunks++;
  /* Finished chunks per sample */
  s->fin_chunks_sample++;
  s->fin_chunks_ross_sample++;

  /* WE do not allow self messages through dragonfly */
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

  if(msg->chunk_id == num_chunks - 1)
  {
    bf->c31 = 1;
    s->packet_fin++;
    packet_fin++;
  }
  if(msg->path_type != MINIMAL && msg->path_type != NON_MINIMAL)
    printf("\n Wrong message path type %d ", msg->path_type);

  /* save the sample time */
    msg->saved_sample_time = s->fin_chunks_time;
    s->fin_chunks_time += (tw_now(lp) - msg->travel_start_time);
    msg->saved_fin_chunks_ross = s->fin_chunks_time_ross_sample;
    s->fin_chunks_time_ross_sample += (tw_now(lp) - msg->travel_start_time);
    
    /* save the total time per LP */
    msg->saved_avg_time = s->total_time;
    s->total_time += (tw_now(lp) - msg->travel_start_time); 

    msg->saved_total_time = dragonfly_total_time;
    dragonfly_total_time += tw_now( lp ) - msg->travel_start_time;
    total_hops += msg->my_N_hop;
    s->total_hops += msg->my_N_hop;
    s->fin_hops_sample += msg->my_N_hop;
    s->fin_hops_ross_sample += msg->my_N_hop;

    mn_stats* stat = model_net_find_stats(msg->category, s->dragonfly_stats_array);
    msg->saved_rcv_time = stat->recv_time;
    stat->recv_time += (tw_now(lp) - msg->travel_start_time);

#if DEBUG == 1
 if( msg->packet_ID == TRACK 
          && msg->chunk_id == num_chunks-1
          && msg->message_id == TRACK_MSG)
  {
    printf( "(%lf) [Terminal %d] packet %lld has arrived  \n",
        tw_now(lp), (int)lp->gid, msg->packet_ID);

    printf("travel start time is %f\n",
        msg->travel_start_time);

    printf("My hop now is %d\n",msg->my_N_hop);
  }
#endif

   /* Now retreieve the number of chunks completed from the hash and update
    * them */
   void *m_data_src = model_net_method_get_edata(DRAGONFLY, msg);

   /* If an entry does not exist then create one */
   if(!tmp)
   {
        bf->c5 = 1;
       struct dfly_qhash_entry * d_entry = malloc(sizeof (struct dfly_qhash_entry));
       d_entry->num_chunks = 0;
       d_entry->key = key;
       d_entry->remote_event_data = NULL;
       d_entry->remote_event_size = 0;
       qhash_add(s->rank_tbl, &key, &(d_entry->hash_link));
       s->rank_tbl_pop++;
            
       if(s->rank_tbl_pop >= DFLY_HASH_TABLE_SIZE)
            tw_error(TW_LOC, "\n Exceeded allocated qhash size, increase hash size in dragonfly model");
      
       hash_link = &(d_entry->hash_link);
       tmp = d_entry;
   }
    
    assert(tmp);
    tmp->num_chunks++;

    if(msg->chunk_id == num_chunks - 1)
    {
        bf->c1 = 1;
        stat->recv_count++;
        stat->recv_bytes += msg->packet_size;

        N_finished_packets++;
        s->finished_packets++;
    }
    /* if its the last chunk of the packet then handle the remote event data */
    if(msg->remote_event_size_bytes > 0 && !tmp->remote_event_data)
    {
        /* Retreive the remote event entry */
         tmp->remote_event_data = (void*)malloc(msg->remote_event_size_bytes);
         assert(tmp->remote_event_data);
         tmp->remote_event_size = msg->remote_event_size_bytes; 
         memcpy(tmp->remote_event_data, m_data_src, msg->remote_event_size_bytes);
    }
        if (dragonfly_max_latency < tw_now( lp ) - msg->travel_start_time) {
          bf->c3 = 1;
          msg->saved_available_time = dragonfly_max_latency;
          dragonfly_max_latency = tw_now( lp ) - msg->travel_start_time;
        }
    /* If all chunks of a message have arrived then send a remote event to the
     * callee*/
    //assert(tmp->num_chunks <= total_chunks);

    if(tmp->num_chunks >= total_chunks)
    {
        bf->c7 = 1;

        N_finished_msgs++;
        total_msg_sz += msg->total_size;
        s->total_msg_size += msg->total_size;
        s->finished_msgs++;
        s->data_size_sample += msg->total_size;
        s->data_size_ross_sample += msg->total_size;
        
        if(tmp->remote_event_data && tmp->remote_event_size > 0) {
            bf->c8 = 1;
            send_remote_event(s, msg, lp, bf, tmp->remote_event_data, tmp->remote_event_size);
        }
        
        /* Remove the hash entry */
        qhash_del(hash_link);
        rc_stack_push(lp, tmp, free_tmp, s->st);
        s->rank_tbl_pop--;
   }
  return;
}

/* collective operation for the torus network */
static void dragonfly_collective(char const * category, int message_size, int remote_event_size, const void* remote_event, tw_lp* sender)
{
    tw_event * e_new;
    tw_stime xfer_to_nic_time;
    terminal_message * msg;
    tw_lpid local_nic_id;
    char* tmp_ptr;

    codes_mapping_get_lp_info(sender->gid, lp_group_name, &mapping_grp_id,
            NULL, &mapping_type_id, NULL, &mapping_rep_id, &mapping_offset);
    codes_mapping_get_lp_id(lp_group_name, LP_CONFIG_NM_TERM, NULL, 1,
            mapping_rep_id, mapping_offset, &local_nic_id);

    xfer_to_nic_time = codes_local_latency(sender);
    e_new = model_net_method_event_new(local_nic_id, xfer_to_nic_time,
            sender, DRAGONFLY, (void**)&msg, (void**)&tmp_ptr);

    msg->remote_event_size_bytes = message_size;
    strcpy(msg->category, category);
    msg->sender_svr=sender->gid;
    msg->type = D_COLLECTIVE_INIT;

    tmp_ptr = (char*)msg;
    tmp_ptr += dragonfly_get_msg_sz();
    if(remote_event_size > 0)
     {
            msg->remote_event_size_bytes = remote_event_size;
            memcpy(tmp_ptr, remote_event, remote_event_size);
            tmp_ptr += remote_event_size;
     }

    tw_event_send(e_new);
    return;
}

/* reverse for collective operation of the dragonfly network */
static void dragonfly_collective_rc(int message_size, tw_lp* sender)
{
    (void)message_size;
     codes_local_latency_reverse(sender);
     return;
}

static void send_collective_remote_event(terminal_state * s,
                        terminal_message * msg,
                        tw_lp * lp)
{
    // Trigger an event on receiving server
    if(msg->remote_event_size_bytes)
     {
            tw_event* e;
            tw_stime ts;
            terminal_message * m;
            ts = (1/s->params->cn_bandwidth) * msg->remote_event_size_bytes;
            e = tw_event_new(s->origin_svr, ts, lp);
            m = tw_event_data(e);
            char* tmp_ptr = (char*)msg;
            tmp_ptr += dragonfly_get_msg_sz();
            memcpy(m, tmp_ptr, msg->remote_event_size_bytes);
            tw_event_send(e);
     }
}

static void node_collective_init(terminal_state * s,
                        terminal_message * msg,
                        tw_lp * lp)
{
        tw_event * e_new;
        tw_lpid parent_nic_id;
        tw_stime xfer_to_nic_time;
        terminal_message * msg_new;
        int num_lps;

        msg->saved_busy_time = s->collective_init_time;
        s->collective_init_time = tw_now(lp);
	s->origin_svr = msg->sender_svr;
	
        if(s->is_leaf)
        {
            //printf("\n LP %ld sending message to parent %ld ", s->node_id, s->parent_node_id);
            /* get the global LP ID of the parent node */
            // TODO: be annotation-aware
            codes_mapping_get_lp_info(lp->gid, lp_group_name, &mapping_grp_id,
                    NULL, &mapping_type_id, NULL, &mapping_rep_id,
                    &mapping_offset);
            num_lps = codes_mapping_get_lp_count(lp_group_name, 1, LP_CONFIG_NM_TERM,
                    s->anno, 0);
            codes_mapping_get_lp_id(lp_group_name, LP_CONFIG_NM_TERM, s->anno, 0,
                    s->parent_node_id/num_lps, (s->parent_node_id % num_lps),
                    &parent_nic_id);

           /* send a message to the parent that the LP has entered the collective operation */
            xfer_to_nic_time = g_tw_lookahead + LEVEL_DELAY;
            //e_new = tw_event_new(parent_nic_id, xfer_to_nic_time, lp);
	    void* m_data;
	    e_new = model_net_method_event_new(parent_nic_id, xfer_to_nic_time,
            	lp, DRAGONFLY, (void**)&msg_new, (void**)&m_data);
	    	
            memcpy(msg_new, msg, sizeof(terminal_message));
	    if (msg->remote_event_size_bytes){
        	memcpy(m_data, model_net_method_get_edata(DRAGONFLY, msg),
                	msg->remote_event_size_bytes);
      	    }
	    
            msg_new->type = D_COLLECTIVE_FAN_IN;
            msg_new->sender_node = s->node_id;

            tw_event_send(e_new);
        }
        return;
}

static void node_collective_fan_in(terminal_state * s,
                        tw_bf * bf,
                        terminal_message * msg,
                        tw_lp * lp)
{
        int i;
        s->num_fan_nodes++;

        codes_mapping_get_lp_info(lp->gid, lp_group_name, &mapping_grp_id,
                NULL, &mapping_type_id, NULL, &mapping_rep_id, &mapping_offset);
        int num_lps = codes_mapping_get_lp_count(lp_group_name, 1, LP_CONFIG_NM_TERM,
                s->anno, 0);

        tw_event* e_new;
        terminal_message * msg_new;
        tw_stime xfer_to_nic_time;

        bf->c1 = 0;
        bf->c2 = 0;

        /* if the number of fanned in nodes have completed at the current node then signal the parent */
        if((s->num_fan_nodes == s->num_children) && !s->is_root)
        {
            bf->c1 = 1;
            msg->saved_fan_nodes = s->num_fan_nodes-1;
            s->num_fan_nodes = 0;
            tw_lpid parent_nic_id;
            xfer_to_nic_time = g_tw_lookahead + LEVEL_DELAY;

            /* get the global LP ID of the parent node */
            codes_mapping_get_lp_id(lp_group_name, LP_CONFIG_NM_TERM, s->anno, 0,
                    s->parent_node_id/num_lps, (s->parent_node_id % num_lps),
                    &parent_nic_id);

           /* send a message to the parent that the LP has entered the collective operation */
            //e_new = tw_event_new(parent_nic_id, xfer_to_nic_time, lp);
            //msg_new = tw_event_data(e_new);
	    void * m_data;
      	    e_new = model_net_method_event_new(parent_nic_id,
              xfer_to_nic_time,
              lp, DRAGONFLY, (void**)&msg_new, &m_data);
	    
            memcpy(msg_new, msg, sizeof(terminal_message));
            msg_new->type = D_COLLECTIVE_FAN_IN;
            msg_new->sender_node = s->node_id;

            if (msg->remote_event_size_bytes){
	        memcpy(m_data, model_net_method_get_edata(DRAGONFLY, msg),
        	        msg->remote_event_size_bytes);
      	   }
	    
            tw_event_send(e_new);
      }

      /* root node starts off with the fan-out phase */
      if(s->is_root && (s->num_fan_nodes == s->num_children))
      {
           bf->c2 = 1;
           msg->saved_fan_nodes = s->num_fan_nodes-1;
           s->num_fan_nodes = 0;
           send_collective_remote_event(s, msg, lp);

           for( i = 0; i < s->num_children; i++ )
           {
                tw_lpid child_nic_id;
                /* Do some computation and fan out immediate child nodes from the collective */
                xfer_to_nic_time = g_tw_lookahead + COLLECTIVE_COMPUTATION_DELAY + LEVEL_DELAY + tw_rand_exponential(lp->rng, (double)LEVEL_DELAY/50);

                /* get global LP ID of the child node */
                codes_mapping_get_lp_id(lp_group_name, LP_CONFIG_NM_TERM, NULL, 1,
                        s->children[i]/num_lps, (s->children[i] % num_lps),
                        &child_nic_id);
                //e_new = tw_event_new(child_nic_id, xfer_to_nic_time, lp);

                //msg_new = tw_event_data(e_new);
                void * m_data;
	        e_new = model_net_method_event_new(child_nic_id,
                xfer_to_nic_time,
		lp, DRAGONFLY, (void**)&msg_new, &m_data);

		memcpy(msg_new, msg, sizeof(terminal_message));
	        if (msg->remote_event_size_bytes){
	                memcpy(m_data, model_net_method_get_edata(DRAGONFLY, msg),
        	               msg->remote_event_size_bytes);
      		}
		
                msg_new->type = D_COLLECTIVE_FAN_OUT;
                msg_new->sender_node = s->node_id;

                tw_event_send(e_new);
           }
      }
}

static void node_collective_fan_out(terminal_state * s,
                        tw_bf * bf,
                        terminal_message * msg,
                        tw_lp * lp)
{
        int i;
        int num_lps = codes_mapping_get_lp_count(lp_group_name, 1, LP_CONFIG_NM_TERM,
                NULL, 1);
        bf->c1 = 0;
        bf->c2 = 0;

        send_collective_remote_event(s, msg, lp);

        if(!s->is_leaf)
        {
            bf->c1 = 1;
            tw_event* e_new;
            nodes_message * msg_new;
            tw_stime xfer_to_nic_time;

           for( i = 0; i < s->num_children; i++ )
           {
                xfer_to_nic_time = g_tw_lookahead + DRAGONFLY_FAN_OUT_DELAY + tw_rand_exponential(lp->rng, (double)DRAGONFLY_FAN_OUT_DELAY/10);

                if(s->children[i] > 0)
                {
                        tw_lpid child_nic_id;

                        /* get global LP ID of the child node */
                        codes_mapping_get_lp_id(lp_group_name, LP_CONFIG_NM_TERM,
                                s->anno, 0, s->children[i]/num_lps,
                                (s->children[i] % num_lps), &child_nic_id);
                        //e_new = tw_event_new(child_nic_id, xfer_to_nic_time, lp);
                        //msg_new = tw_event_data(e_new);
                        //memcpy(msg_new, msg, sizeof(nodes_message) + msg->remote_event_size_bytes);
			void* m_data;
			e_new = model_net_method_event_new(child_nic_id,
							xfer_to_nic_time,
					                lp, DRAGONFLY, (void**)&msg_new, &m_data);
		        memcpy(msg_new, msg, sizeof(nodes_message));
		        if (msg->remote_event_size_bytes){
			        memcpy(m_data, model_net_method_get_edata(DRAGONFLY, msg),
			                msg->remote_event_size_bytes);
      			}

                        //TODO: this is probably incorrect - need to fix
                        msg_new->type = (nodes_event_t) D_COLLECTIVE_FAN_OUT;
                        msg_new->sender_node = s->node_id;
                        tw_event_send(e_new);
                }
           }
         }
	//printf("\n Fan out phase completed %ld ", lp->gid);
        if(max_collective < tw_now(lp) - s->collective_init_time )
          {
              bf->c2 = 1;
              max_collective = tw_now(lp) - s->collective_init_time;
          }
}

static void dragonfly_rsample_init(router_state * s,
        tw_lp * lp)
{
   (void)lp;
   int i = 0;
   const dragonfly_param * p = s->params;

   assert(p->radix);

   s->max_arr_size = MAX_STATS;
   s->rsamples = malloc(MAX_STATS * sizeof(struct dfly_router_sample)); 
   for(; i < s->max_arr_size; i++)
   {
    s->rsamples[i].busy_time = malloc(sizeof(tw_stime) * p->radix); 
    s->rsamples[i].link_traffic_sample = malloc(sizeof(int64_t) * p->radix);
   }
}
static void dragonfly_rsample_rc_fn(router_state * s,
        tw_bf * bf,
        terminal_message * msg, 
        tw_lp * lp)
{
    (void)bf;
    (void)lp;
    (void)msg;

    s->op_arr_size--;
    int cur_indx = s->op_arr_size;
    struct dfly_router_sample stat = s->rsamples[cur_indx];

    const dragonfly_param * p = s->params;
    int i =0;

    for(; i < p->radix; i++)
    {
        s->busy_time_sample[i] = stat.busy_time[i];
        s->link_traffic_sample[i] = stat.link_traffic_sample[i];
    }

    for( i = 0; i < p->radix; i++)
    {
        stat.busy_time[i] = 0;
        stat.link_traffic_sample[i] = 0;
    }
    s->fwd_events = stat.fwd_events;
    s->rev_events = stat.rev_events;
}

static void dragonfly_rsample_fn(router_state * s,
        tw_bf * bf,
        terminal_message * msg, 
        tw_lp * lp)
{
  (void)bf;
  (void)lp;
  (void)msg;

  const dragonfly_param * p = s->params; 

  if(s->op_arr_size >= s->max_arr_size) 
  {
    struct dfly_router_sample * tmp = malloc((MAX_STATS + s->max_arr_size) * sizeof(struct dfly_router_sample));
    memcpy(tmp, s->rsamples, s->op_arr_size * sizeof(struct dfly_router_sample));
    free(s->rsamples);
    s->rsamples = tmp;
    s->max_arr_size += MAX_STATS;
  }

  int i = 0;
  int cur_indx = s->op_arr_size; 

  s->rsamples[cur_indx].router_id = s->router_id;
  s->rsamples[cur_indx].end_time = tw_now(lp);
  s->rsamples[cur_indx].fwd_events = s->fwd_events;
  s->rsamples[cur_indx].rev_events = s->rev_events;

  for(; i < p->radix; i++)
  {
    s->rsamples[cur_indx].busy_time[i] = s->busy_time_sample[i]; 
    s->rsamples[cur_indx].link_traffic_sample[i] = s->link_traffic_sample[i]; 
  }

  s->op_arr_size++;

  /* clear up the current router stats */
  s->fwd_events = 0;
  s->rev_events = 0;

  for( i = 0; i < p->radix; i++)
  {
    s->busy_time_sample[i] = 0;
    s->link_traffic_sample[i] = 0;
  }
}

static void dragonfly_rsample_fin(router_state * s,
        tw_lp * lp)
{
    (void)lp;
    const dragonfly_param * p = s->params;

    if(s->router_id == 0)
    {
    
        /* write metadata file */
        char meta_fname[64];
        sprintf(meta_fname, "dragonfly-router-sampling.meta");

        FILE * fp = fopen(meta_fname, "w");
        fprintf(fp, "Router sample struct format: \nrouter_id (tw_lpid) \nbusy time for each of the %d links (double) \n"
                "link traffic for each of the %d links (int64_t) \nsample end time (double) forward events per sample \nreverse events per sample ",
                p->radix, p->radix);
        fprintf(fp, "\n\nOrdering of links \n%d local (router-router same group) channels \n%d global (router-router remote group)"
                " channels \n%d terminal channels", p->radix/2, p->radix/4, p->radix/4);
        fclose(fp);
    }
    char rt_fn[MAX_NAME_LENGTH];
    if(strcmp(router_sample_file, "") == 0)
        sprintf(rt_fn, "dragonfly-router-sampling-%ld.bin", g_tw_mynode); 
    else
        sprintf(rt_fn, "%s-%ld.bin", router_sample_file, g_tw_mynode);
    
    int i = 0;

    int size_sample = sizeof(tw_lpid) + p->radix * (sizeof(int64_t) + sizeof(tw_stime)) + sizeof(tw_stime) + 2 * sizeof(long);
    FILE * fp = fopen(rt_fn, "a");
    fseek(fp, sample_rtr_bytes_written, SEEK_SET);

    for(; i < s->op_arr_size; i++)
    {
        fwrite((void*)&(s->rsamples[i].router_id), sizeof(tw_lpid), 1, fp);
        fwrite(s->rsamples[i].busy_time, sizeof(tw_stime), p->radix, fp);
        fwrite(s->rsamples[i].link_traffic_sample, sizeof(int64_t), p->radix, fp);
        fwrite((void*)&(s->rsamples[i].end_time), sizeof(tw_stime), 1, fp);
        fwrite((void*)&(s->rsamples[i].fwd_events), sizeof(long), 1, fp);
        fwrite((void*)&(s->rsamples[i].rev_events), sizeof(long), 1, fp);
    }
    sample_rtr_bytes_written += (s->op_arr_size * size_sample);
    fclose(fp);
}
static void dragonfly_sample_init(terminal_state * s,
        tw_lp * lp)
{
    (void)lp;
    s->fin_chunks_sample = 0;
    s->data_size_sample = 0;
    s->fin_hops_sample = 0;
    s->fin_chunks_time = 0;
    s->busy_time_sample = 0;

    s->op_arr_size = 0;
    s->max_arr_size = MAX_STATS;

    s->sample_stat = malloc(MAX_STATS * sizeof(struct dfly_cn_sample));
    
    /*char buf[1024];
    int written = 0;
    if(!s->terminal_id)
    {
       written = sprintf(buf, "# Format <LP ID> <Terminal ID> <Data size> <Avg packet latency> <#flits/packets finished> <Avg hops> <Busy Time>"); 
        lp_io_write(lp->gid, "dragonfly-sampling-stats", written, buf);
    }*/
}
static void dragonfly_sample_rc_fn(terminal_state * s,
        tw_bf * bf,
        terminal_message * msg, 
        tw_lp * lp)
{
    (void)lp;
    (void)bf;
    (void)msg;

    s->op_arr_size--;
    int cur_indx = s->op_arr_size;
    struct dfly_cn_sample stat = s->sample_stat[cur_indx];
    s->busy_time_sample = stat.busy_time_sample;
    s->fin_chunks_time = stat.fin_chunks_time;
    s->fin_hops_sample = stat.fin_hops_sample;
    s->data_size_sample = stat.data_size_sample;
    s->fin_chunks_sample = stat.fin_chunks_sample;
    s->fwd_events = stat.fwd_events;
    s->rev_events = stat.rev_events;

    stat.busy_time_sample = 0;
    stat.fin_chunks_time = 0;
    stat.fin_hops_sample = 0;
    stat.data_size_sample = 0;
    stat.fin_chunks_sample = 0;
    stat.end_time = 0;
    stat.terminal_id = 0;
    stat.fwd_events = 0;
    stat.rev_events = 0;
}

static void dragonfly_sample_fn(terminal_state * s,
        tw_bf * bf,
        terminal_message * msg,
        tw_lp * lp)
{
    (void)lp;
    (void)msg;
    (void)bf;
    
    //int i = 0, free_slots = 0;

    /* checkout which samples are past the GVT, write them to the file */
    /*for(i = 0; i < s->op_arr_size; i++)
    {
        if(s->sample_stat[i].end_time == 0 || s->sample_stat[i].end_time >= lp->pe->GVT)
            break;

        free_slots++;
    }
    if(free_slots > 0)
    {
        fwrite(s->sample_stat, sizeof(struct dfly_sample_stats), free_slots, fp);
        s->op_arr_size -= free_slots;
        memmove(s->sample_stat, &(s->sample_stat[free_slots]), sizeof(struct dfly_sample_stats) * s->op_arr_size);
    }*/

    if(s->op_arr_size >= s->max_arr_size)
    {
        /* In the worst case, copy array to a new memory location, its very
         * expensive operation though */
        struct dfly_cn_sample * tmp = malloc((MAX_STATS + s->max_arr_size) * sizeof(struct dfly_cn_sample));
        memcpy(tmp, s->sample_stat, s->op_arr_size * sizeof(struct dfly_cn_sample));
        free(s->sample_stat);
        s->sample_stat = tmp;
        s->max_arr_size += MAX_STATS;
    }
    
    int cur_indx = s->op_arr_size;

    s->sample_stat[cur_indx].terminal_id = s->terminal_id;
    s->sample_stat[cur_indx].fin_chunks_sample = s->fin_chunks_sample;
    s->sample_stat[cur_indx].data_size_sample = s->data_size_sample;
    s->sample_stat[cur_indx].fin_hops_sample = s->fin_hops_sample;
    s->sample_stat[cur_indx].fin_chunks_time = s->fin_chunks_time;
    s->sample_stat[cur_indx].busy_time_sample = s->busy_time_sample;
    s->sample_stat[cur_indx].end_time = tw_now(lp);
    s->sample_stat[cur_indx].fwd_events = s->fwd_events;
    s->sample_stat[cur_indx].rev_events = s->rev_events;

    s->op_arr_size++;
    s->fin_chunks_sample = 0;
    s->data_size_sample = 0;
    s->fin_hops_sample = 0;
    s->fwd_events = 0;
    s->rev_events = 0;
    s->fin_chunks_time = 0;
    s->busy_time_sample = 0;
}

static void dragonfly_sample_fin(terminal_state * s,
        tw_lp * lp)
{
    (void)lp;
 

    /* int i = 0;
     * for(; i < s->op_arr_size; i++)
    {
        printf("\n Terminal id %ld data size sample %ld fin chunks %ld end time %lf ",
                s->terminal_id, s->sample_stat[i].data_size_sample, s->sample_stat[i].fin_chunks_sample, s->sample_stat[i].end_time);
    }*/
    if(!g_tw_mynode)
    {
    
        /* write metadata file */
        char meta_fname[64];
        sprintf(meta_fname, "dragonfly-cn-sampling.meta");

        FILE * fp = fopen(meta_fname, "w");
        fprintf(fp, "Compute node sample format\nterminal_id (tw_lpid) \nfinished chunks (long)"
                "\ndata size per sample (long) \nfinished hops (double) \ntime to finish chunks (double)"
                "\nbusy time (double)\nsample end time(double) \nforward events (long) \nreverse events (long)");
        fclose(fp);
    }
    char rt_fn[MAX_NAME_LENGTH];
    if(strncmp(cn_sample_file, "", 10) == 0)
        sprintf(rt_fn, "dragonfly-cn-sampling-%ld.bin", g_tw_mynode); 
    else
        sprintf(rt_fn, "%s-%ld.bin", cn_sample_file, g_tw_mynode);

    FILE * fp = fopen(rt_fn, "a");
    fseek(fp, sample_bytes_written, SEEK_SET);
    fwrite(s->sample_stat, sizeof(struct dfly_cn_sample), s->op_arr_size, fp);
    fclose(fp);

    sample_bytes_written += (s->op_arr_size * sizeof(struct dfly_cn_sample));
}

static void terminal_buf_update_rc(terminal_state * s,
		    tw_bf * bf, 
		    terminal_message * msg, 
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
static void 
terminal_buf_update(terminal_state * s, 
		    tw_bf * bf, 
		    terminal_message * msg, 
		    tw_lp * lp)
{
  (void)msg;

  bf->c1 = 0;
  bf->c2 = 0;
  bf->c3 = 0;

  tw_stime ts = codes_local_latency(lp);
  s->vc_occupancy[0] -= s->params->chunk_size;
  

  if(s->in_send_loop == 0 && s->terminal_msgs[0] != NULL) {
    terminal_message *m;
    bf->c1 = 1;
    tw_event* e = model_net_method_event_new(lp->gid, ts, lp, DRAGONFLY, 
        (void**)&m, NULL);
    m->type = T_SEND;
    m->magic = terminal_magic_num;
    s->in_send_loop = 1;
    tw_event_send(e);
  }
  return;
}

static void 
terminal_event( terminal_state * s, 
		tw_bf * bf, 
		terminal_message * msg, 
		tw_lp * lp )
{
  s->fwd_events++;
  //*(int *)bf = (int)0;
  assert(msg->magic == terminal_magic_num);

  rc_stack_gc(lp, s->st);
  switch(msg->type)
    {
    case T_GENERATE:
       packet_generate(s,bf,msg,lp);
    break;
    
    case T_ARRIVE:
        packet_arrive(s,bf,msg,lp);
    break;
    
    case T_SEND:
      packet_send(s,bf,msg,lp);
    break;
    
    case T_BUFFER:
       terminal_buf_update(s, bf, msg, lp);
     break;
    
    case D_COLLECTIVE_INIT:
      node_collective_init(s, msg, lp);
    break;

    case D_COLLECTIVE_FAN_IN:
      node_collective_fan_in(s, bf, msg, lp);
    break;

    case D_COLLECTIVE_FAN_OUT:
      node_collective_fan_out(s, bf, msg, lp);
    break;
    
    default:
       printf("\n LP %d Terminal message type not supported %d ", (int)lp->gid, msg->type);
       tw_error(TW_LOC, "Msg type not supported");
    }
}

static void 
dragonfly_terminal_final( terminal_state * s, 
      tw_lp * lp )
{
	model_net_print_stats(lp->gid, s->dragonfly_stats_array);
  
    if(s->terminal_id == 0)
    {
        /* write metadata file */
        char meta_fname[64];
        sprintf(meta_fname, "dragonfly-msg-stats.meta");

        FILE * fp = fopen(meta_fname, "w");
        fprintf(fp, "# Format <LP id> <Terminal ID> <Total Data Size> <Aggregate packet latency> <# Flits/Packets finished> <Avg hops> <Busy Time>");

        fclose(fp);
    }
    
    int written = 0;
    if(!s->terminal_id)
        written = sprintf(s->output_buf, "# Format <LP id> <Terminal ID> <Total Data Size> <Aggregate packet latency> <# Flits/Packets finished> <Avg hops> <Busy Time>");

    written += sprintf(s->output_buf + written, "\n %llu %u %lld %lf %ld %lf %lf",
            LLU(lp->gid), s->terminal_id, s->total_msg_size, (double)s->total_time/s->finished_packets, 
            s->finished_packets, (double)s->total_hops/s->finished_chunks,
            s->busy_time);

    lp_io_write(lp->gid, "dragonfly-msg-stats", written, s->output_buf); 
    
    if(s->terminal_msgs[0] != NULL) 
      printf("[%llu] leftover terminal messages \n", LLU(lp->gid));


    //if(s->packet_gen != s->packet_fin)
    //    printf("\n generated %d finished %d ", s->packet_gen, s->packet_fin);
   
    if(s->rank_tbl)
        qhash_finalize(s->rank_tbl);
    
    rc_stack_destroy(s->st);
    free(s->vc_occupancy);
    free(s->terminal_msgs);
    free(s->terminal_msgs_tail);
    free(s->children);
}

static void dragonfly_router_final(router_state * s,
		tw_lp * lp)
{
   //rc_stack_gc(lp, s->st);
   free(s->global_channel);
    int i, j;
    for(i = 0; i < s->params->radix; i++) {
      for(j = 0; j < 3; j++) {
        if(s->queued_msgs[i][j] != NULL) {
          printf("[%llu] leftover queued messages %d %d %d\n", LLU(lp->gid), i, j,
          s->vc_occupancy[i][j]);
        }
        if(s->pending_msgs[i][j] != NULL) {
          printf("[%llu] lefover pending messages %d %d\n", LLU(lp->gid), i, j);
        }
      }
    }

    rc_stack_destroy(s->st);
    
    const dragonfly_param *p = s->params;
    int written = 0;
    if(s->router_id == 0)
    {
        /* write metadata file */
        char meta_fname[64];
        sprintf(meta_fname, "dragonfly-msg-stats.meta");

        FILE * fp = fopen(meta_fname, "w");
        fprintf(fp, "# Format <LP ID> <Group ID> <Router ID> <Busy time per router port(s)>");
        fprintf(fp, "# Router ports in the order: %d local channels, %d global channels", 
                p->num_routers, p->num_global_channels);
        fclose(fp);
    }
    written += sprintf(s->output_buf + written, "\n %llu %d %d", 
            LLU(lp->gid),
            s->router_id / p->num_routers,
            s->router_id % p->num_routers);
    for(int d = 0; d < p->num_routers + p->num_global_channels; d++) 
        written += sprintf(s->output_buf + written, " %lf", s->busy_time[d]);

    lp_io_write(lp->gid, "dragonfly-router-stats", written, s->output_buf);

    written = 0;
    if(!s->router_id)
    {
        written = sprintf(s->output_buf2, "# Format <LP ID> <Group ID> <Router ID> <Link traffic per router port(s)>");
        written += sprintf(s->output_buf2 + written, "# Router ports in the order: %d local channels, %d global channels",
            p->num_routers, p->num_global_channels);
    }
    written += sprintf(s->output_buf2 + written, "\n %llu %d %d",
        LLU(lp->gid),
        s->router_id / p->num_routers,
        s->router_id % p->num_routers);

    for(int d = 0; d < p->num_routers + p->num_global_channels; d++) 
        written += sprintf(s->output_buf2 + written, " %lld", LLD(s->link_traffic[d]));

    assert(written < 4096);
    lp_io_write(lp->gid, "dragonfly-router-traffic", written, s->output_buf2);
}

/* Get the number of hops for this particular path source and destination groups */
int get_num_hops(int local_router_id,
		 int dest_router_id,
		 int num_routers,
                 int total_groups)
{
   int local_grp_id = local_router_id / num_routers;
   int dest_group_id = dest_router_id / num_routers;
   int num_hops = 4;

   /* Already at the destination router */
   if(local_router_id == dest_router_id)
    {
	return 1; /* already at the destination, traverse one hop only*/
    }
   else if(local_grp_id == dest_group_id)
    {
		return 2; /* in the same group, each router is connected so 2 additional hops to traverse (source and dest routers). */		
    }	

     /* if the router in the source group has direct connection to the destination group */
     int src_connecting_router = getRouterFromGroupID(dest_group_id, 
        local_grp_id, num_routers, total_groups);

     if(src_connecting_router == local_router_id)		
		num_hops--;

     int dest_connecting_router = getRouterFromGroupID(local_grp_id, 
        dest_group_id, num_routers, total_groups);	

     if(dest_connecting_router == dest_router_id)	
			num_hops--;

     return num_hops;
}

/* get the next stop for the current packet
 * determines if it is a router within a group, a router in another group
 * or the destination terminal */
tw_lpid 
get_next_stop(router_state * s, 
		      terminal_message * msg, 
		      int path,
		      int dest_router_id,
              int intm_id)
{
   int dest_lp;
   tw_lpid router_dest_id;
   int dest_group_id;

   int local_router_id = s->router_id;
   int origin_grp_id = msg->origin_router_id / s->params->num_routers;

   dest_group_id = dest_router_id / s->params->num_routers;

  /* If the packet has arrived at the destination router */
   if(dest_router_id == local_router_id)
    {
        dest_lp = msg->dest_terminal_id;
        return dest_lp;
    }
   if(s->group_id == dest_group_id)
   {
       codes_mapping_get_lp_id(lp_group_name, LP_CONFIG_NM_ROUT, s->anno, 0, dest_router_id,
          0, &router_dest_id);
       return router_dest_id;
   }
   /* Generate inter-mediate destination for non-minimal routing (selecting a random group) */
   if(msg->last_hop == TERMINAL && path == NON_MINIMAL)
    {
    	    msg->intm_group_id = intm_id;
    }
  /******************** DECIDE THE DESTINATION GROUP ***********************/
  /* It means that the packet has arrived at the inter-mediate group for non-minimal routing. Reset the group now. */
  /* Intermediate group ID is set. Divert the packet to the intermediate group. */
  if(path == NON_MINIMAL && msg->intm_group_id >= 0 &&
          (s->group_id == origin_grp_id))
   {
      dest_group_id = msg->intm_group_id;
   }
 
/********************** DECIDE THE ROUTER IN THE DESTINATION GROUP ***************/ 
  /* It means the packet has arrived at the destination group. Now divert it to the destination router. */
      /* Packet is at the source or intermediate group. Find a router that has a path to the destination group. */
  dest_lp=getRouterFromGroupID(dest_group_id, 
    s->group_id, s->params->num_routers, 
    s->params->num_groups);

      if(dest_lp == local_router_id)
      {
#if USE_DIRECT_SCHEME
          int my_pos = s->group_id % s->params->num_routers;
          if(s->group_id == s->params->num_groups - 1) {
              my_pos = dest_group_id % s->params->num_routers;
          }
          dest_lp = dest_group_id * s->params->num_routers + my_pos;
#else
        for(int i=0; i < s->params->num_global_channels; i++)
           {
            if(s->global_channel[i] / s->params->num_routers == dest_group_id)
                dest_lp=s->global_channel[i];
          }
#endif
      }
  codes_mapping_get_lp_id(lp_group_name, LP_CONFIG_NM_ROUT, s->anno, 0, dest_lp,
          0, &router_dest_id);

  return router_dest_id;
}
/* gets the output port corresponding to the next stop of the message */
int 
get_output_port( router_state * s, 
		terminal_message * msg, 
		int next_stop )
{
  int output_port = -1, terminal_id;
  codes_mapping_get_lp_info(msg->dest_terminal_id, lp_group_name,
          &mapping_grp_id, NULL, &mapping_type_id, NULL, &mapping_rep_id,
          &mapping_offset);
  int num_lps = codes_mapping_get_lp_count(lp_group_name,1,LP_CONFIG_NM_TERM,s->anno,0);
  terminal_id = (mapping_rep_id * num_lps) + mapping_offset;

  if((tw_lpid)next_stop == msg->dest_terminal_id)
   {
      output_port = s->params->num_routers + s->params->num_global_channels +
          ( terminal_id % s->params->num_cn);
    }
    else
    {
     codes_mapping_get_lp_info(next_stop, lp_group_name, &mapping_grp_id,
             NULL, &mapping_type_id, NULL, &mapping_rep_id, &mapping_offset);
     int local_router_id = mapping_rep_id + mapping_offset;
     int intm_grp_id = local_router_id / s->params->num_routers;

     if(intm_grp_id != s->group_id)
      {
#if USE_DIRECT_SCHEME
          int target_grp = intm_grp_id;
          if(target_grp == s->params->num_groups - 1) {
              target_grp = s->group_id;
          }
          output_port = s->params->num_routers + (target_grp) / 
                s->params->num_routers;
#else
        for(int i=0; i < s->params->num_global_channels; i++)
         {
           if(s->global_channel[i] == local_router_id)
             output_port = s->params->num_routers + i;
          }
#endif
      }
      else
       {
        output_port = local_router_id % s->params->num_routers;
       }
    }
    return output_port;
}


/* UGAL (first condition is from booksim), output port equality check comes from Dally dragonfly'09*/
static int do_adaptive_routing( router_state * s,
				 terminal_message * msg,
				 int dest_router_id,
                 int intm_id) {
  int next_stop;
  int minimal_out_port = -1, nonmin_out_port = -1;
  // decide which routing to take
  // get the queue occupancy of both the minimal and non-minimal output ports 
  int minimal_next_stop=get_next_stop(s, msg, MINIMAL, dest_router_id, -1);
  minimal_out_port = get_output_port(s, msg, minimal_next_stop);
  int nonmin_next_stop = get_next_stop(s, msg, NON_MINIMAL, dest_router_id, intm_id);
  nonmin_out_port = get_output_port(s, msg, nonmin_next_stop);
  
#if 0
TODO: do we need this code? nomin_vc and min_vc not used anywhere...
  int nomin_vc = 0;
  if(nonmin_out_port < s->params->num_routers) {
    nomin_vc = msg->my_l_hop;
  } else if(nonmin_out_port < (s->params->num_routers + 
        s->params->num_global_channels)) {
    nomin_vc = msg->my_g_hop;
  }
  int min_vc = 0;
  if(minimal_out_port < s->params->num_routers) {
    min_vc = msg->my_l_hop;
  } else if(minimal_out_port < (s->params->num_routers + 
        s->params->num_global_channels)) {
    min_vc = msg->my_g_hop;
  }
#endif
  int min_port_count = s->vc_occupancy[minimal_out_port][0] + 
      s->vc_occupancy[minimal_out_port][1] + s->vc_occupancy[minimal_out_port][2]
      + s->queued_count[minimal_out_port];

  // Now get the expected number of hops to be traversed for both routes 
  //int num_min_hops = get_num_hops(s->router_id, dest_router_id, 
  //    s->params->num_routers, s->params->num_groups);

  int intm_router_id = getRouterFromGroupID(msg->intm_group_id, 
      s->router_id / s->params->num_routers, s->params->num_routers, 
      s->params->num_groups);

  int num_nonmin_hops = 6;
  
  if(msg->intm_group_id >= 0)
  {
      num_nonmin_hops = get_num_hops(s->router_id, intm_router_id, 
      s->params->num_routers, s->params->num_groups) + 
    get_num_hops(intm_router_id, dest_router_id, s->params->num_routers,
        s->params->num_groups);
  }
  assert(num_nonmin_hops <= 6);

  /* average the local queues of the router */
  unsigned int q_avg = 0;
  int i;
  for( i = 0; i < s->params->radix; i++)
  {
    if( i != minimal_out_port)
      q_avg += s->vc_occupancy[i][0] + s->vc_occupancy[i][1] +
        s->vc_occupancy[i][2];
  }
  q_avg = q_avg / (s->params->radix - 1);

  //int min_out_chan = minimal_out_port;
  //int nonmin_out_chan = nonmin_out_port;

  /* Adding history window approach, not taking the queue status at every 
   * simulation time thats why, we are maintaining the current history 
   * window number and an average of the previous history window number. */
  //int min_hist_count = s->cur_hist_num[min_out_chan] + 
  //  (s->prev_hist_num[min_out_chan]/2);
  //int nonmin_hist_count = s->cur_hist_num[nonmin_out_chan] + 
  //  (s->prev_hist_num[min_out_chan]/2);

  int nonmin_port_count = s->vc_occupancy[nonmin_out_port][0] +
      s->vc_occupancy[nonmin_out_port][1] + s->vc_occupancy[nonmin_out_port][2]
      + s->queued_count[nonmin_out_port];
  // VARIATION 1:
  // if(num_min_hops * min_port_count <= num_nonmin_hops * nonmin_port_count) {
  // VARIATION 2:
  //if(num_min_hops * min_port_count <= (num_nonmin_hops * (q_avg + 1))) {
    if(min_port_count <= nonmin_port_count) {
    msg->path_type = MINIMAL;
    next_stop = minimal_next_stop;
    msg->intm_group_id = -1;
  }
  else
  {
    msg->path_type = NON_MINIMAL;
    next_stop = nonmin_next_stop;
  }
  return next_stop;
}

static void router_packet_receive_rc(router_state * s,
        tw_bf * bf,
        terminal_message * msg,
        tw_lp * lp)
{
    router_rev_ecount++;
	router_ecount--;
      
    int output_port = msg->saved_vc;
    int output_chan = msg->saved_channel;

    tw_rand_reverse_unif(lp->rng);
      
    if(bf->c2) {
        tw_rand_reverse_unif(lp->rng);
        terminal_message_list * tail = return_tail(s->pending_msgs[output_port], s->pending_msgs_tail[output_port], output_chan);
        delete_terminal_message_list(tail);
        s->vc_occupancy[output_port][output_chan] -= s->params->chunk_size;
        if(bf->c3) {
          codes_local_latency_reverse(lp);
          s->in_send_loop[output_port] = 0;
        }
      }
      if(bf->c4) {
      s->last_buf_full[output_port] = msg->saved_busy_time;
      delete_terminal_message_list(return_tail(s->queued_msgs[output_port], 
          s->queued_msgs_tail[output_port], output_chan));
      s->queued_count[output_port] -= s->params->chunk_size; 
      }
}

/* Packet arrives at the router and a credit is sent back to the sending terminal/router */
static void 
router_packet_receive( router_state * s, 
			tw_bf * bf, 
			terminal_message * msg, 
			tw_lp * lp )
{
  router_ecount++;

  bf->c1 = 0;
  bf->c2 = 0;
  bf->c3 = 0;
  bf->c4 = 0;
  bf->c5 = 0;

  tw_stime ts;

  int next_stop = -1, output_port = -1, output_chan = -1;

  codes_mapping_get_lp_info(msg->dest_terminal_id, lp_group_name,
      &mapping_grp_id, NULL, &mapping_type_id, NULL, &mapping_rep_id,
      &mapping_offset); 
  int num_lps = codes_mapping_get_lp_count(lp_group_name, 1, LP_CONFIG_NM_TERM,
      s->anno, 0);
  int dest_router_id = (mapping_offset + (mapping_rep_id * num_lps)) / 
    s->params->num_cn;
  int local_grp_id = s->router_id / s->params->num_routers;

  int intm_id = tw_rand_integer(lp->rng, 1, s->params->num_real_groups - 1); 
  intm_id = (local_grp_id + intm_id) % s->params->num_real_groups;

  /* progressive adaptive routing makes a check at every node/router at the 
   * source group to sense congestion. Once it does and decides on taking 
   * non-minimal path, it does not check any longer. */
  terminal_message_list * cur_chunk = (terminal_message_list*)malloc(sizeof(terminal_message_list));
 init_terminal_message_list(cur_chunk, msg);
  
  if(routing == PROG_ADAPTIVE
      && msg->path_type != NON_MINIMAL
      && (unsigned int) local_grp_id == ( msg->origin_router_id / s->params->num_routers)) {
    next_stop = do_adaptive_routing(s, &(cur_chunk->msg), dest_router_id, intm_id);	
  } else if(msg->last_hop == TERMINAL && routing == ADAPTIVE) {
    next_stop = do_adaptive_routing(s, &(cur_chunk->msg), dest_router_id, intm_id);
  } else {
    if(routing == MINIMAL || routing == NON_MINIMAL)	
      cur_chunk->msg.path_type = routing; /*defaults to the routing algorithm if we 
                                don't have adaptive routing here*/
    assert(cur_chunk->msg.path_type == MINIMAL || cur_chunk->msg.path_type == NON_MINIMAL);
    next_stop = get_next_stop(s, &(cur_chunk->msg), cur_chunk->msg.path_type, dest_router_id, intm_id);
  }
  
  if(msg->remote_event_size_bytes > 0) {
    void *m_data_src = model_net_method_get_edata(DRAGONFLY_ROUTER, msg);
    cur_chunk->event_data = (char*)malloc(msg->remote_event_size_bytes);
    memcpy(cur_chunk->event_data, m_data_src, msg->remote_event_size_bytes);
  }
  output_port = get_output_port(s, &(cur_chunk->msg), next_stop); 
  assert(output_port >= 0);
  output_chan = 0;
  int max_vc_size = s->params->cn_vc_size;

  cur_chunk->msg.vc_index = output_port;
  cur_chunk->msg.next_stop = next_stop;

  if(msg->packet_ID == LLU(TRACK_PKT))
      printf("\n Router packet %llu arrived lp id %llu final dest %llu output port %d ", msg->packet_ID, LLU(lp->gid), LLU(msg->dest_terminal_id), output_port);
  
  if(output_port < s->params->num_routers) {
    output_chan = msg->my_l_hop;
    if(msg->my_g_hop == 1) output_chan = 1;
    if(msg->my_g_hop == 2) output_chan = 2;
    max_vc_size = s->params->local_vc_size;
    cur_chunk->msg.my_l_hop++;
  } else if(output_port < (s->params->num_routers + 
        s->params->num_global_channels)) {
    output_chan = msg->my_g_hop;
    max_vc_size = s->params->global_vc_size;
    cur_chunk->msg.my_g_hop++;
  }

  cur_chunk->msg.output_chan = output_chan;
  cur_chunk->msg.my_N_hop++;

  assert(output_chan < 3);
  assert(output_port < s->params->radix);

  if(s->vc_occupancy[output_port][output_chan] + s->params->chunk_size 
      <= max_vc_size) {
    bf->c2 = 1;
    router_credit_send(s, msg, lp, -1);
    append_to_terminal_message_list( s->pending_msgs[output_port], 
      s->pending_msgs_tail[output_port], output_chan, cur_chunk);
    s->vc_occupancy[output_port][output_chan] += s->params->chunk_size;
    if(s->in_send_loop[output_port] == 0) {
      bf->c3 = 1;
      terminal_message *m;
      ts = codes_local_latency(lp); 
      tw_event *e = model_net_method_event_new(lp->gid, ts, lp,
              DRAGONFLY_ROUTER, (void**)&m, NULL);
      m->type = R_SEND;
      m->magic = router_magic_num;
      m->vc_index = output_port;
      
      tw_event_send(e);
      s->in_send_loop[output_port] = 1;
    }
  } else {
    bf->c4 = 1;
    cur_chunk->msg.saved_vc = msg->vc_index;
    cur_chunk->msg.saved_channel = msg->output_chan;
    append_to_terminal_message_list( s->queued_msgs[output_port], 
      s->queued_msgs_tail[output_port], output_chan, cur_chunk);
    s->queued_count[output_port] += s->params->chunk_size;
    msg->saved_busy_time = s->last_buf_full[output_port];
    s->last_buf_full[output_port] = tw_now(lp);
  }

  msg->saved_vc = output_port;
  msg->saved_channel = output_chan;
  return;
}

static void router_packet_send_rc(router_state * s, 
		    tw_bf * bf, 
		     terminal_message * msg, tw_lp * lp)
{
    router_ecount--;
    router_rev_ecount++;
    
    int output_port = msg->saved_vc;
    int output_chan = msg->saved_channel;
    if(bf->c1) {
        s->in_send_loop[output_port] = 1;
        return;  
    }
      
    tw_rand_reverse_unif(lp->rng);
      
    terminal_message_list * cur_entry = rc_stack_pop(s->st);
    assert(cur_entry);

    if(bf->c11)
    {
        s->link_traffic[output_port] -= cur_entry->msg.packet_size % s->params->chunk_size;
        s->link_traffic_sample[output_port] -= cur_entry->msg.packet_size % s->params->chunk_size; 
        s->link_traffic_ross_sample[output_port] -= cur_entry->msg.packet_size % s->params->chunk_size; 
    }
    if(bf->c12)
    {
        s->link_traffic[output_port] -= s->params->chunk_size;
        s->link_traffic_sample[output_port] -= s->params->chunk_size;
        s->link_traffic_ross_sample[output_port] -= s->params->chunk_size;
    }
    s->next_output_available_time[output_port] = msg->saved_available_time;

    prepend_to_terminal_message_list(s->pending_msgs[output_port],
            s->pending_msgs_tail[output_port], output_chan, cur_entry);

    if(routing == PROG_ADAPTIVE)
	{
		if(bf->c2)
		{
		   s->cur_hist_num[output_port] = s->prev_hist_num[output_port];
		   s->prev_hist_num[output_port] = msg->saved_hist_num;
		   s->cur_hist_start_time[output_port] = msg->saved_hist_start_time;	
		}
		else
		  s->cur_hist_num[output_port]--;
 	}
    if(bf->c3) {
        tw_rand_reverse_unif(lp->rng);
      }
      
    if(bf->c4) {
        s->in_send_loop[output_port] = 1;
      }
}
/* routes the current packet to the next stop */
static void 
router_packet_send( router_state * s, 
		    tw_bf * bf, 
		     terminal_message * msg, tw_lp * lp)
{
  router_ecount++;

  tw_stime ts;
  tw_event *e;
  terminal_message *m;
  int output_port = msg->vc_index;
  int output_chan = 2;

  terminal_message_list *cur_entry = s->pending_msgs[output_port][2];
  if(cur_entry == NULL) {
    cur_entry = s->pending_msgs[output_port][1];
    output_chan = 1;
    if(cur_entry == NULL) {
      cur_entry = s->pending_msgs[output_port][0];
      output_chan = 0;
    }
  }
  msg->saved_vc = output_port;
  msg->saved_channel = output_chan;

  if(cur_entry == NULL) {
    bf->c1 = 1;
    s->in_send_loop[output_port] = 0;
    return;
  }

  int to_terminal = 1, global = 0;
  double delay = s->params->cn_delay;
  double bandwidth = s->params->cn_bandwidth;

  if(output_port < s->params->num_routers) {
    to_terminal = 0;
    delay = s->params->local_delay;
    bandwidth = s->params->local_bandwidth;
  } else if(output_port < s->params->num_routers + 
    s->params->num_global_channels) {
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
  
  ts = g_tw_lookahead + tw_rand_unif( lp->rng) + bytetime + s->params->router_delay;

  msg->saved_available_time = s->next_output_available_time[output_port];
  s->next_output_available_time[output_port] = 
    maxd(s->next_output_available_time[output_port], tw_now(lp));
  s->next_output_available_time[output_port] += ts;

  ts = s->next_output_available_time[output_port] - tw_now(lp);
  // dest can be a router or a terminal, so we must check
  void * m_data;
  if (to_terminal) {
    assert(cur_entry->msg.next_stop == cur_entry->msg.dest_terminal_id);
    e = model_net_method_event_new(cur_entry->msg.next_stop, 
        s->next_output_available_time[output_port] - tw_now(lp), lp,
        DRAGONFLY, (void**)&m, &m_data);
  } else {
    e = model_net_method_event_new(cur_entry->msg.next_stop,
            s->next_output_available_time[output_port] - tw_now(lp), lp,
            DRAGONFLY_ROUTER, (void**)&m, &m_data);
  }
  memcpy(m, &cur_entry->msg, sizeof(terminal_message));
  if (m->remote_event_size_bytes){
    memcpy(m_data, cur_entry->event_data, m->remote_event_size_bytes);
  }

  if(global)
    m->last_hop = GLOBAL;
  else
    m->last_hop = LOCAL;

  m->intm_lp_id = lp->gid;
  m->magic = router_magic_num;

  if((cur_entry->msg.packet_size % s->params->chunk_size) && (cur_entry->msg.chunk_id == num_chunks - 1)) {
      bf->c11 = 1;
      s->link_traffic[output_port] +=  (cur_entry->msg.packet_size %
              s->params->chunk_size); 
      s->link_traffic_sample[output_port] += (cur_entry->msg.packet_size % 
               s->params->chunk_size);
      s->link_traffic_ross_sample[output_port] += (cur_entry->msg.packet_size % 
               s->params->chunk_size);
  } else {
    bf->c12 = 1;
    s->link_traffic[output_port] += s->params->chunk_size;
    s->link_traffic_sample[output_port] += s->params->chunk_size;
    s->link_traffic_ross_sample[output_port] += s->params->chunk_size;
  }

  if(routing == PROG_ADAPTIVE)
  {
      if(tw_now(lp) - s->cur_hist_start_time[output_port] >= WINDOW_LENGTH) {
        bf->c2 = 1;
        msg->saved_hist_start_time = s->cur_hist_start_time[output_port];
        s->prev_hist_num[output_port] = s->cur_hist_num[output_port];
        s->cur_hist_start_time[output_port] = tw_now(lp);
        msg->saved_hist_num = s->cur_hist_num[output_port];
        s->cur_hist_num[output_port] = 1;
      } else {
        s->cur_hist_num[output_port]++;
      }
  }
  /* Determine the event type. If the packet has arrived at the final 
   * destination router then it should arrive at the destination terminal 
   * next.*/
  if(to_terminal) {
    m->type = T_ARRIVE;
    m->magic = terminal_magic_num;
  } else {
      /* The packet has to be sent to another router */
    m->magic = router_magic_num;
    m->type = R_ARRIVE;
  }
  tw_event_send(e);
  
  cur_entry = return_head(s->pending_msgs[output_port], 
    s->pending_msgs_tail[output_port], output_chan);
  rc_stack_push(lp, cur_entry, (void*)delete_terminal_message_list, s->st);
  
  cur_entry = s->pending_msgs[output_port][2];
 
  s->next_output_available_time[output_port] -= s->params->router_delay;
  ts -= s->params->router_delay;

  if(cur_entry == NULL) cur_entry = s->pending_msgs[output_port][1];
  if(cur_entry == NULL) cur_entry = s->pending_msgs[output_port][0];
  if(cur_entry != NULL) {
    bf->c3 = 1;
    terminal_message *m_new;
    ts = ts + g_tw_lookahead * tw_rand_unif(lp->rng);
    e = model_net_method_event_new(lp->gid, ts, lp, DRAGONFLY_ROUTER,
            (void**)&m_new, NULL);
    m_new->type = R_SEND;
    m_new->magic = router_magic_num;
    m_new->vc_index = output_port;
    tw_event_send(e);
  } else {
    bf->c4 = 1;
    s->in_send_loop[output_port] = 0;
  }
  return;
}

static void router_buf_update_rc(router_state * s,
        tw_bf * bf,
        terminal_message * msg,
        tw_lp * lp)
{
      int indx = msg->vc_index;
      int output_chan = msg->output_chan;
      s->vc_occupancy[indx][output_chan] += s->params->chunk_size;
      if(bf->c3)
      {
        s->busy_time[indx] = msg->saved_rcv_time;
        s->busy_time_sample[indx] = msg->saved_sample_time;
        s->busy_time_ross_sample[indx] = msg->saved_busy_time_ross;
        s->last_buf_full[indx] = msg->saved_busy_time;
      }
      if(bf->c1) {
        terminal_message_list* head = return_tail(s->pending_msgs[indx],
            s->pending_msgs_tail[indx], output_chan);
        tw_rand_reverse_unif(lp->rng);
        prepend_to_terminal_message_list(s->queued_msgs[indx], 
            s->queued_msgs_tail[indx], output_chan, head);
        s->vc_occupancy[indx][output_chan] -= s->params->chunk_size;
        s->queued_count[indx] += s->params->chunk_size;
      }
      if(bf->c2) {
        codes_local_latency_reverse(lp);
        s->in_send_loop[indx] = 0;
      }
}
/* Update the buffer space associated with this router LP */
static void router_buf_update(router_state * s, tw_bf * bf, terminal_message * msg, tw_lp * lp)
{
  int indx = msg->vc_index;
  int output_chan = msg->output_chan;
  s->vc_occupancy[indx][output_chan] -= s->params->chunk_size;
  
  if(s->last_buf_full[indx])
  {
    bf->c3 = 1;
    msg->saved_rcv_time = s->busy_time[indx];
    msg->saved_busy_time = s->last_buf_full[indx];
    msg->saved_sample_time = s->busy_time_sample[indx];
    msg->saved_busy_time_ross = s->busy_time_ross_sample[indx];
    s->busy_time[indx] += (tw_now(lp) - s->last_buf_full[indx]);
    s->busy_time_sample[indx] += (tw_now(lp) - s->last_buf_full[indx]);
    s->busy_time_ross_sample[indx] += (tw_now(lp) - s->last_buf_full[indx]);
    s->last_buf_full[indx] = 0.0;
  }
  if(s->queued_msgs[indx][output_chan] != NULL) {
    bf->c1 = 1;
    terminal_message_list *head = return_head(s->queued_msgs[indx],
        s->queued_msgs_tail[indx], output_chan);
    router_credit_send(s, &head->msg, lp, 1); 
    append_to_terminal_message_list(s->pending_msgs[indx], 
      s->pending_msgs_tail[indx], output_chan, head);
    s->vc_occupancy[indx][output_chan] += s->params->chunk_size;
    s->queued_count[indx] -= s->params->chunk_size; 
  }
  if(s->in_send_loop[indx] == 0 && s->pending_msgs[indx][output_chan] != NULL) {
    bf->c2 = 1;
    terminal_message *m;
    tw_stime ts = codes_local_latency(lp);
    tw_event *e = model_net_method_event_new(lp->gid, ts, lp, DRAGONFLY_ROUTER,
            (void**)&m, NULL);
    m->type = R_SEND;
    m->vc_index = indx;
    m->magic = router_magic_num;
    s->in_send_loop[indx] = 1;
    tw_event_send(e);
  }
  
  return;
}

static void router_event(router_state * s, tw_bf * bf, terminal_message * msg, 
    tw_lp * lp) {

  s->fwd_events++;
  rc_stack_gc(lp, s->st);
  
  assert(msg->magic == router_magic_num);
  switch(msg->type)
  {
    case R_SEND: // Router has sent a packet to an intra-group router (local channel)
      router_packet_send(s, bf, msg, lp);
      break;

    case R_ARRIVE: // Router has received a packet from an intra-group router (local channel)
      router_packet_receive(s, bf, msg, lp);
      break;

    case R_BUFFER:
      router_buf_update(s, bf, msg, lp);
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
static void terminal_rc_event_handler(terminal_state * s, tw_bf * bf, 
    terminal_message * msg, tw_lp * lp) {

   s->rev_events++;
   switch(msg->type)
   {
    case T_GENERATE:
        packet_generate_rc(s, bf, msg, lp); 
        break;

    case T_SEND:
        packet_send_rc(s, bf, msg, lp);
        break;

    case T_ARRIVE:
        packet_arrive_rc(s, bf, msg, lp);
        break;

    case T_BUFFER:
        terminal_buf_update_rc(s, bf, msg, lp); 
        break;

    case D_COLLECTIVE_INIT:
            {
                s->collective_init_time = msg->saved_busy_time;
            }
      break;
    case D_COLLECTIVE_FAN_IN: {
        int i;
        s->num_fan_nodes--;
        if(bf->c1)
        {
          s->num_fan_nodes = msg->saved_fan_nodes;
        }
        if(bf->c2)
        {
          s->num_fan_nodes = msg->saved_fan_nodes;
          for( i = 0; i < s->num_children; i++ )
            tw_rand_reverse_unif(lp->rng);
        }
      }
      break;

    case D_COLLECTIVE_FAN_OUT: {
        int i;
        if(bf->c1)
        {
          for( i = 0; i < s->num_children; i++ )
            tw_rand_reverse_unif(lp->rng);
        }
      }	 
  }
}

/* Reverse computation handler for a router event */
static void router_rc_event_handler(router_state * s, tw_bf * bf, 
  terminal_message * msg, tw_lp * lp) {
    s->rev_events++;

    switch(msg->type) {
    case R_SEND: 
        router_packet_send_rc(s, bf, msg, lp);
    break;
    case R_ARRIVE: 
        router_packet_receive_rc(s, bf, msg, lp);
    break;

    case R_BUFFER: 
        router_buf_update_rc(s, bf, msg, lp);
    break;
  }
}

/* dragonfly compute node and router LP types */
tw_lptype dragonfly_lps[] =
{
   // Terminal handling functions
   {
    (init_f)terminal_init,
    (pre_run_f) NULL,
    (event_f) terminal_event,
    (revent_f) terminal_rc_event_handler,
    (commit_f) NULL,
    (final_f) dragonfly_terminal_final,
    (map_f) codes_mapping,
    sizeof(terminal_state)
    },
   {
     (init_f) router_setup,
     (pre_run_f) NULL,
     (event_f) router_event,
     (revent_f) router_rc_event_handler,
     (commit_f) NULL,
     (final_f) dragonfly_router_final,
     (map_f) codes_mapping,
     sizeof(router_state),
   },
   {NULL, NULL, NULL, NULL, NULL, NULL, NULL, 0},
};

/* For ROSS event tracing */
void dragonfly_event_collect(terminal_message *m, tw_lp *lp, char *buffer, int *collect_flag)
{
    (void)lp;
    (void)collect_flag;

    int type = (int) m->type;
    memcpy(buffer, &type, sizeof(type));
}

void dragonfly_model_stat_collect(terminal_state *s, tw_lp *lp, char *buffer)
{
    (void)lp;

    int index = 0;
    tw_lpid id = 0;
    long tmp = 0;
    tw_stime tmp2 = 0;
    
    id = s->terminal_id;
    memcpy(&buffer[index], &id, sizeof(id));
    index += sizeof(id);

    tmp = s->fin_chunks_ross_sample;
    memcpy(&buffer[index], &tmp, sizeof(tmp));
    index += sizeof(tmp);
    s->fin_chunks_ross_sample = 0;

    tmp = s->data_size_ross_sample;
    memcpy(&buffer[index], &tmp, sizeof(tmp));
    index += sizeof(tmp);
    s->data_size_ross_sample = 0;

    tmp = s->fin_hops_ross_sample;
    memcpy(&buffer[index], &tmp, sizeof(tmp));
    index += sizeof(tmp);
    s->fin_hops_ross_sample = 0;

    tmp2 = s->fin_chunks_time_ross_sample;
    memcpy(&buffer[index], &tmp2, sizeof(tmp2));
    index += sizeof(tmp2);
    s->fin_chunks_time_ross_sample = 0;

    tmp2 = s->busy_time_ross_sample;
    memcpy(&buffer[index], &tmp2, sizeof(tmp2));
    index += sizeof(tmp2);
    s->busy_time_ross_sample = 0;

    return;
}

void dfly_router_model_stat_collect(router_state *s, tw_lp *lp, char *buffer)
{
    (void)lp;

    const dragonfly_param * p = s->params; 
    int i, index = 0;

    tw_lpid id = 0;
    tw_stime tmp = 0;
    int64_t tmp2 = 0;

    id = s->router_id;
    memcpy(&buffer[index], &id, sizeof(id));
    index += sizeof(id);

    for(i = 0; i < p->radix; i++)
    {
        tmp = s->busy_time_ross_sample[i];
        memcpy(&buffer[index], &tmp, sizeof(tmp));
        index += sizeof(tmp);
        s->busy_time_ross_sample[i] = 0; 

        tmp2 = s->link_traffic_ross_sample[i];
        memcpy(&buffer[index], &tmp2, sizeof(tmp2));
        index += sizeof(tmp2);
        s->link_traffic_ross_sample[i] = 0; 
    }
    return;
}

static const st_model_types  *dragonfly_get_model_types(void)
{
    return(&dragonfly_model_types[0]);
}

static const st_model_types  *dfly_router_get_model_types(void)
{
    return(&dragonfly_model_types[1]);
}

static void dragonfly_register_model_types(st_model_types *base_type)
{
    st_model_type_register(LP_CONFIG_NM_TERM, base_type);
}

static void router_register_model_types(st_model_types *base_type)
{
    st_model_type_register(LP_CONFIG_NM_ROUT, base_type);
}
/*** END of ROSS event tracing additions */

/* returns the dragonfly lp type for lp registration */
static const tw_lptype* dragonfly_get_cn_lp_type(void)
{
	   return(&dragonfly_lps[0]);
}
static const tw_lptype* router_get_lp_type(void)
{
    return (&dragonfly_lps[1]);
}

static void dragonfly_register(tw_lptype *base_type) {
    lp_type_register(LP_CONFIG_NM_TERM, base_type);
}

static void router_register(tw_lptype *base_type) {
    lp_type_register(LP_CONFIG_NM_ROUT, base_type);
}

/* data structure for dragonfly statistics */
struct model_net_method dragonfly_method =
{
    .mn_configure = dragonfly_configure,
    .mn_register = dragonfly_register,
    .model_net_method_packet_event = dragonfly_packet_event,
    .model_net_method_packet_event_rc = dragonfly_packet_event_rc,
    .model_net_method_recv_msg_event = NULL,
    .model_net_method_recv_msg_event_rc = NULL,
    .mn_get_lp_type = dragonfly_get_cn_lp_type,
    .mn_get_msg_sz = dragonfly_get_msg_sz,
    .mn_report_stats = dragonfly_report_stats,
    .mn_collective_call = dragonfly_collective,
    .mn_collective_call_rc = dragonfly_collective_rc,   
    .mn_sample_fn = (void*)dragonfly_sample_fn,    
    .mn_sample_rc_fn = (void*)dragonfly_sample_rc_fn,
    .mn_sample_init_fn = (void*)dragonfly_sample_init,
    .mn_sample_fini_fn = (void*)dragonfly_sample_fin,
    .mn_model_stat_register = dragonfly_register_model_types,
    .mn_get_model_stat_types = dragonfly_get_model_types,
};

struct model_net_method dragonfly_router_method =
{
    .mn_configure = NULL, // handled by dragonfly_configure
    .mn_register  = router_register,
    .model_net_method_packet_event = NULL,
    .model_net_method_packet_event_rc = NULL,
    .model_net_method_recv_msg_event = NULL,
    .model_net_method_recv_msg_event_rc = NULL,
    .mn_get_lp_type = router_get_lp_type,
    .mn_get_msg_sz = dragonfly_get_msg_sz,
    .mn_report_stats = NULL, // not yet supported
    .mn_collective_call = NULL,
    .mn_collective_call_rc = NULL,
    .mn_sample_fn = (void*)dragonfly_rsample_fn,
    .mn_sample_rc_fn = (void*)dragonfly_rsample_rc_fn,
    .mn_sample_init_fn = (void*)dragonfly_rsample_init,
    .mn_sample_fini_fn = (void*)dragonfly_rsample_fin,
    .mn_model_stat_register = router_register_model_types,
    .mn_get_model_stat_types = dfly_router_get_model_types,
};

#ifdef ENABLE_CORTEX

static int dragonfly_get_number_of_compute_nodes(void* topo) {
		// TODO
        (void)topo;
		return -1;
}

static int dragonfly_get_number_of_routers(void* topo) {
		// TODO
        (void)topo;
		return -1;
}

static double dragonfly_get_router_link_bandwidth(void* topo, router_id_t r1, router_id_t r2) {
        // TODO
        (void)topo;
        (void)r2;
        (void)r1;
        return -1.0;
}

static double dragonfly_get_compute_node_bandwidth(void* topo, cn_id_t node) {
        // TODO
        (void)topo;
        (void)node;
        return -1.0;
}

static int dragonfly_get_router_neighbor_count(void* topo, router_id_t r) {
        // TODO
        (void)topo;
        (void)r;
        return 0;
}

static void dragonfly_get_router_neighbor_list(void* topo, router_id_t r, router_id_t* neighbors) {
        // TODO
        (void)topo;
        (void)r;
        (void)neighbors;
}

static int dragonfly_get_router_location(void* topo, router_id_t r, int32_t* location, int size) {
        // TODO
        (void)topo;
        (void)r;
        (void)location;
        (void)size;
        return 0;
}

static int dragonfly_get_compute_node_location(void* topo, cn_id_t node, int32_t* location, int size) {
        // TODO
        (void)topo;
        (void)node;
        (void)location;
        (void)size;
        return 0;
}

static router_id_t dragonfly_get_router_from_compute_node(void* topo, cn_id_t node) {
        // TODO
        (void)topo;
        (void)node;
        return -1;
}

static int dragonfly_get_router_compute_node_count(void* topo, router_id_t r) {
        // TODO
        (void)topo;
        (void)r;
        return 0;
}

static void dragonfly_get_router_compute_node_list(void* topo, router_id_t r, cn_id_t* nodes) {
        // TODO
        (void)topo;
        (void)r;
        (void)nodes;
}

cortex_topology dragonfly_cortex_topology = {
        .internal = NULL,
		.get_number_of_routers			= dragonfly_get_number_of_routers,
		.get_number_of_compute_nodes	= dragonfly_get_number_of_compute_nodes,
        .get_router_link_bandwidth      = dragonfly_get_router_link_bandwidth,
        .get_compute_node_bandwidth     = dragonfly_get_compute_node_bandwidth,
        .get_router_neighbor_count      = dragonfly_get_router_neighbor_count,
        .get_router_neighbor_list       = dragonfly_get_router_neighbor_list,
        .get_router_location            = dragonfly_get_router_location,
        .get_compute_node_location      = dragonfly_get_compute_node_location,
        .get_router_from_compute_node   = dragonfly_get_router_from_compute_node,
        .get_router_compute_node_count  = dragonfly_get_router_compute_node_count,
        .get_router_compute_node_list   = dragonfly_get_router_compute_node_list,
};

#endif
