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
#include "codes/net/dragonfly.h"
#include "sys/file.h"
#include "codes/quickhash.h"
#include "codes/rc-stack.h"

#define CREDIT_SIZE 8
#define MEAN_PROCESS 1.0

/* collective specific parameters */
#define TREE_DEGREE 4
#define LEVEL_DELAY 1000
#define DRAGONFLY_COLLECTIVE_DEBUG 0
#define NUM_COLLECTIVES  1
#define COLLECTIVE_COMPUTATION_DELAY 5700
#define DRAGONFLY_FAN_OUT_DELAY 20.0
#define WINDOW_LENGTH 0
#define DFLY_HASH_TABLE_SIZE 65536

// debugging parameters
#define TRACK -1
#define TRACK_MSG -1
#define PRINT_ROUTER_TABLE 1
#define DEBUG 0
#define USE_DIRECT_SCHEME 1

#define LP_CONFIG_NM (model_net_lp_config_names[DRAGONFLY])
#define LP_METHOD_NM (model_net_method_names[DRAGONFLY])

long term_ecount, router_ecount, term_rev_ecount, router_rev_ecount;

static double maxd(double a, double b) { return a < b ? b : a; }

/* minimal and non-minimal packet counts for adaptive routing*/
static unsigned int minimal_count=0, nonmin_count=0;

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

typedef struct terminal_message_list terminal_message_list;
struct terminal_message_list {
    terminal_message msg;
    char* event_data;
    terminal_message_list *next;
    terminal_message_list *prev;
};

void init_terminal_message_list(terminal_message_list *this, 
    terminal_message *inmsg) {
    this->msg = *inmsg;
    this->event_data = NULL;
    this->next = NULL;
    this->prev = NULL;
}

void delete_terminal_message_list(terminal_message_list *this) {
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
    int radix;
    int total_routers;
    int total_terminals;
    int num_global_channels;
    double cn_delay;
    double local_delay;
    double global_delay;
    double credit_delay;
};

struct dfly_hash_key
{
    uint64_t message_id;
    tw_lpid sender_id;
};

struct dfly_qhash_entry
{
   struct dfly_hash_key key;
   char * remote_event_data;
   int num_chunks;
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
   const dragonfly_param *params;

   struct qhash_table *rank_tbl;
   uint64_t rank_tbl_pop;

   tw_stime   total_time;
   long total_msg_size;
   long finished_msgs;
   long finished_chunks;
   long finished_packets;

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
   unsigned int group_id;
  
   int* global_channel; 
   
   tw_stime* next_output_available_time;
   tw_stime* cur_hist_start_time;
   terminal_message_list ***pending_msgs;
   terminal_message_list ***pending_msgs_tail;
   terminal_message_list ***queued_msgs;
   terminal_message_list ***queued_msgs_tail;
   int *in_send_loop;
   
   int** vc_occupancy;
   int* link_traffic;

   const char * anno;
   const dragonfly_param *params;

   int* prev_hist_num;
   int* cur_hist_num;
};

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
    struct dfly_qhash_entry *tmp;

    tmp = qhash_entry(link, struct dfly_qhash_entry, hash_link);
    
    if (tmp->key.message_id == message_key->message_id
            && tmp->key.sender_id == message_key->sender_id)
        return 1;

    return 0;
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

/* returns the dragonfly message size */
static int dragonfly_get_msg_sz(void)
{
	   return sizeof(terminal_message);
}

