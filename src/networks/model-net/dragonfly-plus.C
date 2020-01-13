/*
 * Neil McGlohon - Rensselaer Polytechnic Institute
 * Original Dragonfly-Custom Base Code by Misbah Mubarak - Argonne National Labs
 *
 * Copyright (C) 2017 Rensselaer Polytechnic Institute.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#include <ross.h>

#define DEBUG_LP 892
#include <map>
#include <set>
#include <vector>
#include "codes/codes.h"
#include "codes/codes_mapping.h"
#include "codes/jenkins-hash.h"
#include "codes/model-net-lp.h"
#include "codes/model-net-method.h"
#include "codes/model-net.h"
#include "codes/net/dragonfly-plus.h"
#include "codes/quickhash.h"
#include "codes/rc-stack.h"
#include "sys/file.h"

#include "codes/connection-manager.h"

#ifdef ENABLE_CORTEX
#include <cortex/cortex.h>
#include <cortex/topology.h>
#endif

#define DEBUG_QOS 0
#define DUMP_CONNECTIONS 0
#define PRINT_CONFIG 1
#define T_ID 1
#define DFLY_HASH_TABLE_SIZE 40000
#define SHOW_ADAPTIVE_STATS 1
#define BW_MONITOR 1
// maximum number of characters allowed to represent the routing algorithm as a string
#define MAX_ROUTING_CHARS 32

// debugging parameters
#define TRACK -1
#define TRACK_PKT 0
#define TRACK_MSG -1
#define DEBUG 0
#define MAX_STATS 65536

#define LP_CONFIG_NM_TERM (model_net_lp_config_names[DRAGONFLY_PLUS])
#define LP_METHOD_NM_TERM (model_net_method_names[DRAGONFLY_PLUS])
#define LP_CONFIG_NM_ROUT (model_net_lp_config_names[DRAGONFLY_PLUS_ROUTER])
#define LP_METHOD_NM_ROUT (model_net_method_names[DRAGONFLY_PLUS_ROUTER])

using namespace std;

/*MM: Maintains a list of routers connecting the source and destination groups */
static vector< vector< vector< int > > > connectionList;

static vector< ConnectionManager > connManagerList;

/* IntraGroupLink is a struct used to unpack binary data regarding inter group
   connections from the supplied inter-group file. This struct should not be
   utilized anywhere else in the model.
*/
struct IntraGroupLink {
    int src, dest;
};

/* InterGroupLink is a struct used to unpack binary data regarding inter group
   connections from the supplied inter-group file. This struct should not be
   utilized anywhere else in the model.
*/
struct InterGroupLink {
    int src, dest;
};

#ifdef ENABLE_CORTEX
/* This structure is defined at the end of the file */
extern "C" {
//extern cortex_topology dragonfly_plus_cortex_topology;
}
#endif

static tw_stime max_qos_monitor = 5000000000;
static int debug_slot_count = 0;
static long term_ecount, router_ecount, term_rev_ecount, router_rev_ecount;
static long packet_gen = 0, packet_fin = 0;

/* bw monitoring time in nanosecs */
static int bw_reset_window = 5000000;

static FILE * dragonfly_rtr_bw_log = NULL;
#define indexer3d(_ptr, _x, _y, _z, _maxx, _maxy, _maxz) \
        ((_ptr) + _z * (_maxx * _maxy) + _y * (_maxx) + _x)

#define indexer2d(_ptr, _x, _y, _maxx, _maxy) \
        ((_ptr) + _y * (_maxx) + _x)

static double maxd(double a, double b) { return a < b ? b : a; }

/* minimal and non-minimal packet counts for adaptive routing*/
static int minimal_count = 0, nonmin_count = 0;
static int num_routers_per_mgrp = 0;

typedef struct dragonfly_plus_param dragonfly_plus_param;
/* annotation-specific parameters (unannotated entry occurs at the
 * last index) */
static uint64_t num_params = 0;
static dragonfly_plus_param *all_params = NULL;
static const config_anno_map_t *anno_map = NULL;

/* global variables for codes mapping */
static char lp_group_name[MAX_NAME_LENGTH];
static int mapping_grp_id, mapping_type_id, mapping_rep_id, mapping_offset;

/* router magic number */
static int router_magic_num = 0;

/* terminal magic number */
static int terminal_magic_num = 0;

static long num_local_packets_sr = 0;
static long num_local_packets_sg = 0;
static long num_remote_packets = 0;

/* Hops within a group */
static int num_intra_nonmin_hops = 4;
static int num_intra_min_hops = 2;

static FILE *dragonfly_log = NULL;

static int sample_bytes_written = 0;
static int sample_rtr_bytes_written = 0;

static char cn_sample_file[MAX_NAME_LENGTH];
static char router_sample_file[MAX_NAME_LENGTH];

// don't do overhead here - job of MPI layer
static tw_stime mpi_soft_overhead = 0;

typedef struct terminal_plus_message_list terminal_plus_message_list;
struct terminal_plus_message_list
{
    terminal_plus_message msg;
    char *event_data;
    terminal_plus_message_list *next;
    terminal_plus_message_list *prev;
};

static void init_terminal_plus_message_list(terminal_plus_message_list *thisO, terminal_plus_message *inmsg)
{
    thisO->msg = *inmsg;
    thisO->event_data = NULL;
    thisO->next = NULL;
    thisO->prev = NULL;
}

static void delete_terminal_plus_message_list(void *thisO)
{
    terminal_plus_message_list *toDel = (terminal_plus_message_list *) thisO;
    if (toDel->event_data != NULL)
        free(toDel->event_data);
    free(toDel);
}

template <class InputIterator1, class InputIterator2, class OutputIterator>
OutputIterator _set_difference (InputIterator1 first1, InputIterator1 last1,
                                 InputIterator2 first2, InputIterator2 last2,
                                 OutputIterator result)
{
  while (first1!=last1 && first2!=last2)
  {
    if (*first1<*first2) { *result = *first1; ++result; ++first1; }
    else if (*first2<*first1) ++first2;
    else { ++first1; ++first2; }
  }
  return std::copy(first1,last1,result);
}

template <class T>
vector< T > set_difference_vectors(vector<T> vec1, vector<T> vec2)
{
    int max_len = std::max(vec1.size(), vec2.size());
    vector< T > retVec(max_len);
    typename vector< T >::iterator retIt;

    retIt = _set_difference(vec1.begin(), vec1.end(), vec2.begin(), vec2.end(), retVec.begin());

    retVec.resize(retIt - retVec.begin());

    return retVec;
}

template <class InputIterator1, class InputIterator2, class OutputIterator>
OutputIterator _set_common (InputIterator1 first1, InputIterator1 last1,
                                 InputIterator2 first2, InputIterator2 last2,
                                 OutputIterator result)
{
  while (first1!=last1 && first2!=last2)
  {
    // if(*first1==*first2){ *result = *first1; ++result; ++first1; ++first2;} // yao compile error???
    if (*first1<*first2) ++first1;
    else if (*first2<*first1) ++first2;
    else { *result = *first1; ++result; ++first1; ++first2;}
  }
  return result;
}

template <class T>
vector< T > set_common_vectors(vector<T> vec1, vector<T> vec2)
{
    int max_len = std::max(vec1.size(), vec2.size());
    vector< T > retVec(max_len);
    typename vector< T >::iterator retIt;

    retIt = _set_common(vec1.begin(), vec1.end(), vec2.begin(), vec2.end(), retVec.begin());

    retVec.resize(retIt - retVec.begin());

    return retVec;
}

struct dragonfly_plus_param
{
    // configuration parameters
    int num_routers;         /*Number of routers in a group*/
    double local_bandwidth;  /* bandwidth of the router-router channels within a group */
    double global_bandwidth; /* bandwidth of the inter-group router connections */
    double cn_bandwidth;     /* bandwidth of the compute node channels connected to routers */
    int num_vcs;             /* number of virtual channels */
    int local_vc_size;       /* buffer size of the router-router channels */
    int global_vc_size;      /* buffer size of the global channels */
    int cn_vc_size;          /* buffer size of the compute node channels */
    int chunk_size;          /* full-sized packets are broken into smaller chunks.*/
    // derived parameters
    int num_cn;
    int intra_grp_radix;

    // qos params
    int num_qos_levels;
    int * qos_bandwidths;

    // dfp params start
    int num_router_spine;  // number of spine routers (top level)
    int num_router_leaf;   // number of leaf routers (bottom level)
    int adaptive_threshold;   // predefined queue length threshold T before a packet is routed through a lower priority queue

    bool source_leaf_consider_nonmin;
    bool int_spine_consider_min;
    bool dest_spine_consider_nonmin;
    bool dest_spine_consider_global_nonmin;

    int max_hops_notify; //maximum number of hops allowed before notifying via printout

    long max_port_score;   // maximum score that can be given to any port during route scoring
    // dfp params end

    int num_groups;
    int radix;
    int total_routers;
    int total_terminals;
    int num_global_connections;
    double cn_delay;
    double local_delay;
    double global_delay;
    int credit_size;
    double local_credit_delay;
    double global_credit_delay;
    double cn_credit_delay;
    double router_delay;

    //couting app bandwidth usage percentage on a router
    int counting_bool;
    tw_stime counting_start; 
    tw_stime counting_end; 
    tw_stime counting_interval; 
    int counting_windows;
};

static const dragonfly_plus_param* stored_params;

struct dfly_hash_key
{
    uint64_t message_id;
    tw_lpid sender_id;
};

struct dfly_router_sample
{
    tw_lpid router_id;
    tw_stime *busy_time;
    int64_t *link_traffic_sample;
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
    char *remote_event_data;
    int num_chunks;
    int remote_event_size;
    struct qhash_head hash_link;
};

/* terminal event type (1-4) */
typedef enum event_t {
    T_GENERATE = 1,
    T_ARRIVE,
    T_SEND,
    T_BUFFER,
    R_SEND,
    R_ARRIVE,
    R_BUFFER,
    R_BANDWIDTH,
    T_BANDWIDTH,
} event_t;

/* whether the last hop of a packet was global, local or a terminal */
enum last_hop
{
    GLOBAL = 1,
    LOCAL,
    TERMINAL,
};

typedef enum qos_priority
{
    Q_HIGH =0,
    Q_MEDIUM,
    Q_LOW,
    Q_UNKNOWN,
} qos_priority;

typedef enum qos_status
{
    Q_ACTIVE = 1,
    Q_OVERBW,
} qos_status;

// Used to denote whether a connection is one that would allow a packet to continue along a minimal path or not
// Specifically used to clearly pass whether a connection is a minimal one through to the connection scoring function
typedef enum conn_minimality_t
{
    C_MIN = 1,
    C_NONMIN
} conn_minimality_t;

typedef enum routing_alg_t
{
    MINIMAL = 1, //will always follow the minimal route from host to host
    NON_MINIMAL_SPINE, //will always route through an intermediate spine in an intermediate group for inter group traffic
    NON_MINIMAL_LEAF, //will always route through an intermediate leaf in an intermediate group for inter group traffic
    PROG_ADAPTIVE, //Choose between Minimal, Nonmin spine, and nonmin leaf at the router level based on own congestion
    FULLY_PROG_ADAPTIVE, //OTFA with ARNs
    NON_MINIMAL //A catch all for adaptive routing to determine if a path had deviated from minimal - not an algorithm!!!!!
} routing_alg_t;

typedef enum route_scoring_metric_t
{
    ALPHA = 1, //Count queue lengths and pending messages for a port
    BETA, //Expected Hops to Destination * (Count queue lengths and pending messages) for a port
    GAMMA,
    DELTA
} route_scoring_metric_t;

typedef enum route_scoring_preference_t
{
    LOWER = 1,
    HIGHER
} route_scoring_preference_t;

enum router_type
{
    SPINE = 1,
    LEAF
};

typedef enum intermediate_router_t
{
    INT_CHOICE_LEAF = 1,
    INT_CHOICE_SPINE,
    INT_CHOICE_BOTH
} intermediate_router_t;

static map< int, router_type> router_type_map;

static char* get_routing_alg_chararray(int routing_alg_int)
{
    char* rt_alg = (char*) calloc(MAX_ROUTING_CHARS, sizeof(char));
    switch (routing_alg_int)
    {
        case MINIMAL:
            strcpy(rt_alg, "MINIMAL");
        break;
        case NON_MINIMAL_SPINE:
            strcpy(rt_alg, "NON_MINIMAL_SPINE");
        break;
        case NON_MINIMAL_LEAF:
            strcpy(rt_alg, "NON_MINIMAL_LEAF");
        break;
        case PROG_ADAPTIVE:
            strcpy(rt_alg, "PROG_ADAPTIVE");
        break;
        case FULLY_PROG_ADAPTIVE:
            strcpy(rt_alg, "FULL_PROG_ADAPTIVE");
        default:
            tw_error(TW_LOC, "Routing Algorithm is UNDEFINED - did you call get_routing_alg_string() before setting the static global variable: 'routing'?");
        break;
    }
    return rt_alg;
}

static bool isRoutingAdaptive(int alg)
{
    if (alg == PROG_ADAPTIVE || alg == FULLY_PROG_ADAPTIVE)
        return true;
    else
        return false;
}

static bool isRoutingMinimal(int alg)
{
    if (alg == MINIMAL)
        return true;
    else
        return false;
}

static bool isRoutingNonminimalExplicit(int alg)
{
    if (alg == NON_MINIMAL_LEAF || alg == NON_MINIMAL_SPINE)
        return true;
    else
        return false;
}

/* handles terminal and router events like packet generate/send/receive/buffer */
typedef struct terminal_state terminal_state;
typedef struct router_state router_state;

/* dragonfly compute node data structure */
struct terminal_state
{
    uint64_t packet_counter;

    int packet_gen;
    int packet_fin;

    int total_gen_size;

    // Dragonfly specific parameters
    unsigned int router_id;
    unsigned int terminal_id;

    // Each terminal will have an input and output channel with the router
    int *vc_occupancy;  // NUM_VC
    int num_vcs;
    tw_stime terminal_available_time;
    terminal_plus_message_list **terminal_msgs;
    terminal_plus_message_list **terminal_msgs_tail;
    int in_send_loop;
    struct mn_stats dragonfly_stats_array[CATEGORY_MAX];
   
    int * qos_status;
    unsigned long long* qos_data;

    int last_qos_lvl;
    int is_monitoring_bw;

    struct rc_stack *st;
    int issueIdle;
    unsigned long long * terminal_length;

    const char *anno;
    const dragonfly_plus_param *params;

    struct qhash_table *rank_tbl;
    uint64_t rank_tbl_pop;

    tw_stime total_time;
    uint64_t total_msg_size;
    double total_hops;
    long finished_msgs;
    long finished_chunks;
    long finished_packets;

    tw_stime last_buf_full;
    tw_stime busy_time;

    unsigned long stalled_chunks; //Counter for when a packet cannot be immediately routed due to full VC

    tw_stime max_latency;
    tw_stime min_latency;

    char output_buf[4096];
    char output_buf2[4096];
    /* For LP suspend functionality */
    int error_ct;

    /* For sampling */
    long fin_chunks_sample;
    long data_size_sample;
    double fin_hops_sample;
    tw_stime fin_chunks_time;
    tw_stime busy_time_sample;

    char sample_buf[4096];
    struct dfly_cn_sample *sample_stat;
    int op_arr_size;
    int max_arr_size;

    /* for logging forward and reverse events */
    long fwd_events;
    long rev_events;

    // for ROSS Instrumentation
    long fin_chunks_ross_sample;
    long data_size_ross_sample;
    long fin_hops_ross_sample;
    tw_stime fin_chunks_time_ross_sample;
    tw_stime busy_time_ross_sample;
    struct dfly_cn_sample ross_sample;
};

struct router_state
{
    int router_id;
    int group_id;
    int op_arr_size;
    int max_arr_size;

    router_type dfp_router_type;  // Enum to specify whether this router is a spine or a leaf

    ConnectionManager *connMan;
    int *gc_usage;

    int *global_channel;

    tw_stime *next_output_available_time;
    tw_stime *last_buf_full;
  
    /* qos related state variables */
    int num_rtr_rc_windows;
    int is_monitoring_bw;
    int* last_qos_lvl;
    int** qos_status;
    unsigned long long** qos_data;

    tw_stime *busy_time;
    tw_stime *busy_time_sample;

    unsigned long* stalled_chunks; //Coutner for when a packet is put into queued messages instead of routing due to full VC

    terminal_plus_message_list ***pending_msgs;
    terminal_plus_message_list ***pending_msgs_tail;
    terminal_plus_message_list ***queued_msgs;
    terminal_plus_message_list ***queued_msgs_tail;
    int *in_send_loop;
    int *queued_count;
    struct rc_stack *st;

    int **vc_occupancy;
    int64_t *link_traffic;
    int64_t *link_traffic_sample;

    const char *anno;
    const dragonfly_plus_param *params;

    char output_buf[4096];
    char output_buf2[4096];

    struct dfly_router_sample *rsamples;

    long fwd_events;
    long rev_events;

    // for ROSS instrumentation
    tw_stime* busy_time_ross_sample;
    int64_t * link_traffic_ross_sample;
    struct dfly_router_sample ross_rsample;

    //GC occupancy report usage
    //for msg app id counting rec
    char output_buf4[4096]; 
    int **msg_counting;
    int **msg_counting_out;
    //counting total number of packets received during all counting windows, used to verify correct reverse computation
    int total_packets_received;
    int total_packets_sent;

};

/* ROSS model instrumentation */
void dfly_plus_event_collect(terminal_plus_message *m, tw_lp *lp, char *buffer, int *collect_flag);
void dfly_plus_model_stat_collect(terminal_state *s, tw_lp *lp, char *buffer);
void dfly_plus_router_model_stat_collect(router_state *s, tw_lp *lp, char *buffer);
static void ross_dfly_plus_rsample_fn(router_state * s, tw_bf * bf, tw_lp * lp, struct dfly_router_sample *sample);
static void ross_dfly_plus_rsample_rc_fn(router_state * s, tw_bf * bf, tw_lp * lp, struct dfly_router_sample *sample);
static void ross_dfly_plus_sample_fn(terminal_state * s, tw_bf * bf, tw_lp * lp, struct dfly_cn_sample *sample);
static void ross_dfly_plus_sample_rc_fn(terminal_state * s, tw_bf * bf, tw_lp * lp, struct dfly_cn_sample *sample);

st_model_types dfly_plus_model_types[] = {
    {(ev_trace_f) dfly_plus_event_collect,
     sizeof(int),
     (model_stat_f) dfly_plus_model_stat_collect,
     sizeof(tw_lpid) + sizeof(long) * 2 + sizeof(double) + sizeof(tw_stime) *2,
     (sample_event_f) ross_dfly_plus_sample_fn,
     (sample_revent_f) ross_dfly_plus_sample_rc_fn,
     sizeof(struct dfly_cn_sample) } , 
    {(ev_trace_f) dfly_plus_event_collect,
     sizeof(int),
     (model_stat_f) dfly_plus_router_model_stat_collect,
     0, //updated in router_plus_init() since it's based on the radix
     (sample_event_f) ross_dfly_plus_rsample_fn,
     (sample_revent_f) ross_dfly_plus_rsample_rc_fn,
     0 } , //updated in router_plus_init() since it's based on the radix    
    {NULL, 0, NULL, 0, NULL, NULL, 0}
};
/* End of ROSS model instrumentation */

// event tracing callback - used router and terminal LPs
void dfly_plus_event_collect(terminal_plus_message *m, tw_lp *lp, char *buffer, int *collect_flag)
{
    (void)lp;
    (void)collect_flag;

    int type = (int) m->type;
    memcpy(buffer, &type, sizeof(type));
}

// GVT-based and real time sampling callback for terminals
void dfly_plus_model_stat_collect(terminal_state *s, tw_lp *lp, char *buffer)
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

// GVT-based and real time sampling callback for routers
void dfly_plus_router_model_stat_collect(router_state *s, tw_lp *lp, char *buffer)
{
    (void)lp;

    const dragonfly_plus_param * p = s->params; 
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

static const st_model_types  *dfly_plus_get_model_types(void)
{
    return(&dfly_plus_model_types[0]);
}

static const st_model_types  *dfly_plus_router_get_model_types(void)
{
    return(&dfly_plus_model_types[1]);
}

static void dfly_plus_register_model_types(st_model_types *base_type)
{
    st_model_type_register(LP_CONFIG_NM_TERM, base_type);
}

static void dfly_plus_router_register_model_types(st_model_types *base_type)
{
    st_model_type_register(LP_CONFIG_NM_ROUT, base_type);
}
/*** END of ROSS Instrumentation support */

/* ROSS Instrumentation layer */
// virtual time sampling callback - router forward
static void ross_dfly_plus_rsample_fn(router_state * s, tw_bf * bf, tw_lp * lp, struct dfly_router_sample *sample)
{
    (void)lp;
    (void)bf;

    const dragonfly_plus_param * p = s->params; 
    int i = 0;

    sample->router_id = s->router_id;
    sample->end_time = tw_now(lp);
    sample->fwd_events = s->fwd_events;
    sample->rev_events = s->rev_events;
    sample->busy_time = (tw_stime*)((&sample->rev_events) + 1);
    sample->link_traffic_sample = (int64_t*)((&sample->busy_time[0]) + p->radix);

    for(; i < p->radix; i++)
    {
        sample->busy_time[i] = s->ross_rsample.busy_time[i]; 
        sample->link_traffic_sample[i] = s->ross_rsample.link_traffic_sample[i]; 
    }

    /* clear up the current router stats */
    s->fwd_events = 0;
    s->rev_events = 0;

    for( i = 0; i < p->radix; i++)
    {
        s->ross_rsample.busy_time[i] = 0;
        s->ross_rsample.link_traffic_sample[i] = 0;
    }
}

// virtual time sampling callback - router reverse
static void ross_dfly_plus_rsample_rc_fn(router_state * s, tw_bf * bf, tw_lp * lp, struct dfly_router_sample *sample)
{
    (void)lp;
    (void)bf;
    
    const dragonfly_plus_param * p = s->params;
    int i =0;

    for(; i < p->radix; i++)
    {
        s->ross_rsample.busy_time[i] = sample->busy_time[i];
        s->ross_rsample.link_traffic_sample[i] = sample->link_traffic_sample[i];
    }

    s->fwd_events = sample->fwd_events;
    s->rev_events = sample->rev_events;
}

// virtual time sampling callback - terminal forward
static void ross_dfly_plus_sample_fn(terminal_state * s, tw_bf * bf, tw_lp * lp, struct dfly_cn_sample *sample)
{
    (void)lp;
    (void)bf;
    
    sample->terminal_id = s->terminal_id;
    sample->fin_chunks_sample = s->ross_sample.fin_chunks_sample;
    sample->data_size_sample = s->ross_sample.data_size_sample;
    sample->fin_hops_sample = s->ross_sample.fin_hops_sample;
    sample->fin_chunks_time = s->ross_sample.fin_chunks_time;
    sample->busy_time_sample = s->ross_sample.busy_time_sample;
    sample->end_time = tw_now(lp);
    sample->fwd_events = s->fwd_events;
    sample->rev_events = s->rev_events;

    s->ross_sample.fin_chunks_sample = 0;
    s->ross_sample.data_size_sample = 0;
    s->ross_sample.fin_hops_sample = 0;
    s->fwd_events = 0;
    s->rev_events = 0;
    s->ross_sample.fin_chunks_time = 0;
    s->ross_sample.busy_time_sample = 0;
}

// virtual time sampling callback - terminal reverse
static void ross_dfly_plus_sample_rc_fn(terminal_state * s, tw_bf * bf, tw_lp * lp, struct dfly_cn_sample *sample)
{
    (void)lp;
    (void)bf;

    s->ross_sample.busy_time_sample = sample->busy_time_sample;
    s->ross_sample.fin_chunks_time = sample->fin_chunks_time;
    s->ross_sample.fin_hops_sample = sample->fin_hops_sample;
    s->ross_sample.data_size_sample = sample->data_size_sample;
    s->ross_sample.fin_chunks_sample = sample->fin_chunks_sample;
    s->fwd_events = sample->fwd_events;
    s->rev_events = sample->rev_events;
}

void dragonfly_plus_rsample_init(router_state *s, tw_lp *lp)
{
    (void) lp;
    int i = 0;
    const dragonfly_plus_param *p = s->params;

    assert(p->radix);

    s->max_arr_size = MAX_STATS;
    s->rsamples = (struct dfly_router_sample *) calloc(MAX_STATS, sizeof(struct dfly_router_sample));
    for (; i < s->max_arr_size; i++) {
        s->rsamples[i].busy_time = (tw_stime *) calloc(p->radix, sizeof(tw_stime));
        s->rsamples[i].link_traffic_sample = (int64_t *) calloc(p->radix, sizeof(int64_t));
    }
}
void dragonfly_plus_rsample_rc_fn(router_state *s, tw_bf *bf, terminal_plus_message *msg, tw_lp *lp)
{
    (void) bf;
    (void) lp;
    (void) msg;

    s->op_arr_size--;
    int cur_indx = s->op_arr_size;
    struct dfly_router_sample stat = s->rsamples[cur_indx];

    const dragonfly_plus_param *p = s->params;
    int i = 0;

    for (; i < p->radix; i++) {
        s->busy_time_sample[i] = stat.busy_time[i];
        s->link_traffic_sample[i] = stat.link_traffic_sample[i];
    }

    for (i = 0; i < p->radix; i++) {
        stat.busy_time[i] = 0;
        stat.link_traffic_sample[i] = 0;
    }
    s->fwd_events = stat.fwd_events;
    s->rev_events = stat.rev_events;
}

void dragonfly_plus_rsample_fn(router_state *s, tw_bf *bf, terminal_plus_message *msg, tw_lp *lp)
{
    (void) bf;
    (void) lp;
    (void) msg;

    const dragonfly_plus_param *p = s->params;

    if (s->op_arr_size >= s->max_arr_size) {
        struct dfly_router_sample *tmp =
            (dfly_router_sample *) calloc((MAX_STATS + s->max_arr_size), sizeof(struct dfly_router_sample));
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

    for (; i < p->radix; i++) {
        s->rsamples[cur_indx].busy_time[i] = s->busy_time_sample[i];
        s->rsamples[cur_indx].link_traffic_sample[i] = s->link_traffic_sample[i];
    }

    s->op_arr_size++;

    /* clear up the current router stats */
    s->fwd_events = 0;
    s->rev_events = 0;

    for (i = 0; i < p->radix; i++) {
        s->busy_time_sample[i] = 0;
        s->link_traffic_sample[i] = 0;
    }
}

void dragonfly_plus_rsample_fin(router_state *s, tw_lp *lp)
{
    (void) lp;
    const dragonfly_plus_param *p = s->params;

    if (s->router_id == 0) {
        /* write metadata file */
        char meta_fname[64];
        sprintf(meta_fname, "dragonfly-router-sampling.meta");

        FILE *fp = fopen(meta_fname, "w");
        fprintf(fp,
                "Router sample struct format: \nrouter_id (tw_lpid) \nbusy time for each of the %d links "
                "(double) \n"
                "link traffic for each of the %d links (int64_t) \nsample end time (double) forward events "
                "per sample \nreverse events per sample ",
                p->radix, p->radix);
        fclose(fp);
    }
    char rt_fn[MAX_NAME_LENGTH];
    if (strcmp(router_sample_file, "") == 0)
        sprintf(rt_fn, "dragonfly-router-sampling-%ld.bin", g_tw_mynode);
    else
        sprintf(rt_fn, "%s-%ld.bin", router_sample_file, g_tw_mynode);

    int i = 0;

    int size_sample = sizeof(tw_lpid) + p->radix * (sizeof(int64_t) + sizeof(tw_stime)) +
                      sizeof(tw_stime) + 2 * sizeof(long);
    FILE *fp = fopen(rt_fn, "a");
    fseek(fp, sample_rtr_bytes_written, SEEK_SET);

    for (; i < s->op_arr_size; i++) {
        fwrite((void *) &(s->rsamples[i].router_id), sizeof(tw_lpid), 1, fp);
        fwrite(s->rsamples[i].busy_time, sizeof(tw_stime), p->radix, fp);
        fwrite(s->rsamples[i].link_traffic_sample, sizeof(int64_t), p->radix, fp);
        fwrite((void *) &(s->rsamples[i].end_time), sizeof(tw_stime), 1, fp);
        fwrite((void *) &(s->rsamples[i].fwd_events), sizeof(long), 1, fp);
        fwrite((void *) &(s->rsamples[i].rev_events), sizeof(long), 1, fp);
    }
    sample_rtr_bytes_written += (s->op_arr_size * size_sample);
    fclose(fp);
}
void dragonfly_plus_sample_init(terminal_state *s, tw_lp *lp)
{
    (void) lp;
    s->fin_chunks_sample = 0;
    s->data_size_sample = 0;
    s->fin_hops_sample = 0;
    s->fin_chunks_time = 0;
    s->busy_time_sample = 0;

    s->op_arr_size = 0;
    s->max_arr_size = MAX_STATS;

    s->sample_stat = (dfly_cn_sample *) calloc(MAX_STATS, sizeof(struct dfly_cn_sample));
}
void dragonfly_plus_sample_rc_fn(terminal_state *s, tw_bf *bf, terminal_plus_message *msg, tw_lp *lp)
{
    (void) lp;
    (void) bf;
    (void) msg;

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

void dragonfly_plus_sample_fn(terminal_state *s, tw_bf *bf, terminal_plus_message *msg, tw_lp *lp)
{
    (void) lp;
    (void) msg;
    (void) bf;

    if (s->op_arr_size >= s->max_arr_size) {
        /* In the worst case, copy array to a new memory location, its very
         * expensive operation though */
        struct dfly_cn_sample *tmp =
            (dfly_cn_sample *) calloc((MAX_STATS + s->max_arr_size), sizeof(struct dfly_cn_sample));
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

void dragonfly_plus_sample_fin(terminal_state *s, tw_lp *lp)
{
    (void) lp;


    if (!g_tw_mynode) {
        /* write metadata file */
        char meta_fname[64];
        sprintf(meta_fname, "dragonfly-cn-sampling.meta");

        FILE *fp = fopen(meta_fname, "w");
        fprintf(
            fp,
            "Compute node sample format\nterminal_id (tw_lpid) \nfinished chunks (long)"
            "\ndata size per sample (long) \nfinished hops (double) \ntime to finish chunks (double)"
            "\nbusy time (double)\nsample end time(double) \nforward events (long) \nreverse events (long)");
        fclose(fp);
    }

    char rt_fn[MAX_NAME_LENGTH];
    if (strncmp(cn_sample_file, "", 10) == 0)
        sprintf(rt_fn, "dragonfly-cn-sampling-%ld.bin", g_tw_mynode);
    else
        sprintf(rt_fn, "%s-%ld.bin", cn_sample_file, g_tw_mynode);

    FILE *fp = fopen(rt_fn, "a");
    fseek(fp, sample_bytes_written, SEEK_SET);
    fwrite(s->sample_stat, sizeof(struct dfly_cn_sample), s->op_arr_size, fp);
    fclose(fp);

    sample_bytes_written += (s->op_arr_size * sizeof(struct dfly_cn_sample));
}

int dragonfly_plus_get_assigned_router_id(int terminal_id, const dragonfly_plus_param *p);

static short routing = MINIMAL;
static short scoring = ALPHA;
static short scoring_preference = LOWER;

/*Routing Implementation Declarations*/
static Connection do_dfp_prog_adaptive_routing(router_state *s, tw_bf *bf, terminal_plus_message *msg, tw_lp *lp, int fdest_router_id);
static Connection do_dfp_FPAR(router_state *s, tw_bf *bf, terminal_plus_message *msg, tw_lp *lp, int fdest_router_id);


/*Routing Helper Declarations*/
static int get_min_hops_to_dest_from_conn(router_state *s, tw_bf *bf, terminal_plus_message *msg, tw_lp *lp, Connection conn);
static vector< Connection > get_legal_minimal_stops(router_state *s, tw_bf *bf, terminal_plus_message *msg, tw_lp *lp, int fdest_router_id);
static vector< Connection > get_legal_nonminimal_stops(router_state *s, tw_bf *bf, terminal_plus_message *msg, tw_lp *lp, vector< Connection > possible_minimal_stops, int fdest_router_id);


static tw_stime dragonfly_total_time = 0;
static tw_stime dragonfly_max_latency = 0;


static long long total_hops = 0;
static long long N_finished_packets = 0;
static long long total_msg_sz = 0;
static long long N_finished_msgs = 0;
static long long N_finished_chunks = 0;

/* convert GiB/s and bytes to ns */
static tw_stime bytes_to_ns(uint64_t bytes, double GB_p_s)
{
    tw_stime time;

    /* bytes to GB */
    time = ((double) bytes) / (1024.0 * 1024.0 * 1024.0);
    /* GiB to s */
    time = time / GB_p_s;
    /* s to ns */
    time = time * 1000.0 * 1000.0 * 1000.0;

    return (time);
}

static int dragonfly_rank_hash_compare(void *key, struct qhash_head *link)
{
    struct dfly_hash_key *message_key = (struct dfly_hash_key *) key;
    struct dfly_qhash_entry *tmp = NULL;

    tmp = qhash_entry(link, struct dfly_qhash_entry, hash_link);

    if (tmp->key.message_id == message_key->message_id && tmp->key.sender_id == message_key->sender_id)
        return 1;

    return 0;
}
static int dragonfly_hash_func(void *k, int table_size)
{
    struct dfly_hash_key *tmp = (struct dfly_hash_key *) k;
    uint32_t pc = 0, pb = 0;
    bj_hashlittle2(tmp, sizeof(*tmp), &pc, &pb);
    return (int) (pc % (table_size - 1));
    /*uint64_t key = (~tmp->message_id) + (tmp->message_id << 18);
    key = key * 21;
    key = ~key ^ (tmp->sender_id >> 4);
    key = key * tmp->sender_id;
    return (int)(key & (table_size - 1));*/
}

/* returns the dragonfly message size */
int dragonfly_plus_get_msg_sz(void)
{
    return sizeof(terminal_plus_message);
}

static void free_tmp(void *ptr)
{
    struct dfly_qhash_entry *dfly = (dfly_qhash_entry *) ptr;
    if (dfly->remote_event_data)
        free(dfly->remote_event_data);

    if (dfly)
        free(dfly);
}

/**
 * Scores a connection based on the metric provided in the function
 * @param isMinimalPort a boolean variable used in the Gamma metric to pass whether a given port would lead to the destination in a minimal way
 */
static int dfp_score_connection(router_state *s, tw_bf *bf, terminal_plus_message *msg, tw_lp *lp, Connection conn, conn_minimality_t c_minimality)
{
    int score = 0; //can't forget to initialize this to zero.
    int port = conn.port;

    int port_size = 0;
    int vc_size = 0;

    if (port == -1) {
        if (scoring_preference == LOWER)
            return INT_MAX;
        else
            return 0;
    }

    switch(conn.conn_type) {
        case CONN_LOCAL:
            vc_size = s->params->local_vc_size;
            break;
        case CONN_GLOBAL:
            vc_size = s->params->global_vc_size;
            break;
        case CONN_TERMINAL:
            vc_size = s->params->cn_vc_size;
            break;
        default:
            tw_error(TW_LOC, "Error connection type");
    }

    switch(scoring) {
        case ALPHA: //considers vc occupancy and queued count only LOWER SCORE IS BETTER
        {
            for(int k=0; k < s->params->num_vcs; k++)
            {
                score += s->vc_occupancy[port][k];
            }
            score += s->queued_count[port];

            //score normalized to port size if FPAR is used
            if (routing == FULLY_PROG_ADAPTIVE) {
                score = (int)((double)score/port_size *100);                
            }
            break;
        }
        case BETA: //consideres vc occupancy and queued count multiplied by the number of minimum hops to the destination LOWER SCORE IS BETTER
        {
            int base_score = 0;
            for(int k=0; k < s->params->num_vcs; k++)
            {
                base_score += s->vc_occupancy[port][k];
            }
            base_score += s->queued_count[port];
            score = base_score * get_min_hops_to_dest_from_conn(s, bf, msg, lp, conn);
            break;
        }
        case GAMMA: //consideres vc occupancy and queue count but ports that follow a minimal path to fdest are biased 2:1 bonus by multiplying minimal by 2 HIGHER SCORE IS BETTER
        {
            score = s->params->max_port_score; //initialize this to max score.
            int to_subtract = 0;
            for(int k=0; k < s->params->num_vcs; k++)
            {
                to_subtract += s->vc_occupancy[port][k];
            }
            to_subtract += s->queued_count[port];
            score -= to_subtract;

            if (c_minimality == C_MIN) //the connection maintains the paths minimality - gets a bonus of 2x
                score = score * 2;
            break;
        }
        case DELTA: //consideres vc occupancy and queue count but ports that follow a minimal path to fdest are biased 2:1 through dividing minimal by 2 Lower SCORE IS BETTER
        {
            for(int k=0; k < s->params->num_vcs; k++)
            {
                score += s->vc_occupancy[port][k];
            }
            score += s->queued_count[port];

            if (c_minimality != C_MIN)
                score = score * 2;
            break;
        }
        default:
            tw_error(TW_LOC, "Unsupported Scoring Protocol Error\n");

    }
    return score;
}

static Connection get_absolute_best_connection_from_conns(router_state *s, tw_bf *bf, terminal_plus_message *msg, tw_lp *lp, vector<Connection> conns)
{
    if (conns.size() == 0) {
        Connection bad_conn;
        bad_conn.src_gid = -1;
        bad_conn.port = -1;
        return bad_conn;
    }
    if (conns.size() == 1) {
        return conns[0];
    }

    int num_to_compare = conns.size();

    int scores[num_to_compare];
    int best_score_index = 0;
    vector<int> best_indexes; //in case multiple ports are equally best, choose a random one
    if (scoring_preference == LOWER) {
        
        int best_score = INT_MAX;
        for(int i = 0; i < num_to_compare; i++)
        {
            scores[i] = dfp_score_connection(s, bf, msg, lp, conns[i], C_MIN);

            if (scores[i] <= best_score) {
                if (scores[i] < best_score) {
                    best_indexes.clear(); //we found a better one so we need to clear it
                }
                best_score_index = i;
                best_score = scores[i];
                best_indexes.push_back(i);
            }
        }
    }
    else {
        
        int best_score = 0;
        for(int i = 0; i < num_to_compare; i++)
        {
            scores[i] = dfp_score_connection(s, bf, msg, lp, conns[i], C_MIN);

            if (scores[i] >= best_score) {
                if (scores[i] > best_score) {
                    best_indexes.clear(); //we found a better one so we need to clear it
                }
                best_score_index = i;
                best_score = scores[i];
                best_indexes.push_back(i);
            }
        }
    }

    if (best_indexes.size() > 1) {
        msg->num_rngs++;
        best_score_index = best_indexes[tw_rand_ulong(lp->rng, 0, best_indexes.size()-1)];
    }

    return conns[best_score_index];
}

static vector< Connection > dfp_select_two_connections(router_state *s, tw_bf *bf, terminal_plus_message *msg, tw_lp *lp, vector< Connection > conns)
{
    if(conns.size() < 2) {
        if(conns.size() == 1)
            return conns;
        if(conns.size() == 0)
            return vector< Connection>();
    }

    int rand_sel_1, rand_sel_2_offset;

    int num_conns = conns.size();

    msg->num_rngs +=2;
    rand_sel_1 = tw_rand_integer(lp->rng, 0, num_conns-1);
    rand_sel_2_offset = tw_rand_integer(lp->rng, 0, num_conns-1); //number of indices to count up from the previous selected one. Avoids selecting same one twice
    int rand_sel_2 = (rand_sel_1 + rand_sel_2_offset) % num_conns;

    vector< Connection > retVec;
    retVec.push_back(conns[rand_sel_1]);
    retVec.push_back(conns[rand_sel_2]);

    return retVec;
}

//two rngs per call
//TODO this defaults to minimality of min, at time of implementation all connections in conns are of same minimality so their scores compared to each other don't matter on minimality
static Connection get_best_connection_from_conns(router_state *s, tw_bf *bf, terminal_plus_message *msg, tw_lp *lp, vector<Connection> conns)
{
    if (conns.size() == 0) {
        Connection bad_conn;
        bad_conn.src_gid = -1;
        bad_conn.port = -1;
        return bad_conn;
    }
    if (conns.size() < 2) {
        return conns[0];
    }
    int num_to_compare = 2; //TODO make this a configurable
    vector< Connection > selected_conns = dfp_select_two_connections(s, bf, msg, lp, conns);

    int scores[num_to_compare];
    int best_score_index = 0;
    vector<int> best_indexes; //in case multiple ports are equally best, choose a random one
    if (scoring_preference == LOWER) {
        
        int best_score = INT_MAX;
        for(int i = 0; i < num_to_compare; i++)
        {
            scores[i] = dfp_score_connection(s, bf, msg, lp, selected_conns[i], C_MIN);

            if (scores[i] <= best_score) {
                if (scores[i] < best_score) {
                    best_indexes.clear(); //we found a better one so we need to clear it
                }
                best_score_index = i;
                best_score = scores[i];
                best_indexes.push_back(i);
            }
        }
    }
    else {
        
        int best_score = 0;
        for(int i = 0; i < num_to_compare; i++)
        {
            scores[i] = dfp_score_connection(s, bf, msg, lp, selected_conns[i], C_MIN);

            if (scores[i] >= best_score) {
                if (scores[i] > best_score) {
                    best_indexes.clear(); //we found a better one so we need to clear it
                }
                best_score_index = i;
                best_score = scores[i];
                best_indexes.push_back(i);
            }
        }
    }

    if (best_indexes.size() > 1) {
        msg->num_rngs++;
        best_score_index = best_indexes[tw_rand_ulong(lp->rng, 0, best_indexes.size()-1)];
    }

    return selected_conns[best_score_index];
}

int dragonfly_plus_get_router_type(int router_id, const dragonfly_plus_param *p)
{
    int num_groups = p->num_groups;
    int num_routers = p->num_routers;
    int num_router_leaf = p->num_router_leaf;
    int router_local_id = router_id % num_routers;

    if (router_local_id < num_router_leaf)
        return LEAF;
    else
        return SPINE;
}

/* get the router id associated with a given terminal id */
int dragonfly_plus_get_assigned_router_id(int terminal_id, const dragonfly_plus_param *p)
{
    // currently supports symmetrical bipartite spine/leaf router configurations
    // first half of routers in a given group are leafs which have terminals
    // second half of routers in a given group are spines which have no terminals
    int num_groups = p->num_groups;            // number of groups of routers in the network
    int num_routers = p->num_routers;          // num routers per group
    int num_router_leaf = p->num_router_leaf;  // num leaf routers per group
    int num_cn = p->num_cn;                    // num compute nodes per leaf router
    int num_cn_per_group = (num_router_leaf * num_cn);

    int group_id = terminal_id / num_cn_per_group;
    int local_router_id = (terminal_id / num_cn) % num_router_leaf;
    int router_id = (group_id * num_routers) + local_router_id;

    return router_id;
}

static void append_to_terminal_plus_message_list(terminal_plus_message_list **thisq,
                                                 terminal_plus_message_list **thistail,
                                                 int index,
                                                 terminal_plus_message_list *msg)
{
    if (thisq[index] == NULL) {
        thisq[index] = msg;
    }
    else {
        thistail[index]->next = msg;
        msg->prev = thistail[index];
    }
    thistail[index] = msg;
}

static void prepend_to_terminal_plus_message_list(terminal_plus_message_list **thisq,
                                                  terminal_plus_message_list **thistail,
                                                  int index,
                                                  terminal_plus_message_list *msg)
{
    if (thisq[index] == NULL) {
        thistail[index] = msg;
    }
    else {
        thisq[index]->prev = msg;
        msg->next = thisq[index];
    }
    thisq[index] = msg;
}

static terminal_plus_message_list *return_head(terminal_plus_message_list **thisq,
                                               terminal_plus_message_list **thistail,
                                               int index)
{
    terminal_plus_message_list *head = thisq[index];
    if (head != NULL) {
        thisq[index] = head->next;
        if (head->next != NULL) {
            head->next->prev = NULL;
            head->next = NULL;
        }
        else {
            thistail[index] = NULL;
        }
    }
    return head;
}

static terminal_plus_message_list *return_tail(terminal_plus_message_list **thisq,
                                               terminal_plus_message_list **thistail,
                                               int index)
{
    terminal_plus_message_list *tail = thistail[index];
    assert(tail);
    if (tail->prev != NULL) {
        tail->prev->next = NULL;
        thistail[index] = tail->prev;
        tail->prev = NULL;
    }
    else {
        thistail[index] = NULL;
        thisq[index] = NULL;
    }
    return tail;
}

void dragonfly_plus_print_params(const dragonfly_plus_param *p, FILE * st)
{
    if (!st)
        st = stdout;

    fprintf(st,"\n------------------ Dragonfly Plus Parameters ---------\n");
    fprintf(st,"\tnum_routers =                 %d\n",p->num_routers);
    fprintf(st,"\tlocal_bandwidth =             %.2f\n",p->local_bandwidth);
    fprintf(st,"\tglobal_bandwidth =            %.2f\n",p->global_bandwidth);
    fprintf(st,"\tcn_bandwidth =                %.2f\n",p->cn_bandwidth);
    fprintf(st,"\tnum_vcs =                     %d\n",p->num_vcs);
    fprintf(st,"\tlocal_vc_size =               %d\n",p->local_vc_size);
    fprintf(st,"\tglobal_vc_size =              %d\n",p->global_vc_size);
    fprintf(st,"\tcn_vc_size =                  %d\n",p->cn_vc_size);
    fprintf(st,"\tchunk_size =                  %d\n",p->chunk_size);
    fprintf(st,"\tnum_cn =                      %d\n",p->num_cn);
    fprintf(st,"\tintra_grp_radix =             %d\n",p->intra_grp_radix);
    fprintf(st,"\tnum_qos_levels =              %d\n",p->num_qos_levels);
    fprintf(st,"\tnum_router_spine =            %d\n",p->num_router_spine);
    fprintf(st,"\tnum_router_leaf =             %d\n",p->num_router_leaf);
    fprintf(st,"\tmax_port_score =              %ld\n",p->max_port_score);
    fprintf(st,"\tnum_groups =                  %d\n",p->num_groups);
    fprintf(st,"\tvirtual radix =               %d\n",p->radix);
    fprintf(st,"\ttotal_routers =               %d\n",p->total_routers);
    fprintf(st,"\ttotal_terminals =             %d\n",p->total_terminals);
    fprintf(st,"\tnum_global_connections =      %d\n",p->num_global_connections);
    fprintf(st,"\tcn_delay =                    %.2f\n",p->cn_delay);
    fprintf(st,"\tlocal_delay =                 %.2f\n",p->local_delay);
    fprintf(st,"\tglobal_delay =                %.2f\n",p->global_delay);
    fprintf(st,"\tlocal credit_delay =          %.2f\n",p->local_credit_delay);
    fprintf(st,"\tglobal credit_delay =         %.2f\n",p->global_credit_delay);
    fprintf(st,"\tcn credit_delay =             %.2f\n",p->cn_credit_delay);
    fprintf(st,"\trouter_delay =                %.2f\n",p->router_delay);
    fprintf(st,"\tscoring =                     %d\n",scoring);
    fprintf(st,"\tadaptive_threshold =          %d\n",p->adaptive_threshold);
    fprintf(st,"\trouting =                     %s\n",get_routing_alg_chararray(routing));
    fprintf(st,"\tsource_leaf_consider_nonmin = %s\n", (p->source_leaf_consider_nonmin ? "true" : "false"));
    fprintf(st,"\tint_spine_consider_min =      %s\n", (p->int_spine_consider_min ? "true" : "false"));
    fprintf(st,"\tdest_spine_consider_nonmin =  %s\n", (p->dest_spine_consider_nonmin ? "true" : "false"));
    fprintf(st,"\tdest_spine_consider_gnonmin = %s\n", (p->dest_spine_consider_global_nonmin ? "true" : "false"));
    fprintf(st,"\tmax hops notification =       %d\n",p->max_hops_notify);
    if (p->counting_bool > 0)
    {   
        fprintf(st,"\tcounting msg app id =         Yes\n");
        fprintf(st,"\tcounting_start(ns) =          %.2f\n",p->counting_start);
        fprintf(st,"\tcounting_end(ns) =            %.2f\n",p->counting_end);
        fprintf(st,"\tcounting_interval(ns) =       %.2f\n",p->counting_interval);
        fprintf(st,"\t# counting_windows =          %d\n",p->counting_windows);
    }
    fprintf(st,"------------------------------------------------------\n\n");
}

static void dragonfly_read_config(const char *anno, dragonfly_plus_param *params)
{
    /*Adding init for router magic number*/
    uint32_t h1 = 0, h2 = 0;
    bj_hashlittle2(LP_METHOD_NM_ROUT, strlen(LP_METHOD_NM_ROUT), &h1, &h2);
    router_magic_num = h1 + h2;

    bj_hashlittle2(LP_METHOD_NM_TERM, strlen(LP_METHOD_NM_TERM), &h1, &h2);
    terminal_magic_num = h1 + h2;

    // shorthand
    dragonfly_plus_param *p = params;
    int myRank;
    MPI_Comm_rank(MPI_COMM_CODES, &myRank);

    int rc = configuration_get_value_int(&config, "PARAMS", "local_vc_size", anno, &p->local_vc_size);
    if (rc) {
        p->local_vc_size = 1024;
        if(!myRank)
            fprintf(stderr, "Buffer size of local channels not specified, setting to %d\n", p->local_vc_size);
    }

    rc = configuration_get_value_int(&config, "PARAMS", "global_vc_size", anno, &p->global_vc_size);
    if (rc) {
        p->global_vc_size = 2048;
        if(!myRank)
            fprintf(stderr, "Buffer size of global channels not specified, setting to %d\n", p->global_vc_size);
    }

    //For App msgs percentage counting on router
    rc = configuration_get_value_int(&config, "PARAMS", "counting_bool", anno, &p->counting_bool);
    if(p->counting_bool) {
        int rc1 = configuration_get_value_double(&config, "PARAMS", "counting_start", anno, &p->counting_start);
        if(rc1)
            printf("missing counting_start\n");
        int rc2 = configuration_get_value_double(&config, "PARAMS", "counting_end", anno, &p->counting_end);
        if(rc2)
            printf("missing counting_end\n");
        int rc3 = configuration_get_value_double(&config, "PARAMS", "counting_interval", anno, &p->counting_interval);
        if(rc3)
            printf("missing counting_interval\n");
        if(rc1 || rc2 || rc3)
            tw_error(TW_LOC, "\n Missing couting values, (counting_start/end/interval) check for config files\n");

        p->counting_windows = (int) round((p->counting_end - p->counting_start)/p->counting_interval); 

        //convert us to ns
        p->counting_start = p->counting_start * 1000;
        p->counting_end = p->counting_end * 1000;
        p->counting_interval = p->counting_interval * 1000;
    }
    
    rc = configuration_get_value_int(&config, "PARAMS", "num_qos_levels", anno, &p->num_qos_levels);
    if(rc) {
        p->num_qos_levels = 1;
        if(!myRank)
            fprintf(stderr, "Number of QOS levels not specified, setting to %d\n", p->num_qos_levels);
    }
    
    char qos_levels_str[MAX_NAME_LENGTH];
    rc = configuration_get_value(&config, "PARAMS", "qos_bandwidth", anno, qos_levels_str, MAX_NAME_LENGTH);
    p->qos_bandwidths = (int*)calloc(p->num_qos_levels, sizeof(int));

    if(p->num_qos_levels > 1)
    {
        int total_bw = 0;
        char * token;
        token = strtok(qos_levels_str, ",");
        int i = 0;
        while(token != NULL)
        {
            sscanf(token, "%d", &p->qos_bandwidths[i]);
            total_bw += p->qos_bandwidths[i];
            if(p->qos_bandwidths[i] <= 0)
            {
                tw_error(TW_LOC, "\nInvalid bandwidth levels\n");
            }
            i++;
            token = strtok(NULL,",");
        }
        assert(total_bw <= 100);
    }
    else
        p->qos_bandwidths[0] = 100;

    rc = configuration_get_value_double(&config, "PARAMS", "max_qos_monitor", anno, &max_qos_monitor);
    if(rc) {
        if(!myRank)
            fprintf(stderr, "Setting max_qos_monitor to %lf\n", max_qos_monitor);
	}

    rc = configuration_get_value_int(&config, "PARAMS", "cn_vc_size", anno, &p->cn_vc_size);
    if (rc) {
        p->cn_vc_size = 1024;
        if(!myRank)
            fprintf(stderr, "Buffer size of compute node channels not specified, setting to %d\n", p->cn_vc_size);
    }

    rc = configuration_get_value_int(&config, "PARAMS", "chunk_size", anno, &p->chunk_size);
    if (rc) {
        p->chunk_size = 512;
        if(!myRank)
            fprintf(stderr, "Chunk size for packets is specified, setting to %d\n", p->chunk_size);
    }

    rc = configuration_get_value_double(&config, "PARAMS", "local_bandwidth", anno, &p->local_bandwidth);
    if (rc) {
        p->local_bandwidth = 5.25;
        if(!myRank)
            fprintf(stderr, "Bandwidth of local channels not specified, setting to %lf\n", p->local_bandwidth);
    }

    rc = configuration_get_value_double(&config, "PARAMS", "global_bandwidth", anno, &p->global_bandwidth);
    if (rc) {
        p->global_bandwidth = 4.7;
        if(!myRank)
            fprintf(stderr, "Bandwidth of global channels not specified, setting to %lf\n", p->global_bandwidth);
    }

    rc = configuration_get_value_double(&config, "PARAMS", "cn_bandwidth", anno, &p->cn_bandwidth);
    if (rc) {
        p->cn_bandwidth = 5.25;
        if(!myRank)
            fprintf(stderr, "Bandwidth of compute node channels not specified, setting to %lf\n", p->cn_bandwidth);
    }

    rc = configuration_get_value_double(&config, "PARAMS", "router_delay", anno, &p->router_delay);
    if (rc) {
        p->router_delay = 100;
    }

    configuration_get_value(&config, "PARAMS", "cn_sample_file", anno, cn_sample_file, MAX_NAME_LENGTH);
    configuration_get_value(&config, "PARAMS", "rt_sample_file", anno, router_sample_file, MAX_NAME_LENGTH);

    char routing_str[MAX_NAME_LENGTH];
    configuration_get_value(&config, "PARAMS", "routing", anno, routing_str, MAX_NAME_LENGTH);
    if (strcmp(routing_str, "minimal") == 0)
        routing = MINIMAL;
    else if (strcmp(routing_str, "non-minimal-spine") == 0)
        routing = NON_MINIMAL_SPINE;
    else if (strcmp(routing_str, "non-minimal-leaf") == 0)
        routing = NON_MINIMAL_LEAF;
    else if (strcmp(routing_str, "prog-adaptive") == 0)
        routing = PROG_ADAPTIVE;
    else if (strcmp(routing_str, "fully-prog-adaptive") == 0)
        routing = FULLY_PROG_ADAPTIVE;
    else {
        if(!myRank)
            fprintf(stderr, "No routing protocol specified, setting to minimal routing\n");
        routing = MINIMAL;
    }

    rc = configuration_get_value_int(&config, "PARAMS", "notification_on_hops_greater_than", anno, &p->max_hops_notify);
    if (rc) {
        if(!myRank)
            fprintf(stderr, "Maximum hops for notifying not specified, setting to INT MAX\n");
        p->max_hops_notify = INT_MAX;
    }

    int src_leaf_cons_choice;
    rc = configuration_get_value_int(&config, "PARAMS", "source_leaf_consider_nonmin", anno, &src_leaf_cons_choice);
    if (rc) {
        // fprintf(stderr, "Source leaf consideration of nonmin ports not specified. Defaulting to True\n");
        p->source_leaf_consider_nonmin = true;
    }
    else if (src_leaf_cons_choice == 1) {
        p->source_leaf_consider_nonmin = true;
    }
    else
        p->source_leaf_consider_nonmin = false;


    int int_spn_cons_choice;
    rc = configuration_get_value_int(&config, "PARAMS", "int_spine_consider_min", anno, &int_spn_cons_choice);
    if (rc) {
        // fprintf(stderr, "Int spine consideration of min ports not specified. Defaulting to False\n");
        p->int_spine_consider_min = false;
    }
    else if (int_spn_cons_choice == 1) {
        p->int_spine_consider_min = true;
    }
    else
        p->int_spine_consider_min = false;

    int dst_spn_cons_choice;
    rc = configuration_get_value_int(&config, "PARAMS", "dest_spine_consider_nonmin", anno, &dst_spn_cons_choice);
    if (rc) {
        // fprintf(stderr, "Dest spine consideration of nonmin ports not specified. Defaulting to False\n");
        p->dest_spine_consider_nonmin = false;
    }
    else if (dst_spn_cons_choice == 1) {
        p->dest_spine_consider_nonmin = true;
    }
    else
        p->dest_spine_consider_nonmin = false;


    int dst_spn_gcons_choice;
    rc = configuration_get_value_int(&config, "PARAMS", "dest_spine_consider_global_nonmin", anno, &dst_spn_gcons_choice);
    if (rc) {
        // fprintf(stderr, "Dest spine consideration of global nonmin ports not specified. Defaulting to True\n");
        p->dest_spine_consider_global_nonmin = true;
    }
    else if (dst_spn_gcons_choice == 1) {
        p->dest_spine_consider_global_nonmin = true;
    }
    else
        p->dest_spine_consider_global_nonmin = false;


    /* MM: This should be 2 for dragonfly plus*/
    p->num_vcs = 2;
    
    if(p->num_qos_levels > 1)
        p->num_vcs = p->num_qos_levels * p->num_vcs;

    rc = configuration_get_value_int(&config, "PARAMS", "num_groups", anno, &p->num_groups);
    if (rc) {
        tw_error(TW_LOC, "\nnum_groups not specified, Aborting\n");
    }
    rc = configuration_get_value_int(&config, "PARAMS", "num_router_spine", anno, &p->num_router_spine);
    if (rc) {
        tw_error(TW_LOC, "\nnum_router_spine not specified, Aborting\n");
    }
    rc = configuration_get_value_int(&config, "PARAMS", "num_router_leaf", anno, &p->num_router_leaf);
    if (rc) {
        tw_error(TW_LOC, "\nnum_router_leaf not specified, Aborting\n");
    }

    p->num_routers = p->num_router_spine + p->num_router_leaf;  // num routers per group
    p->intra_grp_radix = max(p->num_router_spine, p->num_router_leaf); //TODO: Is this sufficient? If there are parallel intra connecitons, this will break.

    rc = configuration_get_value_int(&config, "PARAMS", "num_cns_per_router", anno, &p->num_cn);
    if (rc) {
        if(!myRank)
            fprintf(stderr,"Number of cns per router not specified, setting to %d\n", 4);
        p->num_cn = 4;
    }

    rc = configuration_get_value_int(&config, "PARAMS", "num_global_connections", anno, &p->num_global_connections);
    if (rc) {
        tw_error(TW_LOC, "\nnum_global_connections per router not specified, abortin...");
    }
    p->radix = p->intra_grp_radix + p->num_global_connections +
               p->num_cn;  // TODO this may not be sufficient, radix isn't same for leaf and spine routers
    p->total_routers = p->num_groups * p->num_routers;
    p->total_terminals = (p->num_groups * p->num_router_leaf) * p->num_cn;

    char scoring_str[MAX_NAME_LENGTH];
    configuration_get_value(&config, "PARAMS", "route_scoring_metric", anno, scoring_str, MAX_NAME_LENGTH);
    if (strcmp(scoring_str, "alpha") == 0) {
        scoring = ALPHA;
        scoring_preference = LOWER;
    }
    else if (strcmp(scoring_str, "beta") == 0) {
        scoring = BETA;
        scoring_preference = LOWER;
    }
    else if (strcmp(scoring_str, "gamma") == 0) {
        tw_error(TW_LOC, "Gamma scoring protocol currently non-functional"); //TODO: Fix gamma scoring protocol
        scoring = GAMMA;
        scoring_preference = HIGHER;
    }
    else if (strcmp(scoring_str, "delta") == 0) {
        scoring = DELTA;
        scoring_preference = LOWER;
    }
    else {
        if(!myRank)
            fprintf(stderr, "No route scoring protocol specified, setting to alpha scoring\n");
        scoring = ALPHA;
        scoring_preference = LOWER;
    }

    rc = configuration_get_value_int(&config, "PARAMS", "adaptive_threshold", anno, &p->adaptive_threshold);
    if (rc) {
        if(!myRank)
            fprintf(stderr, "Adaptive Minimal Routing Threshold not specified: setting to default = 0. (Will consider minimal and nonminimal routes based on scoring metric alone)\n");
        p->adaptive_threshold = 0;
    }


    int largest_vc_size = 0;
    if (p->local_vc_size > largest_vc_size)
        largest_vc_size = p->local_vc_size;
    if (p->global_vc_size > largest_vc_size)
        largest_vc_size = p->global_vc_size;
    if (p->cn_vc_size > largest_vc_size)
        largest_vc_size = p->cn_vc_size;

    p->max_port_score = (p->num_vcs * largest_vc_size) + largest_vc_size; //The maximum score that a port can get during the scoring metrics.

    // read intra group connections, store from a router's perspective
    // all links to the same router form a vector
    char intraFile[MAX_NAME_LENGTH];
    configuration_get_value(&config, "PARAMS", "intra-group-connections", anno, intraFile, MAX_NAME_LENGTH);
    if (strlen(intraFile) <= 0) {
        tw_error(TW_LOC, "\nIntra group connections file not specified. Aborting\n");
    }

    //setup Connection Managers for each router
    for(int i = 0; i < p->total_routers; i++)
    {
        int src_id_global = i;
        int src_id_local = i % p->num_routers;
        int src_group = i / p->num_routers;

        ConnectionManager conman = ConnectionManager(src_id_local, src_id_global, src_group, p->intra_grp_radix, p->num_global_connections, p->num_cn, p->num_routers);
        connManagerList.push_back(conman);
    }

    FILE *groupFile = fopen(intraFile, "rb");
    if (!groupFile)
        tw_error(TW_LOC, "\nintra-group file not found\n");

    IntraGroupLink newLink;
    while (fread(&newLink, sizeof(IntraGroupLink), 1, groupFile) != 0) {
        int src_id_local = newLink.src;
        int dest_id_local = newLink.dest;
        for(int i = 0; i < p->total_routers; i++)
        {
            int group_id = i/p->num_routers;
            if (i % p->num_routers == src_id_local)
            {
                int dest_id_global = group_id * p->num_routers + dest_id_local;
                connManagerList[i].add_connection(dest_id_global, CONN_LOCAL);
            }
        }
    }
    fclose(groupFile);

    //terminal assignment!
    for(int i = 0; i < p->total_terminals; i++)
    {
        int assigned_router_id = dragonfly_plus_get_assigned_router_id(i, p);
        int assigned_group_id = assigned_router_id / p->num_routers;
        connManagerList[assigned_router_id].add_connection(i, CONN_TERMINAL);
    }

    // read inter group connections, store from a router's perspective
    // also create a group level table that tells all the connecting routers
    char interFile[MAX_NAME_LENGTH];
    configuration_get_value(&config, "PARAMS", "inter-group-connections", anno, interFile, MAX_NAME_LENGTH);
    if (strlen(interFile) <= 0) {
        tw_error(TW_LOC, "\nInter group connections file not specified. Aborting\n");
    }
    FILE *systemFile = fopen(interFile, "rb");
    if (!myRank) {
        printf("Reading inter-group connectivity file: %s\n", interFile);
        printf("\nTotal routers: %d; total groups: %d \n", p->total_routers, p->num_groups);
    }

    connectionList.resize(p->num_groups);
    for (int g = 0; g < connectionList.size(); g++) {
        connectionList[g].resize(p->num_groups);
    }

    InterGroupLink newInterLink;
    while (fread(&newInterLink, sizeof(InterGroupLink), 1, systemFile) != 0) {
        int src_id_global = newInterLink.src;
        int src_group_id = src_id_global / p->num_routers;
        int dest_id_global = newInterLink.dest;
        int dest_group_id = dest_id_global / p->num_routers;

        // printf("[%d -> %d]\n",src_id_global, dest_id_global);
        connManagerList[src_id_global].add_connection(dest_id_global, CONN_GLOBAL);

        int r;
        for (r = 0; r < connectionList[src_group_id][dest_group_id].size(); r++) {
            if (connectionList[src_group_id][dest_group_id][r] == newInterLink.src)
                break;
        }
        if (r == connectionList[src_group_id][dest_group_id].size()) {
            connectionList[src_group_id][dest_group_id].push_back(newInterLink.src);
        }
    }

    if (DUMP_CONNECTIONS)
    {
        if (!myRank) {
            for(int i=0; i < connManagerList.size(); i++)
            {
                connManagerList[i].print_connections();
            }
        }
    }

    for(int i = 0; i < p->total_routers; i++){
        int loc_id = i % p->num_routers;
        if (loc_id < p->num_router_leaf)
            router_type_map[i] = LEAF;
        else
            router_type_map[i] = SPINE;
    }
    
    if (!myRank) {
        printf("\nTotal nodes: %d,  Total routers: %d, Num groups: %d, Routers per group: %d, Virtual radix: %d\n",
               p->num_cn * p->num_router_leaf * p->num_groups, p->total_routers, p->num_groups, p->num_routers, p->radix);
    }

    rc = configuration_get_value_double(&config, "PARAMS", "cn_delay", anno, &p->cn_delay);
    if (rc) {
        p->cn_delay = bytes_to_ns(p->chunk_size, p->cn_bandwidth);
        if(!myRank)
            fprintf(stderr, "cn_delay not specified, using default calculation: %.2f\n", p->cn_delay);
    }

    rc = configuration_get_value_double(&config, "PARAMS", "local_delay", anno, &p->local_delay);
    if (rc) {
        p->local_delay = bytes_to_ns(p->chunk_size, p->local_bandwidth);
        if(!myRank)
            fprintf(stderr, "local_delay not specified, using default calculation: %.2f\n", p->local_delay);
    }
    rc = configuration_get_value_double(&config, "PARAMS", "global_delay", anno, &p->global_delay);
    if (rc) {
        p->global_delay = bytes_to_ns(p->chunk_size, p->global_bandwidth);
        if(!myRank)
            fprintf(stderr, "global_delay not specified, using default calculation: %.2f\n", p->global_delay);
    }


    //CREDIT DELAY CONFIGURATION LOGIC ------------
    rc = configuration_get_value_int(&config, "PARAMS", "credit_size", anno, &p->credit_size);
    if (rc) {
        p->credit_size = 8;
        if(!myRank)
            fprintf(stderr, "credit_size not specified, using default: %d\n", p->credit_size);
    }

    double general_credit_delay;
    int credit_delay_unset = configuration_get_value_double(&config, "PARAMS", "credit_delay", anno, &general_credit_delay); 
    int local_credit_delay_unset = configuration_get_value_double(&config, "PARAMS", "local_credit_delay", anno, &p->local_credit_delay);
    int global_credit_delay_unset = configuration_get_value_double(&config, "PARAMS", "global_credit_delay", anno, &p->global_credit_delay);
    int cn_credit_delay_unset = configuration_get_value_double(&config, "PARAMS", "cn_credit_delay", anno, &p->cn_credit_delay);

    int auto_credit_delay_flag;
    rc = configuration_get_value_int(&config, "PARAMS", "auto_credit_delay", anno, &auto_credit_delay_flag);
    if (rc) {
        auto_credit_delay_flag = 0;
    }
    else {
        if(!myRank && auto_credit_delay_flag)
            fprintf(stderr, "auto_credit_delay flag enabled. All credit delays will be calculated based on their respective bandwidths\n");
    }

    //If the user specifies a general "credit_delay" AND any of the more specific credit delays, throw an error to make sure they correct their configuration
    if (!credit_delay_unset && !(local_credit_delay_unset || global_credit_delay_unset || cn_credit_delay_unset))
        tw_error(TW_LOC, "\nCannot set both a general credit delay and specific (local/global/cn) credit delays. Check configuration file.");
    
    //If the user specifies ANY credit delays general or otherwise AND has the auto credit delay flag enabled, throw an error to make sure they correct the conflicting configuration
    if ((!credit_delay_unset || !local_credit_delay_unset || !global_credit_delay_unset || !cn_credit_delay_unset) && auto_credit_delay_flag)
        tw_error(TW_LOC, "\nCannot set both a credit delay (general or specific) and also enable auto credit delay calculation. Check Configuration file.");

    //If the user doesn't specify either general or specific credit delays - calculate credit delay based on local bandwidth.
    //This is old legacy behavior that is left in to make sure that the credit delay configurations of old aren't semantically different
    //Other possible way to program this would be to make each credit delay be set based on their respective bandwidths but this semantically
    //changes the behavior of old configuration files. (although it would be more accurate)
    if (credit_delay_unset && local_credit_delay_unset && global_credit_delay_unset && cn_credit_delay_unset && !auto_credit_delay_flag) {
        p->local_credit_delay = bytes_to_ns(p->credit_size, p->local_bandwidth);
        p->global_credit_delay = p->local_credit_delay;
        p->cn_credit_delay = p->local_credit_delay;
        if(!myRank)
            fprintf(stderr, "no credit_delay specified - all credit delays set to %.2f\n",p->local_credit_delay);
    }
    //If the user doesn't specify a general credit delay but leaves any of the specific credit delay values unset, then we need to set those (the above conditional handles if none of them had been set)
    else if (credit_delay_unset) {
        if (local_credit_delay_unset) {
            p->local_credit_delay = bytes_to_ns(p->credit_size, p->local_bandwidth);
            if(!myRank && !auto_credit_delay_flag) //if the auto credit delay flag is true then we've already printed what we're going to do
                fprintf(stderr, "local_credit_delay not specified, using calculation based on local bandwidth: %.2f\n", p->local_credit_delay);
        }
        if (global_credit_delay_unset) {
            p->global_credit_delay = bytes_to_ns(p->credit_size, p->global_bandwidth);
            if(!myRank && !auto_credit_delay_flag) //if the auto credit delay flag is true then we've already printed what we're going to do
                fprintf(stderr, "global_credit_delay not specified, using calculation based on global bandwidth: %.2f\n", p->global_credit_delay);   
        }
        if (cn_credit_delay_unset) {
            p->cn_credit_delay = bytes_to_ns(p->credit_size, p->cn_bandwidth);
            if(!myRank && !auto_credit_delay_flag) //if the auto credit delay flag is true then we've already printed what we're going to do
                fprintf(stderr, "cn_credit_delay not specified, using calculation based on cn bandwidth: %.2f\n", p->cn_credit_delay);
        }
    }
    //If the user specifies a general credit delay (but didn't specify any specific credit delays) then we set all specific credit delays to the general
    else if (!credit_delay_unset) {
        p->local_credit_delay = general_credit_delay;
        p->global_credit_delay = general_credit_delay;
        p->cn_credit_delay = general_credit_delay;
        
        if(!myRank)
            fprintf(stderr, "general credit_delay specified - all credit delays set to %.2f\n",general_credit_delay);
    }
    //END CREDIT DELAY CONFIGURATION LOGIC ----------------

    if (PRINT_CONFIG && !myRank) {
        dragonfly_plus_print_params(p, stderr);
    }
    stored_params = p;
}

void dragonfly_plus_configure()
{
    anno_map = codes_mapping_get_lp_anno_map(LP_CONFIG_NM_TERM);
    assert(anno_map);
    num_params = anno_map->num_annos + (anno_map->has_unanno_lp > 0);
    all_params = (dragonfly_plus_param *) calloc(num_params, sizeof(*all_params));

    for (int i = 0; i < anno_map->num_annos; i++) {
        const char *anno = anno_map->annotations[i].ptr;
        dragonfly_read_config(anno, &all_params[i]);
    }
    if (anno_map->has_unanno_lp > 0) {
        dragonfly_read_config(NULL, &all_params[anno_map->num_annos]);
    }
#ifdef ENABLE_CORTEX
//    model_net_topology = dragonfly_plus_cortex_topology;
#endif
}

/* report dragonfly statistics like average and maximum packet latency, average number of hops traversed */
void dragonfly_plus_report_stats()
{
    long long avg_hops, total_finished_packets, total_finished_chunks;
    long long total_finished_msgs, final_msg_sz;
    tw_stime avg_time, max_time;
    int total_minimal_packets, total_nonmin_packets;
    long total_gen, total_fin;
    long total_local_packets_sr, total_local_packets_sg, total_remote_packets;

    MPI_Reduce(&total_hops, &avg_hops, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_CODES);
    MPI_Reduce(&N_finished_packets, &total_finished_packets, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_CODES);
    MPI_Reduce(&N_finished_msgs, &total_finished_msgs, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_CODES);
    MPI_Reduce(&N_finished_chunks, &total_finished_chunks, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_CODES);
    MPI_Reduce(&total_msg_sz, &final_msg_sz, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_CODES);
    MPI_Reduce(&dragonfly_total_time, &avg_time, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_CODES);
    MPI_Reduce(&dragonfly_max_latency, &max_time, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_CODES);

    MPI_Reduce(&packet_gen, &total_gen, 1, MPI_LONG, MPI_SUM, 0, MPI_COMM_CODES);
    MPI_Reduce(&packet_fin, &total_fin, 1, MPI_LONG, MPI_SUM, 0, MPI_COMM_CODES);
    MPI_Reduce( &num_local_packets_sr, &total_local_packets_sr, 1, MPI_LONG, MPI_SUM, 0, MPI_COMM_CODES);
    MPI_Reduce( &num_local_packets_sg, &total_local_packets_sg, 1, MPI_LONG, MPI_SUM, 0, MPI_COMM_CODES);
    MPI_Reduce( &num_remote_packets, &total_remote_packets, 1, MPI_LONG, MPI_SUM, 0, MPI_COMM_CODES);
    if(isRoutingAdaptive(routing) || SHOW_ADAPTIVE_STATS) {
        MPI_Reduce(&minimal_count, &total_minimal_packets, 1, MPI_INT, MPI_SUM, 0, MPI_COMM_CODES);
        MPI_Reduce(&nonmin_count, &total_nonmin_packets, 1, MPI_INT, MPI_SUM, 0, MPI_COMM_CODES);
    }

    /* print statistics */
    if (!g_tw_mynode) {
        if (PRINT_CONFIG) 
            dragonfly_plus_print_params(stored_params, NULL);


        printf(
            "Average number of router hops traversed: %f; average chunk latency: %lf us; maximum chunk latency: %lf us; "
            "avg message size: %lf bytes; finished messages: %lld; finished chunks: %lld \n",
            (float) avg_hops / total_finished_chunks, avg_time / (total_finished_chunks * 1000),
            max_time / 1000, (float) final_msg_sz / total_finished_msgs, total_finished_msgs,
            total_finished_chunks);
        if(isRoutingAdaptive(routing) || SHOW_ADAPTIVE_STATS) {
            printf("\nADAPTIVE ROUTING STATS: %d chunks routed minimally %d chunks routed non-minimally - completed packets: %lld \n",
                total_minimal_packets, total_nonmin_packets, total_finished_chunks);
        }
      printf("\nTotal packets generated: %ld; finished: %ld; Locally routed: same router: %ld, different-router: %ld; Remote (inter-group): %ld \n", total_gen, total_fin, total_local_packets_sr, total_local_packets_sg, total_remote_packets);
    }
    return;
}

int get_vcg_from_category(terminal_plus_message * msg)
{
   if(strcmp(msg->category, "high") == 0)
       return Q_HIGH;
   else if(strcmp(msg->category, "medium") == 0)
       return Q_MEDIUM;
   else
       tw_error(TW_LOC, "\n priority needs to be specified with qos_levels > 1 %s", msg->category);
}

static int get_term_bandwidth_consumption(terminal_state * s, int qos_lvl)
{
    assert(qos_lvl >= Q_HIGH && qos_lvl <= Q_LOW);

    /* conversion into bytes/sec from GiB/sec */
    double max_bw = s->params->cn_bandwidth * 1024.0 * 1024.0 * 1024.0;
    /* conversion into bytes per one nanosecs */
    double max_bw_per_ns = max_bw / (1000.0 * 1000.0 * 1000.0);
    /* derive maximum bytes that can be transferred during the window */
    double max_bytes_per_win = max_bw_per_ns * bw_reset_window;
    int percent_bw = (((double)s->qos_data[qos_lvl]) / max_bytes_per_win) * 100;
//    printf("\n At terminal %lf max bytes %d percent %d ", max_bytes_per_win, s->qos_data[qos_lvl], percent_bw);
    return percent_bw;
}

static int get_rtr_bandwidth_consumption(router_state * s, int qos_lvl, int output_port)
{
    assert(qos_lvl >= Q_HIGH && qos_lvl <= Q_LOW);
    assert(output_port < s->params->intra_grp_radix + s->params->num_global_connections + s->params->num_cn);

    int bandwidth = s->params->cn_bandwidth;
    if(output_port < s->params->intra_grp_radix)
        bandwidth = s->params->local_bandwidth;
    else if(output_port < s->params->intra_grp_radix + s->params->num_global_connections)
        bandwidth = s->params->global_bandwidth;

    /* conversion into bytes/sec from GiB/sec */
    double max_bw = bandwidth * 1024.0 * 1024.0 * 1024.0;
    /* conversion into bytes per one nanosecs */
    double max_bw_per_ns = max_bw / (1000.0 * 1000.0 * 1000.0);
    /* derive maximum bytes that can be transferred during the window */
    double max_bytes_per_win = max_bw_per_ns * bw_reset_window;

    int percent_bw = (((double)s->qos_data[output_port][qos_lvl]) / max_bytes_per_win) * 100;
//    printf("\n percent bw consumed by qos_lvl %d is %d bytes transferred %d max_bw %lf ", qos_lvl, percent_bw, s->qos_data[output_port][qos_lvl], max_bw_per_ns);
    return percent_bw;
}

void issue_bw_monitor_event_rc(terminal_state * s, tw_bf * bf, terminal_plus_message * msg, tw_lp * lp)
{
    for(int i = 0 ; i < msg->num_cll; i++)
        codes_local_latency_reverse(lp);
    
    int num_qos_levels = s->params->num_qos_levels;
    
    if(msg->rc_is_qos_set == 1)
    {
        for(int i = 0; i < num_qos_levels; i++)
        {
            s->qos_data[i] = msg->rc_qos_data[i];
            s->qos_status[i] = msg->rc_qos_status[i];    
        }

        free(msg->rc_qos_data);
        free(msg->rc_qos_status);
        msg->rc_is_qos_set = 0;
    }
    
}
/* resets the bandwidth numbers recorded so far */
void issue_bw_monitor_event(terminal_state * s, tw_bf * bf, terminal_plus_message * msg, tw_lp * lp)
{
   
    msg->num_cll = 0;
    msg->num_rngs = 0;
    int num_qos_levels = s->params->num_qos_levels;
    
    //RC data storage start.
    //Allocate memory here for these pointers that are stored in the events. FREE THESE IN RC OR IN COMMIT_F
    msg->rc_qos_data = (unsigned long long *) calloc(num_qos_levels, sizeof(unsigned long long));
    msg->rc_qos_status = (int *) calloc(num_qos_levels, sizeof(int));

    //store qos data and status into the arrays. Pointers to the arrays are stored in events.
    for(int i = 0; i < num_qos_levels; i++)
    {
        msg->rc_qos_data[i] = s->qos_data[i];
        msg->rc_qos_status[i] = s->qos_status[i];
    }
    msg->rc_is_qos_set = 1;
    //RC data storage end.

    /* Reset the qos status and bandwidth consumption. */
    for(int i = 0; i < num_qos_levels; i++)
    {
        s->qos_status[i] = Q_ACTIVE;
        s->qos_data[i] = 0;
    }

    if(tw_now(lp) > max_qos_monitor)
        return;
    
    msg->num_cll++;
    terminal_plus_message * m; 
    tw_stime bw_ts = bw_reset_window + codes_local_latency(lp);
    tw_event * e = model_net_method_event_new(lp->gid, bw_ts, lp, DRAGONFLY_PLUS,
            (void**)&m, NULL); 
    m->type = T_BANDWIDTH;
    m->magic = terminal_magic_num; 
    tw_event_send(e);
}

void issue_rtr_bw_monitor_event_rc(router_state *s, tw_bf *bf, terminal_plus_message *msg, tw_lp *lp)
{
    int radix = s->params->radix;
    int num_qos_levels = s->params->num_qos_levels;

    for(int i = 0 ; i < msg->num_cll; i++)
        codes_local_latency_reverse(lp);

    if(msg->rc_is_qos_set == 1)
    {
        for(int i = 0; i < radix; i++)
        {
            for(int j = 0; j < num_qos_levels; j++)
            {
                s->qos_data[i][j] = *(indexer2d(msg->rc_qos_data, i, j, radix, num_qos_levels));
                s->qos_status[i][j] = *(indexer2d(msg->rc_qos_status, i, j, radix, num_qos_levels));
            }
        }

        free(msg->rc_qos_data);
        free(msg->rc_qos_status);
        msg->rc_is_qos_set = 0;
    }
}
void issue_rtr_bw_monitor_event(router_state *s, tw_bf *bf, terminal_plus_message *msg, tw_lp *lp)
{
    msg->num_cll = 0;
    msg->num_rngs = 0;

    int radix = s->params->radix;
    int num_qos_levels = s->params->num_qos_levels;
    

    //RC data storage start.
    //Allocate memory here for these pointers that are stored in the events. FREE THESE IN RC OR IN COMMIT_F
    msg->rc_qos_data = (unsigned long long *) calloc(radix * num_qos_levels, sizeof(unsigned long long));
    msg->rc_qos_status = (int *) calloc(radix * num_qos_levels, sizeof(int));

    //store qos data and status into the arrays. Pointers to the arrays are stored in events.
    for(int i = 0; i < radix; i++)
    {
        for(int j = 0; j < num_qos_levels; j++)
        {
            *(indexer2d(msg->rc_qos_data, i, j, radix, num_qos_levels)) = s->qos_data[i][j];
            *(indexer2d(msg->rc_qos_status, i, j, radix, num_qos_levels)) = s->qos_status[i][j];
        }
    }
    msg->rc_is_qos_set = 1;
    //RC data storage end.


    for(int i = 0; i < radix; i++)
    {
        for(int j = 0; j < num_qos_levels; j++)
        {
            int bw_consumed = get_rtr_bandwidth_consumption(s, j, i);
        
            #if DEBUG_QOS == 1 
            if(dragonfly_rtr_bw_log != NULL)
            {
                if(s->qos_data[j][k] > 0)
                {
                    fprintf(dragonfly_rtr_bw_log, "\n %d %f %d %d %d %d %d %f", s->router_id, tw_now(lp), i, j, bw_consumed, s->qos_status[i][j], s->qos_data[i][j], s->busy_time_sample[i]);
                }
            }
            #endif   
        }
    }

    /* Reset the qos status and bandwidth consumption. */
    for(int i = 0; i < s->params->radix; i++)
    {
        for(int j = 0; j < num_qos_levels; j++)
        {
            s->qos_status[i][j] = Q_ACTIVE;
            s->qos_data[i][j] = 0;
        }
        s->busy_time_sample[i] = 0;
        s->ross_rsample.busy_time[i] = 0;
    }

    if(tw_now(lp) > max_qos_monitor)
        return;
    
    msg->num_cll++;
    tw_stime bw_ts = bw_reset_window + codes_local_latency(lp);
    terminal_plus_message *m;
    tw_event * e = model_net_method_event_new(lp->gid, bw_ts, lp,
            DRAGONFLY_PLUS_ROUTER, (void**)&m, NULL);
    m->type = R_BANDWIDTH;
    m->magic = router_magic_num;
    tw_event_send(e);
}

static int get_next_vcg(terminal_state * s, tw_bf * bf, terminal_plus_message * msg, tw_lp * lp)
{
    int num_qos_levels = s->params->num_qos_levels;
  
    if(num_qos_levels == 1)
    {
        if(s->terminal_msgs[0] == NULL || ((s->vc_occupancy[0] + s->params->chunk_size) > s->params->cn_vc_size))
            return -1;
        else
            return 0;
    }

  int bw_consumption[num_qos_levels];

    /* First make sure the bandwidth consumptions are up to date. */
    for(int k = 0; k < num_qos_levels; k++)
    {
        if(s->qos_status[k] != Q_OVERBW)
        {
            bw_consumption[k] = get_term_bandwidth_consumption(s, k);
            if(bw_consumption[k] > s->params->qos_bandwidths[k]) 
            {
                if(k == 0)
                    msg->qos_reset1 = 1;
                else if(k == 1)
                    msg->qos_reset2 = 1;
                
                s->qos_status[k] = Q_OVERBW;
            }
        }
    }
    if(BW_MONITOR == 1)
    {
        for(int i = 0; i < num_qos_levels; i++)
        {
            if(s->qos_status[i] == Q_ACTIVE)
            {
                if(s->terminal_msgs[i] != NULL && ((s->vc_occupancy[i] + s->params->chunk_size) <= s->params->cn_vc_size))
                    return i;
            }
        }
    }

    int next_rr_vcg = (s->last_qos_lvl + 1) % num_qos_levels;
    /* All vcgs are exceeding their bandwidth limits*/
    for(int i = 0; i < num_qos_levels; i++)
    {
        if(s->terminal_msgs[i] != NULL && ((s->vc_occupancy[i] + s->params->chunk_size) <= s->params->cn_vc_size))
        {
            bf->c2 = 1;
            
            if(msg->last_saved_qos < 0)
                msg->last_saved_qos = s->last_qos_lvl;
            
            s->last_qos_lvl = next_rr_vcg;
            return i;
        }
        next_rr_vcg = (next_rr_vcg + 1) % num_qos_levels;
    }
    return -1;
}

static int get_next_router_vcg(router_state * s, tw_bf * bf, terminal_plus_message * msg, tw_lp * lp)
{
  int num_qos_levels = s->params->num_qos_levels;

  int vcs_per_qos = s->params->num_vcs / num_qos_levels;
  int output_port = msg->vc_index;
  int vcg = 0;
  int base_limit = 0;
    
  int chunk_size = s->params->chunk_size;
  int bw_consumption[num_qos_levels];
  /* First make sure the bandwidth consumptions are up to date. */
  if(BW_MONITOR == 1)
  {
    for(int k = 0; k < num_qos_levels; k++)
    {
        if(s->qos_status[output_port][k] != Q_OVERBW)
        {
            bw_consumption[k] = get_rtr_bandwidth_consumption(s, k, output_port);
            if(bw_consumption[k] > s->params->qos_bandwidths[k]) 
        {
//            printf("\n Router %d QoS %d exceeded allowed bandwidth %d ", s->router_id, k, bw_consumption[k]);
            if(k == 0)
                msg->qos_reset1 = 1;   
            else if(k == 1)
                msg->qos_reset2 = 1;

            s->qos_status[output_port][k] = Q_OVERBW;
        }
      }
  }
  int vc_size = s->params->global_vc_size;
  if(output_port < s->params->intra_grp_radix)
      vc_size = s->params->local_vc_size;

  /* TODO: If none of the vcg is exceeding bandwidth limit then select high
   * priority traffic first. */
   for(int i = 0; i < num_qos_levels; i++)
   {
        if(s->qos_status[output_port][i] == Q_ACTIVE)
        {
            int base_limit = i * vcs_per_qos;
            for(int k = base_limit; k < base_limit + vcs_per_qos; k ++)
            {
                if(s->pending_msgs[output_port][k] != NULL)
                    return k;
            }
        }
   }
  }
       
   /* All vcgs are exceeding their bandwidth limits*/
   msg->last_saved_qos = s->last_qos_lvl[output_port];
   int next_rr_vcg = (s->last_qos_lvl[output_port] + 1) % num_qos_levels;

   for(int i = 0; i < num_qos_levels; i++)
   {
        base_limit = next_rr_vcg * vcs_per_qos; 
        for(int k = base_limit; k < base_limit + vcs_per_qos; k++)
        {
            if(s->pending_msgs[output_port][k] != NULL)
            {
                if(msg->last_saved_qos < 0)
                    msg->last_saved_qos = s->last_qos_lvl[output_port]; 

                s->last_qos_lvl[output_port] = next_rr_vcg;
                return k;
            }
        }
        next_rr_vcg = (next_rr_vcg + 1) % num_qos_levels;
        assert(next_rr_vcg < 2);
   }
   return -1;
}

void terminal_plus_commit(terminal_state * s,
		tw_bf * bf, 
		terminal_plus_message * msg, 
        tw_lp * lp)
{
    if(msg->type == T_BANDWIDTH)
    {
        if(msg->rc_is_qos_set == 1) {
            free(msg->rc_qos_data);
            free(msg->rc_qos_status);
            msg->rc_is_qos_set = 0;
        }
    }
}

void router_plus_commit(router_state * s,
		tw_bf * bf, 
		terminal_plus_message * msg, 
        tw_lp * lp)
{
    if(msg->type == R_BANDWIDTH)
    {
        if(msg->rc_is_qos_set == 1) {
            free(msg->rc_qos_data);
            free(msg->rc_qos_status);
            msg->rc_is_qos_set = 0;
        }
    }
}

/* initialize a dragonfly compute node terminal */
void terminal_plus_init(terminal_state *s, tw_lp *lp)
{
    // printf("%d: Terminal Init()\n",lp->gid);
    s->packet_gen = 0;
    s->packet_fin = 0;
    s->total_gen_size = 0;
    s->is_monitoring_bw = 0;

    int i;
    char anno[MAX_NAME_LENGTH];

    // Assign the global router ID
    // TODO: be annotation-aware
    codes_mapping_get_lp_info(lp->gid, lp_group_name, &mapping_grp_id, NULL, &mapping_type_id, anno,
                              &mapping_rep_id, &mapping_offset);
    if (anno[0] == '\0') {
        s->anno = NULL;
        s->params = &all_params[num_params - 1];
    }
    else {
        s->anno = strdup(anno);
        int id = configuration_get_annotation_index(anno, anno_map);
        s->params = &all_params[id];
    }

    int num_qos_levels = s->params->num_qos_levels;
    int num_lps = codes_mapping_get_lp_count(lp_group_name, 1, LP_CONFIG_NM_TERM, s->anno, 0);
    

    s->terminal_id = codes_mapping_get_lp_relative_id(lp->gid, 0, 0);
    s->router_id = dragonfly_plus_get_assigned_router_id(s->terminal_id, s->params);
    //    s->router_id=(int)s->terminal_id / (s->params->num_cn); //TODO I think this is where the router that
    //    the terminal is connected to is specified

    // printf("%d gid is TERMINAL %d with assigned router %d\n",lp->gid,s->terminal_id,s->router_id);
    s->terminal_available_time = 0.0;
    s->packet_counter = 0;
    s->min_latency = INT_MAX;
    s->max_latency = 0;

    s->finished_msgs = 0;
    s->finished_chunks = 0;
    s->finished_packets = 0;
    s->total_time = 0.0;
    s->total_msg_size = 0;

    s->stalled_chunks = 0;
    s->busy_time = 0.0;

    s->fwd_events = 0;
    s->rev_events = 0;

    rc_stack_create(&s->st);
    
    s->num_vcs = 1;
    if(num_qos_levels > 1)
        s->num_vcs *= num_qos_levels;
   
    /* Whether the virtual channel group is active or over-bw*/
    s->qos_status = (int*)calloc(num_qos_levels, sizeof(int));
   
    /* How much data has been transmitted on the virtual channel group within
    * the window */
    s->qos_data = (unsigned long long*)calloc(num_qos_levels, sizeof(unsigned long long));
    s->vc_occupancy = (int*)calloc(s->num_vcs, sizeof(int));

    
    for(i = 0; i < num_qos_levels; i++)
    {
       s->qos_data[i] = 0;
       s->qos_status[i] = Q_ACTIVE;
    }

    for(i = 0; i < s->num_vcs; i++)
    {
        s->vc_occupancy[i] = 0;
    }

    s->last_qos_lvl = 0;
    s->last_buf_full = 0;

    s->rank_tbl = NULL;
    s->terminal_msgs =
        (terminal_plus_message_list **) calloc(s->num_vcs, sizeof(terminal_plus_message_list *));
    s->terminal_msgs_tail =
        (terminal_plus_message_list **) calloc(s->num_vcs, sizeof(terminal_plus_message_list *));

    for(int i = 0; i < s->num_vcs; i++)
    {
        s->terminal_msgs[i] = NULL;
        s->terminal_msgs_tail[i] = NULL;
    }

    s->terminal_length = (unsigned long long*)calloc(s->num_vcs, sizeof(unsigned long long));
    s->in_send_loop = 0;
    s->issueIdle = 0;

    return;
}

/* sets up the router virtual channels, global channels,
 * local channels, compute node channels */
void router_plus_init(router_state *r, tw_lp *lp)
{
    // printf("%d: Router Init()\n",lp->gid);

    char anno[MAX_NAME_LENGTH];
    codes_mapping_get_lp_info(lp->gid, lp_group_name, &mapping_grp_id, NULL, &mapping_type_id, anno,
                              &mapping_rep_id, &mapping_offset);

    if (anno[0] == '\0') {
        r->anno = NULL;
        r->params = &all_params[num_params - 1];
    }
    else {
        r->anno = strdup(anno);
        int id = configuration_get_annotation_index(anno, anno_map);
        r->params = &all_params[id];
    }

    // shorthand
    const dragonfly_plus_param *p = r->params;

    num_routers_per_mgrp =
        codes_mapping_get_lp_count(lp_group_name, 1, "modelnet_dragonfly_plus_router", NULL, 0);
    int num_grp_reps = codes_mapping_get_group_reps(lp_group_name);
    if (p->total_routers != num_grp_reps * num_routers_per_mgrp)
        tw_error(TW_LOC,
                 "\n Config error: num_routers specified %d total routers computed in the network %d "
                 "does not match with repetitions * dragonfly_router %d  ",
                 p->num_routers, p->total_routers, num_grp_reps * num_routers_per_mgrp);

    r->router_id = codes_mapping_get_lp_relative_id(lp->gid, 0, 0);
    r->group_id = r->router_id / p->num_routers;

    // printf("\n Local router id %d global id %d ", r->router_id, lp->gid);

    r->num_rtr_rc_windows = 100;
    r->is_monitoring_bw = 0;
    r->fwd_events = 0;
    r->rev_events = 0;

    // QoS related variables
    int num_qos_levels = p->num_qos_levels;

    // Determine if router is a spine or a leaf
    int intra_group_id = r->router_id % p->num_routers;
    if (intra_group_id >= (p->num_routers / 2)) { //TODO this assumes symmetric spine and leafs
        r->dfp_router_type = SPINE;
        assert(router_type_map[r->router_id] == SPINE);
        // printf("%lu: %i is a SPINE\n",lp->gid, r->router_id);
    }
    else {
        r->dfp_router_type = LEAF;
        assert(router_type_map[r->router_id] == LEAF);
        // printf("%lu: %i is a LEAF\n",lp->gid, r->router_id);
    }
#if DEBUG_QOS == 1 
        char rtr_bw_log[128];
        sprintf(rtr_bw_log, "router-bw-tracker-%d", g_tw_mynode);
       
        if(dragonfly_rtr_bw_log == NULL)
        {
            dragonfly_rtr_bw_log = fopen(rtr_bw_log, "w+");
       
           fprintf(dragonfly_rtr_bw_log, "\n router-id time-stamp port-id qos-level bw-consumed qos-status qos-data busy-time");
        }
#endif 
    r->connMan = &connManagerList[r->router_id];

    r->gc_usage = (int *) calloc(p->num_global_connections, sizeof(int));

    r->global_channel = (int *) calloc(p->num_global_connections, sizeof(int));
    r->next_output_available_time = (tw_stime *) calloc(p->radix, sizeof(tw_stime));
    r->link_traffic = (int64_t *) calloc(p->radix, sizeof(int64_t));
    r->link_traffic_sample = (int64_t *) calloc(p->radix, sizeof(int64_t));

    r->stalled_chunks = (unsigned long*)calloc(p->radix, sizeof(unsigned long));

    r->vc_occupancy = (int **) calloc(p->radix, sizeof(int *));
    r->qos_data = (unsigned long long**)calloc(p->radix, sizeof(unsigned long long*));
    r->last_qos_lvl = (int*)calloc(p->radix, sizeof(int));
    r->qos_status = (int**)calloc(p->radix, sizeof(int*));
    r->in_send_loop = (int *) calloc(p->radix, sizeof(int));
    r->pending_msgs =
        (terminal_plus_message_list ***) calloc(p->radix, sizeof(terminal_plus_message_list **));
    r->pending_msgs_tail =
        (terminal_plus_message_list ***) calloc(p->radix, sizeof(terminal_plus_message_list **));
    r->queued_msgs =
        (terminal_plus_message_list ***) calloc(p->radix, sizeof(terminal_plus_message_list **));
    r->queued_msgs_tail =
        (terminal_plus_message_list ***) calloc(p->radix, sizeof(terminal_plus_message_list **));
    r->queued_count = (int *) calloc(p->radix, sizeof(int));
    r->last_buf_full = (tw_stime*) calloc(p->radix, sizeof(tw_stime *));
    r->busy_time = (tw_stime *) calloc(p->radix, sizeof(tw_stime));
    r->busy_time_sample = (tw_stime *) calloc(p->radix, sizeof(tw_stime));

    /* set up for ROSS stats sampling */
    r->link_traffic_ross_sample = (int64_t*)calloc(p->radix, sizeof(int64_t));
    r->busy_time_ross_sample = (tw_stime*)calloc(p->radix, sizeof(tw_stime));
    if (g_st_model_stats)
        lp->model_types->mstat_sz = sizeof(tw_lpid) + (sizeof(int64_t) + sizeof(tw_stime)) * p->radix;
    if (g_st_use_analysis_lps && g_st_model_stats)
        lp->model_types->sample_struct_sz = sizeof(struct dfly_router_sample) + (sizeof(tw_stime) + sizeof(int64_t)) * p->radix;
    r->ross_rsample.busy_time = (tw_stime*)calloc(p->radix, sizeof(tw_stime));
    r->ross_rsample.link_traffic_sample = (int64_t*)calloc(p->radix, sizeof(int64_t));

    //for counting app message percentage 
    if(p->counting_bool > 0)
    {   
        r->total_packets_received = 0;
        r->total_packets_sent = 0;
        r->msg_counting = (int **) calloc(p->counting_windows, sizeof(int *));
        r->msg_counting_out = (int **) calloc(p->counting_windows, sizeof(int *));

        for (int i = 0; i < p->counting_windows; ++i)
        {
            r->msg_counting[i] = (int*) calloc(2, sizeof(int));
            r->msg_counting_out[i] = (int*) calloc(2, sizeof(int));

            r->msg_counting[i][0] = 0;
            r->msg_counting[i][1] = 0;

            r->msg_counting_out[i][0] = 0;
            r->msg_counting_out[i][1] = 0;
        }
    }

    rc_stack_create(&r->st);

    for (int i = 0; i < p->radix; i++) {
        // Set credit & router occupancy
        r->last_buf_full[i] = 0.0;
        r->busy_time[i] = 0.0;
        r->busy_time_sample[i] = 0.0;
        r->next_output_available_time[i] = 0;
        r->last_qos_lvl[i] = 0;
        r->link_traffic[i] = 0;
        r->link_traffic_sample[i] = 0;
        r->queued_count[i] = 0;
        r->in_send_loop[i] = 0;
        r->vc_occupancy[i] = (int *) calloc(p->num_vcs, sizeof(int));
        r->pending_msgs[i] =
            (terminal_plus_message_list **) calloc(p->num_vcs, sizeof(terminal_plus_message_list *));
        r->pending_msgs_tail[i] =
            (terminal_plus_message_list **) calloc(p->num_vcs, sizeof(terminal_plus_message_list *));
        r->queued_msgs[i] =
            (terminal_plus_message_list **) calloc(p->num_vcs, sizeof(terminal_plus_message_list *));
        r->queued_msgs_tail[i] =
            (terminal_plus_message_list **) calloc(p->num_vcs, sizeof(terminal_plus_message_list *));
    
        r->qos_status[i] = (int*)calloc(num_qos_levels, sizeof(int));
        r->qos_data[i] = (unsigned long long*)calloc(num_qos_levels, sizeof(unsigned long long));
    
        for(int j = 0; j < num_qos_levels; j++)
        {
            r->qos_status[i][j] = Q_ACTIVE;
            r->qos_data[i][j] = 0;
        }
        for (int j = 0; j < p->num_vcs; j++) {
            r->vc_occupancy[i][j] = 0;
            r->pending_msgs[i][j] = NULL;
            r->pending_msgs_tail[i][j] = NULL;
            r->queued_msgs[i][j] = NULL;
            r->queued_msgs_tail[i][j] = NULL;
        }
    }

    r->connMan->solidify_connections();

    return;
}

/* dragonfly packet event reverse handler */
static void dragonfly_plus_packet_event_rc(tw_lp *sender)
{
    codes_local_latency_reverse(sender);
    return;
}

/* MM: These packet events (packet_send, packet_receive etc.) will be used as is, basically, the routing
 * functions will be changed only. */
/* dragonfly packet event , generates a dragonfly packet on the compute node */
static tw_stime dragonfly_plus_packet_event(model_net_request const *req,
                                            uint64_t message_offset,
                                            uint64_t packet_size,
                                            tw_stime offset,
                                            mn_sched_params const *sched_params,
                                            void const *remote_event,
                                            void const *self_event,
                                            tw_lp *sender,
                                            int is_last_pckt)
{
    (void) message_offset;
    (void) sched_params;
    tw_event *e_new;
    tw_stime xfer_to_nic_time;
    terminal_plus_message *msg;
    char *tmp_ptr;

    xfer_to_nic_time = codes_local_latency(sender);
    // e_new = tw_event_new(sender->gid, xfer_to_nic_time+offset, sender);
    // msg = tw_event_data(e_new);
    e_new = model_net_method_event_new(sender->gid, xfer_to_nic_time + offset, sender, DRAGONFLY_PLUS,
                                       (void **) &msg, (void **) &tmp_ptr);
    strcpy(msg->category, req->category);
    msg->final_dest_gid = req->final_dest_lp;
    msg->total_size = req->msg_size;
    msg->sender_lp = req->src_lp;
    msg->sender_mn_lp = sender->gid;
    msg->packet_size = packet_size;
    msg->travel_start_time = tw_now(sender);
    msg->remote_event_size_bytes = 0;
    msg->local_event_size_bytes = 0;
    msg->type = T_GENERATE;
    msg->dest_terminal_id = req->dest_mn_lp;
    msg->dfp_dest_terminal_id = codes_mapping_get_lp_relative_id(msg->dest_terminal_id,0,0);
    msg->message_id = req->msg_id;
    msg->is_pull = req->is_pull;
    msg->pull_size = req->pull_size;
    msg->magic = terminal_magic_num;
    msg->msg_start_time = req->msg_start_time;

    //for message app id tracking
    msg->dfp_src_terminal_id = 0;
    msg->app_id = req->app_id;

    if (is_last_pckt) /* Its the last packet so pass in remote and local event information*/
    {
        if (req->remote_event_size > 0) {
            msg->remote_event_size_bytes = req->remote_event_size;
            memcpy(tmp_ptr, remote_event, req->remote_event_size);
            tmp_ptr += req->remote_event_size;
        }
        if (req->self_event_size > 0) {
            msg->local_event_size_bytes = req->self_event_size;
            memcpy(tmp_ptr, self_event, req->self_event_size);
            tmp_ptr += req->self_event_size;
        }
    }
    // printf("\n dragonfly remote event %d local event %d last packet %d %lf ", msg->remote_event_size_bytes,
    // msg->local_event_size_bytes, is_last_pckt, xfer_to_nic_time);
    tw_event_send(e_new);
    return xfer_to_nic_time;
}

static void packet_generate_rc(terminal_state *s, tw_bf *bf, terminal_plus_message *msg, tw_lp *lp)
{
    s->total_gen_size -= msg->packet_size;
    s->packet_gen--;
    packet_gen--;
    s->packet_counter--;

    if(bf->c2)
        num_local_packets_sr--;
    if(bf->c3)
        num_local_packets_sg--;
    if(bf->c4)
        num_remote_packets--;
   
    for(int i = 0; i < msg->num_rngs; i++)
        tw_rand_reverse_unif(lp->rng);

    for(int i = 0; i < msg->num_cll; i++)
       codes_local_latency_reverse(lp);

    int num_qos_levels = s->params->num_qos_levels;
    if(bf->c1)
        s->is_monitoring_bw = 0;

    int num_chunks = msg->packet_size / s->params->chunk_size;
    if (msg->packet_size < s->params->chunk_size)
        num_chunks++;

   
    int vcg = 0;
    if(num_qos_levels > 1)
    {
       vcg = get_vcg_from_category(msg); 
       assert(vcg == Q_HIGH || vcg == Q_MEDIUM);
    }
    assert(vcg < num_qos_levels);
    
    int i;
    for (i = 0; i < num_chunks; i++) {
        delete_terminal_plus_message_list(return_tail(s->terminal_msgs, s->terminal_msgs_tail, vcg));
        s->terminal_length[vcg] -= s->params->chunk_size;
    }
    if (bf->c5) {
        s->in_send_loop = 0;
    }
    if (bf->c11) {
        s->issueIdle = 0;
        s->stalled_chunks--;
        if(bf->c8) {
            s->last_buf_full = msg->saved_busy_time;
        }
    }
    struct mn_stats *stat;
    stat = model_net_find_stats(msg->category, s->dragonfly_stats_array);
    stat->send_count--;
    stat->send_bytes -= msg->packet_size;
    stat->send_time -= (1 / s->params->cn_bandwidth) * msg->packet_size;
}

/* generates packet at the current dragonfly compute node */
static void packet_generate(terminal_state *s, tw_bf *bf, terminal_plus_message *msg, tw_lp *lp)
{
    msg->num_rngs = 0;
    msg->num_cll = 0;
    
    packet_gen++;
    s->packet_gen++;
    s->total_gen_size += msg->packet_size;
    
    int num_qos_levels = s->params->num_qos_levels;
    int vcg = 0; //by default there's only one VC for terminals, VC0. There can be more based on the number of QoS levels

    if (num_qos_levels > 1) {
        tw_lpid router_id;
        codes_mapping_get_lp_info(lp->gid, lp_group_name, &mapping_grp_id, NULL, &mapping_type_id, NULL, &mapping_rep_id, &mapping_offset);
        codes_mapping_get_lp_id(lp_group_name, LP_CONFIG_NM_ROUT, NULL, 0, s->router_id / num_routers_per_mgrp, s->router_id % num_routers_per_mgrp, &router_id);

        if (s->is_monitoring_bw == 0) {
            bf->c1 = 1;
            /* Issue an event on both terminal and router to monitor bandwidth */
            msg->num_cll++;
            tw_stime bw_ts = bw_reset_window + codes_local_latency(lp);
            terminal_plus_message * m;
            tw_event * e = model_net_method_event_new(lp->gid, bw_ts, lp, DRAGONFLY_PLUS, (void**)&m, NULL);
            m->type = T_BANDWIDTH; 
            m->magic = terminal_magic_num;
            s->is_monitoring_bw = 1;
            tw_event_send(e);
        }

        vcg = get_vcg_from_category(msg);
        assert(vcg == Q_HIGH || vcg == Q_MEDIUM);
    }
    assert(vcg < num_qos_levels);

    tw_stime ts, nic_ts;

    assert(lp->gid != msg->dest_terminal_id);
    const dragonfly_plus_param *p = s->params;

    int total_event_size;
    uint64_t num_chunks = msg->packet_size / p->chunk_size;
    double cn_delay = s->params->cn_delay;

    int dest_router_id = dragonfly_plus_get_assigned_router_id(msg->dfp_dest_terminal_id, s->params);
    int dest_grp_id = dest_router_id / s->params->num_routers;
    int src_grp_id = s->router_id / s->params->num_routers;
    
    if(src_grp_id == dest_grp_id)
    {
        if(dest_router_id == s->router_id)
        {
            bf->c2 = 1;
            num_local_packets_sr++;
        }
        else
        {
            bf->c3 = 1;
            num_local_packets_sg++;
        }
    }
    else
    {
        bf->c4 = 1;
        num_remote_packets++;
    }

    if (msg->packet_size < s->params->chunk_size)
        num_chunks++;

    if (msg->packet_size < s->params->chunk_size)
        cn_delay = bytes_to_ns(msg->packet_size % s->params->chunk_size, s->params->cn_bandwidth);

    msg->num_rngs++;
    nic_ts = g_tw_lookahead + (num_chunks * cn_delay) + tw_rand_unif(lp->rng);

    // msg->packet_ID = lp->gid + g_tw_nlp * s->packet_counter;
    msg->packet_ID = s->packet_counter;
    s->packet_counter++;
    msg->my_N_hop = 0;
    msg->my_l_hop = 0;
    msg->my_g_hop = 0;

    // if(msg->dest_terminal_id == TRACK)
    if (msg->packet_ID == LLU(TRACK_PKT) && lp->gid == T_ID)
        printf("\n Packet %llu generated at terminal %d dest %llu size %llu num chunks %llu ", msg->packet_ID,
               s->terminal_id, LLU(msg->dest_terminal_id), LLU(msg->packet_size), LLU(num_chunks));

    for (int i = 0; i < num_chunks; i++) {
        terminal_plus_message_list *cur_chunk =
            (terminal_plus_message_list *) calloc(1, sizeof(terminal_plus_message_list));
        msg->origin_router_id = s->router_id;
        msg->dfp_src_terminal_id = s->terminal_id;
        init_terminal_plus_message_list(cur_chunk, msg);

        if (msg->remote_event_size_bytes + msg->local_event_size_bytes > 0) {
            cur_chunk->event_data =
                (char *) calloc(1, msg->remote_event_size_bytes + msg->local_event_size_bytes);
        }

        void *m_data_src = model_net_method_get_edata(DRAGONFLY_PLUS, msg);
        if (msg->remote_event_size_bytes) {
            memcpy(cur_chunk->event_data, m_data_src, msg->remote_event_size_bytes);
        }
        if (msg->local_event_size_bytes) {
            m_data_src = (char *) m_data_src + msg->remote_event_size_bytes;
            memcpy((char *) cur_chunk->event_data + msg->remote_event_size_bytes, m_data_src,
                   msg->local_event_size_bytes);
        }

        cur_chunk->msg.output_chan = vcg; //By default is 0 but QoS can mean more than just a single VC for terminals
        cur_chunk->msg.chunk_id = i;
        cur_chunk->msg.origin_router_id = s->router_id;
        append_to_terminal_plus_message_list(s->terminal_msgs, s->terminal_msgs_tail, vcg, cur_chunk);
        s->terminal_length[vcg] += s->params->chunk_size;
    }

    if (s->terminal_length[vcg] < s->params->cn_vc_size) {
        model_net_method_idle_event(nic_ts, 0, lp);
    }
    else {
        bf->c11 = 1;
        s->issueIdle = 1;
        s->stalled_chunks++;

        //this block was missing from when QOS was added - readded 10-31-19
        if(s->last_buf_full == 0.0)
        {
            bf->c8 = 1;
            msg->saved_busy_time = s->last_buf_full;
            /* TODO: Assumes a single vc from terminal to router */
            s->last_buf_full = tw_now(lp);
        }
    }

    if (s->in_send_loop == 0) {
        bf->c5 = 1;
        msg->num_cll++;
        ts = codes_local_latency(lp);
        terminal_plus_message *m;
        tw_event *e = model_net_method_event_new(lp->gid, ts, lp, DRAGONFLY_PLUS, (void **) &m, NULL);
        m->type = T_SEND;
        m->magic = terminal_magic_num;
        s->in_send_loop = 1;
        tw_event_send(e);
    }

    total_event_size =
        model_net_get_msg_sz(DRAGONFLY_PLUS) + msg->remote_event_size_bytes + msg->local_event_size_bytes;
    mn_stats *stat;
    stat = model_net_find_stats(msg->category, s->dragonfly_stats_array);
    stat->send_count++;
    stat->send_bytes += msg->packet_size;
    stat->send_time += (1 / p->cn_bandwidth) * msg->packet_size;
    if (stat->max_event_size < total_event_size)
        stat->max_event_size = total_event_size;

    return;
}

static void packet_send_rc(terminal_state * s, tw_bf * bf, terminal_plus_message * msg, tw_lp * lp)
{
    int num_qos_levels = s->params->num_qos_levels;

    if(msg->qos_reset1)
        s->qos_status[0] = Q_ACTIVE;
    if(msg->qos_reset2)
        s->qos_status[1] = Q_ACTIVE;
    
    if(msg->last_saved_qos)
        s->last_qos_lvl = msg->last_saved_qos;

    if(bf->c1) {
        s->in_send_loop = 1;
        if(bf->c3)
            s->last_buf_full = msg->saved_busy_time;
        return;
    }
     
    for(int i = 0; i < msg->num_cll; i++) 
    {
        codes_local_latency_reverse(lp);
    }
  
    for(int i = 0; i < msg->num_rngs; i++)
    {
        tw_rand_reverse_unif(lp->rng);
    }
    int vcg = msg->saved_vc;
    s->terminal_available_time = msg->saved_available_time;
    
    s->terminal_length[vcg] += s->params->chunk_size;
    /*TODO: MM change this to the vcg */
    s->vc_occupancy[vcg] -= s->params->chunk_size;

    terminal_plus_message_list* cur_entry = (terminal_plus_message_list *)rc_stack_pop(s->st);
    
    int data_size = s->params->chunk_size;
    if(cur_entry->msg.packet_size < s->params->chunk_size)
        data_size = cur_entry->msg.packet_size % s->params->chunk_size;

    s->qos_data[vcg] -= data_size;

    prepend_to_terminal_plus_message_list(s->terminal_msgs, 
            s->terminal_msgs_tail, vcg, cur_entry);
    if(bf->c4) {
        s->in_send_loop = 1;
    }
    if (bf->c5) {
        s->issueIdle = 1;
        if (bf->c6) {
            s->busy_time = msg->saved_total_time;
            s->last_buf_full = msg->saved_busy_time;
            s->busy_time_sample = msg->saved_sample_time;
            s->ross_sample.busy_time_sample = msg->saved_sample_time;
            s->busy_time_ross_sample = msg->saved_busy_time_ross;
        }
    }
    return;
}
/* sends the packet from the current dragonfly compute node to the attached router */
static void packet_send(terminal_state *s, tw_bf *bf, terminal_plus_message *msg, tw_lp *lp)
{
    tw_stime ts;
    tw_event *e;
    terminal_plus_message *m;
    tw_lpid router_id;
  
    int vcg = 0;
    int num_qos_levels = s->params->num_qos_levels;
 
    msg->last_saved_qos = -1;
    msg->qos_reset1 = -1;
    msg->qos_reset2 = -1;
    msg->num_rngs = 0;
    msg->num_cll = 0;
  
    vcg = get_next_vcg(s, bf, msg, lp);
  
    /* For a terminal to router connection, there would be as many VCGs as number
    * of VCs*/

    if(vcg == -1) {
        bf->c1 = 1;
        s->in_send_loop = 0;
        if(!s->last_buf_full) {
            bf->c3 = 1;
            msg->saved_busy_time = s->last_buf_full;
            s->last_buf_full = tw_now(lp); 
        }
        return;
    }
    
    msg->saved_vc = vcg;
    terminal_plus_message_list* cur_entry = s->terminal_msgs[vcg];
    int data_size = s->params->chunk_size;
    uint64_t num_chunks = cur_entry->msg.packet_size / s->params->chunk_size;
    if (cur_entry->msg.packet_size < s->params->chunk_size)
        num_chunks++;

    tw_stime delay = s->params->cn_delay;
    if ((cur_entry->msg.packet_size < s->params->chunk_size) && (cur_entry->msg.chunk_id == num_chunks - 1)) {
        data_size = cur_entry->msg.packet_size % s->params->chunk_size;
        delay = bytes_to_ns(cur_entry->msg.packet_size % s->params->chunk_size, s->params->cn_bandwidth);
    }

    s->qos_data[vcg] += data_size;

    msg->saved_available_time = s->terminal_available_time;

    msg->num_rngs++;
    ts = g_tw_lookahead + delay + tw_rand_unif(lp->rng);

    s->terminal_available_time = maxd(s->terminal_available_time, tw_now(lp));
    s->terminal_available_time += ts;

    ts = s->terminal_available_time - tw_now(lp);
    // TODO: be annotation-aware
    codes_mapping_get_lp_info(lp->gid, lp_group_name, &mapping_grp_id, NULL, &mapping_type_id, NULL,
                              &mapping_rep_id, &mapping_offset);
    codes_mapping_get_lp_id(lp_group_name, LP_CONFIG_NM_ROUT, NULL, 1, s->router_id / num_routers_per_mgrp,
                            s->router_id % num_routers_per_mgrp, &router_id);
    //  printf("\n Local router id %d global router id %d ", s->router_id, router_id);
    // we are sending an event to the router, so no method_event here
    void *remote_event;
    e = model_net_method_event_new(router_id, ts, lp, DRAGONFLY_PLUS_ROUTER, (void **) &m, &remote_event);
    memcpy(m, &cur_entry->msg, sizeof(terminal_plus_message));
    if (m->remote_event_size_bytes) {
        memcpy(remote_event, cur_entry->event_data, m->remote_event_size_bytes);
    }

    m->type = R_ARRIVE;
    m->src_terminal_id = lp->gid;
    m->vc_index = vcg;
    m->last_hop = TERMINAL;
    m->magic = router_magic_num;
    m->path_type = -1;
    m->local_event_size_bytes = 0;
    m->intm_rtr_id = -1;
    m->intm_group_id = -1;
    m->dfp_upward_channel_flag = 0;
    tw_event_send(e);


    if (cur_entry->msg.chunk_id == num_chunks - 1 && (cur_entry->msg.local_event_size_bytes > 0)) {
        msg->num_cll++;
        tw_stime local_ts = codes_local_latency(lp);
        tw_event *e_new = tw_event_new(cur_entry->msg.sender_lp, local_ts, lp);
        void *m_new = tw_event_data(e_new);
        void *local_event = (char *) cur_entry->event_data + cur_entry->msg.remote_event_size_bytes;
        memcpy(m_new, local_event, cur_entry->msg.local_event_size_bytes);
        tw_event_send(e_new);
    }
  
    // s->packet_counter++;
    s->vc_occupancy[vcg] += s->params->chunk_size;
    cur_entry = return_head(s->terminal_msgs, s->terminal_msgs_tail, vcg);
    rc_stack_push(lp, cur_entry, delete_terminal_plus_message_list, s->st);
    s->terminal_length[vcg] -= s->params->chunk_size;
    
    int next_vcg = 0; 
  
    if(num_qos_levels > 1)
      next_vcg = get_next_vcg(s, bf, msg, lp);

    cur_entry = NULL;
    if(next_vcg >= 0)
        cur_entry = s->terminal_msgs[next_vcg];

    /* if there is another packet inline then schedule another send event */
    if (cur_entry != NULL && s->vc_occupancy[next_vcg] + s->params->chunk_size <= s->params->cn_vc_size) {
        terminal_plus_message *m_new;
        msg->num_rngs++;
        ts += tw_rand_unif(lp->rng);
        e = model_net_method_event_new(lp->gid, ts, lp, DRAGONFLY_PLUS, (void **) &m_new, NULL);
        m_new->type = T_SEND;
        m_new->magic = terminal_magic_num;
        tw_event_send(e);
    }
    else {
        /* If not then the LP will wait for another credit or packet generation */
        bf->c4 = 1;
        s->in_send_loop = 0;
    }
    if (s->issueIdle) {
        bf->c5 = 1;
        s->issueIdle = 0;
        msg->num_rngs++;
        ts += tw_rand_unif(lp->rng);
        model_net_method_idle_event(ts, 0, lp);

        if (s->last_buf_full > 0.0) {
            bf->c6 = 1;
            msg->saved_total_time = s->busy_time;
            msg->saved_busy_time = s->last_buf_full;
            msg->saved_sample_time = s->busy_time_sample;

            s->busy_time += (tw_now(lp) - s->last_buf_full);
            s->busy_time_sample += (tw_now(lp) - s->last_buf_full);
            s->ross_sample.busy_time_sample += (tw_now(lp) - s->last_buf_full);
            msg->saved_busy_time_ross = s->busy_time_ross_sample;
            s->busy_time_ross_sample += (tw_now(lp) - s->last_buf_full);
            s->last_buf_full = 0.0;
        }
    }
    return;
}

static void send_remote_event(terminal_state *s, terminal_plus_message *msg, tw_lp *lp, tw_bf *bf,
                              char *event_data, int remote_event_size)
{
    void *tmp_ptr = model_net_method_get_edata(DRAGONFLY_PLUS, msg);
    // tw_stime ts = g_tw_lookahead + bytes_to_ns(msg->remote_event_size_bytes, (1/s->params->cn_bandwidth));
    msg->num_rngs++;
    tw_stime ts = g_tw_lookahead + mpi_soft_overhead + tw_rand_unif(lp->rng);
    if (msg->is_pull) {
        bf->c4 = 1;
        struct codes_mctx mc_dst = codes_mctx_set_global_direct(msg->sender_mn_lp);
        struct codes_mctx mc_src = codes_mctx_set_global_direct(lp->gid);
        int net_id = model_net_get_id(LP_METHOD_NM_TERM);

        model_net_set_msg_param(MN_MSG_PARAM_START_TIME, MN_MSG_PARAM_START_TIME_VAL, &(msg->msg_start_time));

        msg->event_rc = model_net_event_mctx(net_id, &mc_src, &mc_dst, msg->category, msg->sender_lp,
                                             msg->pull_size, ts, remote_event_size, tmp_ptr, 0, NULL, lp);
    }
    else {
        tw_event *e = tw_event_new(msg->final_dest_gid, ts, lp);
        void *m_remote = tw_event_data(e);
        memcpy(m_remote, event_data, remote_event_size);
        tw_event_send(e);
    }
    return;
}

static void packet_arrive_rc(terminal_state *s, tw_bf *bf, terminal_plus_message *msg, tw_lp *lp)
{
    for(int i = 0; i < msg->num_rngs; i++)
      tw_rand_reverse_unif(lp->rng);

    for(int i = 0; i < msg->num_cll; i++)
      codes_local_latency_reverse(lp);
      
    if (bf->c31) {
        s->packet_fin--;
        packet_fin--;
    }
    if (msg->path_type == MINIMAL)
        minimal_count--;
    else
        nonmin_count--;
    // if (msg->path_type == NON_MINIMAL)
    //     nonmin_count--;

    N_finished_chunks--;
    s->finished_chunks--;
    s->fin_chunks_sample--;
    s->ross_sample.fin_chunks_sample--;
    s->fin_chunks_ross_sample--;

    total_hops -= msg->my_N_hop;
    s->total_hops -= msg->my_N_hop;
    s->fin_hops_sample -= msg->my_N_hop;
    s->ross_sample.fin_hops_sample -= msg->my_N_hop;
    s->fin_hops_ross_sample -= msg->my_N_hop;
    s->fin_chunks_time = msg->saved_sample_time;
    s->ross_sample.fin_chunks_time = msg->saved_sample_time;
    s->fin_chunks_time_ross_sample = msg->saved_fin_chunks_ross;
    s->total_time = msg->saved_avg_time;

    struct qhash_head *hash_link = NULL;
    struct dfly_qhash_entry *tmp = NULL;

    struct dfly_hash_key key;
    key.message_id = msg->message_id;
    key.sender_id = msg->sender_lp;

    hash_link = qhash_search(s->rank_tbl, &key);
    tmp = qhash_entry(hash_link, struct dfly_qhash_entry, hash_link);

    mn_stats *stat;
    stat = model_net_find_stats(msg->category, s->dragonfly_stats_array);
    stat->recv_time = msg->saved_rcv_time;

    if (bf->c1) {
        stat->recv_count--;
        stat->recv_bytes -= msg->packet_size;
        N_finished_packets--;
        s->finished_packets--;
    }

    if (bf->c22) {
        s->max_latency = msg->saved_available_time;
    }
    if (bf->c7) {
        // assert(!hash_link);
        N_finished_msgs--;
        s->finished_msgs--;
        total_msg_sz -= msg->total_size;
        s->total_msg_size -= msg->total_size;
        s->data_size_sample -= msg->total_size;
        s->ross_sample.data_size_sample -= msg->total_size;
        s->data_size_ross_sample -= msg->total_size;

        struct dfly_qhash_entry *d_entry_pop = (dfly_qhash_entry *) rc_stack_pop(s->st);
        qhash_add(s->rank_tbl, &key, &(d_entry_pop->hash_link));
        s->rank_tbl_pop++;

        if (s->rank_tbl_pop >= DFLY_HASH_TABLE_SIZE)
            tw_error(TW_LOC, "\n Exceeded allocated qhash size, increase hash size in dragonfly model");

        hash_link = &(d_entry_pop->hash_link);
        tmp = d_entry_pop;

        if (bf->c4)
            model_net_event_rc2(lp, &msg->event_rc);
    }

    assert(tmp);
    tmp->num_chunks--;

    if (bf->c5) {
        qhash_del(hash_link);
        free_tmp(tmp);
        s->rank_tbl_pop--;
    }
    return;
}

/* packet arrives at the destination terminal */
static void packet_arrive(terminal_state *s, tw_bf *bf, terminal_plus_message *msg, tw_lp *lp)
{
    // NIC aggregation - should this be a separate function?
    // Trigger an event on receiving server

    if (msg->my_N_hop > s->params->max_hops_notify)
    {
        printf("Terminal received a packet with %d hops! (Notify on > than %d)\n",msg->my_N_hop, s->params->max_hops_notify);
    }
    
    msg->num_rngs = 0;
    msg->num_cll = 0;

    if (!s->rank_tbl)
        s->rank_tbl = qhash_init(dragonfly_rank_hash_compare, dragonfly_hash_func, DFLY_HASH_TABLE_SIZE);

    struct dfly_hash_key key;
    key.message_id = msg->message_id;
    key.sender_id = msg->sender_lp;

    struct qhash_head *hash_link = NULL;
    struct dfly_qhash_entry *tmp = NULL;

    hash_link = qhash_search(s->rank_tbl, &key);

    if (hash_link)
        tmp = qhash_entry(hash_link, struct dfly_qhash_entry, hash_link);

    uint64_t total_chunks = msg->total_size / s->params->chunk_size;

    if (msg->total_size % s->params->chunk_size)
        total_chunks++;

    if (!total_chunks)
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

    if (msg->packet_ID == LLU(TRACK_PKT) && msg->src_terminal_id == T_ID)
        printf("\n Packet %llu arrived at lp %llu hops %d ", msg->packet_ID, LLU(lp->gid), msg->my_N_hop);

    msg->num_rngs++;
    tw_stime ts = g_tw_lookahead + s->params->cn_credit_delay + tw_rand_unif(lp->rng);

    // no method_event here - message going to router
    tw_event *buf_e;
    terminal_plus_message *buf_msg;
    buf_e =
        model_net_method_event_new(msg->intm_lp_id, ts, lp, DRAGONFLY_PLUS_ROUTER, (void **) &buf_msg, NULL);
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
    s->ross_sample.fin_chunks_sample++;
    s->fin_chunks_ross_sample++;

    /* WE do not allow self messages through dragonfly */
    assert(lp->gid != msg->src_terminal_id);

    // Verify that the router that send the packet to this terminal is the router assigned to this terminal
    int dest_router_id = dragonfly_plus_get_assigned_router_id(s->terminal_id, s->params);
    int received_from_rel_id = codes_mapping_get_lp_relative_id(msg->intm_lp_id,0,0);
    assert(dest_router_id == received_from_rel_id);

    uint64_t num_chunks = msg->packet_size / s->params->chunk_size;
    if (msg->packet_size < s->params->chunk_size)
        num_chunks++;

    if (msg->path_type == MINIMAL)
        minimal_count++;
    else
        nonmin_count++;

    if (msg->chunk_id == num_chunks - 1) {
        bf->c31 = 1;
        s->packet_fin++;
        packet_fin++;
    }
    // if (msg->path_type != MINIMAL)
    //     printf("\n Wrong message path type %d ", msg->path_type);

    /* save the sample time */
    msg->saved_sample_time = s->fin_chunks_time;
    s->fin_chunks_time += (tw_now(lp) - msg->travel_start_time);
    s->ross_sample.fin_chunks_time += (tw_now(lp) - msg->travel_start_time);
    msg->saved_fin_chunks_ross = s->fin_chunks_time_ross_sample;
    s->fin_chunks_time_ross_sample += (tw_now(lp) - msg->travel_start_time);

    /* save the total time per LP */
    msg->saved_avg_time = s->total_time;
    s->total_time += (tw_now(lp) - msg->travel_start_time);
    total_hops += msg->my_N_hop;
    s->total_hops += msg->my_N_hop;
    s->fin_hops_sample += msg->my_N_hop;
    s->ross_sample.fin_hops_sample += msg->my_N_hop;
    s->fin_hops_ross_sample += msg->my_N_hop;

    mn_stats *stat = model_net_find_stats(msg->category, s->dragonfly_stats_array);
    msg->saved_rcv_time = stat->recv_time;
    stat->recv_time += (tw_now(lp) - msg->travel_start_time);

#if DEBUG == 1
    if (msg->packet_ID == TRACK && msg->chunk_id == num_chunks - 1 && msg->message_id == TRACK_MSG) {
        printf("(%lf) [Terminal %d] packet %lld has arrived  \n", tw_now(lp), (int) lp->gid, msg->packet_ID);

        printf("travel start time is %f\n", msg->travel_start_time);

        printf("My hop now is %d\n", msg->my_N_hop);
    }
#endif

    /* Now retreieve the number of chunks completed from the hash and update
     * them */
    void *m_data_src = model_net_method_get_edata(DRAGONFLY_PLUS, msg);

    /* If an entry does not exist then create one */
    if (!tmp) {
        bf->c5 = 1;
        struct dfly_qhash_entry *d_entry = (dfly_qhash_entry *) calloc(1, sizeof(struct dfly_qhash_entry));
        d_entry->num_chunks = 0;
        d_entry->key = key;
        d_entry->remote_event_data = NULL;
        d_entry->remote_event_size = 0;
        qhash_add(s->rank_tbl, &key, &(d_entry->hash_link));
        s->rank_tbl_pop++;

        if (s->rank_tbl_pop >= DFLY_HASH_TABLE_SIZE)
            tw_error(TW_LOC, "\n Exceeded allocated qhash size, increase hash size in dragonfly model");

        hash_link = &(d_entry->hash_link);
        tmp = d_entry;
    }

    assert(tmp);
    tmp->num_chunks++;

    if (msg->chunk_id == num_chunks - 1) {
        bf->c1 = 1;
        stat->recv_count++;
        stat->recv_bytes += msg->packet_size;

        N_finished_packets++;
        s->finished_packets++;
    }
    /* if its the last chunk of the packet then handle the remote event data */
    if (msg->remote_event_size_bytes > 0 && !tmp->remote_event_data) {
        /* Retreive the remote event entry */
        tmp->remote_event_data = (char *) calloc(1, msg->remote_event_size_bytes);
        assert(tmp->remote_event_data);
        tmp->remote_event_size = msg->remote_event_size_bytes;
        memcpy(tmp->remote_event_data, m_data_src, msg->remote_event_size_bytes);
    }
    if (s->min_latency > tw_now(lp) - msg->travel_start_time) {
        s->min_latency = tw_now(lp) - msg->travel_start_time;
    }

    if (s->max_latency < tw_now(lp) - msg->travel_start_time) {
        bf->c22 = 1;
        msg->saved_available_time = s->max_latency;
        s->max_latency = tw_now(lp) - msg->travel_start_time;
    }
    /* If all chunks of a message have arrived then send a remote event to the
     * callee*/
    // assert(tmp->num_chunks <= total_chunks);

    if (tmp->num_chunks >= total_chunks) {
        bf->c7 = 1;

        s->data_size_sample += msg->total_size;
        s->ross_sample.data_size_sample += msg->total_size;
        s->data_size_ross_sample += msg->total_size;
        N_finished_msgs++;
        total_msg_sz += msg->total_size;
        s->total_msg_size += msg->total_size;
        s->finished_msgs++;

        // assert(tmp->remote_event_data && tmp->remote_event_size > 0);
        if (tmp->remote_event_data && tmp->remote_event_size > 0) {
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

static void terminal_buf_update_rc(terminal_state *s, tw_bf *bf, terminal_plus_message *msg, tw_lp *lp)
{
    int vcg = 0;
    int num_qos_levels = s->params->num_qos_levels;

    for(int i = 0; i < msg->num_cll; i++)
       codes_local_latency_reverse(lp);

    if(num_qos_levels > 1)
      vcg = get_vcg_from_category(msg);
      
    s->vc_occupancy[vcg] += s->params->chunk_size;
    if (bf->c1) {
        s->in_send_loop = 0;
    }
    return;
}
/* update the compute node-router channel buffer */
static void terminal_buf_update(terminal_state *s, tw_bf *bf, terminal_plus_message *msg, tw_lp *lp)
{
    msg->num_cll = 0;
    msg->num_rngs = 0;
    
    bf->c1 = 0;
    bf->c2 = 0;
    bf->c3 = 0;
    int vcg = 0;

    if(s->params->num_qos_levels > 1)
      vcg = get_vcg_from_category(msg);

    msg->num_cll++;
    tw_stime ts = codes_local_latency(lp);
    s->vc_occupancy[vcg] -= s->params->chunk_size;

    if (s->in_send_loop == 0 && s->terminal_msgs[vcg] != NULL) {
        terminal_plus_message *m;
        bf->c1 = 1;
        tw_event *e = model_net_method_event_new(lp->gid, ts, lp, DRAGONFLY_PLUS, (void **) &m, NULL);
        m->type = T_SEND;
        m->magic = terminal_magic_num;
        s->in_send_loop = 1;
        tw_event_send(e);
    }
    return;
}

void dragonfly_plus_terminal_final(terminal_state *s, tw_lp *lp)
{
    dragonfly_total_time += s->total_time; //increment the PE level time counter

    if (s->max_latency > dragonfly_max_latency)
        dragonfly_max_latency = s->max_latency; //get maximum latency across all LPs on this PE

    model_net_print_stats(lp->gid, s->dragonfly_stats_array);

    int written = 0;
    if (s->terminal_id == 0) {
        written += sprintf(s->output_buf + written, "# Format <source_id> <source_type> <dest_id> < dest_type>  <link_type> <link_traffic> <link_saturation> <stalled_chunks>\n");
    }
    written += sprintf(s->output_buf + written, "%u %s %u %s %s %llu %lf %lu\n",
        s->terminal_id, "T", s->router_id, "R", "CN", LLU(s->total_msg_size), s->busy_time, s->stalled_chunks);

    lp_io_write(lp->gid, (char*)"dragonfly-plus-local-link-stats", written, s->output_buf);

    // if (s->terminal_id == 0) {
    //     char meta_filename[64];
    //     sprintf(meta_filename, "dragonfly-plus-cn-stats.meta");

    //     FILE * fp = fopen(meta_filename, "w+");
    //     fprintf(fp, "# Format <LP id> <Terminal ID> <Total Data Size> <Avg packet latency> <# Flits/Packets finished> <Busy Time> <Max packet Latency> <Min packet Latency >\n");
    // }

    // written = 0;
    // if (s->terminal_id == 0)
    //     written += sprintf(s->output_buf2 + written, "# Format <LP id> <Terminal ID> <recvd_msgs> <recvd_chunks> <recvd_packet> <Total Msg. Latency> <Avg Msg Latency> <busy time> <Avg hops/chunks>\n" );

    // written += sprintf(s->output_buf2 + written, "%llu %llu %ld %ld %ld %lf %lf %lf %lf\n", 
    //         LLU(lp->gid), LLU(s->terminal_id), s->finished_msgs, s->finished_chunks, s->finished_packets, s->total_time, s->total_time/s->finished_msgs, 
    //         s->busy_time, (double)s->total_hops/s->finished_chunks);

    written = 0;
    if(s->terminal_id == 0)
    {
        written += sprintf(s->output_buf2 + written, "# Format <LP id> <Terminal ID> <Total Data Sent> <Total Data Received> <Avg packet latency> <Max packet Latency> <Min packet Latency> <# Packets finished> <Avg Hops> <Busy Time>\n");
    }
    written += sprintf(s->output_buf2 + written, "%llu %u %d %llu %lf %lf %lf %ld %lf %lf\n", 
            LLU(lp->gid), s->terminal_id, s->total_gen_size, LLU(s->total_msg_size), s->total_time/s->finished_chunks, s->max_latency, s->min_latency,
            s->finished_packets, (double)s->total_hops/s->finished_chunks, s->busy_time);

   
    // written = 0;
    // written += sprintf(s->output_buf2 + written, "%llu %u %lf %lf %lf %lf %ld %lf\n", 
    //         LLU(lp->gid), s->terminal_id, s->total_time/s->finished_chunks, 
    //         s->busy_time, s->max_latency, s->min_latency,
    //         s->finished_packets, (double)s->total_hops/s->finished_chunks);


    lp_io_write(lp->gid, (char*)"dragonfly-plus-cn-stats", written, s->output_buf2); 

    // if (s->terminal_id == 0) {
    //     char meta_filename[64];
    //     sprintf(meta_filename, "dragonfly-msg-stats.meta");

    //     FILE *fp = fopen(meta_filename, "w+");
    //     fprintf(fp,
    //             "# Format <LP id> <Terminal ID> <Total Data Size> <Avg packet latency> <# Flits/Packets "
    //             "finished> <Avg hops> <Busy Time> <Max packet Latency> <Min packet Latency >\n");
    // }
    // int written = 0;

    // written += sprintf(s->output_buf + written, "%llu %u %llu %lf %ld %lf %lf %lf %lf\n", LLU(lp->gid),
    //                    s->terminal_id, LLU(s->total_msg_size), s->total_time / s->finished_chunks,
    //                    s->finished_packets, (double) s->total_hops / s->finished_chunks, s->busy_time,
    //                    s->max_latency, s->min_latency);

    // lp_io_write(lp->gid, (char *) "dragonfly-msg-stats", written, s->output_buf);

    if (s->terminal_msgs[0] != NULL)
        printf("[%llu] leftover terminal messages \n", LLU(lp->gid));

    // if(s->packet_gen != s->packet_fin)
    //    printf("\n generated %d finished %d ", s->packet_gen, s->packet_fin);

    if (s->rank_tbl)
        qhash_finalize(s->rank_tbl);

    rc_stack_destroy(s->st);
    free(s->vc_occupancy);
    free(s->terminal_msgs);
    free(s->terminal_msgs_tail);
}

void dragonfly_plus_router_final(router_state *s, tw_lp *lp)
{
    // int max_gc_usage = 0;
    // int min_gc_usage = INT_MAX;

    // int running_sum = 0;
    // for(int i = 0; i < s->params->num_global_connections; i++)
    // {
    //     int gc_val = s->gc_usage[i];
    //     running_sum += gc_val;

    //     if (gc_val > max_gc_usage)
    //         max_gc_usage = gc_val;
    //     if (gc_val < min_gc_usage)
    //         min_gc_usage = gc_val;
    // }
    // double mean_gc_usage = (double) running_sum / (double) s->params->num_global_connections; 

    // if (s->dfp_router_type == SPINE) {
    //     printf("Router %d in group %d:   Min GC Usage= %d    Max GC Usage= %d     Mean GC Usage= %.2f", s->router_id, s->router_id / s->params->num_routers, min_gc_usage, max_gc_usage, mean_gc_usage);
    //     printf("\t[");
    //     for(int i = 0; i < s->params->num_global_connections; i++)
    //     {
    //         printf("%d ",s->gc_usage[i]);
    //     }
    //     printf("]\n");
    // }

#if DEBUG_QOS
    if(s->router_id == 0)
        fclose(dragonfly_rtr_bw_log);
#endif

    free(s->global_channel);
    int i, j;
    for (i = 0; i < s->params->radix; i++) {
        for (j = 0; j < s->params->num_vcs; j++) {
            if (s->queued_msgs[i][j] != NULL) {
                printf("[%llu] leftover queued messages %d %d %d\n", LLU(lp->gid), i, j,
                       s->vc_occupancy[i][j]);
            }
            if (s->pending_msgs[i][j] != NULL) {
                printf("[%llu] leftover pending messages %d %d\n", LLU(lp->gid), i, j);
            }
        }
    }

    rc_stack_destroy(s->st);

    const dragonfly_plus_param *p = s->params;
    int written = 0; //local link
    int written1 = 0; //global link
    int written2 = 0; //msg app id rec
    int src_rel_id = s->router_id % p->num_routers;
    int local_grp_id = s->router_id / p->num_routers;

    int total_packet_verify = 0;

    vector< Connection > my_local_links = s->connMan->get_connections_by_type(CONN_LOCAL);
    vector< Connection >::iterator it = my_local_links.begin();

    for(; it != my_local_links.end(); it++)
    {
        int dest_rtr_id = it->dest_gid;
        int port_no = it->port;
        assert(port_no >= 0 && port_no < p->radix);
        written += sprintf(s->output_buf + written, "\n%d %s %d %s %s %llu %lf %lu",
            s->router_id,
            "R",
            dest_rtr_id,
            "R",
            "G",
            LLU(s->link_traffic[port_no]),
            s->busy_time[port_no],
            s->stalled_chunks[port_no]);
    }

    sprintf(s->output_buf + written, "\n");
    lp_io_write(lp->gid, (char*)"dragonfly-plus-local-link-stats", written, s->output_buf);

    vector< Connection > my_global_links = s->connMan->get_connections_by_type(CONN_GLOBAL);
    it = my_global_links.begin();

    if (s->router_id == 0) {
        written1 += sprintf(s->output_buf2 + written1, "# Format <source_id> <source_type> <source group> || <dest_id> < dest_type> <destination group>, <link_type> <link_traffic> <link_saturation> <stalled_chunks>");
    }

    for(; it != my_global_links.end(); it++)
    {
        int dest_rtr_id = it->dest_gid;
        int dest_group_id = it->dest_group_id;
        int port_no = it->port;
        assert(port_no >= 0 && port_no < p->radix);
        assert(dragonfly_plus_get_router_type(dest_rtr_id, p) == SPINE);
        written1 += sprintf(s->output_buf + written1, "\n%d %s G%d || %d %s G%d, %s %llu %lf %lu",
            s->router_id,
            "R",
            s->group_id,
            dest_rtr_id,
            "R",
            dest_group_id,
            "G",
            LLU(s->link_traffic[port_no]),
            s->busy_time[port_no],
            s->stalled_chunks[port_no]);
    }

    sprintf(s->output_buf2 + written1, "\n");
    lp_io_write(lp->gid, (char*)"dragonfly-plus-global-link-stats", written1, s->output_buf2);

    // I/O for couting of msg app id
    // wirtten2 + buf4 rec
    // written3 + buf5 sent
    int result = 0;

    if (s->router_id == 0 && s->params->counting_bool>0) {
        //rec
        written2 = sprintf(s->output_buf4, "# Received <group_id> <router_id> <window_id> <#app_0> <#app_1>");
        result=lp_io_write(lp->gid, (char*)"dragonfly-plus-msg-app-id-rec", written2, s->output_buf4);
        if(result!=0)
            tw_error(TW_LOC, "\nERROR: msg app id i/o failed, lpio result %d, svr %d, lpgid %llu, writing dragonfly-plus-msg-app-id-rec failed, written %d, buf %s\n", result, s->router_id, LLU(lp->gid), written2, s->output_buf4);
    }

    if (s->dfp_router_type == SPINE && s->params->counting_bool >0) {
        for(int i =0; i < s->params->counting_windows; i++) {
            //rec
            if(s->msg_counting[i][0] != 0 || s->msg_counting[i][1] != 0) {
                written2 = sprintf(s->output_buf4, "\n%d %d %d %d %d", s->group_id, s->router_id, i, s->msg_counting[i][0], s->msg_counting[i][1]);

                result=lp_io_write(lp->gid, (char*)"dragonfly-plus-msg-app-id-rec", written2, s->output_buf4);
                
                if(result!=0)
                    tw_error(TW_LOC, "\nERROR: msg app id i/o failed 2, lpio result %d, svr %llu, lpgid %llu, writting dragonfly-plus-msg-app-id-rec failed, written %d, buf %s, index %d\n", result, LLU(s->router_id), LLU(lp->gid), written2, s->output_buf4, i);

                //printf("\nGroupID %d, RouterID %d, Window %d, #App0 %d #App1 %d", s->group_id, s->router_id, i, s->msg_counting[i][0], s->msg_counting[i][1]);
                total_packet_verify += (s->msg_counting[i][0] + s->msg_counting[i][1]);
            }
        }

        if (total_packet_verify != s->total_packets_received)
            tw_error(TW_LOC, "\nERROR: Router %d counting msg app id failed: calculated totoal packets %d, recorded total packets %d, Check with roll back computing\n", s->router_id, total_packet_verify, s->total_packets_received);
    }

    // /*MM: These statistics will need to be updated for dragonfly plus.
    //  * Especially the meta file information on router ports still have green
    //  * and black links. */
    // const dragonfly_plus_param *p = s->params;
    // int written = 0;
    // if (!s->router_id) {
    //     written =
    //         sprintf(s->output_buf, "# Format <Type> <LP ID> <Group ID> <Router ID> <Busy time per router port(s)>");
    //     written += sprintf(s->output_buf + written, "\n# Router ports in the order: %d Intra Links, %d Inter Links %d Terminal Links. Hyphens for Unconnected ports (No terminals on Spine routers)", p->intra_grp_radix, p->num_global_connections, p->num_cn);  
    // }

    // char router_type[10];
    // if (s->dfp_router_type == LEAF)
    //     strcpy(router_type,"LEAF");
    // else if(s->dfp_router_type == SPINE)
    //     strcpy(router_type,"SPINE");

    // written += sprintf(s->output_buf + written, "\n%s %llu %d %d", router_type, LLU(lp->gid), s->router_id / p->num_routers, s->router_id % p->num_routers);
    // for (int d = 0; d < p->radix; d++) {
    //     bool printed_hyphen = false;
    //     ConnectionType port_type = s->connMan->get_port_type(d);
        
    //     if (port_type == 0) {
    //         written += sprintf(s->output_buf + written, " -");
    //         printed_hyphen = true;
    //     }
    //     if (printed_hyphen == false)
    //         written += sprintf(s->output_buf + written, " %lf", s->busy_time[d]);
    // }

    // sprintf(s->output_buf + written, "\n");
    // lp_io_write(lp->gid, (char *) "dragonfly-plus-router-stats", written, s->output_buf);

    // written = 0;
    // if (!s->router_id) {
    //     written =
    //         sprintf(s->output_buf2, "# Format <LP ID> <Group ID> <Router ID> <Busy time per router port(s)>");
    //     written += sprintf(s->output_buf2 + written, "\n# Router ports in the order: %d Intra Links, %d Inter Links %d Terminal Links. Hyphens for Unconnected ports (No terminals on Spine routers)", p->intra_grp_radix, p->num_global_connections, p->num_cn);  
    // }
    // written += sprintf(s->output_buf2 + written, "\n%s %llu %d %d", router_type, LLU(lp->gid), s->router_id / p->num_routers, s->router_id % p->num_routers);

    // for (int d = 0; d < p->radix; d++) {
    //     bool printed_hyphen = false;
    //     ConnectionType port_type = s->connMan->get_port_type(d);

    //     if (port_type == 0) {
    //         written += sprintf(s->output_buf2 + written, " -");
    //         printed_hyphen = true;
    //     }
    //     if (printed_hyphen == false)
    //         written += sprintf(s->output_buf2 + written, " %lld", LLD(s->link_traffic[d]));
    // }

    // lp_io_write(lp->gid, (char *) "dragonfly-plus-router-traffic", written, s->output_buf2);

    // if (!g_tw_mynode) {
    //     if (s->router_id == 0) {
    //         if (PRINT_CONFIG) 
    //             dragonfly_plus_print_params(s->params);
    //     }
    // }
}

static Connection do_dfp_routing(router_state *s,
                                tw_bf *bf,
                                terminal_plus_message *msg,
                                tw_lp *lp,
                                int fdest_router_id)
{

    int my_router_id = s->router_id;
    int my_group_id = s->router_id / s->params->num_routers;
    int fdest_group_id = fdest_router_id / s->params->num_routers;
    int origin_group_id = msg->origin_router_id / s->params->num_routers;
    bool in_intermediate_group = (my_group_id != origin_group_id) && (my_group_id != fdest_group_id);

    Connection nextStopConn; //the connection that we will forward the packet to

    //----------- DESTINATION LOCAL GROUP ROUTING --------------
    if (my_router_id == fdest_router_id) {
        vector< Connection > poss_next_stops = s->connMan->get_connections_to_gid(msg->dfp_dest_terminal_id, CONN_TERMINAL);
        if (poss_next_stops.size() < 1)
            tw_error(TW_LOC, "Destination Router: No connection to destination terminal\n");
        Connection best_min_conn = get_absolute_best_connection_from_conns(s, bf, msg, lp, poss_next_stops);
        return best_min_conn;
    }
    else if (my_group_id == fdest_group_id) { //then we just route minimally
        vector< Connection > poss_next_stops = get_legal_minimal_stops(s, bf, msg, lp, fdest_router_id);
        if (poss_next_stops.size() < 1)
            tw_error(TW_LOC, "DEAD END WHEN ROUTING LOCALLY - My Router ID: %d    FDest Router ID: %d\n", my_router_id, fdest_router_id);
        
        Connection best_min_conn = get_absolute_best_connection_from_conns(s, bf, msg, lp, poss_next_stops);
        return best_min_conn;
    }
    //------------ END DESTINATION LOCAL GROUP ROUTING ---------
    // from here we can assume that we are not in the destination group


    if (isRoutingAdaptive(routing)) {
        if (routing == PROG_ADAPTIVE)
            nextStopConn = do_dfp_prog_adaptive_routing(s, bf, msg, lp, fdest_router_id);
        else if (routing == FULLY_PROG_ADAPTIVE){
            nextStopConn = do_dfp_FPAR(s, bf, msg, lp, fdest_router_id);
        }
        return nextStopConn;
    }

    else if (isRoutingMinimal(routing)) {
        vector< Connection > poss_next_stops = get_legal_minimal_stops(s, bf, msg, lp, fdest_router_id);
        if (poss_next_stops.size() < 1)
            tw_error(TW_LOC, "MINIMAL DEAD END\n");

        // int rand_sel = tw_rand_integer(lp->rng, 0, poss_next_stops.size() -1 );
        // return poss_next_stops[rand_sel];

        ConnectionType conn_type = poss_next_stops[0].conn_type;
        Connection best_min_conn;
        if (conn_type == CONN_GLOBAL) {
            msg->num_rngs++;
            int rand_sel = tw_rand_integer(lp->rng, 0, poss_next_stops.size() -1);
            return poss_next_stops[rand_sel];
        }
        else
            best_min_conn = get_absolute_best_connection_from_conns(s, bf, msg, lp, poss_next_stops); //gets absolute best
        return best_min_conn;
    }

    else { //routing algorithm is specified in routing
        bool route_to_fdest = false;
        if (routing == NON_MINIMAL_LEAF) {
            if (msg->dfp_upward_channel_flag == 1) //we only route to minimal if this flag is true, otherwise we keep routing nonminimally
                route_to_fdest = true;

            else if (s->dfp_router_type == LEAF) {
                if (in_intermediate_group) { //then we are the intermediate router
                    msg->dfp_upward_channel_flag = 1;
                    route_to_fdest = true;
                }
            }
        }
        else if (routing == NON_MINIMAL_SPINE) {
            tw_error(TW_LOC, "nonminimal spine routing not yet supported");
        }

        vector< Connection > poss_min_next_stops = get_legal_minimal_stops(s, bf, msg, lp, fdest_router_id);
        vector< Connection > poss_intm_next_stops = get_legal_nonminimal_stops(s, bf, msg, lp, poss_min_next_stops, fdest_router_id);


        vector< Connection > poss_next_stops;
        if (route_to_fdest) {
            poss_next_stops = poss_min_next_stops;
            if (poss_next_stops.size() < 1)
                tw_error(TW_LOC, "Dead end when finding stops to fdest router");            
        }
        else { //then we need to be going toward the intermediate router
            msg->path_type = NON_MINIMAL;
            poss_next_stops = poss_intm_next_stops;
            if (poss_next_stops.size() < 1)
                tw_error(TW_LOC, "Dead end when finding stops to intermediate router");
        }

        ConnectionType conn_type = poss_next_stops[0].conn_type; //should all be the same

        Connection best_conn;
        if (conn_type == CONN_GLOBAL)
            best_conn = get_best_connection_from_conns(s, bf, msg, lp, poss_next_stops); //pick 2 randomly and pick best from those
        else
            best_conn = get_absolute_best_connection_from_conns(s, bf, msg, lp, poss_next_stops); //pick absolute best from poss_next_stops

        if (best_conn.port == -1)
            tw_error(TW_LOC, "Get best connection returned a bad connection");
        
        return best_conn;
    }

    tw_error(TW_LOC, "do_dfp_routing(): No route chosen!\n");
}

static void router_verify_valid_receipt(router_state *s, tw_bf *bf, terminal_plus_message *msg, tw_lp *lp)
{
    if (msg->my_N_hop > s->params->max_hops_notify)
    {
        printf("Router received a packet with %d hops so far! (Notify on > than %d)\n",msg->my_N_hop, s->params->max_hops_notify);
    }

    tw_lpid last_sender_lpid = msg->intm_lp_id;
    int rel_id, src_term_rel_id;

    bool has_valid_connection = false;
    if (msg->last_hop == TERMINAL)
    {
        tw_lpid src_term_lpgid = msg->src_terminal_id;
        try {
            src_term_rel_id = codes_mapping_get_lp_relative_id(src_term_lpgid,0,0);
        }
        catch (...) {
            tw_error(TW_LOC, "\nRouter Receipt Verify: Codes Mapping Get LP Rel ID Failure - Terminal");
        }
        has_valid_connection = s->connMan->is_connected_to_by_type(src_term_rel_id, CONN_TERMINAL);
    }
    else if (msg->last_hop == LOCAL)
    {
        try {
            rel_id = codes_mapping_get_lp_relative_id(last_sender_lpid,0,0);
        }
        catch (...) {
            tw_error(TW_LOC, "\nRouter Receipt Verify: Codes Mapping Get LP Rel ID Failure - Local");
        }

        int rel_loc_id = rel_id % s->params->num_routers;
        has_valid_connection = s->connMan->is_connected_to_by_type(rel_loc_id, CONN_LOCAL);
    }
    else if (msg->last_hop == GLOBAL)
    {
        try {
            rel_id = codes_mapping_get_lp_relative_id(last_sender_lpid,0,0);
        }
        catch (...) {
            tw_error(TW_LOC, "\nRouter Receipt Verify: Codes Mapping Get LP Rel ID Failure - Global");
        }
        has_valid_connection = s->connMan->is_connected_to_by_type(rel_id, CONN_GLOBAL);
    }
    else
    {
        tw_error(TW_LOC, "\nDFP Router Verify Valid Receipt: Last Hop invalidly defined");
    }

    if (!has_valid_connection){
        if (msg->last_hop == TERMINAL)
            printf("ERROR: Router ID %d has no connection to Terminal %d but received a message from it!",s->router_id, src_term_rel_id);
        else
            printf("ERROR: Router ID %d has no connection to Router %d but received a message from it!",s->router_id, rel_id);
    }
    assert(has_valid_connection);
}

/*MM: This will also be used as is. This is meant to sent a credit back to the
 * sending router. */
/*When a packet is sent from the current router and a buffer slot becomes available, a credit is sent back to
 * schedule another packet event*/
static void router_credit_send(router_state *s, terminal_plus_message *msg, tw_lp *lp, int sq, short* rng_counter)
{
    tw_event *buf_e;
    tw_stime ts;
    terminal_plus_message *buf_msg;

    int dest = 0, type = R_BUFFER;
    int is_terminal = 0;
    double credit_delay;

    const dragonfly_plus_param *p = s->params;

    // Notify sender terminal about available buffer space
    if (msg->last_hop == TERMINAL) {
        dest = msg->src_terminal_id;
        type = T_BUFFER;
        is_terminal = 1;
        credit_delay = p->cn_credit_delay;
    }
    else if (msg->last_hop == GLOBAL) {
        dest = msg->intm_lp_id;
        credit_delay = p->global_credit_delay;
    }    
    else if (msg->last_hop == LOCAL) {
        dest = msg->intm_lp_id;
        credit_delay = p->local_credit_delay;
    }
    else
        printf("\n Invalid message type");

    (*rng_counter)++;
    ts = g_tw_lookahead + credit_delay + tw_rand_unif(lp->rng);

    if (is_terminal) {
        buf_e = model_net_method_event_new(dest, ts, lp, DRAGONFLY_PLUS, (void **) &buf_msg, NULL);
        buf_msg->magic = terminal_magic_num;
    }
    else {
        buf_e = model_net_method_event_new(dest, ts, lp, DRAGONFLY_PLUS_ROUTER, (void **) &buf_msg, NULL);
        buf_msg->magic = router_magic_num;
    }

    buf_msg->origin_router_id = s->router_id;
    if (sq == -1) {
        buf_msg->vc_index = msg->vc_index;
        buf_msg->output_chan = msg->output_chan;
    }
    else {
        buf_msg->vc_index = msg->saved_vc;
        buf_msg->output_chan = msg->saved_channel;
    }

    strcpy(buf_msg->category, msg->category);
    buf_msg->type = type;

    tw_event_send(buf_e);
    return;
}

static void router_packet_receive_rc(router_state *s, tw_bf *bf, terminal_plus_message *msg, tw_lp *lp)
{
    router_rev_ecount++;
    router_ecount--;
    
    int output_port = msg->saved_vc;
    int output_chan = msg->saved_channel;
    int dest_gid = msg->dfp_dest_terminal_id / (s->params->total_terminals / s->params->num_groups);
    int inter_group_transmit = 0;
    
    //rc for msg app id counting
    if(dest_gid != s->group_id)
        inter_group_transmit = 1;

    if(inter_group_transmit>0 && s->params->counting_bool>0 && s->dfp_router_type == SPINE && msg->last_received_time >= s->params->counting_start && msg->last_received_time <= s->params->counting_end) {
        s->total_packets_received--;
        tw_stime time_now = msg->last_received_time - s->params->counting_start;

        int window = (int) floor(time_now/s->params->counting_interval);
        if (window >= s->params->counting_windows)
            tw_error(TW_LOC, "Router %d, will access window %d, greater than max %d\n", s->router_id, window, s->params->counting_windows);
        //printf("msg app id counting rc\n");
        s->msg_counting[window][msg->app_id]--;
    }
    for(int i = 0 ; i < msg->num_cll; i++)
        codes_local_latency_reverse(lp);

    for(int i = 0; i < msg->num_rngs; i++)
        tw_rand_reverse_unif(lp->rng);

    if(bf->c1)
      s->is_monitoring_bw = 0;

    //do dfp routing reverse
    // int dest_router_id = dragonfly_plus_get_assigned_router_id(msg->dfp_dest_terminal_id, s->params);
    // do_dfp_routing_rc(s, bf, msg, lp, dest_router_id);

    if (bf->c2) {
        terminal_plus_message_list *tail =
            return_tail(s->pending_msgs[output_port], s->pending_msgs_tail[output_port], output_chan);
        delete_terminal_plus_message_list(tail);
        s->vc_occupancy[output_port][output_chan] -= s->params->chunk_size;
        if (bf->c3) {
            s->in_send_loop[output_port] = 0;
        }
    }
    if (bf->c4) {
        s->stalled_chunks[output_port]--;
        if(bf->c22)
        {
            s->last_buf_full[output_port] = msg->saved_busy_time;
        }
        delete_terminal_plus_message_list(
            return_tail(s->queued_msgs[output_port], s->queued_msgs_tail[output_port], output_chan));
        s->queued_count[output_port] -= s->params->chunk_size;
    }
}

/* MM: This will need changes for the port selection and routing part. The
 * progressive adaptive routing would need modification as well. */
/* Packet arrives at the router and a credit is sent back to the sending terminal/router */
static void router_packet_receive(router_state *s, tw_bf *bf, terminal_plus_message *msg, tw_lp *lp)
{
    msg->num_cll = 0;
    msg->num_rngs = 0;
    
    router_verify_valid_receipt(s, bf, msg, lp);
    router_ecount++;

    tw_stime ts;
  
    int num_qos_levels = s->params->num_qos_levels;
    int vcs_per_qos = s->params->num_vcs / num_qos_levels;

    if(num_qos_levels > 1 && (s->is_monitoring_bw == 0))
    {
        bf->c1 = 1;
        msg->num_cll++;
        tw_stime bw_ts = bw_reset_window + codes_local_latency(lp);
        terminal_plus_message * m;
        tw_event * e = model_net_method_event_new(lp->gid, bw_ts, lp, 
             DRAGONFLY_PLUS_ROUTER, (void**)&m, NULL); 
        m->type = R_BANDWIDTH; 
        m->magic = router_magic_num;
        tw_event_send(e);
        s->is_monitoring_bw = 1;
    }
    int vcg = 0;
    if(num_qos_levels > 1)
      vcg = get_vcg_from_category(msg);


    //MSG COUNTING SPECIFIC -------
    //If spine router, count how many packets I have received & identify their app id 
    msg->last_received_time = tw_now(lp);

    int fdest_group_id = msg->dfp_dest_terminal_id / (s->params->total_terminals / s->params->num_groups);
    int inter_group_transmit = 0;
    if(fdest_group_id != s->group_id)
        inter_group_transmit = 1;

    //printf("msg [%d ==> %d] of App %d: Received on router %d(type %d) in group %d, at time %.5f\n", msg->dfp_src_terminal_id, msg->dfp_dest_terminal_id, msg->app_id ,s->router_id, s->dfp_router_type, s->group_id, msg->last_received_time);

    if(inter_group_transmit>0 && s->params->counting_bool>0 && s->dfp_router_type == SPINE && msg->last_received_time >= s->params->counting_start && msg->last_received_time <= s->params->counting_end) {

        s->total_packets_received++;
        tw_stime time_now = msg->last_received_time - s->params->counting_start;
        int window = (int) floor(time_now/s->params->counting_interval);
        if (window >= s->params->counting_windows)
            tw_error(TW_LOC, "Router %d, will access window %d, greater than max %d\n", s->router_id, window, s->params->counting_windows);
        s->msg_counting[window][msg->app_id]++;
    }
    //MSG COUNTING SPECIFIC END -------

    int next_stop = -1, output_port = -1, output_chan = -1, adap_chan = -1;
    int dfp_dest_terminal_id = msg->dfp_dest_terminal_id;
    int dest_router_id = dragonfly_plus_get_assigned_router_id(dfp_dest_terminal_id, s->params);
    // int dest_router_id = codes_mapping_get_lp_relative_id(msg->dest_terminal_id, 0, 0) / s->params->num_cn;
    int local_grp_id = s->router_id / s->params->num_routers;
    int src_grp_id = msg->origin_router_id / s->params->num_routers;
    int dest_grp_id = dest_router_id / s->params->num_routers;
    int intm_router_id;
    short prev_path_type = 0, next_path_type = 0;

    terminal_plus_message_list *cur_chunk =
        (terminal_plus_message_list *) calloc(1, sizeof(terminal_plus_message_list));
    init_terminal_plus_message_list(cur_chunk, msg);

    // packets start out as minimal when received from a terminal. The path type is changed off of minimal if/when the packet takes a nonminimal path during routing
    if( cur_chunk->msg.last_hop == TERMINAL) {
        cur_chunk->msg.path_type = MINIMAL;
    }

    Connection next_stop_conn = do_dfp_routing(s, bf, &(cur_chunk->msg), lp, dest_router_id);
    msg->num_rngs += (cur_chunk->msg).num_rngs; //make sure we're counting the rngs called during do_dfp_routing()

    if (s->connMan->is_any_connection_to(next_stop_conn.dest_gid) == false)
        tw_error(TW_LOC, "Router %d does not have a connection to chosen destination %d\n", s->router_id, next_stop_conn.dest_gid);


    output_port = next_stop_conn.port;

    if (next_stop_conn.conn_type != CONN_TERMINAL) {
        int id = next_stop_conn.dest_gid;
        tw_lpid router_dest_id;
        codes_mapping_get_lp_id(lp_group_name, LP_CONFIG_NM_ROUT, s->anno, 0, id / num_routers_per_mgrp, id % num_routers_per_mgrp, &router_dest_id);
        next_stop = router_dest_id;
    }
    else {
        next_stop = cur_chunk->msg.dest_terminal_id;
    }


    //From here the output port is known and output_chan is determined shortly
    assert(output_port >= 0);
    cur_chunk->msg.vc_index = output_port;
    cur_chunk->msg.next_stop = next_stop;

    output_chan = 0;
    if(cur_chunk->msg.dfp_upward_channel_flag == 1) {
        output_chan = 1;
    }
    else if (cur_chunk->msg.dfp_upward_channel_flag == 0) {
        output_chan = 0;
    }
    else
        tw_error(TW_LOC, "upward channel flag has invalid value\n");
  
    assert(output_chan < vcs_per_qos);
    output_chan = output_chan + (vcg * vcs_per_qos);

    ConnectionType port_type = s->connMan->get_port_type(output_port);
    int max_vc_size = 0;
    if (port_type == CONN_GLOBAL) {
        max_vc_size = s->params->global_vc_size;
    }
    else if (port_type == CONN_LOCAL) {
        max_vc_size = s->params->local_vc_size;
    }
    else {
        assert(port_type == CONN_TERMINAL);
        max_vc_size = s->params->cn_vc_size;
    }

    // if (port_type == CONN_GLOBAL) {
    //     int gc_num = output_port - s->params->intra_grp_radix;
    //     s->gc_usage[gc_num]++;
    // }

    cur_chunk->msg.output_chan = output_chan;
    cur_chunk->msg.my_N_hop++;

    if (output_port >= s->params->radix)
        tw_error(TW_LOC, "\n%d: Output port greater than router radix %d ",s->router_id, output_port);

    if (output_chan >= s->params->num_vcs || output_chan < 0)
        printf(
            "\n Packet %llu Output chan %d output port %d my rid %d dest rid %d path %d my gid %d dest gid "
            "%d",
            cur_chunk->msg.packet_ID, output_chan, output_port, s->router_id, dest_router_id,
            cur_chunk->msg.path_type, src_grp_id, dest_grp_id);

    assert(output_chan < s->params->num_vcs && output_chan >= 0);

    if(cur_chunk->msg.packet_ID == LLU(TRACK_PKT) && cur_chunk->msg.src_terminal_id == T_ID)
            printf("\n Packet %llu arrived at router %u next stop %d final stop %d local hops %d global hops %d", cur_chunk->msg.packet_ID, s->router_id, next_stop, dest_router_id, cur_chunk->msg.my_l_hop, cur_chunk->msg.my_g_hop);

    if (msg->remote_event_size_bytes > 0) {
        void *m_data_src = model_net_method_get_edata(DRAGONFLY_PLUS_ROUTER, msg);
        cur_chunk->event_data = (char *) calloc(1, msg->remote_event_size_bytes);
        memcpy(cur_chunk->event_data, m_data_src, msg->remote_event_size_bytes);
    }

    if (s->vc_occupancy[output_port][output_chan] + s->params->chunk_size <= max_vc_size) {
        bf->c2 = 1;
        router_credit_send(s, msg, lp, -1, &(msg->num_rngs));
        append_to_terminal_plus_message_list(s->pending_msgs[output_port], s->pending_msgs_tail[output_port],
                                             output_chan, cur_chunk);
        s->vc_occupancy[output_port][output_chan] += s->params->chunk_size;
        if (s->in_send_loop[output_port] == 0) {
            bf->c3 = 1;
            terminal_plus_message *m;
            msg->num_cll++;
            ts = codes_local_latency(lp);
            tw_event *e =
                model_net_method_event_new(lp->gid, ts, lp, DRAGONFLY_PLUS_ROUTER, (void **) &m, NULL);
            m->type = R_SEND;
            m->magic = router_magic_num;
            m->vc_index = output_port;

            tw_event_send(e);
            s->in_send_loop[output_port] = 1;
        }
    }
    else {
        bf->c4 = 1;
        s->stalled_chunks[output_port]++;
        cur_chunk->msg.saved_vc = msg->vc_index;
        cur_chunk->msg.saved_channel = msg->output_chan;
        append_to_terminal_plus_message_list(s->queued_msgs[output_port], s->queued_msgs_tail[output_port],
                                             output_chan, cur_chunk);
        s->queued_count[output_port] += s->params->chunk_size;

        //THIS WAS REMOVED WHEN QOS WAS INSTITUTED - READDED 5/20/19
        /* a check for pending msgs is non-empty then we dont set anything. If
        * that is empty then we check if last_buf_full is set or not. If already
        * set then we don't overwrite it. If two packets arrive next to each other
        * then the first person should be setting it. */
        if(s->pending_msgs[output_port][output_chan] == NULL && s->last_buf_full[output_port] == 0.0)
        {
            bf->c22 = 1;
            msg->saved_busy_time = s->last_buf_full[output_port];
            s->last_buf_full[output_port] = tw_now(lp);
        }
    }

    msg->saved_vc = output_port;
    msg->saved_channel = output_chan;
    return;
}

static void router_packet_send_rc(router_state *s, tw_bf *bf, terminal_plus_message *msg, tw_lp *lp)
{
    router_ecount--;
    router_rev_ecount++;
    
    int num_qos_levels = s->params->num_qos_levels;
    int output_port = msg->saved_vc;
      
    if(msg->qos_reset1)
          s->qos_status[output_port][0] = Q_ACTIVE;
     if(msg->qos_reset2)
          s->qos_status[output_port][1] = Q_ACTIVE;
    
    if(msg->last_saved_qos)
       s->last_qos_lvl[output_port] = msg->last_saved_qos; 
     
    if(bf->c1) {
        s->in_send_loop[output_port] = 1;
        if(bf->c2) {
            s->last_buf_full[output_port] = msg->saved_busy_time;
        }
        return;  
    }
   
    for(int i = 0; i < msg->num_rngs; i++)
        tw_rand_reverse_unif(lp->rng);

   for(int i = 0; i < msg->num_cll; i++)
       codes_local_latency_reverse(lp);
   
   int output_chan = msg->saved_channel;
   if(bf->c8)
    {
        s->busy_time[output_port] = msg->saved_rcv_time;
        s->busy_time_sample[output_port] = msg->saved_sample_time;
        s->ross_rsample.busy_time[output_port] = msg->saved_sample_time;
        s->last_buf_full[output_port] = msg->saved_busy_time;
   }

    terminal_plus_message_list *cur_entry = (terminal_plus_message_list *) rc_stack_pop(s->st);
    assert(cur_entry);
    
    int vcg = 0;
    if (num_qos_levels > 1)
        vcg = get_vcg_from_category(&(cur_entry->msg));

    int msg_size = s->params->chunk_size;
    if(cur_entry->msg.packet_size < s->params->chunk_size)
        msg_size = cur_entry->msg.packet_size;

    s->qos_data[output_port][vcg] -= msg_size;
    s->next_output_available_time[output_port] = msg->saved_available_time;

    if (bf->c11) {
        s->link_traffic[output_port] -= cur_entry->msg.packet_size % s->params->chunk_size;
        s->link_traffic_sample[output_port] -= cur_entry->msg.packet_size % s->params->chunk_size;
        s->ross_rsample.link_traffic_sample[output_port] -= cur_entry->msg.packet_size % s->params->chunk_size;
        s->link_traffic_ross_sample[output_port] -= cur_entry->msg.packet_size % s->params->chunk_size;
    }
    if (bf->c12) {
        s->link_traffic[output_port] -= s->params->chunk_size;
        s->link_traffic_sample[output_port] -= s->params->chunk_size;
        s->ross_rsample.link_traffic_sample[output_port] -= s->params->chunk_size;
        s->link_traffic_ross_sample[output_port] -= s->params->chunk_size;
    }

    prepend_to_terminal_plus_message_list(s->pending_msgs[output_port], s->pending_msgs_tail[output_port],
                                          output_chan, cur_entry);

    if (bf->c4) {
        s->in_send_loop[output_port] = 1;
    }
}

/* MM: I think this mostly stays the same. */
/* routes the current packet to the next stop */
static void router_packet_send(router_state *s, tw_bf *bf, terminal_plus_message *msg, tw_lp *lp)
{
    router_ecount++;

    tw_stime ts;
    tw_event *e;
    terminal_plus_message *m;
    int output_port = msg->vc_index;

    terminal_plus_message_list *cur_entry = NULL;

    /* reset qos rc handler before incrementing it */
    msg->last_saved_qos = -1;
    msg->qos_reset1 = -1;
    msg->qos_reset2 = -1;
    msg->num_cll = 0;
    msg->num_rngs = 0;

    int num_qos_levels = s->params->num_qos_levels;
    int output_chan = get_next_router_vcg(s, bf, msg, lp); //includes default output_chan setting functionality

    msg->saved_vc = output_port;
    msg->saved_channel = output_chan;
  
    if(output_chan < 0) {
      bf->c1 = 1;
      s->in_send_loop[output_port] = 0;
      if(s->queued_count[output_port] && !s->last_buf_full[output_port]) //10-31-19, not sure why this was added here with the qos stuff
      {
        bf->c2 = 1; 
        msg->saved_busy_time = s->last_buf_full[output_port];
        s->last_buf_full[output_port] = tw_now(lp);
      }  
      return;
    }
  
    cur_entry = s->pending_msgs[output_port][output_chan];
 
    assert(cur_entry != NULL);
  
    if(s->last_buf_full[output_port]) //10-31-19, same here as above comment
    {
      bf->c8 = 1;
      msg->saved_rcv_time = s->busy_time[output_port]; 
      msg->saved_busy_time = s->last_buf_full[output_port]; 
      msg->saved_sample_time = s->busy_time_sample[output_port];  
      s->busy_time[output_port] += (tw_now(lp) - s->last_buf_full[output_port]); 
      s->busy_time_sample[output_port] += (tw_now(lp) - s->last_buf_full[output_port]);
      s->ross_rsample.busy_time[output_port] += (tw_now(lp) - s->last_buf_full[output_port]);
      s->last_buf_full[output_port] = 0.0;
    }

    int vcg = 0;
    if (num_qos_levels > 1)
        vcg = get_vcg_from_category(&(cur_entry->msg));

    int to_terminal = 1, global = 0;
    double delay = s->params->cn_delay;
    double bandwidth = s->params->cn_bandwidth;

    if (output_port < s->params->intra_grp_radix) {
        to_terminal = 0;
        delay = s->params->local_delay;
        bandwidth = s->params->local_bandwidth;
    }
    else if (output_port < s->params->intra_grp_radix + s->params->num_global_connections) {
        to_terminal = 0;
        global = 1;
        delay = s->params->global_delay;
        bandwidth = s->params->global_bandwidth;
    }

    uint64_t num_chunks = cur_entry->msg.packet_size / s->params->chunk_size;
    if (cur_entry->msg.packet_size < s->params->chunk_size)
        num_chunks++;

    double bytetime = delay;

    if (cur_entry->msg.packet_size == 0)
        bytetime = bytes_to_ns(s->params->credit_size, bandwidth);

    if ((cur_entry->msg.packet_size < s->params->chunk_size) && (cur_entry->msg.chunk_id == num_chunks - 1))
        bytetime = bytes_to_ns(cur_entry->msg.packet_size % s->params->chunk_size, bandwidth);

    msg->num_rngs++;
    ts = g_tw_lookahead + tw_rand_unif(lp->rng) + bytetime + s->params->router_delay;

    msg->saved_available_time = s->next_output_available_time[output_port];
    s->next_output_available_time[output_port] = maxd(s->next_output_available_time[output_port], tw_now(lp));
    s->next_output_available_time[output_port] += ts;

    ts = s->next_output_available_time[output_port] - tw_now(lp);
    // dest can be a router or a terminal, so we must check
    void *m_data;
    if (to_terminal) {
        assert(cur_entry->msg.next_stop == cur_entry->msg.dest_terminal_id);
        e = model_net_method_event_new(cur_entry->msg.next_stop,
                                       s->next_output_available_time[output_port] - tw_now(lp), lp,
                                       DRAGONFLY_PLUS, (void **) &m, &m_data);
    }
    else {
        e = model_net_method_event_new(cur_entry->msg.next_stop,
                                       s->next_output_available_time[output_port] - tw_now(lp), lp,
                                       DRAGONFLY_PLUS_ROUTER, (void **) &m, &m_data);
    }
    memcpy(m, &cur_entry->msg, sizeof(terminal_plus_message));
    if (m->remote_event_size_bytes) {
        memcpy(m_data, cur_entry->event_data, m->remote_event_size_bytes);
    }

    if (global)
        m->last_hop = GLOBAL;
    else
        m->last_hop = LOCAL;

    m->intm_lp_id = lp->gid;
    m->magic = router_magic_num;

    int msg_size = s->params->chunk_size;
    if ((cur_entry->msg.packet_size % s->params->chunk_size) && (cur_entry->msg.chunk_id == num_chunks - 1)) {
        bf->c11 = 1;
        s->link_traffic[output_port] += (cur_entry->msg.packet_size % s->params->chunk_size);
        s->link_traffic_sample[output_port] += (cur_entry->msg.packet_size % s->params->chunk_size);
        s->ross_rsample.link_traffic_sample[output_port] += (cur_entry->msg.packet_size % s->params->chunk_size);
        msg_size = cur_entry->msg.packet_size % s->params->chunk_size;
    }
    else {
        bf->c12 = 1;
        s->link_traffic[output_port] += s->params->chunk_size;
        s->link_traffic_sample[output_port] += s->params->chunk_size;
        s->ross_rsample.link_traffic_sample[output_port] += s->params->chunk_size;
        s->link_traffic_ross_sample[output_port] += s->params->chunk_size;
    }

    /* Determine the event type. If the packet has arrived at the final
     * destination router then it should arrive at the destination terminal
     * next.*/
    if (to_terminal) {
        m->type = T_ARRIVE;
        m->magic = terminal_magic_num;
    }
    else {
        /* The packet has to be sent to another router */
        m->magic = router_magic_num;
        m->type = R_ARRIVE;
    }
    tw_event_send(e);

    cur_entry = return_head(s->pending_msgs[output_port], s->pending_msgs_tail[output_port], output_chan);
    rc_stack_push(lp, cur_entry, delete_terminal_plus_message_list, s->st);

    s->qos_data[output_port][vcg] += msg_size; 
    s->next_output_available_time[output_port] -= s->params->router_delay;
    ts -= s->params->router_delay;

    int next_output_chan = get_next_router_vcg(s, bf, msg, lp); 

    if(next_output_chan < 0)
    {
      bf->c4 = 1;
      s->in_send_loop[output_port] = 0;
      return;
    }
    cur_entry = s->pending_msgs[output_port][next_output_chan];
    assert(cur_entry != NULL); 

    terminal_plus_message *m_new;
    msg->num_rngs++;
    ts += g_tw_lookahead + tw_rand_unif(lp->rng);
    e = model_net_method_event_new(lp->gid, ts, lp, DRAGONFLY_PLUS_ROUTER, (void **) &m_new, NULL);
    m_new->type = R_SEND;
    m_new->magic = router_magic_num;
    m_new->vc_index = output_port;
    tw_event_send(e);
    
    return;
}

static void router_buf_update_rc(router_state *s, tw_bf *bf, terminal_plus_message *msg, tw_lp *lp)
{
    int indx = msg->vc_index;
    int output_chan = msg->output_chan;
    s->vc_occupancy[indx][output_chan] += s->params->chunk_size;
      
    for(int i = 0; i < msg->num_rngs; i++)
        tw_rand_reverse_unif(lp->rng);

    for(int i = 0; i < msg->num_cll; i++)
       codes_local_latency_reverse(lp);
    
    if (bf->c3) {
        s->busy_time[indx] = msg->saved_rcv_time;
        s->busy_time_sample[indx] = msg->saved_sample_time;
        s->ross_rsample.busy_time[indx] = msg->saved_sample_time;
        s->busy_time_ross_sample[indx] = msg->saved_busy_time_ross;
        s->last_buf_full[indx] = msg->saved_busy_time;
    }
    if (bf->c1) {
        terminal_plus_message_list *head =
            return_tail(s->pending_msgs[indx], s->pending_msgs_tail[indx], output_chan);
        prepend_to_terminal_plus_message_list(s->queued_msgs[indx], s->queued_msgs_tail[indx], output_chan,
                                              head);
        s->vc_occupancy[indx][output_chan] -= s->params->chunk_size;
        s->queued_count[indx] += s->params->chunk_size;
    }
    if (bf->c2) {
        s->in_send_loop[indx] = 0;
    }
}
/* Update the buffer space associated with this router LP */
static void router_buf_update(router_state *s, tw_bf *bf, terminal_plus_message *msg, tw_lp *lp)
{
    msg->num_cll = 0;
    msg->num_rngs = 0;

    int indx = msg->vc_index;
    int output_chan = msg->output_chan;
    s->vc_occupancy[indx][output_chan] -= s->params->chunk_size;

    if (s->last_buf_full[indx] > 0.0) {
        bf->c3 = 1;
        msg->saved_rcv_time = s->busy_time[indx];
        msg->saved_busy_time = s->last_buf_full[indx];
        msg->saved_sample_time = s->busy_time_sample[indx];
        msg->saved_busy_time_ross = s->busy_time_ross_sample[indx];
        s->busy_time[indx] += (tw_now(lp) - s->last_buf_full[indx]);
        s->busy_time_sample[indx] += (tw_now(lp) - s->last_buf_full[indx]);
        s->ross_rsample.busy_time[indx] += (tw_now(lp) - s->last_buf_full[indx]);
        s->busy_time_ross_sample[indx] += (tw_now(lp) - s->last_buf_full[indx]);
        s->last_buf_full[indx] = 0.0;
    }
    if (s->queued_msgs[indx][output_chan] != NULL) {
        bf->c1 = 1;
        assert(indx < s->params->radix);
        assert(output_chan < s->params->num_vcs);
        terminal_plus_message_list *head =
            return_head(s->queued_msgs[indx], s->queued_msgs_tail[indx], output_chan);
        router_credit_send(s, &head->msg, lp, 1, &(msg->num_rngs));
        append_to_terminal_plus_message_list(s->pending_msgs[indx], s->pending_msgs_tail[indx], output_chan,
                                             head);
        s->vc_occupancy[indx][output_chan] += s->params->chunk_size;
        s->queued_count[indx] -= s->params->chunk_size;
    }
    if (s->in_send_loop[indx] == 0 && s->pending_msgs[indx][output_chan] != NULL) {
        bf->c2 = 1;
        terminal_plus_message *m;
        msg->num_cll++;
        tw_stime ts = codes_local_latency(lp);
        tw_event *e = model_net_method_event_new(lp->gid, ts, lp, DRAGONFLY_PLUS_ROUTER, (void **) &m, NULL);
        m->type = R_SEND;
        m->vc_index = indx;
        m->magic = router_magic_num;
        s->in_send_loop[indx] = 1;
        tw_event_send(e);
    }
    return;
}

void terminal_plus_event(terminal_state *s, tw_bf *bf, terminal_plus_message *msg, tw_lp *lp)
{
    s->fwd_events++;
    //*(int *)bf = (int)0;
    assert(msg->magic == terminal_magic_num);

    rc_stack_gc(lp, s->st);
    switch (msg->type) {
        case T_GENERATE:
            packet_generate(s, bf, msg, lp);
            break;

        case T_ARRIVE:
            packet_arrive(s, bf, msg, lp);
            break;

        case T_SEND:
            packet_send(s, bf, msg, lp);
            break;

        case T_BUFFER:
            terminal_buf_update(s, bf, msg, lp);
            break;
        
        case T_BANDWIDTH:
            issue_bw_monitor_event_rc(s, bf, msg, lp);
            break;

        default:
            printf("\n LP %d Terminal message type not supported %d ", (int) lp->gid, msg->type);
            tw_error(TW_LOC, "Msg type not supported");
    }
}

void router_plus_event(router_state *s, tw_bf *bf, terminal_plus_message *msg, tw_lp *lp)
{
    s->fwd_events++;
    rc_stack_gc(lp, s->st);

    assert(msg->magic == router_magic_num);
    switch (msg->type) {
        case R_SEND:  // Router has sent a packet to an intra-group router (local channel)
            router_packet_send(s, bf, msg, lp);
            break;

        case R_ARRIVE:  // Router has received a packet from an intra-group router (local channel)
            router_packet_receive(s, bf, msg, lp);
            break;

        case R_BUFFER:
            router_buf_update(s, bf, msg, lp);
            break;

        case R_BANDWIDTH:
            issue_rtr_bw_monitor_event(s, bf, msg, lp);
            break;

        default:
            printf(
                "\n (%lf) [Router %d] Router Message type not supported %d dest "
                "terminal id %d packet ID %d ",
                tw_now(lp), (int) lp->gid, msg->type, (int) msg->dest_terminal_id, (int) msg->packet_ID);
            tw_error(TW_LOC, "Msg type not supported");
            break;
    }
}

/* Reverse computation handler for a terminal event */
void terminal_plus_rc_event_handler(terminal_state *s, tw_bf *bf, terminal_plus_message *msg, tw_lp *lp)
{
    s->rev_events++;
    switch (msg->type) {
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

        case T_BANDWIDTH:
            issue_bw_monitor_event_rc(s, bf, msg, lp);
            break;

        default:
            tw_error(TW_LOC, "\n Invalid terminal event type %d ", msg->type);
    }
}

/* Reverse computation handler for a router event */
void router_plus_rc_event_handler(router_state *s, tw_bf *bf, terminal_plus_message *msg, tw_lp *lp)
{
    s->rev_events++;

    switch (msg->type) {
        case R_SEND:
            router_packet_send_rc(s, bf, msg, lp);
            break;
        case R_ARRIVE:
            router_packet_receive_rc(s, bf, msg, lp);
            break;

        case R_BUFFER:
            router_buf_update_rc(s, bf, msg, lp);
            break;
        
        case R_BANDWIDTH:
            issue_rtr_bw_monitor_event_rc(s, bf, msg, lp);
            break;
    }
}

/* dragonfly compute node and router LP types */
extern "C" {
tw_lptype dragonfly_plus_lps[] = {
    // Terminal handling functions
    {
        (init_f) terminal_plus_init,
        (pre_run_f) NULL,
        (event_f) terminal_plus_event,
        (revent_f) terminal_plus_rc_event_handler,
        (commit_f) terminal_plus_commit,
        (final_f) dragonfly_plus_terminal_final,
        (map_f) codes_mapping,
        sizeof(terminal_state),
    },
    {
        (init_f) router_plus_init,
        (pre_run_f) NULL,
        (event_f) router_plus_event,
        (revent_f) router_plus_rc_event_handler,
        (commit_f) router_plus_commit,
        (final_f) dragonfly_plus_router_final,
        (map_f) codes_mapping,
        sizeof(router_state),
    },
    {NULL, NULL, NULL, NULL, NULL, NULL, NULL, 0},
};
}

/* returns the dragonfly lp type for lp registration */
static const tw_lptype *dragonfly_plus_get_cn_lp_type(void)
{
    return (&dragonfly_plus_lps[0]);
}
static const tw_lptype *router_plus_get_lp_type(void)
{
    return (&dragonfly_plus_lps[1]);
}

static void dragonfly_plus_register(tw_lptype *base_type)
{
    lp_type_register(LP_CONFIG_NM_TERM, base_type);
}

static void router_plus_register(tw_lptype *base_type)
{
    lp_type_register(LP_CONFIG_NM_ROUT, base_type);
}

/* Routing Functions */
static int get_min_hops_to_dest_from_conn(router_state *s, tw_bf *bf, terminal_plus_message *msg, tw_lp *lp, Connection conn)
{
    int my_type = s->dfp_router_type;
    int next_hops_type = dragonfly_plus_get_router_type(conn.dest_gid, s->params);

    int dfp_dest_terminal_id = msg->dfp_dest_terminal_id;
    int fdest_router_id = dragonfly_plus_get_assigned_router_id(dfp_dest_terminal_id, s->params);
    int fdest_group_id = fdest_router_id / s->params->num_routers;

    if (msg->dfp_upward_channel_flag)
    {
        if (my_type == SPINE)
            return 2; //Next Spine -> Leaf -> dest_term
        else
            return 3; //Next Spine -> Spine -> Leaf -> dest_term
    }

    if(conn.dest_group_id == fdest_group_id) {
        if (next_hops_type == SPINE)
            return 2; //Next Spine -> Leaf -> dest_term
        else {
            assert(next_hops_type == LEAF);
            return 1; //Next Leaf -> dest_term
        }
    }
    else { //next is not in final destination group
        if (next_hops_type == SPINE) {
            vector< Connection > cons_to_dest_group = connManagerList[conn.dest_gid].get_connections_to_group(fdest_group_id);
            if (cons_to_dest_group.size() == 0)
                return 5; //Next Spine -> Leaf -> Spine -> Spine -> Leaf -> dest_term
            else
                return 3;  //Next Spine -> Spine -> Leaf -> dest_term
        }
        else {
            assert(next_hops_type == LEAF);
            return 4; //Next Leaf -> Spine -> Spine -> Leaf -> dest_term
        }
    }
}

//usded by FPAR, compare each port score with T, if muliples are <= T, choose a random one
static Connection get_connection_compare_T(router_state *s, tw_bf *bf, terminal_plus_message *msg, tw_lp *lp, vector<Connection> conns, int threshold)
{
    assert(threshold>=0 && threshold<=100);
    int origin_group_id = msg->origin_router_id / s->params->num_routers;
    int my_group_id = s->router_id / s->params->num_routers;

    if (conns.size() == 0) {
        Connection bad_conn;
        bad_conn.src_gid = -1;
        bad_conn.port = -1;
        return bad_conn;
    }

    int num_to_compare = conns.size();

    int scores[num_to_compare];
    int best_score_index = 0;

    vector<int> best_indexes;

    if (scoring_preference == LOWER) {
        for(int i = 0; i < num_to_compare; i++)
            {
                scores[i] = dfp_score_connection(s, bf, msg, lp, conns[i], C_MIN);
                if (scores[i] <= threshold)
                    best_indexes.push_back(i);
                //if(my_group_id==0 && s->dfp_router_type==SPINE && num_to_compare == 2) printf("\tCompare T: Router %d, to port: %d, score %d\n", s->router_id, conns[i].port, scores[i]);
            }
            if (best_indexes.size() > 0) {
                msg->num_rngs++;
                best_score_index = best_indexes[tw_rand_ulong(lp->rng, 0, best_indexes.size()-1)];
            } else {
                //no high priority queue < T
                Connection bad_conn2;
                bad_conn2.src_gid = -2;
                bad_conn2.port = -2;
                return bad_conn2;
            }
    }
    else 
        tw_error(TW_LOC, "Higher scoring preference currently not implemented for PFAR");

    //if(origin_group_id==0 && s->dfp_router_type==SPINE) printf("\t\tCompare T: Router %d, among size %lu, choose index: %d \n", s->router_id, best_indexes.size(), best_score_index);

    return conns[best_score_index];
}

//give the vector of all routers attached with global link(s) in current group, used by get_legal_nonminimal_stops
static vector< Connection > get_router_with_global_links(router_state *s, tw_bf *bf, terminal_plus_message *msg, tw_lp *lp)
{
    vector< Connection> spine_with_global_link;
    set<int> poss_router_id_set_to_group;
    int my_group_id = s->router_id / s->params->num_routers;

    for(int desg=0; desg< s->params->num_groups; desg++) {
        for(int i = 0; i < connectionList[my_group_id][desg].size(); i++)
        {
            int poss_router_id = connectionList[my_group_id][desg][i];
            // printf("%d\n",poss_router_id);
            if (poss_router_id_set_to_group.count(poss_router_id) == 0) { //if we haven't added the connections from poss_router_id yet
                vector< Connection > conns = s->connMan->get_connections_to_gid(poss_router_id, CONN_LOCAL);
                poss_router_id_set_to_group.insert(poss_router_id);
                spine_with_global_link.insert(spine_with_global_link.end(), conns.begin(), conns.end());
            }
        }
    }
    return spine_with_global_link;
}

//Returns a vector of connections that are legal dragonfly plus routes that specifically would not allow for a minimal connection to the specific router specified in get_possible_stops_to_specific_router()
//Be very wary of using this method, results may not make sense if possible_minimal_stops is not a vector of minimal next stops to fdest_rotuer_id
//Nonminimal specifically refers to any move that does not move directly toward the destination router
static vector< Connection > get_legal_nonminimal_stops(router_state *s, tw_bf *bf, terminal_plus_message *msg, tw_lp *lp, vector< Connection > possible_minimal_stops, int fdest_router_id)
{
    int my_router_id = s->router_id;
    int my_group_id = s->router_id / s->params->num_routers;
    int origin_group_id = msg->origin_router_id / s->params->num_routers;
    int fdest_group_id = fdest_router_id / s->params->num_routers;
    bool in_intermediate_group = (my_group_id != origin_group_id) && (my_group_id != fdest_group_id);


    vector< Connection > possible_nonminimal_stops;

    if (my_group_id == origin_group_id) { //then we're just sending upward out of the group
        if (s->dfp_router_type == LEAF) { //then any connections to spines that are not in possible_minimal_stops should be included
            vector< Connection > conns_to_spines = s->connMan->get_connections_by_type(CONN_LOCAL);
            possible_nonminimal_stops = set_difference_vectors(conns_to_spines, possible_minimal_stops); //get the complement of possible_minimal_stops
        
            // for the case that not all spine routers have global link
            vector< Connection > spine_with_global_link = get_router_with_global_links(s, bf, msg, lp);
            possible_nonminimal_stops = set_common_vectors(possible_nonminimal_stops, spine_with_global_link);
        }
        else if (s->dfp_router_type == SPINE) { //then we have to send via global connections that aren't to the dest group
            vector< Connection > conns_to_other_groups = s->connMan->get_connections_by_type(CONN_GLOBAL);
            possible_nonminimal_stops = set_difference_vectors(conns_to_other_groups, possible_minimal_stops);
        }
    }
    else if (in_intermediate_group) {
        if (msg->dfp_upward_channel_flag == 1) { //then we return an empty vector as the only legal moves are those that already exist in possible_minimal_stops
            assert(possible_nonminimal_stops.size() == 0);
            return possible_nonminimal_stops;
        }
        else if (s->dfp_router_type == LEAF) { //if we're a leaf in an intermediate group then the routing alg should have flipped the dfp_upward channel already but lets return empty just in case
            assert(possible_nonminimal_stops.size() == 0);
            return possible_nonminimal_stops;
        }
        else if (s->dfp_router_type == SPINE) { //then possible_minimal_stops will be the list of connections 
            assert(msg->dfp_upward_channel_flag == 0);
            vector< Connection> conns_to_leaves = s->connMan->get_connections_by_type(CONN_LOCAL);
            possible_nonminimal_stops = set_difference_vectors(conns_to_leaves, possible_minimal_stops); //get the complement of possible_minimal_stops
        }
        else {
            tw_error(TW_LOC, "Impossible Error - Something's majorly wrong if this is tripped");
        }

    }
    else if (my_group_id == fdest_group_id) {
        if (s->params->dest_spine_consider_nonmin == true || s->params->dest_spine_consider_global_nonmin == true) {
            if (s->dfp_router_type == SPINE) {
                vector< Connection > poss_next_conns;

                if (s->params->dest_spine_consider_nonmin == true) {
                    vector< Connection > conns_to_leaves = s->connMan->get_connections_by_type(CONN_LOCAL);
                    for (int i = 0; i < conns_to_leaves.size(); i++)
                    {
                        if (conns_to_leaves[i].dest_gid != fdest_router_id)
                            poss_next_conns.push_back(conns_to_leaves[i]);
                    }
                }
                if (s->params->dest_spine_consider_global_nonmin == true) {
                    vector< Connection > conns_to_spines = s->connMan->get_connections_by_type(CONN_GLOBAL);
                    for (int i = 0; i < conns_to_spines.size(); i++)
                    {
                        if (conns_to_spines[i].dest_group_id != fdest_group_id && conns_to_spines[i].dest_group_id != origin_group_id) {
                            poss_next_conns.push_back(conns_to_spines[i]);
                        }
                    }
                }

                return poss_next_conns;
            }
            else {
                assert(s->dfp_router_type == LEAF);
                assert(possible_nonminimal_stops.size() == 0); //empty because a leaf in the destination group has no legal nonminimal moves
                return possible_nonminimal_stops;
            }


        }
    }
    else {
        tw_error(TW_LOC, "Invalid group classification\n");
    }

    // assert(possible_nonminimal_stops.size() > 0);
    return possible_nonminimal_stops;
}

//The term minimal in this case refers to "Moving toward destination router". If a packet is at an intermediate spine and that spine doesn't
//have a direct connection to the destinatino group, then there wouldn't be any "legal minimal stops". The packet would have to continue to
//some leaf in the group first. THIS DOES NOT INCLUDE REROUTING. The only time an intermediate leaf can send to an intermediate spine is if
//dfp_upward_channel_flag is 0.
static vector< Connection > get_legal_minimal_stops(router_state *s, tw_bf *bf, terminal_plus_message *msg, tw_lp *lp, int fdest_router_id)
{
    int my_router_id = s->router_id;
    int my_group_id = s->router_id / s->params->num_routers;
    int origin_group_id = msg->origin_router_id / s->params->num_routers;
    int fdest_group_id = fdest_router_id / s->params->num_routers;

    if (my_group_id != fdest_group_id) { //then we're in the source or the intermediate group.
        if (s->dfp_router_type == LEAF) {
            vector< Connection> possible_next_conns_to_group;
            set<int> poss_router_id_set_to_group;
            for(int i = 0; i < connectionList[my_group_id][fdest_group_id].size(); i++)
            {
                int poss_router_id = connectionList[my_group_id][fdest_group_id][i];
                // printf("%d\n",poss_router_id);
                if (poss_router_id_set_to_group.count(poss_router_id) == 0) { //if we haven't added the connections from poss_router_id yet
                    vector< Connection > conns = s->connMan->get_connections_to_gid(poss_router_id, CONN_LOCAL);
                    poss_router_id_set_to_group.insert(poss_router_id);
                    possible_next_conns_to_group.insert(possible_next_conns_to_group.end(), conns.begin(), conns.end());
                }
            }
            return possible_next_conns_to_group;
        }
        else if (s->dfp_router_type == SPINE) {
            return s->connMan->get_connections_to_group(fdest_group_id);
        }
    }
    else {
        assert(my_group_id == fdest_group_id);
        if (s->dfp_router_type == SPINE) {
            vector< Connection > possible_next_conns = s->connMan->get_connections_to_gid(fdest_router_id, CONN_LOCAL);
            return possible_next_conns;
        }
        else {
            assert(s->dfp_router_type == LEAF);
            
            if (my_router_id != fdest_router_id) { //then we're also the source group and we need to send to any spine in our group
                assert(my_group_id == origin_group_id);
                return s->connMan->get_connections_by_type(CONN_LOCAL);
            }
            else { //then we're the dest router
                assert(my_router_id == fdest_router_id);
                vector< Connection > empty;
                return empty;
            }
        } 
    }
    vector< Connection > empty;
    return empty;
}


static Connection do_dfp_prog_adaptive_routing(router_state *s, tw_bf *bf, terminal_plus_message *msg, tw_lp *lp, int fdest_router_id)
{
    int my_router_id = s->router_id;
    int my_group_id = s->router_id / s->params->num_routers;
    int fdest_group_id = fdest_router_id / s->params->num_routers;
    int origin_group_id = msg->origin_router_id / s->params->num_routers;
    bool in_intermediate_group = (my_group_id != origin_group_id) && (my_group_id != fdest_group_id);
    bool outside_source_group = (my_group_id != origin_group_id);
    int adaptive_threshold = s->params->adaptive_threshold;
    bool next_hop_is_global = false;

    //The check for dest group local routing has already been completed at this point

    Connection nextStopConn;
    vector< Connection > poss_min_next_stops = get_legal_minimal_stops(s, bf, msg, lp, fdest_router_id);
    vector< Connection > poss_intm_next_stops = get_legal_nonminimal_stops(s, bf, msg, lp, poss_min_next_stops, fdest_router_id);

    if ( (poss_min_next_stops.size() == 0) && (poss_intm_next_stops.size() == 0))
        tw_error(TW_LOC, "No possible next stops!!!");

    Connection best_min_conn, best_intm_conn;
    ConnectionType conn_type_of_mins, conn_type_of_intms;

    //determine if it's a global or local channel next
    if (poss_min_next_stops.size() > 0)
        conn_type_of_mins = poss_min_next_stops[0].conn_type; //they should all be the same
    if (poss_intm_next_stops.size() > 0)
        conn_type_of_intms = poss_intm_next_stops[0].conn_type;

    if (conn_type_of_mins == CONN_GLOBAL)
        best_min_conn = get_best_connection_from_conns(s, bf, msg, lp, poss_min_next_stops);
    else
        best_min_conn = get_absolute_best_connection_from_conns(s, bf, msg, lp, poss_min_next_stops);

    if (conn_type_of_intms == CONN_GLOBAL)
        best_intm_conn = get_best_connection_from_conns(s, bf, msg, lp, poss_intm_next_stops);
    else
        best_intm_conn = get_absolute_best_connection_from_conns(s, bf, msg, lp, poss_intm_next_stops);

    //if the best is intermediate, encode the intermediate router id in the message, set path type to non minimal
    int min_score = dfp_score_connection(s, bf, msg, lp, best_min_conn, C_MIN);
    int intm_score = dfp_score_connection(s, bf, msg, lp, best_intm_conn, C_NONMIN);

    bool route_to_fdest = false;
    if (msg->dfp_upward_channel_flag == 1) { //then we need to route to fdest, no questions asked.
        route_to_fdest = true;
    }
    else {
        if (scoring_preference == LOWER) {
            if (min_score <= adaptive_threshold) {
                route_to_fdest = true;
            }
            else if (min_score <= intm_score) {
                route_to_fdest = true;
            }
        }
        else { //HIGHER is better
            if (adaptive_threshold > 0)
                tw_error(TW_LOC, "Adaptive threshold not compatible with HIGHER score preference yet\n"); //TODO fix this
            if (min_score >= intm_score) {
                route_to_fdest = true;
            }
        }
    }

    if (msg->dfp_upward_channel_flag == 0) { //if the flag is 1, then only minimal hops are considered
        if (s->params->source_leaf_consider_nonmin == false) { //then we aren't supposed to let the source leaves consider any routes that wouldn't also be minimal
            if (my_group_id == origin_group_id) {
                if (s->dfp_router_type == LEAF)
                    route_to_fdest = true; //we aren't supposed to consider nonmin routes as the source leaf with given config
            }
        }
        if (s->params->int_spine_consider_min == false) { //then we aren't supposed to let spines in intermediate group route to minimal even if possible
            if (in_intermediate_group) {
                if (s->dfp_router_type == SPINE) {
                    route_to_fdest = false;
                }
            }
        }
        if (my_group_id == fdest_group_id && s->dfp_router_type == SPINE) {
            if (s->params->dest_spine_consider_nonmin == false && s->params->dest_spine_consider_global_nonmin == false)
                route_to_fdest = true;
        }
    }

    if (route_to_fdest && (poss_min_next_stops.size() == 0))
        route_to_fdest = false;
    if (!route_to_fdest && (poss_intm_next_stops.size() == 0))
        route_to_fdest = true;

    if (route_to_fdest){
        if (in_intermediate_group == true)
            msg->dfp_upward_channel_flag = 1; //if we're in an intermediate group and take a turn toward the dest group, we flip the flag!
        if ( (my_group_id == fdest_group_id) && (s->dfp_router_type == LEAF) && (my_router_id != fdest_router_id) )
            msg->dfp_upward_channel_flag = 1; //if we're a dest leaf but not the fdest leaf we must route using vl 1

        nextStopConn = best_min_conn;
    }
    else {
        nextStopConn = best_intm_conn;
        msg->path_type = NON_MINIMAL;
    }

    if (nextStopConn.port == -1)
        tw_error(TW_LOC, "DFP Prog Adaptive Routing: No valid next hop was chosen\n");

    return nextStopConn;
}


// Fully Progressive Adaptive Routing (FPAR)
// Shpiner, Alexander, et al. "Dragonfly+: Low cost topology for scaling datacenters." HiPINEB 2017
static Connection do_dfp_FPAR(router_state *s, tw_bf *bf, terminal_plus_message *msg, tw_lp *lp, int fdest_router_id)
{
    int my_router_id = s->router_id;
    int my_group_id = s->router_id / s->params->num_routers;
    int fdest_group_id = fdest_router_id / s->params->num_routers;
    int origin_group_id = msg->origin_router_id / s->params->num_routers;
    bool in_src_group = (my_group_id == origin_group_id);
    bool in_intermediate_group = (my_group_id != origin_group_id) && (my_group_id != fdest_group_id);
    bool in_dest_group = (my_group_id == fdest_group_id);
    bool outside_source_group = (my_group_id != origin_group_id);
    int adaptive_threshold = s->params->adaptive_threshold;

    // check FPAR validity: 
    // 1) should use ALPHA scoring, for now
    // 2) threshold within [0-100]
    if (scoring != ALPHA)
        tw_error(TW_LOC, "Using FPAR as routing: currently only works with ALPHA scoring, consider setting route_scoring_metric to alpha in modelnet config file\n");
    if (adaptive_threshold<0 || adaptive_threshold>100)
        tw_error(TW_LOC, "Using FPAR as routing: adaptive threshold is a percentage of router buffer size, consider setting adaptive_threshold an int value between [0--100] in modelnet config file\n");

    // The check for dest group local routing has already been completed at this point
    Connection nextStopConn;
    vector< Connection > poss_min_next_stops = get_legal_minimal_stops(s, bf, msg, lp, fdest_router_id);
    vector< Connection > poss_intm_next_stops = get_legal_nonminimal_stops(s, bf, msg, lp, poss_min_next_stops, fdest_router_id);

    if ( (poss_min_next_stops.size() == 0) && (poss_intm_next_stops.size() == 0))
        tw_error(TW_LOC, "No possible next stops on router %d", my_router_id);

    Connection best_min_conn, best_intm_conn;

    //for compare result with T
    Connection high_priority_conn, low_priority_conn;

    ConnectionType conn_type_of_mins, conn_type_of_intms;

    best_min_conn = get_absolute_best_connection_from_conns(s, bf, msg, lp, poss_min_next_stops);
    best_intm_conn = get_absolute_best_connection_from_conns(s, bf, msg, lp, poss_intm_next_stops);

    high_priority_conn = get_connection_compare_T(s, bf, msg, lp, poss_min_next_stops, adaptive_threshold);
    low_priority_conn = get_connection_compare_T(s, bf, msg, lp, poss_intm_next_stops, adaptive_threshold);

    int minimally_forwarded = 3;  // 0 -- no <=> intermediate path, 1 -- yes <=> min path, >1 report error

    if ( (poss_min_next_stops.size() > 0) && (poss_intm_next_stops.size() > 0)) {
        if (high_priority_conn.port == -2) {
            if(low_priority_conn.port == -2) {
                assert(best_min_conn.port >= 0);
                nextStopConn = best_min_conn;
                minimally_forwarded = 1;
            }
            else{
                nextStopConn = low_priority_conn;
                minimally_forwarded = 0;
            }
        } else {
            nextStopConn = high_priority_conn;
            minimally_forwarded = 1;
        }
    }
    else if ( poss_min_next_stops.size() > 0 ) {
        nextStopConn = best_min_conn;
        minimally_forwarded = 1;
    }
    else if ( poss_intm_next_stops.size() > 0 ) {
        nextStopConn = best_intm_conn;
        minimally_forwarded = 0;
    }
    else {
        tw_error(TW_LOC, "DFP Prog Adaptive Routing: router %d cannot find any forwarding path\n", s->router_id);
    }
    assert(minimally_forwarded < 2);

    if (s->dfp_router_type == LEAF) {
        if (in_src_group) {
            if (minimally_forwarded) {
                msg->path_type = MINIMAL;
            } else {
                msg->path_type = NON_MINIMAL;
            }

        } else if (in_intermediate_group) {
            assert(msg->dfp_upward_channel_flag == 0);
            assert(msg->path_type == NON_MINIMAL);
            //time to change vc
            msg->dfp_upward_channel_flag = 1;
            //nextStopConn = high_priority_conn;
            nextStopConn = best_min_conn;

            //printf("In FPAR, non-min LEAF routing: [MSG: %d => %d ]: router %d, type %d, group %d, nextStopgid %d, nextStop_group_id %d\n", msg->dfp_src_terminal_id, msg->dfp_dest_terminal_id, s->router_id, s->dfp_router_type, my_group_id, nextStopConn.dest_gid, nextStopConn.dest_group_id);

        } else if (in_dest_group) {
            tw_error(TW_LOC, "Routing in Dest group on Leaf is taking cared by do_dfp_routing funciton");

        } else
            tw_error(TW_LOC, "Msg not in any group type");

    } else if (s->dfp_router_type == SPINE) {
        if (in_src_group) {
            assert(msg->dfp_upward_channel_flag == 0);

            if (minimally_forwarded) {
                msg->path_type = MINIMAL;
            } else {
                msg->path_type = NON_MINIMAL;
            }

        } else if (in_intermediate_group) {
            
            if(msg->path_type != NON_MINIMAL) {
                printf("Error In FPAR: [MSG: %d => %d ]: router %d, type %d, group %d, nextStopgid %d, nextStop_group_id %d\n", msg->dfp_src_terminal_id, msg->dfp_dest_terminal_id, s->router_id, s->dfp_router_type, my_group_id, nextStopConn.dest_gid, nextStopConn.dest_group_id);
            } 

            if(msg->dfp_upward_channel_flag != 0) {
                nextStopConn=best_min_conn;
            }
        } else if (in_dest_group) {
            tw_error(TW_LOC, "Routing in Dest group on Spine is taking cared by do_dfp_routing funciton");
        } else
            tw_error(TW_LOC, "Msg not in any group type");

    } else
        tw_error(TW_LOC, "FPAR unkown router type");

    if (nextStopConn.port < 0){
        tw_error(TW_LOC, "DFP Prog Adaptive Routing: No valid next hop was chosen\n packetId %llu [MSG: %d (T_ID %u) => %d ] on router %d, upward %d? \n", msg->packet_ID, msg->dfp_src_terminal_id, msg->src_terminal_id, msg->dfp_dest_terminal_id, s->router_id, msg->dfp_upward_channel_flag);
    }
    return nextStopConn;
}

extern "C" {
/* data structure for dragonfly statistics */
struct model_net_method dragonfly_plus_method = {
    0,
    dragonfly_plus_configure,
    dragonfly_plus_register,
    dragonfly_plus_packet_event,
    dragonfly_plus_packet_event_rc,
    NULL,
    NULL,
    dragonfly_plus_get_cn_lp_type,
    dragonfly_plus_get_msg_sz,
    dragonfly_plus_report_stats,
    NULL,
    NULL,
    NULL, //(event_f)dragonfly_plus_sample_fn,
    NULL, //(revent_f)dragonfly_plus_sample_rc_fn,
    (init_f) dragonfly_plus_sample_init,
    NULL, //(final_f)dragonfly_plus_sample_fin,
    dfly_plus_register_model_types,
    dfly_plus_get_model_types,
};

struct model_net_method dragonfly_plus_router_method = {
    0,
    NULL,  // handled by dragonfly_configure
    router_plus_register,
    NULL,
    NULL,
    NULL,
    NULL,
    router_plus_get_lp_type,
    dragonfly_plus_get_msg_sz,
    NULL,  // not yet supported
    NULL,
    NULL,
    NULL, //(event_f)dragonfly_plus_rsample_fn,
    NULL, //(revent_f)dragonfly_plus_rsample_rc_fn,
    (init_f) dragonfly_plus_rsample_init,
    NULL, //(final_f)dragonfly_plus_rsample_fin,
    dfly_plus_router_register_model_types,
    dfly_plus_router_get_model_types,
};

// #ifdef ENABLE_CORTEX

// static int dragonfly_plus_get_number_of_compute_nodes(void* topo) {
//     printf("DRAGONFLY PLUS CORTEX NOT IMPLEMENTED\n");

//         // const dragonfly_plus_param * params = &all_params[num_params-1];
//         // if(!params)
//         //     return -1.0;

//         // return params->total_terminals;
// }

// static int dragonfly_plus_get_number_of_routers(void* topo) {
//         // TODO
//     printf("DRAGONFLY PLUS CORTEX NOT IMPLEMENTED\n");

//         // const dragonfly_plus_param * params = &all_params[num_params-1];
//         // if(!params)
//         //     return -1.0;

//         // return params->total_routers;
// }

// static double dragonfly_plus_get_router_link_bandwidth(void* topo, router_id_t r1, router_id_t r2) {
//         // TODO: handle this function for multiple cables between the routers.
//         // Right now it returns the bandwidth of a single cable only.
// 	// Given two router ids r1 and r2, this function should return the bandwidth (double)
// 	// of the link between the two routers, or 0 of such a link does not exist in the topology.
// 	// The function should return -1 if one of the router id is invalid.
//     printf("DRAGONFLY PLUS CORTEX NOT IMPLEMENTED\n");

//     // const dragonfly_plus_param * params = &all_params[num_params-1];
//     // if(!params)
//     //     return -1.0;

//     // if(r1 > params->total_routers || r2 > params->total_routers)
//     //     return -1.0;

//     // if(r1 < 0 || r2 < 0)
//     //     return -1.0;

//     // int gid_r1 = r1 / params->num_routers;
//     // int gid_r2 = r2 / params->num_routers;

//     // if(gid_r1 == gid_r2)
//     // {
//     //     int lid_r1 = r1 % params->num_routers;
//     //     int lid_r2 = r2 % params->num_routers;

//     //     /* The connection will be there if the router is in the same row or
//     //      * same column */
//     //     int src_row_r1 = lid_r1 / params->num_router_cols;
//     //     int src_row_r2 = lid_r2 / params->num_router_cols;

//     //     int src_col_r1 = lid_r1 % params->num_router_cols;
//     //     int src_col_r2 = lid_r2 % params->num_router_cols;

//     //     if(src_row_r1 == src_row_r2 || src_col_r1 == src_col_r2)
//     //         return params->local_bandwidth;
//     //     else
//     //         return 0.0;
//     // }
//     // else
//     // {
//     //     vector<bLink> &curVec = interGroupLinks[r1][gid_r2];
//     //     vector<bLink>::iterator it = curVec.begin();

//     //     for(; it != curVec.end(); it++)
//     //     {
//     //         bLink bl = *it;
//     //         if(bl.dest == r2)
//     //             return params->global_bandwidth;
//     //     }

//     //     return 0.0;
//     // }
//     // return 0.0;
// }

// static double dragonfly_plus_get_compute_node_bandwidth(void* topo, cn_id_t node) {
//         // TODO
// 	// Given the id of a compute node, this function should return the bandwidth of the
// 	// link connecting this compute node to its router.
// 	// The function should return -1 if the compute node id is invalid.
//     printf("DRAGONFLY PLUS CORTEX NOT IMPLEMENTED\n");

//     // const dragonfly_plus_param * params = &all_params[num_params-1];
//     // if(!params)
//     //     return -1.0;

//     // if(node < 0 || node >= params->total_terminals)
//     //     return -1.0;

//     // return params->cn_bandwidth;
// }

// static int dragonfly_plus_get_router_neighbor_count(void* topo, router_id_t r) {
//         // TODO
// 	// Given the id of a router, this function should return the number of routers
// 	// (not compute nodes) connected to it. It should return -1 if the router id
// 	// is not valid.
//     printf("DRAGONFLY PLUS CORTEX NOT IMPLEMENTED\n");

//     const dragonfly_plus_param * params = &all_params[num_params-1];
//     if(!params)
//         return -1.0;

//     if(r < 0 || r >= params->total_routers)
//         return -1.0;

//     /* Now count the global channels */
//     set<router_id_t> g_neighbors;

//     map< int, vector<bLink> > &curMap = interGroupLinks[r];
//     map< int, vector<bLink> >::iterator it = curMap.begin();
//     for(; it != curMap.end(); it++) {
//     for(int l = 0; l < it->second.size(); l++) {
//         g_neighbors.insert(it->second[l].dest);
//     }
//     }

//     int intra_group_id = r % params->num_routers;
//     int num_local_neighbors;
//     if (intra_group_id < (p->num_routers/2)) //leaf
//         num_local_neighbors = params->num_router_spine;
//     else //spine
//         num_local_neighbors = params->num_router_leaf;

//     return num_local_neighbors + g_neighbors.size();
// }

// static void dragonfly_plus_get_router_neighbor_list(void* topo, router_id_t r, router_id_t* neighbors) {
// 	// Given a router id r, this function fills the "neighbors" array with the ids of routers
// 	// directly connected to r. It is assumed that enough memory has been allocated to "neighbors"
// 	// (using get_router_neighbor_count to know the required size).
//     printf("DRAGONFLY PLUS CORTEX NOT IMPLEMENTED\n");

//     // const dragonfly_plus_param * params = &all_params[num_params-1];

//     // int gid = r / params->num_routers;
//     // int local_rid = r - (gid * params->num_routers);
//     // int src_row = local_rid / params->num_router_cols;
//     // int src_col = local_rid % params->num_router_cols;

//     // /* First the routers in the same row */
//     //  int i = 0;
//     //  int offset = 0;
//     //  while(i < params->num_router_cols)
//     //  {
//     //    int neighbor = gid * params->num_routers + (src_row * params->num_router_cols) + i;
//     //    if(neighbor != r)
//     //    {
//     //        neighbors[offset] = neighbor;
//     //        offset++;
//     //    }
//     //    i++;
//     //  }

//     // /* Now the routers in the same column. */
//     // offset = 0;
//     // i = 0;
//     // while(i <  params->num_router_rows)
//     // {
//     //     int neighbor = gid * params->num_routers + src_col + (i * params->num_router_cols);

//     //     if(neighbor != r)
//     //     {
//     //         neighbors[offset+params->num_router_cols-1] = neighbor;
//     //         offset++;
//     //     }
//     //     i++;
//     // }
//     // int g_offset = params->num_router_cols + params->num_router_rows - 2;

//     // /* Now fill up global channels */
//     // set<router_id_t> g_neighbors;

//     // map< int, vector<bLink> > &curMap = interGroupLinks[r];
//     // map< int, vector<bLink> >::iterator it = curMap.begin();
//     // for(; it != curMap.end(); it++) {
//     // for(int l = 0; l < it->second.size(); l++) {
//     //     g_neighbors.insert(it->second[l].dest);
//     // }
//     // }
//     // /* Now transfer the content of the sets to the array */
//     // set<router_id_t>::iterator it_set;
//     // int count = 0;

//     // for(it_set = g_neighbors.begin(); it_set != g_neighbors.end(); it_set++)
//     // {
//     //     neighbors[g_offset+count] = *it_set;
//     //     ++count;
//     // }
// }

// static int dragonfly_plus_get_router_location(void* topo, router_id_t r, int32_t* location, int size) {
//         // TODO
// 	// Given a router id r, this function should fill the "location" array (of maximum size "size")
// 	// with information providing the location of the router in the topology. In a Dragonfly network,
// 	// for instance, this can be the array [ group_id, router_id ] where group_id is the id of the
// 	// group in which the router is, and router_id is the id of the router inside this group (as opposed
// 	// to "r" which is its global id). For a torus network, this would be the dimensions.
// 	// If the "size" is sufficient to hold the information, the function should return the size
// 	// effectively used (e.g. 2 in the above example). If however the function did not manage to use
// 	// the provided buffer, it should return -1.
//     printf("DRAGONFLY PLUS CORTEX NOT IMPLEMENTED\n");

//     // const dragonfly_plus_param * params = &all_params[num_params-1];
//     // if(!params)
//     //     return -1;

//     // if(r < 0 || r >= params->total_terminals)
//     //     return -1;

//     // if(size < 2)
//     //     return -1;

//     // int rid = r % params->num_routers;
//     // int gid = r / params->num_routers;
//     // location[0] = gid;
//     // location[1] = rid;
//     // return 2;
// }

// static int dragonfly_plus_get_compute_node_location(void* topo, cn_id_t node, int32_t* location, int size)
// {
//         // TODO
// 	// This function does the same as dragonfly_plus_get_router_location but for a compute node instead
// 	// of a router. E.g., for a dragonfly network, the location could be expressed as the array
// 	// [ group_id, router_id, terminal_id ]
//     printf("DRAGONFLY PLUS CORTEX NOT IMPLEMENTED\n");

//     // const dragonfly_plus_param * params = &all_params[num_params-1];
//     // if(!params)
//     //     return -1;

//     // if(node < 0 || node >= params->total_terminals)
//     //     return -1;

//     // if(size < 3)
//     //     return -1;

//     // int rid = (node / params->num_cn) % params->num_routers;
//     // int rid_global = node / params->num_cn;
//     // int gid = rid_global / params->num_routers;
//     // int lid = node % params->num_cn;

//     // location[0] = gid;
//     // location[1] = rid;
//     // location[2] = lid;

//     // return 3;
// }

// static router_id_t dragonfly_plus_get_router_from_compute_node(void* topo, cn_id_t node) {
//         // TODO
// 	// Given a node id, this function returns the id of the router connected to the node,
// 	// or -1 if the node id is not valid.
//     printf("DRAGONFLY PLUS CORTEX NOT IMPLEMENTED\n");

//         // const dragonfly_plus_param * params = &all_params[num_params-1];
//         // if(!params)
//         //     return -1;

//         // if(node < 0 || node >= params->total_terminals)
//         //     return -1;

//         // router_id_t rid = node / params->num_cn;
//         // return rid;
// }

// static int dragonfly_plus_get_router_compute_node_count(void* topo, router_id_t r) {
// 	// Given the id of a router, returns the number of compute nodes connected to this
// 	// router, or -1 if the router id is not valid.
//     printf("DRAGONFLY PLUS CORTEX NOT IMPLEMENTED\n");

//         // const dragonfly_plus_param * params = &all_params[num_params-1];
//         // if(!params)
//         //     return -1;

//         // if(r < 0 || r >= params->total_routers)
//         //     return -1;

//         // return params->num_cn;
// }

// static void dragonfly_plus_get_router_compute_node_list(void* topo, router_id_t r, cn_id_t* nodes) {
//         // TODO: What if there is an invalid router ID?
// 	// Given the id of a router, fills the "nodes" array with the list of ids of compute nodes
// 	// connected to this router. It is assumed that enough memory has been allocated for the
// 	// "nodes" variable to hold all the ids.
//     printf("DRAGONFLY PLUS CORTEX NOT IMPLEMENTED\n");

//     //   const dragonfly_plus_param * params = &all_params[num_params-1];

//     //   for(int i = 0; i < params->num_cn; i++)
//     //      nodes[i] = r * params->num_cn + i;
// }

// extern "C" {

// cortex_topology dragonfly_plus_cortex_topology = {
// //        .internal =
// 			NULL,
// //		  .get_number_of_routers          =
// 			dragonfly_plus_get_number_of_routers,
// //		  .get_number_of_compute_nodes	  =
// 			dragonfly_plus_get_number_of_compute_nodes,
// //        .get_router_link_bandwidth      =
// 			dragonfly_plus_get_router_link_bandwidth,
// //        .get_compute_node_bandwidth     =
// 			dragonfly_plus_get_compute_node_bandwidth,
// //        .get_router_neighbor_count      =
// 			dragonfly_plus_get_router_neighbor_count,
// //        .get_router_neighbor_list       =
// 			dragonfly_plus_get_router_neighbor_list,
// //        .get_router_location            =
// 			dragonfly_plus_get_router_location,
// //        .get_compute_node_location      =
// 			dragonfly_plus_get_compute_node_location,
// //        .get_router_from_compute_node   =
// 			dragonfly_plus_get_router_from_compute_node,
// //        .get_router_compute_node_count  =
// 			dragonfly_plus_get_router_compute_node_count,
// //        .get_router_compute_node_list   = dragonfly_plus_get_router_compute_node_list,
//             dragonfly_plus_get_router_compute_node_list
// };

// }
// #endif
}