static void free_tmp(void * ptr)
{
    struct dfly_qhash_entry * dfly = ptr; 
    free(dfly->remote_event_data);
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

static void create_prepend_to_terminal_message_list(
        terminal_message_list ** thisq,
        terminal_message_list ** thistail,
        int index, 
        terminal_message *msg) {
    terminal_message_list* new_entry = (terminal_message_list*)malloc(
        sizeof(terminal_message_list));
    init_terminal_message_list(new_entry, msg);
    if(msg->remote_event_size_bytes) {
        void *m_data = model_net_method_get_edata(DRAGONFLY, msg);
        size_t s = msg->remote_event_size_bytes + msg->local_event_size_bytes;
        new_entry->event_data = (void*)malloc(s);
        memcpy(new_entry->event_data, m_data, s);
    }
    prepend_to_terminal_message_list( thisq, thistail, index, new_entry);
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

static void copy_terminal_list_entry( terminal_message_list *cur_entry,
    terminal_message *msg) {
    terminal_message *cur_msg = &cur_entry->msg;
    msg->travel_start_time = cur_msg->travel_start_time;
    msg->packet_ID = cur_msg->packet_ID;    
    strcpy(msg->category, cur_msg->category);
    msg->final_dest_gid = cur_msg->final_dest_gid;
    msg->msg_start_time = msg->msg_start_time;
    msg->sender_lp = cur_msg->sender_lp;
    msg->dest_terminal_id = cur_msg->dest_terminal_id;
    msg->src_terminal_id = cur_msg->src_terminal_id;
    msg->local_id = cur_msg->local_id;
    msg->origin_router_id = cur_msg->origin_router_id;
    msg->my_N_hop = cur_msg->my_N_hop;
    msg->my_l_hop = cur_msg->my_l_hop;
    msg->my_g_hop = cur_msg->my_g_hop;
    msg->intm_lp_id = cur_msg->intm_lp_id;
    msg->saved_channel = cur_msg->saved_channel;
    msg->saved_vc = cur_msg->saved_vc;
    msg->last_hop = cur_msg->last_hop;
    msg->path_type = cur_msg->path_type;
    msg->vc_index = cur_msg->vc_index;
    msg->output_chan = cur_msg->output_chan;
    msg->is_pull = cur_msg->is_pull;
    msg->pull_size = cur_msg->pull_size;
    msg->intm_group_id = cur_msg->intm_group_id;
    msg->chunk_id = cur_msg->chunk_id;
    msg->sender_mn_lp = cur_msg->sender_mn_lp;
    msg->total_size = cur_msg->total_size;
    msg->packet_size = cur_msg->packet_size;
    msg->message_id = cur_msg->message_id;
    msg->local_event_size_bytes = cur_msg->local_event_size_bytes;
    msg->remote_event_size_bytes = cur_msg->remote_event_size_bytes;
    msg->sender_node = cur_msg->sender_node;
    msg->next_stop = cur_msg->next_stop;
    msg->magic = cur_msg->magic;

    if(msg->local_event_size_bytes +  msg->remote_event_size_bytes > 0) {
        void *m_data = model_net_method_get_edata(DRAGONFLY, msg);
        memcpy(m_data, cur_entry->event_data, 
            msg->local_event_size_bytes +  msg->remote_event_size_bytes);
    }
}
static void dragonfly_read_config(const char * anno, dragonfly_param *params){
    // shorthand
    dragonfly_param *p = params;

    configuration_get_value_int(&config, "PARAMS", "num_routers", anno,
            &p->num_routers);
    if(p->num_routers <= 0) {
        p->num_routers = 4;
        fprintf(stderr, "Number of dimensions not specified, setting to %d\n",
                p->num_routers);
    }

    p->num_vcs = 3;

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
    p->radix = (p->num_cn + p->num_global_channels + p->num_routers);
    p->total_routers = p->num_groups * p->num_routers;
    p->total_terminals = p->total_routers * p->num_cn;
    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    if(!rank) {
        printf("\n Total nodes %d routers %d groups %d radix %d \n",
                p->num_cn * p->total_routers, p->total_routers, p->num_groups,
                p->radix);
    }
    
    p->cn_delay = bytes_to_ns(p->chunk_size, p->cn_bandwidth);
    p->local_delay = bytes_to_ns(p->chunk_size, p->local_bandwidth);
    p->global_delay = bytes_to_ns(p->chunk_size, p->global_bandwidth);
    p->credit_delay = bytes_to_ns(8.0, p->local_bandwidth); //assume 8 bytes packet

}

static void dragonfly_configure(){
    anno_map = codes_mapping_get_lp_anno_map(LP_CONFIG_NM);
    assert(anno_map);
    num_params = anno_map->num_annos + (anno_map->has_unanno_lp > 0);
    all_params = malloc(num_params * sizeof(*all_params));

    for (uint64_t i = 0; i < anno_map->num_annos; i++){
        const char * anno = anno_map->annotations[i].ptr;
        dragonfly_read_config(anno, &all_params[i]);
    }
    if (anno_map->has_unanno_lp > 0){
        dragonfly_read_config(NULL, &all_params[anno_map->num_annos]);
    }
}

/* report dragonfly statistics like average and maximum packet latency, average number of hops traversed */
static void dragonfly_report_stats()
{
   long long avg_hops, total_finished_packets, total_finished_chunks;
   long long total_finished_msgs, final_msg_sz;
   tw_stime avg_time, max_time;
   int total_minimal_packets, total_nonmin_packets;

   MPI_Reduce( &total_hops, &avg_hops, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
   MPI_Reduce( &N_finished_packets, &total_finished_packets, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
   MPI_Reduce( &N_finished_msgs, &total_finished_msgs, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
   MPI_Reduce( &N_finished_chunks, &total_finished_chunks, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
   MPI_Reduce( &total_msg_sz, &final_msg_sz, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
   MPI_Reduce( &dragonfly_total_time, &avg_time, 1,MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
   MPI_Reduce( &dragonfly_max_latency, &max_time, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);
   if(routing == ADAPTIVE || routing == PROG_ADAPTIVE)
    {
	MPI_Reduce(&minimal_count, &total_minimal_packets, 1, MPI_INT, MPI_SUM, 0, MPI_COMM_WORLD);
 	MPI_Reduce(&nonmin_count, &total_nonmin_packets, 1, MPI_INT, MPI_SUM, 0, MPI_COMM_WORLD);
    }

   /* print statistics */
   if(!g_tw_mynode)
   {	
      printf(" Average number of hops traversed %f average chunk latency %lf us maximum chunk latency %lf us avg message size %lf bytes finished messages %ld \n", (float)avg_hops/total_finished_chunks, avg_time/(total_finished_chunks*1000), max_time/1000, (float)final_msg_sz/total_finished_msgs, total_finished_msgs);
     if(routing == ADAPTIVE || routing == PROG_ADAPTIVE)
              printf("\n ADAPTIVE ROUTING STATS: %d percent chunks routed minimally %d percent chunks routed non-minimally completed packets %lld ", total_minimal_packets, total_nonmin_packets, total_finished_chunks);
 
  }
   return;
}

void dragonfly_collective_init(terminal_state * s,
           		   tw_lp * lp)
{
    // TODO: be annotation-aware
    codes_mapping_get_lp_info(lp->gid, lp_group_name, &mapping_grp_id, NULL,
            &mapping_type_id, NULL, &mapping_rep_id, &mapping_offset);
    int num_lps = codes_mapping_get_lp_count(lp_group_name, 1, LP_CONFIG_NM,
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
        if(next_child < (num_lps * num_reps))
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
void 
terminal_init( terminal_state * s, 
	       tw_lp * lp )
{
    uint32_t h1 = 0, h2 = 0; 
    bj_hashlittle2(LP_METHOD_NM, strlen(LP_METHOD_NM), &h1, &h2);
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

   int num_lps = codes_mapping_get_lp_count(lp_group_name, 1, LP_CONFIG_NM,
           s->anno, 0);

   s->terminal_id = (mapping_rep_id * num_lps) + mapping_offset;  
   s->router_id=(int)s->terminal_id / (s->params->num_routers/2);
   s->terminal_available_time = 0.0;
   s->packet_counter = 0;
   
   s->finished_msgs = 0;
   s->finished_chunks = 0;
   s->finished_packets = 0;
   s->total_time = 0.0;
   s->total_msg_size = 0;

   rc_stack_create(&s->st);
   s->num_vcs = 1;
   s->vc_occupancy = (int*)malloc(s->num_vcs * sizeof(int));

   for( i = 0; i < s->num_vcs; i++ )
    {
      s->vc_occupancy[i]=0;
    }

   s->rank_tbl = qhash_init(dragonfly_rank_hash_compare, quickhash_64bit_hash, DFLY_HASH_TABLE_SIZE);

   if(!s->rank_tbl)
       tw_error(TW_LOC, "\n Hash table not initialized! ");

   s->terminal_msgs = 
       (terminal_message_list**)malloc(1*sizeof(terminal_message_list*));
   s->terminal_msgs_tail = 
       (terminal_message_list**)malloc(1*sizeof(terminal_message_list*));
   s->terminal_msgs[0] = NULL;
   s->terminal_msgs_tail[0] = NULL;
   s->terminal_length = 0;
   s->in_send_loop = 0;
   s->issueIdle = 0;

   dragonfly_collective_init(s, lp);
   return;
}


/* sets up the router virtual channels, global channels, 
 * local channels, compute node channels */
void router_setup(router_state * r, tw_lp * lp)
{
    uint32_t h1 = 0, h2 = 0; 
    bj_hashlittle2(LP_METHOD_NM, strlen(LP_METHOD_NM), &h1, &h2);
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

    // shorthand
    const dragonfly_param *p = r->params;

   r->router_id=mapping_rep_id + mapping_offset;
   r->group_id=r->router_id/p->num_routers;

   r->global_channel = (int*)malloc(p->num_global_channels * sizeof(int));
   r->next_output_available_time = (tw_stime*)malloc(p->radix * sizeof(tw_stime));
   r->cur_hist_start_time = (tw_stime*)malloc(p->radix * sizeof(tw_stime));
   r->link_traffic = (int*)malloc(p->radix * sizeof(int));
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
  
   for(int i=0; i < p->radix; i++)
    {
       // Set credit & router occupancy
	r->next_output_available_time[i]=0;
	r->cur_hist_start_time[i] = 0;
        r->link_traffic[i]=0;
	r->cur_hist_num[i] = 0;
	r->prev_hist_num[i] = 0;
        
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
void router_credit_send(router_state * s, tw_bf * bf, terminal_message * msg, 
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
    buf_e = tw_event_new(dest, ts , lp);
    buf_msg = tw_event_data(buf_e);
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

void packet_generate_rc(terminal_state * s, tw_bf * bf, terminal_message * msg, tw_lp * lp)
{
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
      }
     struct mn_stats* stat;
     stat = model_net_find_stats(msg->category, s->dragonfly_stats_array);
     stat->send_count--;
     stat->send_bytes -= msg->packet_size;
     stat->send_time -= (1/s->params->cn_bandwidth) * msg->packet_size;
}

/* generates packet at the current dragonfly compute node */
void packet_generate(terminal_state * s, tw_bf * bf, terminal_message * msg, 
  tw_lp * lp) {
  
  tw_stime ts, nic_ts;

  assert(lp->gid != msg->dest_terminal_id);
  const dragonfly_param *p = s->params;

  int i, total_event_size;
  int num_chunks = msg->packet_size / p->chunk_size;
  if (msg->packet_size % s->params->chunk_size) 
      num_chunks++;

  if(!num_chunks)
    num_chunks = 1;

  nic_ts = g_tw_lookahead + s->params->cn_delay * msg->packet_size + tw_rand_unif(lp->rng);
  
  msg->packet_ID = lp->gid + g_tw_nlp * s->packet_counter;
  msg->my_N_hop = 0;
  msg->my_l_hop = 0;
  msg->my_g_hop = 0;
  msg->intm_group_id = -1;

  if(msg->packet_ID == TRACK && msg->message_id == TRACK_MSG)
      printf("\n Packet generated at terminal %lu destination %d ", lp->gid, s->router_id);

  for(i = 0; i < num_chunks; i++)
  {
    terminal_message_list *cur_chunk = (terminal_message_list*)malloc(
      sizeof(terminal_message_list));
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
    append_to_terminal_message_list(s->terminal_msgs, s->terminal_msgs_tail,
      0, cur_chunk);
    s->terminal_length += s->params->chunk_size;
  }

  if(s->terminal_length < 2 * s->params->cn_vc_size) {
    model_net_method_idle_event(nic_ts, 0, lp);
  } else {
    bf->c11 = 1;
    s->issueIdle = 1;
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

void packet_send_rc(terminal_state * s, tw_bf * bf, terminal_message * msg,
        tw_lp * lp)
{
      if(bf->c1) {
        s->in_send_loop = 1;
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

      create_prepend_to_terminal_message_list(s->terminal_msgs,
          s->terminal_msgs_tail, 0, msg);
      if(bf->c3) {
        tw_rand_reverse_unif(lp->rng);
      }
      if(bf->c4) {
        s->in_send_loop = 1;
      }
      if(bf->c5)
      {
          codes_local_latency_reverse(lp);
          s->issueIdle = 1;
      }
      return;
}
/* sends the packet from the current dragonfly compute node to the attached router */
void packet_send(terminal_state * s, tw_bf * bf, terminal_message * msg, 
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
    //printf("[%d] Skipping send %d %d\n", lp->gid, cur_entry == NULL, 
    //  (s->vc_occupancy[0] + s->params->chunk_size > s->params->cn_vc_size));
    return;
  }

//  printf("\n Packet %ld sent at time %lf ", cur_entry->msg.packet_ID, tw_now(lp));
  msg->saved_available_time = s->terminal_available_time;
  ts = g_tw_lookahead + s->params->cn_delay + tw_rand_unif(lp->rng);
  s->terminal_available_time = maxd(s->terminal_available_time, tw_now(lp));
  s->terminal_available_time += ts;

  //TODO: be annotation-aware
  codes_mapping_get_lp_info(lp->gid, lp_group_name, &mapping_grp_id, NULL,
      &mapping_type_id, NULL, &mapping_rep_id, &mapping_offset);
  codes_mapping_get_lp_id(lp_group_name, "dragonfly_router", NULL, 1,
      s->router_id, 0, &router_id);
  // we are sending an event to the router, so no method_event here
  e = tw_event_new(router_id, s->terminal_available_time - tw_now(lp), lp);
  m = tw_event_data(e);
  memcpy(m, &cur_entry->msg, sizeof(terminal_message));
  if (m->remote_event_size_bytes){
    memcpy(model_net_method_get_edata(DRAGONFLY, m), cur_entry->event_data,
        m->remote_event_size_bytes);
  }

  m->origin_router_id = s->router_id;
  m->type = R_ARRIVE;
  m->src_terminal_id = lp->gid;
  m->vc_index = 0;
  m->last_hop = TERMINAL;
  m->intm_group_id = -1;
  m->magic = router_magic_num;
  m->path_type = -1;
  m->local_event_size_bytes = 0;
  m->local_id = s->terminal_id;
  tw_event_send(e);

  int num_chunks = cur_entry->msg.packet_size/s->params->chunk_size;
  if(cur_entry->msg.packet_size % s->params->chunk_size)
    num_chunks++;

  if(!num_chunks)
      num_chunks = 1;

  if(cur_entry->msg.chunk_id == num_chunks - 1 && 
      (cur_entry->msg.local_event_size_bytes > 0)) {
    bf->c2 = 1;
    ts = codes_local_latency(lp); 
    tw_event *e_new = tw_event_new(cur_entry->msg.sender_lp, ts, lp);
    terminal_message* m_new = tw_event_data(e_new);
    void *local_event = (char*)cur_entry->event_data + 
      cur_entry->msg.remote_event_size_bytes;
    memcpy(m_new, local_event, cur_entry->msg.local_event_size_bytes);
    tw_event_send(e_new);
  }
  s->packet_counter++;
  s->vc_occupancy[0] += s->params->chunk_size;
  cur_entry = return_head(s->terminal_msgs, s->terminal_msgs_tail, 0); 
  copy_terminal_list_entry(cur_entry, msg);
  delete_terminal_message_list(cur_entry);
  s->terminal_length -= s->params->chunk_size;

  cur_entry = s->terminal_msgs[0];
  
  if(cur_entry != NULL &&
    s->vc_occupancy[0] + s->params->chunk_size <= s->params->cn_vc_size) {
    bf->c3 = 1;
    terminal_message *m;
    ts = g_tw_lookahead + s->params->cn_delay + tw_rand_unif(lp->rng);
    tw_event* e = model_net_method_event_new(lp->gid, ts, lp, DRAGONFLY, 
      (void**)&m, NULL);
    m->type = T_SEND;
    m->magic = terminal_magic_num;
    tw_event_send(e);
  } else {
    bf->c4 = 1;
    s->in_send_loop = 0;
  }
  if(s->issueIdle) {
    bf->c5 = 1;
    s->issueIdle = 0;
    model_net_method_idle_event(codes_local_latency(lp), 0, lp);
  }
  return;
}

void packet_arrive_rc(terminal_state * s, tw_bf * bf, terminal_message * msg, tw_lp * lp)
{
      tw_rand_reverse_unif(lp->rng);
      if(msg->path_type == MINIMAL)
        minimal_count--;
      if(msg->path_type == NON_MINIMAL)
        nonmin_count--;

      N_finished_chunks--;
      s->finished_chunks--;

      total_hops -= msg->my_N_hop;
       dragonfly_total_time -= (tw_now(lp) - msg->travel_start_time);
       s->total_time = msg->saved_avg_time;
      
      /*if(msg->chunk_id == num_chunks - 1)
      {
        mn_stats* stat;
        stat = model_net_find_stats(msg->category, s->dragonfly_stats_array);
        stat->recv_count--;
        stat->recv_bytes -= msg->packet_size;
        stat->recv_time -= tw_now(lp) - msg->travel_start_time;


        N_finished_packets--;

        dragonfly_total_time -= (tw_now(lp) - msg->travel_start_time);

        if(bf->c3)
            dragonfly_max_latency = msg->saved_available_time;
      }
      if (msg->chunk_id == num_chunks-1 &&
              msg->remote_event_size_bytes &&
              msg->is_pull)
      {
        int net_id = model_net_get_id(LP_METHOD_NM);
        model_net_event_rc(net_id, lp, msg->pull_size);
      }*/
      struct qhash_head * hash_link = NULL;
      struct dfly_qhash_entry * tmp = NULL; 
      
      struct dfly_hash_key key;
      key.message_id = msg->message_id;
      key.sender_id = msg->sender_lp;
      
      hash_link = qhash_search(s->rank_tbl, &key);
      tmp = qhash_entry(hash_link, struct dfly_qhash_entry, hash_link);
      
      mn_stats* stat;
      stat = model_net_find_stats(msg->category, s->dragonfly_stats_array);
      stat->recv_time -= (tw_now(lp) - msg->travel_start_time);

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
            s->finished_msgs--;
            total_msg_sz -= msg->total_size;
            N_finished_msgs--;
            s->total_msg_size -= msg->total_size;

            struct dfly_qhash_entry * d_entry_pop = (struct dfly_qhash_entry*)rc_stack_pop(s->st);
            qhash_add(s->rank_tbl, &key, &(d_entry_pop->hash_link));
            s->rank_tbl_pop++; 

            hash_link = qhash_search(s->rank_tbl, &key);
            tmp = qhash_entry(hash_link, struct dfly_qhash_entry, hash_link);
          

            if(bf->c4)
                model_net_event_rc2(lp, &msg->event_rc);
        }
      
       assert(tmp);
       tmp->num_chunks--;

       return;
}
void send_remote_event(terminal_state * s, terminal_message * msg, tw_lp * lp, tw_bf * bf, char * event_data, int remote_event_size)
{
        void * tmp_ptr = model_net_method_get_edata(DRAGONFLY, msg);
        tw_stime ts = g_tw_lookahead + bytes_to_ns(msg->remote_event_size_bytes, (1/s->params->cn_bandwidth));

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
void packet_arrive(terminal_state * s, tw_bf * bf, terminal_message * msg, 
  tw_lp * lp) {


    // NIC aggregation - should this be a separate function?
    // Trigger an event on receiving server

  tw_stime ts = g_tw_lookahead + s->params->credit_delay + tw_rand_unif(lp->rng);
  
  // no method_event here - message going to router
  tw_event * buf_e;
  terminal_message * buf_msg;
  buf_e = tw_event_new(msg->intm_lp_id, ts, lp);
  buf_msg = tw_event_data(buf_e);
  buf_msg->magic = router_magic_num;
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

  /* WE do not allow self messages through dragonfly */
  assert(lp->gid != msg->src_terminal_id);

  int num_chunks = msg->packet_size / s->params->chunk_size;
  uint64_t total_chunks = msg->total_size / s->params->chunk_size;

  if(msg->total_size % s->params->chunk_size)
      total_chunks++;

  if(!total_chunks)
      total_chunks = 1;

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
    dragonfly_total_time += tw_now( lp ) - msg->travel_start_time;
    total_hops += msg->my_N_hop;
    
    mn_stats* stat = model_net_find_stats(msg->category, s->dragonfly_stats_array);
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

 /*tw_event * e;
 terminal_message * m;
 if(msg->chunk_id == num_chunks-1)
 {
    bf->c2 = 1;
    mn_stats* stat = model_net_find_stats(msg->category, s->dragonfly_stats_array);
    stat->recv_count++;
    stat->recv_bytes += msg->packet_size;
    stat->recv_time += tw_now(lp) - msg->travel_start_time;
    N_finished_packets++;
    total_hops -= msg->my_N_hop;

    dragonfly_total_time += tw_now( lp ) - msg->travel_start_time;
    if (dragonfly_max_latency < tw_now( lp ) - msg->travel_start_time)
    {
        bf->c3 = 1;
        msg->saved_available_time = dragonfly_max_latency;
        dragonfly_max_latency=tw_now( lp ) - msg->travel_start_time;
    }
    if(msg->remote_event_size_bytes)
    {
        void * tmp_ptr = model_net_method_get_edata(DRAGONFLY, msg);
        ts = g_tw_lookahead + 0.1 + (1/s->params->cn_bandwidth) * msg->remote_event_size_bytes;
        if (msg->is_pull){
            struct codes_mctx mc_dst =
                codes_mctx_set_global_direct(msg->sender_mn_lp);
            struct codes_mctx mc_src =
                codes_mctx_set_global_direct(lp->gid);
            int net_id = model_net_get_id(LP_METHOD_NM);
            model_net_event_mctx(net_id, &mc_src, &mc_dst, msg->category,
                    msg->sender_lp, msg->pull_size, ts,
                    msg->remote_event_size_bytes, tmp_ptr, 0, NULL, lp);
        }
        else {
            e = tw_event_new(msg->final_dest_gid, ts, lp);
            m = tw_event_data(e);
            memcpy(m, tmp_ptr, msg->remote_event_size_bytes);
            tw_event_send(e);
        }
    }
 }*/
   /* Now retreieve the number of chunks completed from the hash and update
    * them */
   void *m_data_src = model_net_method_get_edata(DRAGONFLY, msg);
   struct qhash_head *hash_link = NULL;
   struct dfly_qhash_entry * tmp = NULL;
   struct dfly_hash_key key;
   key.message_id = msg->message_id; 
   key.sender_id = msg->sender_lp;
  
   hash_link = qhash_search(s->rank_tbl, &key);
   tmp = qhash_entry(hash_link, struct dfly_qhash_entry, hash_link);

   /* If an entry does not exist then create one */
   if(!hash_link)
   {
       bf->c5 = 1;
       struct dfly_qhash_entry * d_entry = malloc(sizeof (struct dfly_qhash_entry));
       d_entry->num_chunks = 0;
       d_entry->key = key;
       d_entry->remote_event_data = NULL;
       d_entry->remote_event_size = 0;
       qhash_add(s->rank_tbl, &key, &(d_entry->hash_link));
       s->rank_tbl_pop++;
       
       hash_link = qhash_search(s->rank_tbl, &key);
       tmp = qhash_entry(hash_link, struct dfly_qhash_entry, hash_link);
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
        if (dragonfly_max_latency < tw_now( lp ) - msg->travel_start_time) {
          bf->c3 = 1;
          msg->saved_available_time = dragonfly_max_latency;
          dragonfly_max_latency = tw_now( lp ) - msg->travel_start_time;
        }
    /* If all chunks of a message have arrived then send a remote event to the
     * callee*/
    if(tmp->num_chunks >= total_chunks)
    {
        bf->c7 = 1;

        N_finished_msgs++;
        total_msg_sz += msg->total_size;
        s->total_msg_size += msg->total_size;
        s->finished_msgs++;
        
        assert(tmp->remote_event_data && tmp->remote_event_size);
        send_remote_event(s, msg, lp, bf, tmp->remote_event_data, tmp->remote_event_size);
        /* Remove the hash entry */
        qhash_del(hash_link);
        rc_stack_push(lp, tmp, free_tmp, s->st);
        s->rank_tbl_pop--;
   }
  return;
}

/* collective operation for the torus network */
void dragonfly_collective(char const * category, int message_size, int remote_event_size, const void* remote_event, tw_lp* sender)
{
    tw_event * e_new;
    tw_stime xfer_to_nic_time;
    terminal_message * msg;
    tw_lpid local_nic_id;
    char* tmp_ptr;

    codes_mapping_get_lp_info(sender->gid, lp_group_name, &mapping_grp_id,
            NULL, &mapping_type_id, NULL, &mapping_rep_id, &mapping_offset);
    codes_mapping_get_lp_id(lp_group_name, LP_CONFIG_NM, NULL, 1,
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
void dragonfly_collective_rc(int message_size, tw_lp* sender)
{
     codes_local_latency_reverse(sender);
     return;
}

static void send_collective_remote_event(terminal_state * s,
                        tw_bf * bf,
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
            e = codes_event_new(s->origin_svr, ts, lp);
            m = tw_event_data(e);
            char* tmp_ptr = (char*)msg;
            tmp_ptr += dragonfly_get_msg_sz();
            memcpy(m, tmp_ptr, msg->remote_event_size_bytes);
            tw_event_send(e);
     }
}

static void node_collective_init(terminal_state * s,
                        tw_bf * bf,
                        terminal_message * msg,
                        tw_lp * lp)
{
        tw_event * e_new;
        tw_lpid parent_nic_id;
        tw_stime xfer_to_nic_time;
        terminal_message * msg_new;
        int num_lps;

        msg->saved_collective_init_time = s->collective_init_time;
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
            num_lps = codes_mapping_get_lp_count(lp_group_name, 1, LP_CONFIG_NM,
                    s->anno, 0);
            codes_mapping_get_lp_id(lp_group_name, LP_CONFIG_NM, s->anno, 0,
                    s->parent_node_id/num_lps, (s->parent_node_id % num_lps),
                    &parent_nic_id);

           /* send a message to the parent that the LP has entered the collective operation */
            xfer_to_nic_time = g_tw_lookahead + LEVEL_DELAY;
            //e_new = codes_event_new(parent_nic_id, xfer_to_nic_time, lp);
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
        int num_lps = codes_mapping_get_lp_count(lp_group_name, 1, LP_CONFIG_NM,
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
            codes_mapping_get_lp_id(lp_group_name, LP_CONFIG_NM, s->anno, 0,
                    s->parent_node_id/num_lps, (s->parent_node_id % num_lps),
                    &parent_nic_id);

           /* send a message to the parent that the LP has entered the collective operation */
            //e_new = codes_event_new(parent_nic_id, xfer_to_nic_time, lp);
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
           send_collective_remote_event(s, bf, msg, lp);

           for( i = 0; i < s->num_children; i++ )
           {
                tw_lpid child_nic_id;
                /* Do some computation and fan out immediate child nodes from the collective */
                xfer_to_nic_time = g_tw_lookahead + COLLECTIVE_COMPUTATION_DELAY + LEVEL_DELAY + tw_rand_exponential(lp->rng, (double)LEVEL_DELAY/50);

                /* get global LP ID of the child node */
                codes_mapping_get_lp_id(lp_group_name, LP_CONFIG_NM, NULL, 1,
                        s->children[i]/num_lps, (s->children[i] % num_lps),
                        &child_nic_id);
                //e_new = codes_event_new(child_nic_id, xfer_to_nic_time, lp);

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
        int num_lps = codes_mapping_get_lp_count(lp_group_name, 1, LP_CONFIG_NM,
                NULL, 1);
        bf->c1 = 0;
        bf->c2 = 0;

        send_collective_remote_event(s, bf, msg, lp);

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
                        codes_mapping_get_lp_id(lp_group_name, LP_CONFIG_NM,
                                s->anno, 0, s->children[i]/num_lps,
                                (s->children[i] % num_lps), &child_nic_id);
                        //e_new = codes_event_new(child_nic_id, xfer_to_nic_time, lp);
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


                        msg_new->type = D_COLLECTIVE_FAN_OUT;
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

void terminal_buf_update_rc(terminal_state * s,
		    tw_bf * bf, 
		    terminal_message * msg, 
		    tw_lp * lp)
{
      s->vc_occupancy[0] += s->params->chunk_size;
      codes_local_latency_reverse(lp);
      if(bf->c1) {
        s->in_send_loop = 0;
      }

      return;
}
/* update the compute node-router channel buffer */
void 
terminal_buf_update(terminal_state * s, 
		    tw_bf * bf, 
		    terminal_message * msg, 
		    tw_lp * lp)
{
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
  else if(s->in_send_loop == 0 && s->terminal_msgs[0] == NULL)
  {
     bf->c2 = 1;
     model_net_method_idle_event(ts, 0, lp);
  }

  return;
}

void 
terminal_event( terminal_state * s, 
		tw_bf * bf, 
		terminal_message * msg, 
		tw_lp * lp )
{
  *(int *)bf = (int)0;
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
      node_collective_init(s, bf, msg, lp);
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

void 
dragonfly_terminal_final( terminal_state * s, 
      tw_lp * lp )
{
	model_net_print_stats(lp->gid, s->dragonfly_stats_array);
    
    int written = 0;
    if(!s->terminal_id)
        written = sprintf(s->output_buf, "# Format <LP id> <Terminal ID> <Total Data Size> <Total Time Spent> <# Msgs finished> <# Packets finished> <# Chunks finished>\n");

    written += sprintf(s->output_buf + written, "%lu %u %ld %lf %ld %ld\n", lp->gid, s->terminal_id, s->total_msg_size, s->total_time, s->finished_msgs, s->finished_packets, s->finished_chunks);
    lp_io_write(lp->gid, "dragonfly-msg-stats", written, s->output_buf); 

    if(s->terminal_msgs[0] != NULL) 
      printf("[%lu] leftover terminal messages \n", lp->gid);


    qhash_finalize(s->rank_tbl);
    rc_stack_destroy(s->st);
    free(s->vc_occupancy);
    free(s->terminal_msgs);
    free(s->terminal_msgs_tail);
    free(s->children);
}

void dragonfly_router_final(router_state * s,
		tw_lp * lp)
{
   free(s->global_channel);
    /*char *stats_file = getenv("TRACER_LINK_FILE");
    if(stats_file != NULL) {
        FILE *fout = fopen(stats_file, "a");
        const dragonfly_param *p = s->params;
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
      for(j = 0; j < 3; j++) {
        if(s->queued_msgs[i][j] != NULL) {
          printf("[%lu] leftover queued messages %d %d %d\n", lp->gid, i, j,
          s->vc_occupancy[i][j]);
        }
        if(s->pending_msgs[i][j] != NULL) {
          printf("[%lu] lefover pending messages %d %d\n", lp->gid, i, j);
        }
      }
    }
}

/* Get the number of hops for this particular path source and destination groups */
int get_num_hops(int local_router_id,
		 int dest_router_id,
		 int num_routers,
		 int non_min,
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
     tw_lpid src_connecting_router = getRouterFromGroupID(dest_group_id, 
        local_grp_id, num_routers, total_groups);

     if(src_connecting_router == local_router_id)		
		num_hops--;

     tw_lpid dest_connecting_router = getRouterFromGroupID(local_grp_id, 
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
		      tw_bf * bf, 
		      terminal_message * msg, 
		      tw_lp * lp, 
		      int path,
		      int dest_router_id,
		      int intm_id)
{
   int dest_lp;
   tw_lpid router_dest_id = -1;
   int dest_group_id;

   codes_mapping_get_lp_info(lp->gid, lp_group_name, &mapping_grp_id, NULL,
           &mapping_type_id, NULL, &mapping_rep_id, &mapping_offset);
   int local_router_id = (mapping_offset + mapping_rep_id);

   dest_group_id = dest_router_id / s->params->num_routers;

  /* If the packet has arrived at the destination router */
   if(dest_router_id == local_router_id)
    {
        dest_lp = msg->dest_terminal_id;
        return dest_lp;
    }
   /* Generate inter-mediate destination for non-minimal routing (selecting a random group) */
   if(msg->last_hop == TERMINAL && path == NON_MINIMAL)
    {
      if(dest_group_id != s->group_id)
    	    msg->intm_group_id = intm_id;
    }
   /******************** DECIDE THE DESTINATION GROUP ***********************/
  /* It means that the packet has arrived at the inter-mediate group for non-minimal routing. Reset the group now. */
   if(path == NON_MINIMAL && msg->intm_group_id == s->group_id)
   {  
           msg->intm_group_id = -1;//no inter-mediate group
   } 
  /* Intermediate group ID is set. Divert the packet to the intermediate group. */
  if(path == NON_MINIMAL && msg->intm_group_id >= 0 &&
          (dest_group_id != s->group_id))
   {
      dest_group_id = msg->intm_group_id;
   }
 
/********************** DECIDE THE ROUTER IN THE DESTINATION GROUP ***************/ 
  /* It means the packet has arrived at the destination group. Now divert it to the destination router. */
  if(s->group_id == dest_group_id)
   {
     if(msg->last_hop == TERMINAL && path == NON_MINIMAL) {
       dest_lp = (s->group_id * s->params->num_routers) + intm_id % s->params->num_routers;
     } else {
     dest_lp = dest_router_id;
     }
   }
   else
   {
      /* Packet is at the source or intermediate group. Find a router that has a path to the destination group. */
      dest_lp=getRouterFromGroupID(dest_group_id, 
        s->router_id/s->params->num_routers, s->params->num_routers, 
        s->params->num_groups);
  
      if(dest_lp == local_router_id)
      {
#if USE_DIRECT_SCHEME
       //   printf("[%d] tg %d orig \n", lp->gid, target_grp, dest_group_id);
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
   }
  codes_mapping_get_lp_id(lp_group_name, "dragonfly_router", s->anno, 0, dest_lp,
          0, &router_dest_id);

  return router_dest_id;
}
/* gets the output port corresponding to the next stop of the message */
int 
get_output_port( router_state * s, 
		tw_bf * bf, 
		terminal_message * msg, 
		tw_lp * lp, 
		int next_stop )
{
  int output_port = -1, terminal_id;
  codes_mapping_get_lp_info(msg->dest_terminal_id, lp_group_name,
          &mapping_grp_id, NULL, &mapping_type_id, NULL, &mapping_rep_id,
          &mapping_offset);
  int num_lps = codes_mapping_get_lp_count(lp_group_name,1,LP_CONFIG_NM,s->anno,0);
  terminal_id = (mapping_rep_id * num_lps) + mapping_offset;

  if(next_stop == msg->dest_terminal_id)
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
//	      printf("\n output port not found %d next stop %d local router id %d group id %d intm grp id %d %d", output_port, next_stop, local_router_id, s->group_id, intm_grp_id, local_router_id%num_routers);
    }
    return output_port;
}


/* UGAL (first condition is from booksim), output port equality check comes from Dally dragonfly'09*/
static int do_adaptive_routing( router_state * s,
				 tw_bf * bf,
				 terminal_message * msg,
				 tw_lp * lp,
				 int dest_router_id,
				 int intm_id) {
  int next_stop;
  int minimal_out_port = -1, nonmin_out_port = -1;
  // decide which routing to take
  // get the queue occupancy of both the minimal and non-minimal output ports 
  int minimal_next_stop=get_next_stop(s, bf, msg, lp, MINIMAL, dest_router_id, -1);
  minimal_out_port = get_output_port(s, bf, msg, lp, minimal_next_stop);
  int nonmin_next_stop = get_next_stop(s, bf, msg, lp, NON_MINIMAL, dest_router_id, intm_id);
  nonmin_out_port = get_output_port(s, bf, msg, lp, nonmin_next_stop);
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
  int min_port_count = s->vc_occupancy[minimal_out_port][min_vc];

  // Now get the expected number of hops to be traversed for both routes 
  int num_min_hops = get_num_hops(s->router_id, dest_router_id, 
      s->params->num_routers, 0, s->params->num_groups);

  int intm_router_id = getRouterFromGroupID(intm_id, 
      s->router_id / s->params->num_routers, s->params->num_routers, 
      s->params->num_groups);

  int num_nonmin_hops = get_num_hops(s->router_id, intm_router_id, 
      s->params->num_routers, 1, s->params->num_groups) + 
    get_num_hops(intm_router_id, dest_router_id, s->params->num_routers, 1, 
        s->params->num_groups);

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

  int min_out_chan = minimal_out_port;
  int nonmin_out_chan = nonmin_out_port;

  /* Adding history window approach, not taking the queue status at every 
   * simulation time thats why, we are maintaining the current history 
   * window number and an average of the previous history window number. */
  int min_hist_count = s->cur_hist_num[min_out_chan] + 
    (s->prev_hist_num[min_out_chan]/2);
  int nonmin_hist_count = s->cur_hist_num[nonmin_out_chan] + 
    (s->prev_hist_num[min_out_chan]/2);

  int nonmin_port_count = s->vc_occupancy[nonmin_out_port][nomin_vc];
  if(num_min_hops * (min_port_count - min_hist_count) <= (num_nonmin_hops * ((q_avg + 1) - nonmin_hist_count))) {
    //if(min_port_count <= nonmin_port_count) {
    msg->path_type = MINIMAL;
    next_stop = minimal_next_stop;
    msg->intm_group_id = -1;

    if(msg->packet_ID == TRACK && msg->message_id == TRACK_MSG)
      printf("\n (%lf) [Router %d] Packet %d routing minimally ", tw_now(lp), (int)lp->gid, (int)msg->packet_ID);
  }
  else
  {
    msg->path_type = NON_MINIMAL;
    next_stop = nonmin_next_stop;
    msg->intm_group_id = intm_id;

    if(msg->packet_ID == TRACK && msg->message_id == TRACK_MSG)
      printf("\n (%lf) [Router %d] Packet %d routing non-minimally ", tw_now(lp), (int)lp->gid, (int)msg->packet_ID);

  }
  return next_stop;
}

void router_packet_receive_rc(router_state * s,
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
        delete_terminal_message_list(return_tail(s->pending_msgs[output_port], 
          s->pending_msgs_tail[output_port], output_chan));
        s->vc_occupancy[output_port][output_chan] -= s->params->chunk_size;
        if(bf->c3) {
          codes_local_latency_reverse(lp);
          s->in_send_loop[output_port] = 0;
        }
      }
      if(bf->c4) {
        delete_terminal_message_list(return_tail(s->queued_msgs[output_port], 
          s->queued_msgs_tail[output_port], output_chan));
      }
}

/* Packet arrives at the router and a credit is sent back to the sending terminal/router */
void 
router_packet_receive( router_state * s, 
			tw_bf * bf, 
			terminal_message * msg, 
			tw_lp * lp )
{
  router_ecount++;

  bf->c2 = 0;
  bf->c3 = 0;
  bf->c4 = 0;

  tw_stime ts;

  int next_stop = -1, output_port = -1, output_chan = -1;

  codes_mapping_get_lp_info(msg->dest_terminal_id, lp_group_name,
      &mapping_grp_id, NULL, &mapping_type_id, NULL, &mapping_rep_id,
      &mapping_offset); 
  int num_lps = codes_mapping_get_lp_count(lp_group_name, 1, LP_CONFIG_NM,
      s->anno, 0);
  int dest_router_id = (mapping_offset + (mapping_rep_id * num_lps)) / 
    s->params->num_cn;
  int intm_id = tw_rand_integer(lp->rng, 0, s->params->num_groups - 1);  
  int local_grp_id = s->router_id / s->params->num_routers;
  if(intm_id == local_grp_id) 
    intm_id = (local_grp_id + 2) % s->params->num_groups;

  /* progressive adaptive routing makes a check at every node/router at the 
   * source group to sense congestion. Once it does and decides on taking 
   * non-minimal path, it does not check any longer. */
  if(routing == PROG_ADAPTIVE
      && msg->path_type != NON_MINIMAL
      && local_grp_id == ( msg->origin_router_id / s->params->num_routers)) {
    next_stop = do_adaptive_routing(s, bf, msg, lp, dest_router_id, intm_id);	
  } else if(msg->last_hop == TERMINAL && routing == ADAPTIVE) {
    next_stop = do_adaptive_routing(s, bf, msg, lp, dest_router_id, intm_id);
  } else {
    if(routing == MINIMAL || routing == NON_MINIMAL)	
      msg->path_type = routing; /*defaults to the routing algorithm if we 
                                don't have adaptive routing here*/
    next_stop = get_next_stop(s, bf, msg, lp, msg->path_type, dest_router_id, 
      intm_id);
  }
  assert(msg->path_type == MINIMAL || msg->path_type == NON_MINIMAL);
  terminal_message_list * cur_chunk = (terminal_message_list *)malloc( 
      sizeof(terminal_message_list));
  init_terminal_message_list(cur_chunk, msg);
  if(msg->remote_event_size_bytes > 0) {
    void *m_data_src = model_net_method_get_edata(DRAGONFLY, msg);
    cur_chunk->event_data = (char*)malloc(msg->remote_event_size_bytes);
    memcpy(cur_chunk->event_data, m_data_src, msg->remote_event_size_bytes);
  }
   
  output_port = get_output_port(s, bf, msg, lp, next_stop); 
  output_chan = 0;
  int max_vc_size = s->params->cn_vc_size;

  cur_chunk->msg.vc_index = output_port;
  cur_chunk->msg.next_stop = next_stop;

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
    router_credit_send(s, bf, msg, lp, -1);
    append_to_terminal_message_list( s->pending_msgs[output_port], 
      s->pending_msgs_tail[output_port], output_chan, cur_chunk);
    s->vc_occupancy[output_port][output_chan] += s->params->chunk_size;
    if(s->in_send_loop[output_port] == 0) {
      bf->c3 = 1;
      terminal_message *m;
      ts = codes_local_latency(lp); 
      tw_event *e = tw_event_new(lp->gid, ts, lp);
      m = tw_event_data(e);
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
  }

  msg->saved_vc = output_port;
  msg->saved_channel = output_chan;
  return;
}

void router_packet_send_rc(router_state * s, 
		    tw_bf * bf, 
		     terminal_message * msg, tw_lp * lp)
{
    router_ecount--;
    router_rev_ecount++;
    
    int output_port = msg->vc_index;
    int output_chan = msg->output_chan;
    if(bf->c1) {
        s->in_send_loop[output_port] = 1;
        return;  
    }
      
    tw_rand_reverse_unif(lp->rng);
      
    s->next_output_available_time[output_port] = msg->saved_available_time;
    s->link_traffic[output_port] -= s->params->chunk_size;
    create_prepend_to_terminal_message_list(s->pending_msgs[output_port],
          s->pending_msgs_tail[output_port], output_chan, msg);

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
void 
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

  if(cur_entry == NULL) {
    bf->c1 = 1;
    s->in_send_loop[output_port] = 0;
    //printf("[%d] Router skipping send at begin %d \n", lp->gid, output_port);
    return;
  }

  //if(msg->packet_ID == TRACK && msg->message_id == TRACK_MSG)
  //    printf("\n packet sending origin %ld to router %ld at output port %d ", cur_entry->msg.src_terminal_id, cur_entry->msg.next_stop, output_port);
  
  int to_terminal = 1, global = 0;
  double delay = s->params->cn_delay;
  
  if(output_port < s->params->num_routers) {
    to_terminal = 0;
    delay = s->params->global_delay;
  } else if(output_port < s->params->num_routers + 
    s->params->num_global_channels) {
    to_terminal = 0;
    global = 1;
    delay = s->params->global_delay;
  }

  ts = g_tw_lookahead + delay + tw_rand_unif(lp->rng);

  msg->saved_available_time = s->next_output_available_time[output_port];
  s->next_output_available_time[output_port] = 
    maxd(s->next_output_available_time[output_port], tw_now(lp));
  s->next_output_available_time[output_port] += ts;

  // dest can be a router or a terminal, so we must check
  void * m_data;
  if (to_terminal) {
    assert(cur_entry->msg.next_stop == cur_entry->msg.dest_terminal_id);
    e = model_net_method_event_new(cur_entry->msg.next_stop, 
        s->next_output_available_time[output_port] - tw_now(lp), lp,
        DRAGONFLY, (void**)&m, &m_data);
  } else {
    e = tw_event_new(cur_entry->msg.next_stop, 
      s->next_output_available_time[output_port] - tw_now(lp), lp);
    m = tw_event_data(e);
    m_data = model_net_method_get_edata(DRAGONFLY, m);
  }
  memcpy(m, &cur_entry->msg, sizeof(terminal_message));
  if (m->remote_event_size_bytes){
    memcpy(m_data, cur_entry->event_data, m->remote_event_size_bytes);
  }

  if(global)
    m->last_hop = GLOBAL;
  else
    m->last_hop = LOCAL;

  m->local_id = s->router_id;
  m->intm_lp_id = lp->gid;
  m->magic = router_magic_num;

  s->link_traffic[output_port] += s->params->chunk_size;

  if(routing == PROG_ADAPTIVE)
  {
      if(tw_now(lp) - s->cur_hist_start_time[output_port] >= WINDOW_LENGTH) {
        bf->c2 = 1;
        s->prev_hist_num[output_port] = s->cur_hist_num[output_port];
        s->cur_hist_start_time[output_port] = tw_now(lp);
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
  copy_terminal_list_entry(cur_entry, msg);
  delete_terminal_message_list(cur_entry);
  
  cur_entry = s->pending_msgs[output_port][2];
  if(cur_entry == NULL) cur_entry = s->pending_msgs[output_port][1];
  if(cur_entry == NULL) cur_entry = s->pending_msgs[output_port][0];
  if(cur_entry != NULL) {
    bf->c3 = 1;
    terminal_message *m;
    ts = g_tw_lookahead + delay + tw_rand_unif(lp->rng);
    tw_event *e = tw_event_new(lp->gid, ts, lp);
    m = tw_event_data(e);
    m->type = R_SEND;
    m->magic = router_magic_num;
    m->vc_index = output_port;
    tw_event_send(e);
  } else {
    bf->c4 = 1;
    s->in_send_loop[output_port] = 0;
  }
  return;
}

void router_buf_update_rc(router_state * s,
        tw_bf * bf,
        terminal_message * msg,
        tw_lp * lp)
{
      int indx = msg->vc_index;
      int output_chan = msg->output_chan;
      s->vc_occupancy[indx][output_chan] += s->params->chunk_size;
      if(bf->c1) {
        terminal_message_list* head = return_tail(s->pending_msgs[indx],
            s->pending_msgs_tail[indx], output_chan);
        tw_rand_reverse_unif(lp->rng);
        prepend_to_terminal_message_list(s->queued_msgs[indx], 
            s->queued_msgs_tail[indx], output_chan, head);
        s->vc_occupancy[indx][output_chan] -= s->params->chunk_size;
      }
      if(bf->c2) {
        codes_local_latency_reverse(lp);
        s->in_send_loop[indx] = 0;
      }
}
/* Update the buffer space associated with this router LP */
void router_buf_update(router_state * s, tw_bf * bf, terminal_message * msg, tw_lp * lp)
{
  int indx = msg->vc_index;
  int output_chan = msg->output_chan;
  s->vc_occupancy[indx][output_chan] -= s->params->chunk_size;
  /*if(TRACK == msg->packet_ID)
  {
    int i;
    printf("\n channel %d occupancy ", output_chan);
    for(i = 0; i < s->params->radix; i++)
      printf(" %d ", s->vc_occupancy[i][output_chan]);
  }*/
  if(s->queued_msgs[indx][output_chan] != NULL) {
    bf->c1 = 1;
    terminal_message_list *head = return_head(s->queued_msgs[indx],
        s->queued_msgs_tail[indx], output_chan);
    router_credit_send(s, bf,  &head->msg, lp, 1); 
    append_to_terminal_message_list(s->pending_msgs[indx], 
      s->pending_msgs_tail[indx], output_chan, head);
    s->vc_occupancy[indx][output_chan] += s->params->chunk_size;
  }
  if(s->in_send_loop[indx] == 0 && s->pending_msgs[indx][output_chan] != NULL) {
    bf->c2 = 1;
    terminal_message *m;
    tw_stime ts = codes_local_latency(lp);
    tw_event *e = tw_event_new(lp->gid, ts, lp);
    m = tw_event_data(e);
    m->type = R_SEND;
    m->vc_index = indx;
    m->magic = router_magic_num;
    s->in_send_loop[indx] = 1;
    tw_event_send(e);
  }
  
  return;
}

void router_event(router_state * s, tw_bf * bf, terminal_message * msg, 
    tw_lp * lp) {
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
void terminal_rc_event_handler(terminal_state * s, tw_bf * bf, 
    terminal_message * msg, tw_lp * lp) {

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
                s->collective_init_time = msg->saved_collective_init_time;
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
void router_rc_event_handler(router_state * s, tw_bf * bf, 
  terminal_message * msg, tw_lp * lp) {
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
    (final_f) dragonfly_terminal_final,
    (map_f) codes_mapping,
    sizeof(terminal_state)
    },
   {
     (init_f) router_setup,
     (pre_run_f) NULL,
     (event_f) router_event,
     (revent_f) router_rc_event_handler,
     (final_f) dragonfly_router_final,
     (map_f) codes_mapping,
     sizeof(router_state),
   },
   {0},
};

/* returns the dragonfly lp type for lp registration */
static const tw_lptype* dragonfly_get_cn_lp_type(void)
{
	   return(&dragonfly_lps[0]);
}

static void dragonfly_register(tw_lptype *base_type) {
    lp_type_register(LP_CONFIG_NM, base_type);
    lp_type_register("dragonfly_router", &dragonfly_lps[1]);
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
    .mn_collective_call_rc = dragonfly_collective_rc   
};

