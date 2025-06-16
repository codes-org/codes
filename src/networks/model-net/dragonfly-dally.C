/*
 * Copyright (C) 2019, UChicago Argonne and co.
 * See LICENSE in top-level directory
 * 
 * Originally written by Misbah Mubarak
 * Updated by Neil McGlohon and Elkin Cruz-Camacho
 *  
 * A 1D specific dragonfly custom model - diverged from dragonfly-custom.C
 * Differs from dragonfly.C in that it allows for the custom features typically found in
 * dragonfly-custom.C.
 * 
 * This was not intended to be a long term solution, but enough changes had been made that merging
 * into dragonfly-custom.C wasn't feasible at the time of creation. Today, there is enough differences
 * in the two models that there is currently no plan to re-merge the two.
 */

#include <ross.h>

#include "codes/jenkins-hash.h"
#include "codes/codes_mapping.h"
#include "codes/codes.h"
#include "codes/model-net-method.h"
#include "codes/model-net-lp.h"
#include "codes/surrogate/init.h"
#include "codes/net/dragonfly-dally.h"
#include "quicklist.h"
#include "sys/file.h"
#include "codes/quickhash.h"
#include "codes/rc-stack.h"
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <errno.h>
#include <sys/stat.h>

#include "codes/network-manager/dragonfly-network-manager.h"
#include "codes/congestion-controller-model.h"

#ifdef ENABLE_CORTEX
#include <cortex/cortex.h>
#include <cortex/topology.h>
#endif

#define DUMP_CONNECTIONS 0
#define PRINT_CONFIG 1
#define DFLY_HASH_TABLE_SIZE 4999
// debugging parameters
#define BW_MONITOR 1
#define T_ID -1
#define TRACK -1
#define TRACK_PKT -1
#define TRACK_MSG -1
#define DEBUG 0
#define DEBUG_QOS 1
#define MAX_STATS 65536
#define SHOW_ADAP_STATS 1
// maximum number of characters allowed to represent the routing algorithm as a string
#define MAX_ROUTING_CHARS 32

#define ROUTER_BW_LOG 0

#define OUTPUT_END_END_LATENCIES 0
#define OUTPUT_PORT_PORT_LATENCIES 0
#define OUTPUT_LATENCY_MODULO 1

#define ADD_NOISE 0

//Routing Defines
//NONMIN_INCLUDE_SOURCE_DEST: Do we allow source and destination groups to be viable choces for indirect group (i.e. do we allow nonminimal routing to sometimes be minimal?)
#define NONMIN_INCLUDE_SOURCE_DEST 0
//End routing defines

#define LP_CONFIG_NM_TERM (model_net_lp_config_names[DRAGONFLY_DALLY])
#define LP_METHOD_NM_TERM (model_net_method_names[DRAGONFLY_DALLY])
#define LP_CONFIG_NM_ROUT (model_net_lp_config_names[DRAGONFLY_DALLY_ROUTER])
#define LP_METHOD_NM_ROUT (model_net_method_names[DRAGONFLY_DALLY_ROUTER])

// If we have configured the network surrogate, then we will collect packet delay data, which is done via the scheduling of an event. This additional event will shift the random generator and thus the same model will behave differently from the start when compared with the one where the surrogate is not setup. If one wants to test both scenarios (with and without the surrogate) and maintain determinism in high-fidelity, one has to enable this option
#define ALWAYS_DETERMINISTIC_NETWORK 0

/* handles terminal and router events like packet generate/send/receive/buffer */
typedef struct terminal_state terminal_state;
typedef struct router_state router_state;

static int max_lvc_src_g = 1;
static int max_lvc_intm_g = 3;
static int min_gvc_src_g = 0;
static int min_gvc_intm_g = 1;

static int max_hops_per_group = 1;
static int max_global_hops_nonminimal = 2;
static int max_global_hops_minimal = 1;

static long num_local_packets_sr = 0;
static long num_local_packets_sg = 0;
static long num_remote_packets = 0;

static long global_stalled_chunk_counter = 0;

#define OUTPUT_SNAPSHOT 1
static int num_snapshots = 0;
tw_stime * snapshot_times;
char snapshot_filename[128];

/* time in nanosecs */
//static int bw_reset_window = 50000000;
static int bw_reset_window = 5000000;

#define indexer3d(_ptr, _x, _y, _z, _maxx, _maxy, _maxz) \
        ((_ptr) + _z * (_maxx * _maxy) + _y * (_maxx) + _x)

#define indexer2d(_ptr, _x, _y, _maxx, _maxy) \
        ((_ptr) + _y * (_maxx) + _x)

using namespace std;

/*MM: Maintains a list of routers connecting the source and destination groups */
static vector< vector< vector<int> > > connectionList;

static DragonflyNetworkManager netMan;

/* Note: Dragonfly Dally doesn't distinguish intra links into colored "types".
   So the type field here is ignored. This will be changed at some point in the
   future but want to provide a script that will allow for easy converting of
   intra-group files to the new format of (src, dest) instead of the current
   (src, dest, type). If we changed this here now, then all pre-existing
   intra-group files will break.

   IntraGroupLink is a struct used to unpack binary data regarding intra group
   connections from the supplied intra-group file. This struct should not be
   utilized anywhere else in the model.
*/
struct IntraGroupLink {
    int src, dest, type;
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
extern cortex_topology dragonfly_dally_cortex_topology;
}
#endif

static long packet_gen = 0, packet_fin = 0;

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
static int router_magic_num = 0;

/* terminal magic number */
static int terminal_magic_num = 0;

static FILE * dragonfly_rtr_bw_log = NULL;
//static FILE * dragonfly_term_bw_log = NULL;

static int sample_bytes_written = 0;
static int sample_rtr_bytes_written = 0;

static char cn_sample_file[MAX_NAME_LENGTH];
static char router_sample_file[MAX_NAME_LENGTH];

// File to store packet latency from terminal-to-terminal
// NOTE: Only non-predicted latencies are saved to file
static FILE * packet_latency_f = NULL;
static void setup_packet_latency_path(char const * const dir_to_save);


// ==== START OF Parameters to tune surrogate mode ====
// 
static bool dally_surrogate_configured = false;
static bool is_dally_surrogate_on = false;
static bool freeze_network_on_switch = false;
static struct packet_latency_predictor * terminal_predictor = NULL;
static void switch_surrogate(void);
static bool is_surrogate_on_fun(void);
static void dragonfly_dally_terminal_highdef_to_surrogate(terminal_state * s, tw_lp * lp, tw_event **);
static void dragonfly_dally_terminal_surrogate_to_highdef(terminal_state * s, tw_lp * lp, tw_event **);
static bool dragonfly_dally_terminal_should_event_be_frozen(tw_lp * lp, tw_event * event);
static bool dragonfly_dally_router_should_event_be_frozen(tw_lp * lp, tw_event * event);
static void dragonfly_dally_terminal_pre_surrogate_switch_event_queue( terminal_state * s, tw_lp * lp, tw_event * event);
static void dragonfly_dally_terminal_reset_predictor(terminal_state * s, tw_lp * lp, void *);
//
// ==== END OF Parameters to tune surrogate mode ====

typedef struct terminal_dally_message_list terminal_dally_message_list;
struct terminal_dally_message_list {
    terminal_dally_message msg;
    char* event_data;
    terminal_dally_message_list *next;
    terminal_dally_message_list *prev;
};

static void init_terminal_dally_message_list(terminal_dally_message_list *thisO, 
    terminal_dally_message *inmsg) {
    thisO->msg = *inmsg;
    thisO->event_data = NULL;
    thisO->next = NULL;
    thisO->prev = NULL;
}

static void delete_terminal_dally_message_list(void *thisO) {
    terminal_dally_message_list* toDel = (terminal_dally_message_list*)thisO;
    if(toDel->event_data != NULL) free(toDel->event_data);
    free(toDel);
}

struct dragonfly_param
{
    // configuration parameters
    int num_routers; /*Number of routers in a group*/
    int num_injection_queues;
    int num_rails; /* Number of rails connecting a single terminal to the network */
    int num_planes; /* Number of router planes in the network */
    double local_bandwidth;/* bandwidth of the router-router channels within a group */
    double global_bandwidth;/* bandwidth of the inter-group router connections */
    double cn_bandwidth;/* bandwidth of the compute node channels connected to routers */
    int num_vcs; /* number of virtual channels */
    int local_vc_size; /* buffer size of the router-router channels */
    int global_vc_size; /* buffer size of the global channels */
    int cn_vc_size; /* buffer size of the compute node channels */
    int chunk_size; /* full-sized packets are broken into smaller chunks.*/
    int packet_size; /* maximum size of a packet, although we have no control over it. It is model-net who is in charge of generating packets of at most this size */
    int global_k_picks; /* k number of connections to select from when doing local adaptive routing */
    int adaptive_threshold; 
    int rail_select; // method by which rails are selected
    int num_parallel_switch_conns;
    // derived parameters
    int num_rails_per_plane;
    int num_routers_per_plane;
    int num_cn;
    int cn_radix;
    int intra_grp_radix;
    int global_radix;
    int num_groups;
    int total_groups;
    int radix;
    int total_routers;
    int total_terminals;
    int num_global_channels;
    int num_qos_levels;
    int * qos_bandwidths;
    double cn_delay;
    double local_delay;
    double global_delay;
    int credit_size;
    double local_credit_delay;
    double global_credit_delay;
    double cn_credit_delay;
    double router_delay;

    int max_hops_notify; //maximum number of hops allowed before notifying via printout

    //Xin: parameters for message counters of apps
    int counting_bool;
    tw_stime counting_start; 
    tw_stime counting_interval; 
    int counting_windows;
    int num_apps;

};

static const dragonfly_param* stored_params;


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
   tw_stime* busy_time_sample;
   tw_stime end_time;
   long fwd_events;
   long rev_events;
};

struct dfly_qhash_entry
{
    struct dfly_hash_key key;
    char * remote_event_data;
    int num_chunks;
    int remaining_packets;
    int remote_event_size;
    struct qhash_head hash_link;
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

// See implementations in dfdally_score_connection()
typedef enum route_scoring_metric_t
{
    ALPHA = 1, //Count queue lengths and pending messages for a port
    BETA, //Expected Hops to Destination * (Count queue lengths and pending messages) for a port
    GAMMA,
    DELTA //count queue lengths and pending messages for a port, biased 2x against nonminimal conns
} route_scoring_metric_t;

/* Enumeration of types of events sent between model LPs */
enum event_t
{
    T_GENERATE=1,
    T_ARRIVE,
    T_SEND,
    T_BUFFER,
    R_SEND,
    R_ARRIVE,
    R_BUFFER,
    R_BANDWIDTH,
    R_BW_HALT,
    T_BANDWIDTH,
    R_SNAPSHOT, //used for timed statistic outputs
    T_NOTIFY,  // used to notify a source or destination terminal about packets status (useful for informing about latency, zombie packet or delete a zombie packet)
    T_ARRIVE_PREDICTED,  // this event is generated by a latency predictor instead of traversing the network
#if ALWAYS_DETERMINISTIC_NETWORK
    T_VACUOUS_EVENT, // nothing happens with this event, it's just ment to be a dummy event that allows us to keep the number of events produced in a simulation the same regardless of whether packet latency is activated (can be safely removed)
#endif /* ALWAYS_DETERMINISTIC_NETWORK */
};

// Types of notifications between terminals
enum notify_t {
    NOTIFY_LATENCY,  // Notifying the source terminal of the total latency to deliver the packet
    NOTIFY_ZOMBIE,   // Notifying the destination terminal of a packet that should be treated as a zombie
};

/* whether the last hop of a packet was global, local or a terminal */
enum last_hop
{
    GLOBAL=1,
    LOCAL,
    TERMINAL,
};

/* Routing Algorithms Implemented:
    Minimal - Guarantees shortest path between terminals, next stop is polled randomly from set of legal next stops
    Non-Minimal - Valiant routing, picks random group, routes there first, then routes to destination. Next stop is poleld randomly from set of legal next stops
    Adaptive - UGAL, not yet implmented TODO: Implement
    Prog-adaptive - PAR
    Prog-adaptive Legacy - Old implementation of PAR (use at own risk)
*/
enum ROUTING_ALGO
{
    MINIMAL = 1,
    NON_MINIMAL,
    ADAPTIVE,
    PROG_ADAPTIVE,
    PROG_ADAPTIVE_LEGACY,
    SMART_MINIMAL,
    SMART_NON_MINIMAL,
    SMART_PROG_ADAPTIVE
};

enum RAIL_SELECTION_ALGO
{
    RAIL_CONGESTION=1,  // Selects rail with minimal injection congestion
    RAIL_PATH,          // Selects the rail that provides minimal path congestion is tie breaker
    RAIL_DEDICATED,      // Selects a specific rail
    RAIL_RAND
};

static char* get_routing_alg_chararray(int routing_alg_int)
{
    char* rt_alg = (char*) calloc(MAX_ROUTING_CHARS, sizeof(char));
    switch (routing_alg_int)
    {
        case MINIMAL:
            strcpy(rt_alg, "MINIMAL");
        break;
        case NON_MINIMAL:
            strcpy(rt_alg, "NON_MINIMAL");
        break;
        case ADAPTIVE:
            strcpy(rt_alg, "ADAPTIVE");
        break;
        case PROG_ADAPTIVE:
            strcpy(rt_alg, "PROG_ADAPTIVE");
        break;
        case PROG_ADAPTIVE_LEGACY:
            strcpy(rt_alg, "PROG_ADAPTIVE_LEGACY");
        break;
        case SMART_PROG_ADAPTIVE:
            strcpy(rt_alg, "SMART_PROG_ADAPTIVE");
        break;
        case SMART_MINIMAL:
            strcpy(rt_alg, "SMART_MINIMAL");
        break;
        case SMART_NON_MINIMAL:
            strcpy(rt_alg, "SMART_NON_MINIMAL");
        break;
        default:
            tw_error(TW_LOC, "Routing Algorithm is UNDEFINED - did you call get_routing_alg_string() before setting the static global variable: 'routing'?");
        break;
    }
    return rt_alg;
}

static bool isRoutingAdaptive(int alg)
{
    if (alg == ADAPTIVE || alg == PROG_ADAPTIVE || alg == PROG_ADAPTIVE_LEGACY || alg == SMART_PROG_ADAPTIVE)
        return true;
    else
        return false;
}

static bool isRoutingSmart(int alg)
{
    if (alg == SMART_MINIMAL || alg == SMART_PROG_ADAPTIVE || alg == SMART_NON_MINIMAL)
        return true;
    else
        return false;    
}

static bool isRoutingMinimal(int alg)
{
    if (alg == MINIMAL || alg == SMART_MINIMAL)
        return true;
    else
        return false;
}

static bool isRoutingNonminimalExplicit(int alg)
{
    if (alg == NON_MINIMAL)
        return true;
    else
        return false;
}

/**
 * Surrogate definitions and data
 */

struct packet_sent {
    struct packet_start start;
    double next_packet_delay; // When the packet is initially sent, this value is -1, when the next packet is sent this value is updated to the actual delay to process the next packet
    void * message_data;  // Yep, we have to save the entire message just because we might need to resend the message when switching to surrogate-mode. It's wasteful but there is no other way
    void * remote_event_data;  // This and the one above have to be freed. This contains the extra information that the message contains
    void * local_data;  // This and the one above have to be freed. This contains the extra information that the message contains
};

struct packet_id {
    uint64_t packet_ID;
    unsigned int dfdally_src_terminal_id;
};
bool operator<(struct packet_id const &lk, struct packet_id const &rk) {
    return lk.packet_ID == rk.packet_ID ? lk.dfdally_src_terminal_id < rk.dfdally_src_terminal_id : lk.packet_ID < rk.packet_ID;
}
bool operator==(struct packet_id const &lk, struct packet_id const &rk) {
    return lk.packet_ID == rk.packet_ID && lk.dfdally_src_terminal_id == rk.dfdally_src_terminal_id;
}
// Some more function declarations
static void notify_dest_lp_of(terminal_state * s, tw_lp * lp, terminal_dally_message * msg, enum notify_t notification);

/* dragonfly compute node data structure */
struct terminal_state
{
    int64_t packet_counter;

    int packet_gen;
    int packet_fin;

    int total_gen_size;

    // Dragonfly specific parameters
    tw_lpid* router_lp; //one per rail
    unsigned int* router_id; //one per rail
    unsigned int terminal_id;

    DragonflyConnectionManager connMan;
    tlc_state *local_congestion_controller;

    int workloads_finished_flag;

    int** vc_occupancy; // vc_occupancies [rail_id][qos_level]
    tw_stime* terminal_available_time; // [rail_id]
    terminal_dally_message_list ***terminal_msgs; //[rail_id][qos_level]
    terminal_dally_message_list ***terminal_msgs_tail; //[rail_id][qos_level]
    int* in_send_loop; // [rail_id]
    struct mn_stats dragonfly_stats_array[CATEGORY_MAX];

    int ** qos_status; //[rail_id][qos_level]
    int ** qos_data; //[rail_id][qos_level]

    int* last_qos_lvl; //[rail_id]
    int is_monitoring_bw;

    struct rc_stack * st;
    struct rc_stack * cc_st;
    int* issueIdle; //[rail_id]
    int** terminal_length; // [rail_id][qos_level]

    const char * anno;
    const dragonfly_param *params;

    struct qhash_table *rank_tbl;
    uint64_t rank_tbl_pop;

    tw_stime   total_time;
    uint64_t total_msg_size;
    double total_hops;
    long finished_msgs;
    long finished_chunks;
    long finished_packets;

    tw_stime* last_buf_full; //[rail_id]
    tw_stime* busy_time; //[rail_id]
    uint64_t* link_traffic; //[rail_id]
    
    unsigned long* total_chunks; //counter for when a packet is sent
    unsigned long* stalled_chunks; //Counter for when a packet cannot be immediately routed due to full VC - [rail_id]

    unsigned long injected_chunks; //counter for chunks injected
    unsigned long ejected_chunks; //counter for chucnks ejected from network

    tw_stime max_latency;
    tw_stime min_latency;

    char output_buf[4096];
    char output_buf2[4096];

    /* For sampling */
    long fin_chunks_sample;
    long data_size_sample;
    double fin_hops_sample;
    tw_stime fin_chunks_time;
    tw_stime *busy_time_sample;

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
    tw_stime *busy_time_ross_sample;
    struct dfly_cn_sample ross_sample;

    // Variables to recover latency of packets sent to other terminals
    // Sent packets (to be populated at by commit handler of packet sender)
    map<uint64_t, struct packet_sent> sent_packets;
    set<uint64_t> is_pending_local_send;
    int64_t last_packet_sent_id;
    // We need the next packet to be injected in the network before feeding the packet info forward (the predictor needs starting time, delay to send next packet and latency)
    struct {
        int64_t packet_ID;
        double travel_end_time;
    } arrival_of_last_packet;
    // received (and not completed, yet) packets. The value associated to a key is the remaining number of "bytes" to receive before the packet is consumed totally. If a packet size == chunk size, this map will never be used/filled
    map<struct packet_id, uint32_t> remaining_sz_packets;

    // Stores the last time in which a packet was processed (time at which a T_GENERATE event was processed)
    double last_in_queue_time;
    // The predictor kicks in on surrogate mode and predicts the time a packet will take to its destination
    void * predictor_data;

    // Zombie events appear when the network traffic is displaced to the future. By then, all packets that were in the network should have been already delievered, thus zombies
    set<struct packet_id> zombies;

    // Variable to save the entire state of the terminal into before switching to surrogate mode. During surrogate-mode, the terminal should not access the state of the network
    terminal_state * frozen_state;
};

struct router_state
{
    unsigned int router_id;
    int group_id;
    int plane_id;
    int op_arr_size;
    int max_arr_size;

    int* global_channel; 

    DragonflyConnectionManager connMan; //manages and organizes connections from this router
    rlc_state *local_congestion_controller;

    tw_stime* next_output_available_time;
    tw_stime* last_buf_full;

    tw_stime* busy_time;
    tw_stime* busy_time_sample;

    unsigned long* stalled_chunks; //Counter for when a packet is put into queued messages instead of routing due to full VC
    unsigned long* total_chunks; //Counter for when a packet is sent - per port

    terminal_dally_message_list ***pending_msgs;
    terminal_dally_message_list ***pending_msgs_tail;
    terminal_dally_message_list ***queued_msgs;
    terminal_dally_message_list ***queued_msgs_tail;
    int *in_send_loop;
    int *queued_count;
    struct rc_stack * st;
    struct rc_stack * cc_st;

    int workloads_finished_flag;

    double* port_bandwidths; //used by CC
    int* vc_max_sizes; //max for vc sizes indexed by port
    int** vc_occupancy;
    int64_t* link_traffic;
    int64_t * link_traffic_sample;

    int is_monitoring_bw;
    int* last_qos_lvl;
    int** qos_status;
    int** qos_data;

    const char * anno;
    const dragonfly_param *params;
    
    int** snapshot_data; //array of x numbers of snapshots of variable size
    char output_buf[4096];

    struct dfly_router_sample * rsamples;
    
    long fwd_events;
    long rev_events;

    //Xin: buffer for output data
    char output_buf5[4096];
    char output_buf6[4096];
    //Xin: msg couters for apps
    tw_stime **agg_busy_time;
    int64_t **agg_link_traffic;

    /* following used for ROSS model-level stats collection */
    tw_stime* busy_time_ross_sample;
    int64_t * link_traffic_ross_sample;
    struct dfly_router_sample ross_rsample;
    tw_stime last_time;
};



/* had to pull some of the ROSS model stats collection stuff up here */
void custom_dally_dragonfly_event_collect(terminal_dally_message *m, tw_lp *lp, char *buffer, int *collect_flag);
void custom_dally_dragonfly_model_stat_collect(terminal_state *s, tw_lp *lp, char *buffer);
void custom_dally_dfly_router_model_stat_collect(router_state *s, tw_lp *lp, char *buffer);
static void ross_dally_dragonfly_rsample_fn(router_state * s, tw_bf * bf, tw_lp * lp, struct dfly_router_sample *sample);
static void ross_dally_dragonfly_rsample_rc_fn(router_state * s, tw_bf * bf, tw_lp * lp, struct dfly_router_sample *sample);
static void ross_dally_dragonfly_sample_fn(terminal_state * s, tw_bf * bf, tw_lp * lp, struct dfly_cn_sample *sample);
static void ross_dally_dragonfly_sample_rc_fn(terminal_state * s, tw_bf * bf, tw_lp * lp, struct dfly_cn_sample *sample);

st_model_types custom_dally_dragonfly_model_types[] = {
    {(ev_trace_f) custom_dally_dragonfly_event_collect,
     sizeof(int),
     (model_stat_f) custom_dally_dragonfly_model_stat_collect,
     sizeof(tw_lpid) + sizeof(long) * 2 + sizeof(double) + sizeof(tw_stime) *2,
     (sample_event_f) ross_dally_dragonfly_sample_fn,
     (sample_revent_f) ross_dally_dragonfly_sample_rc_fn,
     sizeof(struct dfly_cn_sample) } , 
    {(ev_trace_f) custom_dally_dragonfly_event_collect,
     sizeof(int),
     (model_stat_f) custom_dally_dfly_router_model_stat_collect,
     0, //updated in router_dally_init() since it's based on the radix
     (sample_event_f) ross_dally_dragonfly_rsample_fn,
     (sample_revent_f) ross_dally_dragonfly_rsample_rc_fn,
     0 } , //updated in router_dally_init() since it's based on the radix    
    {NULL, 0, NULL, 0, NULL, NULL, 0}
};
/* End of ROSS model stats collection */

/* For ROSS event tracing */
void custom_dally_dragonfly_event_collect(terminal_dally_message *m, tw_lp *lp, char *buffer, int *collect_flag)
{
    (void)lp;
    (void)collect_flag;

    int type = (int) m->type;
    memcpy(buffer, &type, sizeof(type));
}

void custom_dally_dragonfly_model_stat_collect(terminal_state *s, tw_lp *lp, char *buffer)
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

    for(int i = 0; i < s->params->num_rails; i++)
    {
        tmp2 = s->busy_time_ross_sample[i];
        memcpy(&buffer[index], &tmp2, sizeof(tmp2));
        index += sizeof(tmp2);
        s->busy_time_ross_sample[i] = 0;
    }

    return;
}

void custom_dally_dfly_router_model_stat_collect(router_state *s, tw_lp *lp, char *buffer)
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

static const st_model_types  *custom_dally_dragonfly_get_model_types(void)
{
    return(&custom_dally_dragonfly_model_types[0]);
}

static const st_model_types  *custom_dally_dfly_router_get_model_types(void)
{
    return(&custom_dally_dragonfly_model_types[1]);
}

static void custom_dally_dragonfly_register_model_types(st_model_types *base_type)
{
    st_model_type_register(LP_CONFIG_NM_TERM, base_type);
}

static void custom_dally_router_register_model_types(st_model_types *base_type)
{
    st_model_type_register(LP_CONFIG_NM_ROUT, base_type);
}
/*** END of ROSS event tracing additions */

static void ross_dally_dragonfly_rsample_fn(router_state * s, tw_bf * bf, tw_lp * lp, struct dfly_router_sample *sample)
{
    (void)lp;
    (void)bf;

    const dragonfly_param * p = s->params; 
    int i = 0;

    sample->router_id = s->router_id;
    sample->end_time = tw_now(lp);
    sample->fwd_events = s->ross_rsample.fwd_events;
    sample->rev_events = s->ross_rsample.rev_events;
    sample->busy_time = (tw_stime*)((&sample->rev_events) + 1);
    sample->link_traffic_sample = (int64_t*)((&sample->busy_time[0]) + p->radix);

    for(; i < p->radix; i++)
    {
        sample->busy_time[i] = s->ross_rsample.busy_time[i]; 
        sample->link_traffic_sample[i] = s->ross_rsample.link_traffic_sample[i]; 
    }

    /* clear up the current router stats */
    s->ross_rsample.fwd_events = 0;
    s->ross_rsample.rev_events = 0;

    for( i = 0; i < p->radix; i++)
    {
        s->ross_rsample.busy_time[i] = 0;
        s->ross_rsample.link_traffic_sample[i] = 0;
    }
}

static void ross_dally_dragonfly_rsample_rc_fn(router_state * s, tw_bf * bf, tw_lp * lp, struct dfly_router_sample *sample)
{
    (void)lp;
    (void)bf;
    
    const dragonfly_param * p = s->params;
    int i =0;

    for(; i < p->radix; i++)
    {
        s->ross_rsample.busy_time[i] = sample->busy_time[i];
        s->ross_rsample.link_traffic_sample[i] = sample->link_traffic_sample[i];
    }

    s->ross_rsample.fwd_events = sample->fwd_events;
    s->ross_rsample.rev_events = sample->rev_events;
}

static void ross_dally_dragonfly_sample_fn(terminal_state * s, tw_bf * bf, tw_lp * lp, struct dfly_cn_sample *sample)
{
    (void)lp;
    (void)bf;
    
    sample->terminal_id = s->terminal_id;
    sample->fin_chunks_sample = s->ross_sample.fin_chunks_sample;
    sample->data_size_sample = s->ross_sample.data_size_sample;
    sample->fin_hops_sample = s->ross_sample.fin_hops_sample;
    sample->fin_chunks_time = s->ross_sample.fin_chunks_time;
    sample->busy_time_sample = (tw_stime*)((&sample->busy_time_sample[0] + s->params->num_rails));
    for(int i = 0; i < s->params->num_rails; i++)
        sample->busy_time_sample[i] = s->ross_sample.busy_time_sample[i];
    sample->end_time = tw_now(lp);
    sample->fwd_events = s->ross_sample.fwd_events;
    sample->rev_events = s->ross_sample.rev_events;

    s->ross_sample.fin_chunks_sample = 0;
    s->ross_sample.data_size_sample = 0;
    s->ross_sample.fin_hops_sample = 0;
    s->ross_sample.fwd_events = 0;
    s->ross_sample.rev_events = 0;
    s->ross_sample.fin_chunks_time = 0;
    for(int i = 0; i < s->params->num_rails; i++)
        s->ross_sample.busy_time_sample[i] = 0;
}

static void ross_dally_dragonfly_sample_rc_fn(terminal_state * s, tw_bf * bf, tw_lp * lp, struct dfly_cn_sample *sample)
{
    (void)lp;
    (void)bf;

    for(int i = 0; i < s->params->num_rails; i++)
        s->ross_sample.busy_time_sample[i] = sample->busy_time_sample[i];
    s->ross_sample.fin_chunks_time = sample->fin_chunks_time;
    s->ross_sample.fin_hops_sample = sample->fin_hops_sample;
    s->ross_sample.data_size_sample = sample->data_size_sample;
    s->ross_sample.fin_chunks_sample = sample->fin_chunks_sample;
    s->ross_sample.fwd_events = sample->fwd_events;
    s->ross_sample.rev_events = sample->rev_events;
}

void dragonfly_dally_rsample_init(router_state * s,
        tw_lp * lp)
{
    (void)lp;
    int i = 0;
    const dragonfly_param * p = s->params;

    assert(p->radix);

    s->max_arr_size = MAX_STATS;
    s->rsamples = (struct dfly_router_sample*)calloc(MAX_STATS, sizeof(struct dfly_router_sample)); 
    for(; i < s->max_arr_size; i++)
    {
        s->rsamples[i].busy_time = (tw_stime*)calloc(p->radix, sizeof(tw_stime)); 
        s->rsamples[i].link_traffic_sample = (int64_t*)calloc(p->radix, sizeof(int64_t));
    }
}

void dragonfly_dally_rsample_rc_fn(router_state * s,
        tw_bf * bf,
        terminal_dally_message * msg, 
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

void dragonfly_dally_rsample_fn(router_state * s,
        tw_bf * bf,
        terminal_dally_message * msg, 
        tw_lp * lp)
{
    (void)bf;
    (void)lp;
    (void)msg;

    const dragonfly_param * p = s->params; 

    if(s->op_arr_size >= s->max_arr_size) 
    {
        struct dfly_router_sample * tmp = (dfly_router_sample *)calloc((MAX_STATS + s->max_arr_size), sizeof(struct dfly_router_sample));
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

//TODO redo this
void dragonfly_dally_rsample_fin(router_state * s,
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
        // fprintf(fp, "\n\nOrdering of links \n%d green (router-router same row) channels \n %d black (router-router same column) channels \n %d global (router-router remote group)"
        //         " channels \n %d terminal channels", p->num_router_cols * p->num_row_chans, p->num_router_rows * p->num_col_chans, p->num_global_channels, p->num_cn);
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
void dragonfly_dally_sample_init(terminal_state * s,
        tw_lp * lp)
{
    (void)lp;
    s->fin_chunks_sample = 0;
    s->data_size_sample = 0;
    s->fin_hops_sample = 0;
    s->fin_chunks_time = 0;
    for(int i = 0; i < s->params->num_rails; i++)
        s->busy_time_sample[i] = 0;

    s->op_arr_size = 0;
    s->max_arr_size = MAX_STATS;

    s->sample_stat = (dfly_cn_sample *)calloc(MAX_STATS, sizeof(struct dfly_cn_sample));
    for(int i = 0; i < s->max_arr_size; i++)
    {
        s->sample_stat[i].busy_time_sample = (tw_stime*)calloc(s->params->num_rails, sizeof(tw_stime)); 
    }
    
}
void dragonfly_dally_sample_rc_fn(terminal_state * s,
        tw_bf * bf,
        terminal_dally_message * msg, 
        tw_lp * lp)
{
    (void)lp;
    (void)bf;
    (void)msg;

    s->op_arr_size--;
    int cur_indx = s->op_arr_size;
    struct dfly_cn_sample stat = s->sample_stat[cur_indx];
    for(int i = 0; i < s->params->num_rails; i++)
        s->busy_time_sample[i] = stat.busy_time_sample[i];
    s->fin_chunks_time = stat.fin_chunks_time;
    s->fin_hops_sample = stat.fin_hops_sample;
    s->data_size_sample = stat.data_size_sample;
    s->fin_chunks_sample = stat.fin_chunks_sample;
    s->fwd_events = stat.fwd_events;
    s->rev_events = stat.rev_events;

    for(int i = 0; i < s->params->num_rails; i++)
        stat.busy_time_sample[i] = 0;
    stat.fin_chunks_time = 0;
    stat.fin_hops_sample = 0;
    stat.data_size_sample = 0;
    stat.fin_chunks_sample = 0;
    stat.end_time = 0;
    stat.terminal_id = 0;
    stat.fwd_events = 0;
    stat.rev_events = 0;
}

void dragonfly_dally_sample_fn(terminal_state * s,
        tw_bf * bf,
        terminal_dally_message * msg,
        tw_lp * lp)
{
    (void)lp;
    (void)msg;
    (void)bf;
    
    if(s->op_arr_size >= s->max_arr_size)
    {
        /* In the worst case, copy array to a new memory location, its very
         * expensive operation though */
        struct dfly_cn_sample * tmp = (dfly_cn_sample *)calloc((MAX_STATS + s->max_arr_size), sizeof(struct dfly_cn_sample));
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
    for(int i = 0; i < s->params->num_rails; i++)
        s->sample_stat[cur_indx].busy_time_sample[i] = s->busy_time_sample[i];
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
    for(int i = 0; i < s->params->num_rails; i++)
        s->busy_time_sample[i] = 0;
}

void dragonfly_dally_sample_fin(terminal_state * s,
        tw_lp * lp)
{
    (void)lp;
 
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

static short routing = MINIMAL;
static short scoring = ALPHA;

/*Routing Implementation Declarations*/
static Connection dfdally_minimal_routing(router_state *s, tw_bf *bf, terminal_dally_message *msg, tw_lp *lp, int fdest_router_id);
static Connection dfdally_nonminimal_routing(router_state *s, tw_bf *bf, terminal_dally_message *msg, tw_lp *lp, int fdest_router_id);
static Connection dfdally_prog_adaptive_routing(router_state *s, tw_bf *bf, terminal_dally_message *msg, tw_lp *lp, int fdest_router_id);
static Connection dfdally_prog_adaptive_legacy_routing(router_state *s, tw_bf *bf, terminal_dally_message *msg, tw_lp *lp, int fdest_router_id);
static Connection dfdally_smart_minimal_routing(router_state *s, tw_bf *bf, terminal_dally_message *msg, tw_lp *lp, int fdest_router_id);
static Connection dfdally_smart_prog_adaptive_routing(router_state *s, tw_bf *bf, terminal_dally_message *msg, tw_lp *lp, int fdest_router_id);
static Connection dfdally_smart_minimal_routing(router_state *s, tw_bf *bf, terminal_dally_message *msg, tw_lp *lp, int fdest_router_id);
static Connection dfdally_smart_nonminimal_routing(router_state *s, tw_bf *bf, terminal_dally_message *msg, tw_lp *lp, int fdest_router_id);

/*Routing Helper Declarations*/
static void dfdally_select_intermediate_group(router_state *s, tw_bf *bf, terminal_dally_message *msg, tw_lp *lp, int fdest_router_id);
static vector< Connection > get_legal_minimal_stops(router_state *s, tw_bf *bf, terminal_dally_message *msg, tw_lp *lp, int fdest_router_id);
static set< Connection> get_smart_legal_minimal_stops(router_state *s, tw_bf *bf, terminal_dally_message *msg, tw_lp *lp, int fdest_router_id, int max_global_hops_in_path);

static tw_stime         dragonfly_total_time = 0;
static tw_stime         dragonfly_max_latency = 0;


static long long       total_hops = 0;
static long long       N_finished_packets = 0;
static long long       total_msg_sz = 0;
static long long       N_finished_msgs = 0;
static long long       N_finished_chunks = 0;

static tw_stime gen_noise(tw_lp *lp, short* rng_counter)
{
#if ADD_NOISE == 1
    tw_stime noise = tw_rand_unif(lp->rng);
    (*rng_counter)++;
    return noise;
#else
    (void) lp;
    (void) rng_counter;
    return 0;
#endif
}

/* convert ns to seconds */
static tw_stime ns_to_s(tw_stime ns)
{
    return(ns / (1000.0 * 1000.0 * 1000.0));
}

static double bytes_to_gigabytes(double bytes)
{
    return bytes / (double) (1024 * 1024 * 1024);
}
static int dragonfly_rank_hash_compare(
        void *key, struct qhash_head *link)
{
    struct dfly_hash_key *message_key = (struct dfly_hash_key *)key;
    struct dfly_qhash_entry *tmp = NULL;

    tmp = qhash_entry(link, struct dfly_qhash_entry, hash_link);
    
    if (tmp->key.message_id == message_key->message_id && tmp->key.sender_id == message_key->sender_id)
        return 1;

    return 0;
}
static int dragonfly_hash_func(void *k, int table_size)
{
    struct dfly_hash_key *tmp = (struct dfly_hash_key *)k;
    uint32_t pc = 0, pb = 0;	
    bj_hashlittle2(tmp, sizeof(*tmp), &pc, &pb);
    return (int)(pc % (table_size - 1));
    /*uint64_t key = (~tmp->message_id) + (tmp->message_id << 18);
    key = key * 21;
    key = ~key ^ (tmp->sender_id >> 4);
    key = key * tmp->sender_id; 
    return (int)(key & (table_size - 1));*/
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
int dragonfly_dally_get_msg_sz(void)
{
    return sizeof(terminal_dally_message);
}

static void free_tmp(void * ptr)
{
    struct dfly_qhash_entry * dfly = (dfly_qhash_entry *)ptr; 
    if(dfly->remote_event_data)
        free(dfly->remote_event_data);
   
    if(dfly)
        free(dfly);
}

static int dfdally_get_assigned_router_id_from_terminal(const dragonfly_param *params, int term_gid, int rail_id)
{
    int num_planes = params->num_planes;
    int num_rails = params->num_rails;

    int total_routers = params->total_routers;
    //int total_terminals = params->total_terminals;
    int num_cn_per_router = params->num_cn;

    if(num_planes == 1) //then all rails go to the same router //TODO: this could change - could be cool!
    {
        if(num_rails == 1) //default behavior
            return term_gid / num_cn_per_router; 
        else
        {
            return term_gid / num_cn_per_router; // all rails go to same router - perhaps change this
        }
    }
    else
    {
        int routers_per_plane = total_routers / num_planes;
        if(num_planes == num_rails)
        {
            return (term_gid / num_cn_per_router) + (rail_id * routers_per_plane);
        }
        // NOTE(helq): The compiler has been bothering me about the lack of a
        // return statement here, so I added a message to something that
        // (hopefully) will never happen.
        else
        {
            tw_error(TW_LOC, "Error: this should have never happened. We couldn't "
                    "figure out to which router does a terminal belong to :S");
            return -1;
        }
    }
    
}

static int dfdally_score_connection(router_state *s, tw_bf *bf, terminal_dally_message *msg, tw_lp *lp, Connection conn, conn_minimality_t c_minimality)
{
    int score = 0;
    int port = conn.port;

    if (port == -1) {
        return INT_MAX;
    }

    switch (scoring) {
        case ALPHA: //considers vc occupancy and queued count only
            for(int k=0; k < s->params->num_vcs; k++)
            {
                score += s->vc_occupancy[port][k];
            }
            score += s->queued_count[port];
            break;
        case BETA: //considers vc occupancy and queued count multiplied by the number of minimal hops to destination from the potential next stop
            tw_error(TW_LOC, "Beta scoring not implemented");
            break;
        case GAMMA: //delta scoring but higher is better
            tw_error(TW_LOC, "Gamma scoring not implemented");
            break;
        case DELTA: //alpha but biased 2:1 toward minimal
            for(int k=0; k < s->params->num_vcs; k++)
            {
                score += s->vc_occupancy[port][k];
            }
            score += s->queued_count[port];

            if (c_minimality != C_MIN)
                score = score * 2;
            break;
        default:
            tw_error(TW_LOC, "Unsupported Scoring Protocol Error\n");
    }
    return score;
}

//Now returns random selection from tied best connections.
static Connection get_absolute_best_connection_from_conns(router_state *s, tw_bf *bf, terminal_dally_message *msg, tw_lp *lp, vector< Connection > conns)
{
    if (conns.size() == 0) { //passed no connections to this but we got to return something - return negative filled conn to force a break if not caught
        Connection bad_conn;
        bad_conn.src_gid = -1;
        bad_conn.port = -1;
        return bad_conn;
    }
    if (conns.size() == 1) { //no need to compare singular connection
        return conns[0];
    }

    int num_to_compare = conns.size();
    int scores[num_to_compare];
    vector < Connection > best_conns;
    int best_score = INT_MAX;

    for(int i = 0; i < num_to_compare; i++)
    {
        scores[i] = dfdally_score_connection(s, bf, msg, lp, conns[i], C_MIN);
        if (scores[i] <= best_score) {
            if (scores[i] < best_score) {
                best_score = scores[i];
                best_conns.clear();
                best_conns.push_back(conns[i]);
            }
            else {
                best_conns.push_back(conns[i]);
            }         
        }
    }

    assert(best_conns.size() > 0);
    
    msg->num_rngs++;
    return best_conns[tw_rand_integer(lp->rng, 0, best_conns.size()-1)];
}

//Now returns random selection from tied best connections.
static Connection get_absolute_best_connection_from_conn_set(router_state *s, tw_bf *bf, terminal_dally_message *msg, tw_lp *lp, set< Connection > conns)
{
    if (conns.size() == 0) { //passed no connections to this but we got to return something - return negative filled conn to force a break if not caught
        Connection bad_conn;
        bad_conn.src_gid = -1;
        bad_conn.port = -1;
        return bad_conn;
    }
    if (conns.size() == 1) { //no need to compare singular connection
        return (*(conns.begin()));
    }

    int num_to_compare = conns.size();
    int scores[num_to_compare];
    vector < Connection > best_conns;
    int best_score = INT_MAX;

    set<Connection>::iterator it;
    int count = 0;
    for(it = conns.begin(); it != conns.end(); it++)
    {
        scores[count] = dfdally_score_connection(s, bf, msg, lp, *it, C_MIN);
        if (scores[count] < best_score) {
            best_score = scores[count];
            best_conns.clear();
            best_conns.push_back(*it);
        }
        else if (scores[count] == best_score)
        {
            best_conns.push_back(*it);
        }
        count++;

    }

    assert(best_conns.size() > 0);
    
    msg->num_rngs++;
    return best_conns[tw_rand_integer(lp->rng, 0, best_conns.size()-1)];
}

// note that this is somewhat expensive the larger k is in comparison to the total possible
// consider an optimization to implement an efficient shuffle to poll k random sampling instead
// consider an optimization for the default of 2
static Connection dfdally_get_best_from_k_connection_set(router_state *s, tw_bf *bf, terminal_dally_message *msg, tw_lp *lp, set< Connection > conns, int k)
{
    if (conns.size() == 0)
    {
        Connection bad_conn;
        bad_conn.src_gid = -1;
        bad_conn.port = -1;
        return bad_conn;
    }
    if (conns.size() == 1)
    {
        return *(conns.begin());
    }

    vector<Connection> k_conns;
    set<Connection>::iterator it = conns.begin();
    for(int i = 0; i < k; i++)
    {
        int offset = tw_rand_integer(lp->rng, 0, conns.size()-1);
        msg->num_rngs++;
        advance(it, offset);
        k_conns.push_back(*it);
        conns.erase(it);
        it = conns.begin();
    }
    return get_absolute_best_connection_from_conns(s, bf, msg, lp, k_conns);
}

// This is not the most efficient way to do things as k approaches the size(conns).
// For low k it's more efficient than doing a full shuffle to sample a few random indices, though.
static vector< Connection > dfdally_poll_k_connections(router_state *s, tw_bf *bf, terminal_dally_message *msg, tw_lp *lp, vector< Connection > conns, int k)
{
    vector< Connection > k_conns;
    if (conns.size() == 0)
    {
        return k_conns;
    }

    if (conns.size() == 1)
    {
        k_conns.push_back(conns[0]);
        return k_conns;
    }

    if (k == 2) { //This is the default and so let's make a cheaper optimization for it
        msg->num_rngs += 2;

        int rand_sel_1, rand_sel_2, rand_sel_2_offset;
        rand_sel_1 = tw_rand_integer(lp->rng, 0, conns.size()-1);
        rand_sel_2_offset = tw_rand_integer(lp->rng, 1, conns.size()-1);
        rand_sel_2 = (rand_sel_1 + rand_sel_2_offset) % conns.size();

        k_conns.push_back(conns[rand_sel_1]);
        k_conns.push_back(conns[rand_sel_2]);

        return k_conns;
    }
    // if (k > conns.size())
    //     tw_error(TW_LOC, "Attempted to poll k random connections but k (%d) is greater than number of connections (%d)",k,conns.size());

    // create set of unique random k indicies
    int last_sel = 0;
    set< int > rand_sels;
    for (int i = 0; i < k; i++)
    {
        int rand_int = tw_rand_integer(lp->rng, 0, (conns.size() - 1) - rand_sels.size());
        int attempt_offset = (last_sel + rand_int) % conns.size(); //get a hopefully unused index - this method of sampling without replacement results in only about
        while (rand_sels.count(attempt_offset) != 0) //increment till we find an unused index
        {
            attempt_offset = (attempt_offset + 1) % conns.size();
        }
        rand_sels.insert(attempt_offset);
        last_sel = attempt_offset;
    }
    msg->num_rngs += k; // we only used the rng k times

    // use random k set to create vector of k connections
    for(set<int>:: iterator it = rand_sels.begin() ; it != rand_sels.end() ; it++)
    {
        k_conns.push_back(conns[*it]);
    }

    return k_conns;
}

// note that this is somewhat expensive the larger k is in comparison to the total possible
// consider an optimization to implement an efficient shuffle to poll k random sampling instead
// consider an optimization for the default of 2
static Connection dfdally_get_best_from_k_connections(router_state *s, tw_bf *bf, terminal_dally_message *msg, tw_lp *lp, vector< Connection > conns, int k)
{
    vector< Connection > k_conns = dfdally_poll_k_connections(s, bf, msg, lp, conns, k);
    return get_absolute_best_connection_from_conns(s, bf, msg, lp, k_conns);
}

static void append_to_terminal_dally_message_list(  
        terminal_dally_message_list ** thisq,
        terminal_dally_message_list ** thistail,
        int index, 
        terminal_dally_message_list *msg) 
{
//    printf("\n msg id %d ", msg->msg.packet_ID);
    if (thisq[index] == NULL) {
        thisq[index] = msg;
    } 
    else {
        assert(thistail[index] != NULL);
        thistail[index]->next = msg;
        msg->prev = thistail[index];
    } 
    thistail[index] = msg;
//    printf("\n done adding %d ", msg->msg.packet_ID);
}

static void prepend_to_terminal_dally_message_list(  
        terminal_dally_message_list ** thisq,
        terminal_dally_message_list ** thistail,
        int index, 
        terminal_dally_message_list *msg) 
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

static terminal_dally_message_list* return_head(
        terminal_dally_message_list ** thisq,
        terminal_dally_message_list ** thistail,
        int index) 
{
    terminal_dally_message_list *head = thisq[index];
    if (head != NULL) {
        thisq[index] = head->next;
        if(head->next != NULL) {
            head->next->prev = NULL;
            head->next = NULL;
        } 
        else {
            thistail[index] = NULL;
        }
    }
    return head;
}

static terminal_dally_message_list* return_tail(
        terminal_dally_message_list ** thisq,
        terminal_dally_message_list ** thistail,
        int index) 
{
    terminal_dally_message_list *tail = thistail[index];
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

// Copies a list and returns the tail
static terminal_dally_message_list * copy_terminal_dally_message_list(terminal_dally_message_list ** into_thisq, terminal_dally_message_list const * from_thisq) {
    if (from_thisq == NULL) {
        *into_thisq = NULL;
        return NULL;
    }

    terminal_dally_message_list const * from_head = from_thisq;
    terminal_dally_message_list * prev = NULL;
    while(from_head != NULL) {
        terminal_dally_message_list * copy_head = (terminal_dally_message_list *) malloc(sizeof(terminal_dally_message_list));

        //copy_head->msg = from_head->msg;
        memcpy(copy_head, from_head, sizeof(terminal_dally_message_list));
        copy_head->prev = prev;

        if (from_head->event_data != NULL) {
            int const message_size = from_head->msg.remote_event_size_bytes + from_head->msg.local_event_size_bytes;
            assert(message_size > 0);
            copy_head->event_data = (char *) malloc(message_size);
            memcpy(copy_head->event_data, from_head->event_data, message_size);
        }

        if (prev == NULL) {
            *into_thisq = copy_head;
        } else {
            prev->next = copy_head;
        }

        prev = copy_head;
        from_head = from_head->next;
    }
    prev->next = NULL;

    return prev;
}

static void clean_terminal_dally_message_list(terminal_dally_message_list * thisq) {
    if (thisq == NULL) {
        return;
    }

    terminal_dally_message_list * prev = thisq;
    terminal_dally_message_list * head = prev->next;
    free(prev->event_data);
    while (head != NULL) {
        free(head->event_data);
        free(prev);
        prev = head;
        head = head->next;
    }
    free(prev);
}

static bool check_terminal_dally_message_list(terminal_dally_message_list * before, terminal_dally_message_list * after) {
    bool is_same = true;

    terminal_dally_message_list * head_before = before;
    terminal_dally_message_list * head_after = after;
    while (head_before != NULL && head_after != NULL) {
        is_same &= check_terminal_dally_message(&head_before->msg, &head_after->msg);
        is_same &= (head_before->event_data == NULL) == (head_after->event_data == NULL);

        int const message_size = head_before->msg.remote_event_size_bytes + head_before->msg.local_event_size_bytes;
        int const message_size_after = head_after->msg.remote_event_size_bytes + head_after->msg.local_event_size_bytes;
        is_same &= message_size == message_size_after;

        if (is_same && head_before->event_data != NULL) {
            assert(message_size > 0);

            is_same &= !memcmp(head_before->event_data, head_after->event_data, message_size);
        }

        head_before = head_before->next;
        head_after = head_after->next;
    }

    if (head_before != NULL || head_after != NULL) {
        is_same = false; // at least one of them is longer than the other
    }

    return is_same;
}

static void print_terminal_dally_message_list(FILE * out, char const * prefix, terminal_state * ns, terminal_dally_message_list * thisq) {
    if (thisq == NULL) {
        return;
    }

    char addprefix_2[] = " | | ";
    int len_subprefix = snprintf(NULL, 0, "%s%s", prefix, addprefix_2) + 1;
    char * subprefix = (char *) malloc(len_subprefix * sizeof(char));
    snprintf(subprefix, len_subprefix, "%s%s", prefix, addprefix_2);

    terminal_dally_message_list * head = thisq;
    while (head != NULL) {
        fprintf(out, "%s{\n", prefix);
        fprintf(out, "%s | msg:\n", prefix);
        print_terminal_dally_message(out, subprefix, ns, &head->msg);
        fprintf(out, "%s | event_data = %p\n", prefix, head->event_data);
        int const message_size = head->msg.remote_event_size_bytes + head->msg.local_event_size_bytes;
        if (head->event_data != NULL) {
            assert(message_size > 0);
            tw_fprint_binary_array(out, subprefix, head->event_data, message_size);
        }
        fprintf(out, "%s},\n", prefix);
        head = head->next;
    }

    free(subprefix);
}


static tw_stime* buff_time_storage_create(terminal_state *s)
{
    tw_stime* storage = (tw_stime*)malloc(s->params->num_rails * sizeof(tw_stime));
    return storage;
}

static void buff_time_storage_delete(void * ptr)
{
    if (ptr)
        free(ptr);
}

static int* int_storage_create(terminal_state *s)
{
    int* storage = (int*)malloc(s->params->num_rails * sizeof(int));
    return storage;
}

static void int_storage_delete(void * ptr)
{
    if (ptr)
        free(ptr);
}

void dragonfly_print_params(const dragonfly_param *p, FILE * st)
{
    if(!st)
        st = stdout;

    fprintf(st,"\n------------------ Dragonfly Dally Parameters ---------\n");
    fprintf(st,"\tnum_routers =            %d\n",p->num_routers);
    fprintf(st,"\tlocal_bandwidth =        %.2f\n",p->local_bandwidth);
    fprintf(st,"\tglobal_bandwidth =       %.2f\n",p->global_bandwidth);
    fprintf(st,"\tcn_bandwidth =           %.2f\n",p->cn_bandwidth);
    fprintf(st,"\tnum_vcs =                %d\n",p->num_vcs);
    fprintf(st,"\tnum_qos_levels =         %d\n",p->num_qos_levels);
    fprintf(st,"\tlocal_vc_size =          %d\n",p->local_vc_size);
    fprintf(st,"\tglobal_vc_size =         %d\n",p->global_vc_size);
    fprintf(st,"\tcn_vc_size =             %d\n",p->cn_vc_size);
    fprintf(st,"\tchunk_size =             %d\n",p->chunk_size);
    fprintf(st,"\tnum_cn =                 %d\n",p->num_cn);
    fprintf(st,"\tcn_radix =               %d\n",p->cn_radix);
    fprintf(st,"\tintra_grp_radix =        %d\n",p->intra_grp_radix);
    fprintf(st,"\tnum_groups =             %d\n",p->num_groups);
    fprintf(st,"\ttotal_groups =           %d\n",p->total_groups);
    fprintf(st,"\tvirtual radix =          %d\n",p->radix);
    fprintf(st,"\ttotal_routers =          %d\n",p->total_routers);
    fprintf(st,"\ttotal_terminals =        %d\n",p->total_terminals);
    fprintf(st,"\tnum_global_channels =    %d\n",p->num_global_channels);
    fprintf(st,"\tnum_injection_queues =   %d\n",p->num_injection_queues);
    fprintf(st,"\tnum_rails =              %d\n",p->num_rails);
    fprintf(st,"\tnum_planes =             %d\n",p->num_planes);
    fprintf(st,"\tcn_delay =               %.2f\n",p->cn_delay);
    fprintf(st,"\tlocal_delay =            %.2f\n",p->local_delay);
    fprintf(st,"\tglobal_delay =           %.2f\n",p->global_delay);
    fprintf(st,"\tlocal credit_delay =     %.2f\n",p->local_credit_delay);
    fprintf(st,"\tglobal credit_delay =    %.2f\n",p->global_credit_delay);
    fprintf(st,"\tcn credit_delay =        %.2f\n",p->cn_credit_delay);
    fprintf(st,"\trouter_delay =           %.2f\n",p->router_delay);
    fprintf(st,"\trouting =                %s\n",get_routing_alg_chararray(routing));
    fprintf(st,"\tadaptive_threshold =     %d\n",p->adaptive_threshold);
    fprintf(st,"\tmax hops notification =  %d\n",p->max_hops_notify);
    fprintf(st,"------------------------------------------------------\n\n");
}

static void dragonfly_read_config(const char * anno, dragonfly_param *params)
{
    /*Adding init for router magic number*/
    uint32_t h1 = 0, h2 = 0; 
    bj_hashlittle2(LP_METHOD_NM_ROUT, strlen(LP_METHOD_NM_ROUT), &h1, &h2);
    router_magic_num = h1 + h2;
    
    bj_hashlittle2(LP_METHOD_NM_TERM, strlen(LP_METHOD_NM_TERM), &h1, &h2);
    terminal_magic_num = h1 + h2;
    
    // shorthand
    dragonfly_param *p = params;
    int myRank;
    MPI_Comm_rank(MPI_COMM_CODES, &myRank);

    int rc = configuration_get_value_int(&config, "PARAMS", "local_vc_size", anno, &p->local_vc_size);
    if(rc) {
        p->local_vc_size = 1024;
        if(!myRank)
            fprintf(stderr, "Buffer size of local channels not specified, setting to %d\n", p->local_vc_size);
    }

    rc = configuration_get_value_int(&config, "PARAMS", "global_vc_size", anno, &p->global_vc_size);
    if(rc) {
        p->global_vc_size = 2048;
        if(!myRank)
            fprintf(stderr, "Buffer size of global channels not specified, setting to %d\n", p->global_vc_size);
    }

    rc = configuration_get_value_int(&config, "PARAMS", "cn_vc_size", anno, &p->cn_vc_size);
    if(rc) {
        p->cn_vc_size = 1024;
        if(!myRank)
            fprintf(stderr, "Buffer size of compute node channels not specified, setting to %d\n", p->cn_vc_size);
    }

    rc = configuration_get_value_int(&config, "PARAMS", "chunk_size", anno, &p->chunk_size);
    if(rc) {
        p->chunk_size = 512;
        if(!myRank)
            fprintf(stderr, "Chunk size for packets is specified, setting to %d\n", p->chunk_size);
    }

    rc = configuration_get_value_int(&config, "PARAMS", "packet_size", anno, &p->packet_size);
    if(rc) {
        p->chunk_size = 512;
        if(!myRank)
            fprintf(stderr, "Packet size not specificied, it is assumed to be %d\n", p->packet_size);
    }

    rc = configuration_get_value_double(&config, "PARAMS", "local_bandwidth", anno, &p->local_bandwidth);
    if(rc) {
        p->local_bandwidth = 5.25;
        if(!myRank)
            fprintf(stderr, "Bandwidth of local channels not specified, setting to %lf\n", p->local_bandwidth);
    }

    rc = configuration_get_value_double(&config, "PARAMS", "global_bandwidth", anno, &p->global_bandwidth);
    if(rc) {
        p->global_bandwidth = 4.7;
        if(!myRank)
            fprintf(stderr, "Bandwidth of global channels not specified, setting to %lf\n", p->global_bandwidth);
    }

    rc = configuration_get_value_double(&config, "PARAMS", "cn_bandwidth", anno, &p->cn_bandwidth);
    if(rc) {
        p->cn_bandwidth = 5.25;
        if(!myRank)
            fprintf(stderr, "Bandwidth of compute node channels not specified, setting to %lf\n", p->cn_bandwidth);
    }

    rc = configuration_get_value_double(&config, "PARAMS", "router_delay", anno,
            &p->router_delay);
    if(rc) {
        p->router_delay = 100;
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
                tw_error(TW_LOC, "\n Invalid bandwidth levels");
            }
            i++;
            token = strtok(NULL,",");
        }
        assert(total_bw <= 100);
    }
    else
        p->qos_bandwidths[0] = 100;

    rc = configuration_get_value_int(&config, "PARAMS", "adaptive_threshold", anno, &p->adaptive_threshold);
    if (rc) {
        if(!myRank)
            fprintf(stderr, "Adaptive Minimal Routing Threshold not specified: setting to default = 0. (Will consider minimal and nonminimal routes based on scoring metric alone)\n");
        p->adaptive_threshold = 0;
    }

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
    else if (strcmp(routing_str, "prog-adaptive-legacy") == 0)
	    routing = PROG_ADAPTIVE_LEGACY;
    else if (strcmp(routing_str, "smart-prog-adaptive") == 0)
        routing = SMART_PROG_ADAPTIVE;
    else if (strcmp(routing_str, "smart-minimal") == 0)
        routing = SMART_MINIMAL;
    else if (strcmp(routing_str, "smart-nonminimal") == 0)
        routing = SMART_NON_MINIMAL;
    else
    {
        if(!myRank)
            fprintf(stderr, "No routing protocol specified, setting to minimal routing\n");
        routing = MINIMAL;
    }

    rc = configuration_get_value_int(&config, "PARAMS", "num_injection_queues", anno, &p->num_injection_queues);
    if(rc)
        p->num_injection_queues = 1;

    p->num_rails = 1;
    rc = configuration_get_value_int(&config, "PARAMS", "num_rails", anno, &p->num_rails);
    if (!rc) {
        if (!myRank)
            printf("num_rails set to %d\n",p->num_rails);
    }

    p->num_planes = 1;
    rc = configuration_get_value_int(&config, "PARAMS", "num_planes", anno, &p->num_planes);
    if (!rc) {
        if (!myRank)
            printf("num_planes set to %d\n",p->num_planes);
        // tw_error(TW_LOC, "Multi Planar Dragonfly not implemented yet\n");
    }

    p->num_rails_per_plane = p->num_rails / p->num_planes;
    if (p->num_rails % p->num_planes != 0)
        tw_error(TW_LOC, "Number of rails not evenly divisible by number of planes!\n");

    char rail_select_str[MAX_NAME_LENGTH] = {'\0'};
    rc = configuration_get_value(&config, "PARAMS", "rail_select", anno, rail_select_str,
            MAX_NAME_LENGTH);
    if(strcmp(rail_select_str, "dedicated") == 0)
        p->rail_select = RAIL_DEDICATED;
    else if(strcmp(rail_select_str, "congestion")==0)
        p->rail_select = RAIL_CONGESTION;
    else if(strcmp(rail_select_str, "path")==0)
        p->rail_select = RAIL_PATH;
    else if(strcmp(rail_select_str, "rand")==0)
        p->rail_select = RAIL_RAND;
    else {
        p->rail_select = RAIL_DEDICATED;
    }
    if (p->num_rails == 1)
        p->rail_select = RAIL_DEDICATED;
    if(!myRank) printf("Dragonfly rail selection is %d\n", p->rail_select);

    rc = configuration_get_value_int(&config, "PARAMS", "global_k_picks", anno, &p->global_k_picks);
    if(rc) {
        p->global_k_picks = 2;
        if(!myRank)
            fprintf(stderr, "global_k_picks for global adaptive routing not specified, setting to %d\n",p->global_k_picks);
    }

    char scoring_str[MAX_NAME_LENGTH] = {'\0'};
    configuration_get_value(&config, "PARAMS", "route_scoring_metric", anno, scoring_str, MAX_NAME_LENGTH);
    if (strcmp(scoring_str, "alpha") == 0) {
        scoring = ALPHA;
    }
    else if (strcmp(scoring_str, "beta") == 0) {
        scoring = BETA;
    }
    else if (strcmp(scoring_str, "gamma") == 0) {
        tw_error(TW_LOC, "Gamma scoring protocol currently non-functional"); //TODO: Fix gamma scoring protocol
        scoring = GAMMA;
    }
    else if (strcmp(scoring_str, "delta") == 0) {
        scoring = DELTA;
    }
    else {
        if(!myRank)
            fprintf(stderr, "No route scoring protocol specified, setting to DELTA scoring\n");
        scoring = DELTA;
    }

    rc = configuration_get_value_int(&config, "PARAMS", "notification_on_hops_greater_than", anno, &p->max_hops_notify);
    if (rc) {
        if(!myRank)
            fprintf(stderr, "Maximum hops for notifying not specified, setting to INT MAX\n");
        p->max_hops_notify = INT_MAX;
    }

    p->num_vcs = 4;
    
    if(p->num_qos_levels > 1)
        p->num_vcs = p->num_qos_levels * p->num_vcs;

    //Xin: app msgs counting on routers
    rc = configuration_get_value_int(&config, "PARAMS", "counting_bool", anno, &p->counting_bool);
    if(p->counting_bool) {
        int rc1 = configuration_get_value_double(&config, "PARAMS", "counting_start", anno, &p->counting_start);
        int rc2 = configuration_get_value_int(&config, "PARAMS", "counting_windows", anno, &p->counting_windows);
        int rc3 = configuration_get_value_double(&config, "PARAMS", "counting_interval", anno, &p->counting_interval);
        int rc4 = configuration_get_value_int(&config, "PARAMS", "num_apps", anno, &p->num_apps);
        if(rc1 || rc2 || rc3 || rc4)
            tw_error(TW_LOC, "\n Missing couting values, (counting_start/windows/interval/num_apps) check for config files\n");

        //convert us to ns
        p->counting_start = p->counting_start * 1000;
        p->counting_interval = p->counting_interval * 1000;

        //printf("start %f, end %f, interval %f\n", p->counting_start, p->counting_end, p->counting_interval);
    }


    rc = configuration_get_value_int(&config, "PARAMS", "num_groups", anno, &p->num_groups);
    if(rc) {
        tw_error(TW_LOC, "\nnum_groups not specified, Aborting\n");
    }
    
    rc = configuration_get_value_int(&config, "PARAMS", "num_routers", anno, &p->num_routers);
    if(rc) {
        tw_error(TW_LOC, "\nnum_routers not specified, Aborting\n");
    }
    
    rc = configuration_get_value_int(&config, "PARAMS", "num_cns_per_router", anno, &p->num_cn);
    if(rc) {
        if(!myRank)
            fprintf(stderr,"Number of cns per router not specified, setting to %d\n", p->num_routers/2);
        p->num_cn = p->num_routers/2;
    }

    rc = configuration_get_value_int(&config, "PARAMS", "num_global_channels", anno, &p->num_global_channels);
    if(rc) {
        if(!myRank)
            fprintf(stderr,"Number of global channels per router not specified, setting to 10\n");
        p->num_global_channels = 10;
    }

    rc = configuration_get_value_int(&config, "PARAMS", "num_parallel_switch_conns", anno, &p->num_parallel_switch_conns);
    if(rc) {
        p->num_parallel_switch_conns = 1;
    }


    p->intra_grp_radix = (p->num_routers -1) * p->num_parallel_switch_conns;
    p->cn_radix = (p->num_cn * p->num_rails) / p->num_planes; //number of CNs per router times number of rails taht each CN has, divided by how many planes those CNs are shared across
    p->global_radix = p->num_global_channels * p->num_parallel_switch_conns;
    p->radix = p->intra_grp_radix + p->global_radix + p->cn_radix;
    p->total_groups = p->num_groups * p->num_planes;
    p->total_routers = p->total_groups * p->num_routers;
    p->total_terminals = p->total_routers * p->num_cn / p->num_planes;
    p->num_routers_per_plane = p->total_routers / p->num_planes;

    //Setup DflyNetworkManager
    netMan = DragonflyNetworkManager(p->total_routers, p->total_terminals, p->num_routers, p->intra_grp_radix, p->global_radix, p->cn_radix, p->num_rails, p->num_planes, max_hops_per_group, max_global_hops_nonminimal);

    // read intra group connections, store from a router's perspective
    // all links to the same router form a vector
    char intraFile[MAX_NAME_LENGTH] = {'\0'};
    configuration_get_value(&config, "PARAMS", "intra-group-connections", 
        anno, intraFile, MAX_NAME_LENGTH);
    if (strlen(intraFile) <= 0) {
      tw_error(TW_LOC, "Intra group connections file not specified. Aborting");
    }
    FILE *groupFile = fopen(intraFile, "rb");
    if (!groupFile)
        tw_error(TW_LOC, "intra-group file not found ");

    if (!myRank)
      fprintf(stderr, "Reading intra-group connectivity file: %s\n", intraFile);

    IntraGroupLink newLink;
    while (fread(&newLink, sizeof(IntraGroupLink), 1, groupFile) != 0 ) {
        int src_id_local = newLink.src;
        int dest_id_local = newLink.dest;

        for(int i = 0; i < p->total_routers; i++) //handles all routers in network, including multi planar
        {
            int group_id = i/p->num_routers;
            if (i % p->num_routers == src_id_local)
            {
                int planar_id = i / p->num_routers_per_plane;
                int dest_gid_global = (group_id * p->num_routers + dest_id_local);

                Link_Info new_link;
                new_link.src_gid = i;
                new_link.dest_gid = dest_gid_global;
                new_link.conn_type = CONN_LOCAL;
                new_link.rail_id = planar_id;
                netMan.add_link(new_link);
            }
        }
    }
    fclose(groupFile);

    //terminal assignment
    for(int i = 0; i < p->total_terminals; i++)
    {
        for(int j = 0; j < p->num_rails; j++)
        {
            int assigned_router_id = dfdally_get_assigned_router_id_from_terminal(p, i, j);

            Link_Info new_link;
            new_link.src_gid = assigned_router_id;
            new_link.dest_gid = i;
            new_link.conn_type = CONN_TERMINAL;
            new_link.rail_id = j;
            // printf("R%d <-> T%d   P%d\n",new_link.src_gid, new_link.dest_gid, new_link.rail_id);
            netMan.add_link(new_link);
        }

    }

    // read inter group connections, store from a router's perspective
    // also create a group level table that tells all the connecting routers
    char interFile[MAX_NAME_LENGTH] = {'\0'};
    configuration_get_value(&config, "PARAMS", "inter-group-connections", 
        anno, interFile, MAX_NAME_LENGTH);
    if(strlen(interFile) <= 0) {
        tw_error(TW_LOC, "Inter group connections file not specified. Aborting");
    }
    FILE *systemFile = fopen(interFile, "rb");
    if(!myRank)
    {
        fprintf(stderr, "Reading inter-group connectivity file: %s\n", interFile);
        fprintf(stderr, "\n Total routers %d total groups %d ", p->total_routers, p->total_groups);
    }

    connectionList.resize(p->num_groups);
    for(int g = 0; g < connectionList.size(); g++) {
        connectionList[g].resize(p->num_groups);
    }

    vector< vector< vector<int> > > connectionListEnumerated; //this one will have one final item for EACH link, not just to show existence of some unknown amount of links
    connectionListEnumerated.resize(p->num_groups);
    for(int g = 0; g < connectionListEnumerated.size(); g++) {
        connectionListEnumerated[g].resize(p->num_groups);
    }

    InterGroupLink newInterLink;
    while (fread(&newInterLink, sizeof(InterGroupLink), 1, systemFile) != 0) {

        for(int i = 0; i < p->num_planes; i++)
        {
            int read_src_id_global = newInterLink.src;
            int read_src_group_id = read_src_id_global / p->num_routers;
            int read_dest_id_global = newInterLink.dest;
            int read_dest_group_id = read_dest_id_global / p->num_routers;

            int mapped_src_gid = read_src_id_global + (i*p->num_routers_per_plane);
            int mapped_dest_gid = read_dest_id_global + (i*p->num_routers_per_plane);

            Link_Info new_link;
            new_link.src_gid = mapped_src_gid;
            new_link.dest_gid = mapped_dest_gid;
            new_link.conn_type = CONN_GLOBAL;
            new_link.rail_id = i;
            // printf("R%d G-> R%d   P%d\n",new_link.src_gid, new_link.dest_gid, new_link.rail_id);
            netMan.add_link(new_link);

            if( i == 0) //only need to do this once, not every plane
            {
                // connManagerList[src_id_global].add_connection(dest_id_global, CONN_GLOBAL);
                connectionListEnumerated[read_src_group_id][read_dest_group_id].push_back(newInterLink.src);

                int r;
                for (r = 0; r < connectionList[read_src_group_id][read_dest_group_id].size(); r++) {
                    if (connectionList[read_src_group_id][read_dest_group_id][r] == newInterLink.src)
                        break;
                }
                if (r == connectionList[read_src_group_id][read_dest_group_id].size()) {
                    connectionList[read_src_group_id][read_dest_group_id].push_back(newInterLink.src);
                }
            }

        }
    }


    //read link failure file
    char failureFileName[MAX_NAME_LENGTH] = {'\0'};
    failureFileName[0] = '\0';

    if (strlen(g_nm_link_failure_filepath) == 0) //was this defined already via a command line argument?
    {
        configuration_get_value(&config, "PARAMS", "link-failure-file", 
            anno, failureFileName, MAX_NAME_LENGTH);
        if (strlen(failureFileName) > 0) {
            netMan.enable_link_failures();
        }
    }
    else
    {
        strcpy(failureFileName, g_nm_link_failure_filepath);
        netMan.enable_link_failures();
    }

    if(netMan.is_link_failures_enabled())
    {
        if(!myRank)
            printf("\nDragonfly Dally: Link Failures Feature Enabled\n");

        FILE *failureFile = fopen(failureFileName, "rb");
        if (!failureFile)
            tw_error(TW_LOC, "link failure file not found: %s\n",failureFileName);

        if (!myRank)
        fprintf(stderr, "Reading link failure file: %s\n", failureFileName);

        Link_Info link;
        while (fread(&link, sizeof(Link_Info), 1, failureFile) != 0) {
            netMan.add_link_failure_info(link);
        }
    }
    
    netMan.solidify_network(); //finalize link failures and burn the network links into the connection managers 

    if (DUMP_CONNECTIONS)
    {
        if(!myRank) {
            for (int i = 0; i < p->total_routers; i++)
            {
                netMan.get_connection_manager_for_router(i).print_connections();
            }
            for (int i = 0; i < p->total_terminals; i++)
            {
                netMan.get_connection_manager_for_terminal(i).print_connections();
            }
        }
    }

    fclose(systemFile);

    if(!myRank) {
        fprintf(stderr, "\n Total nodes %d routers %d groups %d routers per group %d radix %d\n\n",
                p->num_cn * p->total_routers, p->total_routers, p->total_groups,
                p->num_routers, p->radix);
    }

    rc = configuration_get_value_double(&config, "PARAMS", "cn_delay", anno, &p->cn_delay);
    if (rc) {
        p->cn_delay = bytes_to_ns(p->chunk_size, p->cn_bandwidth*p->num_rails);
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
            if(!myRank && !auto_credit_delay_flag)
                fprintf(stderr, "global_credit_delay not specified, using calculation based on global bandwidth: %.2f\n", p->global_credit_delay);   
        }
        if (cn_credit_delay_unset) {
            p->cn_credit_delay = bytes_to_ns(p->credit_size, p->cn_bandwidth);
            if(!myRank && !auto_credit_delay_flag)
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

    // CONGESTION CONTROL
    int cc_enabled;
    rc = configuration_get_value_int(&config, "PARAMS", "congestion_control_enabled", anno, &cc_enabled);
    if(rc) {
        if(!myRank)
            fprintf(stderr,"\nCongestion Control Not Enabled\n");
    }
    else {
        g_congestion_control_enabled = cc_enabled;
        if(!myRank) {
            if (cc_enabled)
                fprintf(stderr,"\nCongestion Control Enabled\n");
            else 
                fprintf(stderr,"\nCongestion Control Disabled\n");

        }

        if(cc_enabled)
        {
            congestion_control_register_terminal_lpname(LP_CONFIG_NM_TERM);
            congestion_control_register_router_lpname(LP_CONFIG_NM_ROUT);
        }
    }
    // END CONGESTION CONTROL

    // Router buffer occupancy configuration
    if (OUTPUT_SNAPSHOT) {
        char **timestamps;
        size_t len;
        configuration_get_multivalue(&config, "PARAMS", "router_buffer_snapshots", anno, &timestamps, &len);
        if (len) {
            assert(timestamps != NULL);
            num_snapshots = len;
            snapshot_times = (tw_stime*) malloc(len * sizeof(tw_stime));

            for (size_t i = 0; i < len; i++) {
                errno = 0;
                snapshot_times[i] = strtod(timestamps[i], NULL);
                if (errno == ERANGE || errno == EILSEQ){
                    tw_error(TW_LOC, "Sequence `%s' could not be succesfully interpreted as a _double_.", timestamps[i]);
                }
            }

            if(!myRank) {
                fprintf(stderr, "\nRouter snaptshots activated for timestamps = ");
                for (size_t i = 0; i < len; i++) {
                    fprintf(stderr, "%g%s", snapshot_times[i], i == len-1 ? "" : ", ");
                }
                fprintf(stderr, "\n");
            }

            // freeing some memory
            for (size_t i = 0; i < len; i++) {
                free(timestamps[i]);
            }
            free(timestamps);
        }
    }

    // Packet latency path to store configuration
    char packet_latency_path[MAX_NAME_LENGTH];
    packet_latency_path[0] = '\0';
    configuration_get_value(&config, "PARAMS", "save_packet_latency_path", anno, packet_latency_path, MAX_NAME_LENGTH);
    if(strlen(packet_latency_path) > 0) {
        setup_packet_latency_path(packet_latency_path);
    }

    // START Surrogate configuration
    char enable_str[MAX_NAME_LENGTH];
    enable_str[0] = '\0';
    int const rc_enable = configuration_get_value(&config, "NETWORK_SURROGATE", "enable", anno, enable_str, MAX_NAME_LENGTH);
    bool enable_network_surrogate = false;
    if (rc_enable > 0) {
        enable_network_surrogate = (strcmp(enable_str, "1") == 0 || strcmp(enable_str, "true") == 0);
    }
    // if surrogate mode has been set up
    if (enable_network_surrogate) {
        struct network_surrogate_config surr_conf = {
            .model = {.switch_surrogate = switch_surrogate, .is_surrogate_on = is_surrogate_on_fun},
            .total_terminals = p->total_terminals,
            .n_lp_types = 2,
            .lp_types = {
                {.lpname = "modelnet_dragonfly_dally",
                 .trigger_idle_modelnet = true,
                 .highdef_to_surrogate = (model_switch_f) dragonfly_dally_terminal_highdef_to_surrogate,
                 .surrogate_to_highdef = (model_switch_f) dragonfly_dally_terminal_surrogate_to_highdef,
                 .should_event_be_frozen = dragonfly_dally_terminal_should_event_be_frozen,
                 .should_event_be_deleted = NULL,
                 .check_event_in_queue = (model_check_event_f) dragonfly_dally_terminal_pre_surrogate_switch_event_queue,
                 .reset_predictor = (model_switch_f) dragonfly_dally_terminal_reset_predictor,
                },
                {.lpname = "modelnet_dragonfly_dally_router",
                 .trigger_idle_modelnet = false,
                 .highdef_to_surrogate = NULL,
                 .surrogate_to_highdef = NULL,
                 .should_event_be_frozen = dragonfly_dally_router_should_event_be_frozen,
                 .should_event_be_deleted = NULL,
                 .check_event_in_queue = NULL,
                 .reset_predictor = NULL,
                },
                0
            }
        };
        freeze_network_on_switch = network_surrogate_configure(anno, &surr_conf, &terminal_predictor);
        if (terminal_predictor) {
            dally_surrogate_configured = true;
        } else {
            tw_error(TW_LOC, "Latency predictor is NULL. Something during surrogate configuration failed.");
        }
    }
    // END Surrogate configuration

    if (PRINT_CONFIG && !myRank) {
        dragonfly_print_params(p,stderr);
    }

    stored_params = p;
}

void dragonfly_dally_configure() {
    anno_map = codes_mapping_get_lp_anno_map(LP_CONFIG_NM_TERM);
    assert(anno_map);
    num_params = anno_map->num_annos + (anno_map->has_unanno_lp > 0);
    all_params = (dragonfly_param *)calloc(num_params, sizeof(*all_params));

    for (int i = 0; i < anno_map->num_annos; i++) {
        const char * anno = anno_map->annotations[i].ptr;
        dragonfly_read_config(anno, &all_params[i]);
    }
    if (anno_map->has_unanno_lp > 0){
        dragonfly_read_config(NULL, &all_params[anno_map->num_annos]);
    }
#ifdef ENABLE_CORTEX
	model_net_topology = dragonfly_dally_cortex_topology;
#endif

}

static void setup_packet_latency_path(char const * const dir_to_save) {
    assert(packet_latency_f == NULL);
    // checking 
    int const NO_ERROR = 0;
    struct stat st;
    memset(&st, 0, sizeof(struct stat));
    if(g_tw_mynode == 0 && stat(dir_to_save, &st) == -1) {
        int res = mkdir(dir_to_save, 0700);
        if (res != NO_ERROR) {
            tw_error(TW_LOC, "Error (%d) occurred when attempting to mkdir folder `%s`", errno, dir_to_save);
        }
    }
    MPI_Barrier(MPI_COMM_CODES);

    char const fmt[] = "%s/packets-delay-gid=%lu.txt";
    int sz = snprintf(NULL, 0, fmt, dir_to_save, g_tw_mynode);
    char filename_path[sz + 1]; // `+ 1` for terminating null byte
    snprintf(filename_path, sizeof(filename_path), fmt, dir_to_save, g_tw_mynode);
    packet_latency_f = fopen(filename_path, "w+");
    if(!packet_latency_f) {
        tw_error(TW_LOC, "File %s could not be opened", filename_path);
    }

    fprintf(packet_latency_f, "#src_terminal,dest_terminal,packet_id,is_surrogate_on,is_predicted,size,workload_injection,next_packet_delay,start,end,latency,is_there_another_pckt_in_queue\n");
}

/* report dragonfly statistics like average and maximum packet latency, average number of hops traversed */
void dragonfly_dally_report_stats()
{
    long long avg_hops, total_finished_packets, total_finished_chunks;
    long long total_finished_msgs, final_msg_sz;
    tw_stime avg_time, max_time;
    int total_minimal_packets, total_nonmin_packets;
    long total_gen, total_fin;
    long total_local_packets_sr, total_local_packets_sg, total_remote_packets;

    MPI_Reduce( &total_hops, &avg_hops, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_CODES);
    MPI_Reduce( &N_finished_packets, &total_finished_packets, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_CODES);
    MPI_Reduce( &N_finished_msgs, &total_finished_msgs, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_CODES);
    MPI_Reduce( &N_finished_chunks, &total_finished_chunks, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_CODES);
    MPI_Reduce( &total_msg_sz, &final_msg_sz, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_CODES);
    MPI_Reduce( &dragonfly_total_time, &avg_time, 1,MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_CODES);
    MPI_Reduce( &dragonfly_max_latency, &max_time, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_CODES);
    
    MPI_Reduce( &packet_gen, &total_gen, 1, MPI_LONG, MPI_SUM, 0, MPI_COMM_CODES);
    MPI_Reduce(&packet_fin, &total_fin, 1, MPI_LONG, MPI_SUM, 0, MPI_COMM_CODES);
    MPI_Reduce( &num_local_packets_sr, &total_local_packets_sr, 1, MPI_LONG, MPI_SUM, 0, MPI_COMM_CODES);
    MPI_Reduce( &num_local_packets_sg, &total_local_packets_sg, 1, MPI_LONG, MPI_SUM, 0, MPI_COMM_CODES);
    MPI_Reduce( &num_remote_packets, &total_remote_packets, 1, MPI_LONG, MPI_SUM, 0, MPI_COMM_CODES);
    if(routing == ADAPTIVE || routing == PROG_ADAPTIVE || routing == PROG_ADAPTIVE_LEGACY || SHOW_ADAP_STATS)
    {
        MPI_Reduce(&minimal_count, &total_minimal_packets, 1, MPI_INT, MPI_SUM, 0, MPI_COMM_CODES);
        MPI_Reduce(&nonmin_count, &total_nonmin_packets, 1, MPI_INT, MPI_SUM, 0, MPI_COMM_CODES);
    }

    // long long total_stalled_chunks; //helpful for debugging and determinism checking
    // MPI_Reduce( &global_stalled_chunk_counter, &total_stalled_chunks, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_CODES);

    if (packet_latency_f) {
        fclose(packet_latency_f);
    }
    /* print statistics */
    if(!g_tw_mynode)
    {	
        if (PRINT_CONFIG) 
            dragonfly_print_params(stored_params, NULL);

        printf("\nAverage number of hops traversed %f average chunk latency %lf us maximum chunk latency %lf us avg message size %lf bytes finished messages %lld finished chunks %lld\n", 
                (float)avg_hops/total_finished_chunks, (float) avg_time/total_finished_chunks/1000, max_time/1000, (float)final_msg_sz/total_finished_msgs, total_finished_msgs, total_finished_chunks);
        if(routing == ADAPTIVE || routing == PROG_ADAPTIVE || routing == PROG_ADAPTIVE_LEGACY || SHOW_ADAP_STATS)
                printf("\nADAPTIVE ROUTING STATS: %d chunks routed minimally %d chunks routed non-minimally completed packets %lld \n", 
                        total_minimal_packets, total_nonmin_packets, total_finished_chunks);
    
        printf("\nTotal packets generated %ld finished %ld Locally routed- same router %ld different-router %ld Remote (inter-group) %ld \n", total_gen, total_fin, total_local_packets_sr, total_local_packets_sg, total_remote_packets);
    }
    return;
}

static void dragonfly_dally_terminal_end_sim_notif(terminal_state *s, tw_bf *bf, model_net_wrap_msg *msg, tw_lp *lp)
{
    s->workloads_finished_flag = 1;
}

static void dragonfly_dally_terminal_end_sim_notif_rc(terminal_state *s, tw_bf *bf, model_net_wrap_msg *msg, tw_lp *lp)
{
    s->workloads_finished_flag = 0;
}


static void dragonfly_dally_router_end_sim_notif(router_state *s, tw_bf *bf, model_net_wrap_msg *msg, tw_lp *lp)
{
    s->workloads_finished_flag = 1;
}

static void dragonfly_dally_router_end_sim_notif_rc(router_state *s, tw_bf *bf, model_net_wrap_msg *msg, tw_lp *lp)
{
    s->workloads_finished_flag = 0;
}

static void dragonfly_dally_terminal_congestion_event(terminal_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
{
    cc_terminal_local_congestion_event(s->local_congestion_controller, bf, msg, lp);
}

static void dragonfly_dally_terminal_congestion_event_rc(terminal_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
{
    cc_terminal_local_congestion_event_rc(s->local_congestion_controller, bf, msg, lp);
}

static void dragonfly_dally_terminal_congestion_event_commit(terminal_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
{
    cc_terminal_local_congestion_event_commit(s->local_congestion_controller, bf, msg, lp);
}

static void dragonfly_dally_router_congestion_event(router_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
{
    cc_router_local_congestion_event(s->local_congestion_controller, bf, msg, lp);
}

static void dragonfly_dally_router_congestion_event_rc(router_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
{
    cc_router_local_congestion_event_rc(s->local_congestion_controller, bf, msg, lp);

}

static void dragonfly_dally_router_congestion_event_commit(router_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
{
    cc_router_local_congestion_event_commit(s->local_congestion_controller, bf, msg, lp);
}

static int get_vcg_from_category(terminal_dally_message * msg)
{
   if(strcmp(msg->category, "high") == 0)
       return Q_HIGH;
   else if(strcmp(msg->category, "medium") == 0)
       return Q_MEDIUM;
   else
       tw_error(TW_LOC, "\n priority needs to be specified with qos_levels>1 %d", msg->category);
}

static int get_term_bandwidth_consumption(terminal_state * s, int rail_id, int qos_lvl)
{
    assert(qos_lvl >= Q_HIGH && qos_lvl <= Q_LOW);

    //tw_stime reset_window_s = ns_to_s(bw_reset_window); 
    //double bw_gib = bytes_to_gigabytes(s->qos_data[qos_lvl]);

    //double bw_consumed = ((double)bw_gib / (double)reset_window_s);
    double max_bw = s->params->cn_bandwidth * 1024.0 * 1024.0 * 1024.0;
    double max_bw_per_ns = max_bw / (1000.0 * 1000.0 * 1000.0);
    double max_bytes_per_win = max_bw_per_ns * bw_reset_window;
//    int percent_bw = (bw_consumed / s->params->cn_bandwidth) * 100;
    int percent_bw = (((double)s->qos_data[rail_id][qos_lvl]) / max_bytes_per_win) * 100;
//    printf("\n At terminal %lf max bytes %d percent %d ", max_bytes_per_win, s->qos_data[qos_lvl], percent_bw);
    return percent_bw;
}

/* TODO: Differentiate between local and global bandwidths. */
static int get_rtr_bandwidth_consumption(router_state * s, int qos_lvl, int output_port)
{
    assert(qos_lvl >= Q_HIGH && qos_lvl <= Q_LOW);
    assert(output_port < s->params->intra_grp_radix + s->params->num_global_channels + s->params->cn_radix);

    int bandwidth = s->params->cn_bandwidth;
    if (output_port < s->params->intra_grp_radix)
        bandwidth = s->params->local_bandwidth;
    else if (output_port < s->params->intra_grp_radix + s->params->num_global_channels)
        bandwidth = s->params->global_bandwidth;

    /* conversion into bytes from GiB */
    double max_bw = bandwidth * 1024.0 * 1024.0 * 1024.0;
    double max_bw_per_ns = max_bw / (1000.0 * 1000.0 * 1000.0);
    double max_bytes_per_win = max_bw_per_ns * bw_reset_window;

    /* bw_consumed would be in Gigabytes per second. */
//    tw_stime reset_window_s = ns_to_s(bw_reset_window);
//    double bw_gib = bytes_to_gigabytes(s->qos_data[output_port][qos_lvl]);
//    double bw_consumed = ((double)bw_gib / (double)reset_window_s);
    int percent_bw = (((double)s->qos_data[output_port][qos_lvl]) / max_bytes_per_win) * 100;
//    printf("\n percent bw consumed by qos_lvl %d is %d bytes transferred %d max_bw %lf ", qos_lvl, percent_bw, s->qos_data[output_port][qos_lvl], max_bw_per_ns);
    return percent_bw;
}

static void issue_bw_monitor_event_rc(terminal_state * s, tw_bf * bf, terminal_dally_message * msg, tw_lp * lp)
{
    int num_qos_levels = s->params->num_qos_levels;
    int num_rails = s->params->num_rails;

    
    if(msg->rc_is_qos_set == 1)
    {
        for(int i = 0; i < num_rails; i++) 
        {
            for(int j = 0; j < num_qos_levels; j++)
            {
                s->qos_data[i][j] = *(indexer2d(msg->rc_qos_data, i, j, num_rails, num_qos_levels));
                s->qos_status[i][j] =*(indexer2d(msg->rc_qos_status, i, j, num_rails, num_qos_levels));
            }
        }

        free(msg->rc_qos_data);
        free(msg->rc_qos_status);
        msg->rc_is_qos_set = 0;
    }
    
}
/* resets the bandwidth numbers recorded so far */
static void issue_bw_monitor_event(terminal_state * s, tw_bf * bf, terminal_dally_message * msg, tw_lp * lp)
{
    int num_qos_levels = s->params->num_qos_levels;
    int num_rails = s->params->num_rails;
    
    //RC data storage start.
    //Allocate memory here for these pointers that are stored in the events. FREE THESE IN RC OR IN COMMIT_F
    msg->rc_qos_data = (unsigned long long *) calloc(num_rails*num_qos_levels, sizeof(unsigned long long));
    msg->rc_qos_status = (int *) calloc(num_rails*num_qos_levels, sizeof(int));


    //store qos data and status into the arrays. Pointers to the arrays are stored in events.
    for(int i = 0; i < num_rails; i++)
    {
        for(int j = 0; j < num_qos_levels; j++)
        {
            *(indexer2d(msg->rc_qos_data, i, j, num_rails, num_qos_levels)) = s->qos_data[i][j];
            *(indexer2d(msg->rc_qos_status, i, j, num_rails, num_qos_levels)) = s->qos_status[i][j];
        }
    }
    msg->rc_is_qos_set = 1;
    //RC data storage end.

    /* Reset the qos status and bandwidth consumption. */
    for(int i = 0; i < num_rails; i++)
    {
        for(int j = 0; j < num_qos_levels; j++)
        {
            s->qos_status[i][j] = Q_ACTIVE;
            s->qos_data[i][j] = 0;
        }
    }

    if (s->workloads_finished_flag == 0) {
        terminal_dally_message * m; 
        tw_stime bw_ts = bw_reset_window + gen_noise(lp, &msg->num_rngs);
        tw_event * e = model_net_method_event_new(lp->gid, bw_ts, lp, DRAGONFLY_DALLY,
                (void**)&m, NULL); 
        m->type = T_BANDWIDTH;
        m->magic = terminal_magic_num; 
        tw_event_send(e);
    }
}

static void issue_rtr_bw_monitor_event_rc(router_state *s, tw_bf *bf, terminal_dally_message *msg, tw_lp *lp)
{
    int radix = s->params->radix;
    int num_qos_levels = s->params->num_qos_levels;

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

static void issue_rtr_bw_monitor_event(router_state *s, tw_bf *bf, terminal_dally_message *msg, tw_lp *lp)
{
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
            if(dragonfly_rtr_bw_log != NULL && ROUTER_BW_LOG)
            {
                if(s->qos_data[i][j] > 0)
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

    if (s->workloads_finished_flag == 0) {
        tw_stime bw_ts = bw_reset_window + gen_noise(lp, &msg->num_rngs);
        terminal_dally_message *m;
        tw_event * e = model_net_method_event_new(lp->gid, bw_ts, lp,
                DRAGONFLY_DALLY_ROUTER, (void**)&m, NULL);
        m->type = R_BANDWIDTH;
        m->magic = router_magic_num;
        tw_event_send(e);
    }
}

static int get_next_vcg(terminal_state * s, tw_bf * bf, terminal_dally_message * msg, tw_lp * lp)
{
    int num_qos_levels = s->params->num_qos_levels;
    
    if(num_qos_levels == 1)
    {
        if(s->terminal_msgs[msg->rail_id][0] == NULL || s->vc_occupancy[msg->rail_id][0] + s->params->chunk_size > s->params->cn_vc_size)
            return -1;
        else
            return 0;
    }

    int bw_consumption[num_qos_levels];

    /* First make sure the bandwidth consumptions are up to date. */
    for(int k = 0; k < num_qos_levels; k++)
    {
        if(s->qos_status[msg->rail_id][k] != Q_OVERBW)
        {
            bw_consumption[k] = get_term_bandwidth_consumption(s, msg->rail_id, k);
            if(bw_consumption[k] > s->params->qos_bandwidths[k]) 
            {
                if(k == 0)
                    msg->qos_reset1 = 1;
                else if(k == 1)
                    msg->qos_reset2 = 1;

                s->qos_status[msg->rail_id][k] = Q_OVERBW;
            }
        }
    }
    /* TODO: If none of the vcg is exceeding bandwidth limit then select high
    * priority traffic first. */
    if(BW_MONITOR == 1)
    {
        for(int i = 0; i < num_qos_levels; i++)
        {
            if(s->qos_status[msg->rail_id][i] == Q_ACTIVE)
            {
                if(s->terminal_msgs[msg->rail_id][i] != NULL && s->vc_occupancy[msg->rail_id][i] + s->params->chunk_size <= s->params->cn_vc_size)
                    return i;
            }
        }
    }


    int next_rr_vcg = (s->last_qos_lvl[msg->rail_id] + 1) % num_qos_levels;
    /* All vcgs are exceeding their bandwidth limits*/
    for(int i = 0; i < num_qos_levels; i++)
    {
        if(s->terminal_msgs[msg->rail_id][i] != NULL && s->vc_occupancy[msg->rail_id][i] + s->params->chunk_size <= s->params->cn_vc_size)
        {
            bf->c2 = 1;
            
            if(msg->last_saved_qos < 0)
                msg->last_saved_qos = s->last_qos_lvl[msg->rail_id];
            
            s->last_qos_lvl[msg->rail_id] = next_rr_vcg;
            return i;
        }
        next_rr_vcg = (next_rr_vcg + 1) % num_qos_levels;
    }
    return -1;
}

static int get_next_router_vcg(router_state * s, tw_bf * bf, terminal_dally_message * msg, tw_lp * lp)
{
    int num_qos_levels = s->params->num_qos_levels;

    int vcs_per_qos = s->params->num_vcs / num_qos_levels;
    int output_port = msg->vc_index;
    int vcg = 0;
    int base_limit = 0;
        
    int chunk_size = s->params->chunk_size;
    int bw_consumption[num_qos_levels];
    /* First make sure the bandwidth consumptions are up to date. */
    if(BW_MONITOR == 1 && num_qos_levels > 1)
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
    msg->last_saved_qos = s->last_qos_lvl[output_port]; // last_qos_lvl stores a vc# not a qos# for routers. Terminals store qos#
    //int next_rr_vcg = (s->last_qos_lvl[output_port] + 1) % num_qos_levels;
    int next_rr_vc = (s->last_qos_lvl[output_port] + 1) % s->params->num_vcs;

    for(int i = 0; i < s->params->num_vcs; i++)
    {
        if(s->pending_msgs[output_port][next_rr_vc] != NULL)
        {
            s->last_qos_lvl[output_port] = next_rr_vc;
            return next_rr_vc;
        }
        next_rr_vc = (next_rr_vc + 1) % s->params->num_vcs;
    }
    return -1;
}

static inline void packet_latency_save_to_file(
        unsigned int terminal_id,
        struct packet_start * start,
        struct packet_end * end,
        bool surrogate_on,
        bool is_predicted
) {
    if (!packet_latency_f) { return; } // Don't save if there isn't a file to save to
    if (end->travel_end_time > g_tw_ts_end) { return; } // This packet could never arrive to its destination!
    fprintf(packet_latency_f, "%u,%u,%lu,%d,%d,%u,%f,%f,%f,%f,%f,%d\n",
            terminal_id, start->dfdally_dest_terminal_id, start->packet_ID,
            surrogate_on, is_predicted, start->packet_size,
            start->workload_injection_time,
            end->next_packet_delay, start->travel_start_time,
            end->travel_end_time, end->travel_end_time - start->travel_start_time,
            start->is_there_another_pckt_in_queue);
}

// ==== START OF Surrogate functions definition ====

static void switch_surrogate(void) {
    is_dally_surrogate_on = ! is_dally_surrogate_on;
}

static bool is_surrogate_on_fun(void) {
    return is_dally_surrogate_on;
}

static void feed_packet_to_predictor(terminal_state * s, tw_lp * lp, uint64_t packet_ID, double end_time) {
    assert(s->sent_packets.count(packet_ID) == 1); // packet_ID is in s->sent_packets
    auto sent = s->sent_packets[packet_ID];
    struct packet_end end = {
        .travel_end_time = end_time,
        .next_packet_delay = sent.next_packet_delay,
    };

    packet_latency_save_to_file(s->terminal_id, &sent.start, &end, is_dally_surrogate_on, false);
    if (dally_surrogate_configured && !is_dally_surrogate_on) {
        assert(terminal_predictor != NULL);
        terminal_predictor->feed(s->predictor_data, lp, s->terminal_id, &sent.start, &end);
    }

    // Deallocating memory
    if (sent.message_data) {
        free(sent.message_data);
    }
    if (sent.remote_event_data) {
        free(sent.remote_event_data);
    }
    if (sent.local_data) {
        free(sent.local_data);
    }
}

// We check an event that is in the event queue, thus we do not process it yet
static void dragonfly_dally_terminal_pre_surrogate_switch_event_queue(
    terminal_state * s, tw_lp * lp, tw_event * event) {
    int const event_type = model_net_get_event_type_lp((model_net_wrap_msg *) tw_event_data(event));
    // if event is T_NOTIFY, add event relevant data into hash map for T_NOTIFY event
    if (event_type == MN_BASE_PASS) {
        terminal_dally_message * msg = (terminal_dally_message *)
            model_net_method_msg_from_tw_event(lp, (model_net_wrap_msg *) tw_event_data(event));
        assert(msg != NULL);
        if (msg->type == T_NOTIFY) {
            assert(msg->notify_type == NOTIFY_LATENCY);
            feed_packet_to_predictor(s, lp, msg->packet_ID, msg->travel_end_time);
            s->sent_packets.erase(msg->packet_ID);
        }
    }
}

static void dragonfly_dally_terminal_reset_predictor(terminal_state * s, tw_lp * lp, void * vacuous) {
    (void) vacuous;
    if (terminal_predictor != NULL && s->predictor_data != NULL) {
        terminal_predictor->reset(s->predictor_data, lp);
    }
}

// This function never rollsback because it's called at GVT
static void dragonfly_dally_terminal_highdef_to_surrogate(
        terminal_state * s, tw_lp * lp, tw_event ** terminal_events) {
    (void) terminal_events;

    if (s->arrival_of_last_packet.packet_ID != -1) {
        assert(s->sent_packets.count(s->arrival_of_last_packet.packet_ID) == 1); // packet_ID is in s->sent_packets
        assert(s->sent_packets[s->arrival_of_last_packet.packet_ID].next_packet_delay < 0); // next_packet_delay is -1

        double const travel_end_time = s->arrival_of_last_packet.travel_end_time;
        feed_packet_to_predictor(s, lp, s->arrival_of_last_packet.packet_ID, travel_end_time);
        s->sent_packets.erase(s->arrival_of_last_packet.packet_ID);
        s->arrival_of_last_packet.packet_ID = -1;
    }

    // Going through every packet that was sent but not yet received, remove it
    // from the list, send it to its destination using the predictor, and
    // notify of its zombie status.
    // (deleting all elements from s->sent_packets as we go)
    for (auto it = s->sent_packets.begin(); it != s->sent_packets.end(); it = s->sent_packets.erase(it)) {
        uint64_t packet_ID = it->first;
        auto & sent = it->second;

        assert(packet_ID == sent.start.packet_ID);

        // The packet has not been delievered. Send directly to destination and notify of zombie event
        if (freeze_network_on_switch) {
            struct packet_end predicted_end = 
                terminal_predictor->predict(s->predictor_data, lp, s->terminal_id, &sent.start);

            double latency = predicted_end.travel_end_time - tw_now(lp);
            if (predicted_end.travel_end_time < tw_now(lp) || latency < 0) {
                predicted_end.travel_end_time = tw_now(lp);
                latency = 0;
            }

            packet_latency_save_to_file(s->terminal_id, &sent.start, &predicted_end, is_dally_surrogate_on, true);

            assert(sent.message_data);
            terminal_dally_message * const msg_data = (terminal_dally_message*) sent.message_data;
            terminal_dally_message * m;
            void * remote_event;
            tw_event * const e = model_net_method_event_new(
                    sent.start.dest_terminal_lpid, latency, lp, DRAGONFLY_DALLY, (void**)&m, &remote_event);
            memcpy(m, msg_data, sizeof(terminal_dally_message));
            if (m->remote_event_size_bytes) {
                memcpy(remote_event, sent.remote_event_data, m->remote_event_size_bytes);
            }
            m->magic = terminal_magic_num;
            m->type = T_ARRIVE_PREDICTED;
            m->src_terminal_id = lp->gid;
            //m->travel_start_time = tw_now(lp);
            //m->rail_id = msg->rail_id;
            //m->vc_index = vcg;
            //m->last_hop = TERMINAL;
            m->path_type = -1;
            m->local_event_size_bytes = 0;
            m->is_intm_visited = 0;
            m->intm_grp_id = -1;
            m->intm_rtr_id = -1; //for legacy prog-adaptive
            assert(m->dfdally_src_terminal_id  == s->terminal_id);
            assert(m->packet_ID                == sent.start.packet_ID);
            assert(m->dest_terminal_lpid       == sent.start.dest_terminal_lpid);
            assert(m->dfdally_dest_terminal_id == sent.start.dfdally_dest_terminal_id);
            //assert(m->travel_start_time        >= sent.start.travel_start_time);
            assert(m->packet_size              == sent.start.packet_size);
            tw_event_send(e);

            //printf("NOTIFYING of zombie: packet dest id %d dest gid %d\n", sent.start.dest_terminal_lpid, sent.start.dfdally_dest_terminal_id);
            notify_dest_lp_of(s, lp, msg_data, NOTIFY_ZOMBIE);

            if (s->is_pending_local_send.count(packet_ID) == 1) {
                assert(sent.local_data);
                assert(msg_data->local_event_size_bytes);
                double const local_ts = 11;
                tw_event *e_new = tw_event_new(msg_data->sender_lp, local_ts, lp);
                void * m_new = tw_event_data(e_new);
                memcpy(m_new, sent.local_data, msg_data->local_event_size_bytes);
                tw_event_send(e_new);
                s->is_pending_local_send.erase(packet_ID);
            }

            // Deallocating memory from packet_start
            if (sent.message_data) {
                free(sent.message_data);
            }
            if (sent.remote_event_data) {
                free(sent.remote_event_data);
            }
            if (sent.local_data) {
                free(sent.local_data);
            }
        }
    }
    assert(s->sent_packets.empty());
    assert(s->is_pending_local_send.empty());

    // Hide current state and clean current state. Hidding the network information is in principle
    // the same as freezing the state of the network.
    assert(s->frozen_state == NULL);
    terminal_state * frozen_state = (terminal_state*) malloc(sizeof(terminal_state));
    memcpy(frozen_state, s, sizeof(terminal_state));
    memset(s, 0, sizeof(terminal_state));
    for (size_t i = 0; i < CATEGORY_MAX; i++) {
        s->dragonfly_stats_array[i] = frozen_state->dragonfly_stats_array[i];
    }
    s->packet_gen                   = frozen_state->packet_gen;
    s->total_gen_size               = frozen_state->total_gen_size;
    s->params                       = frozen_state->params;
    s->packet_counter               = frozen_state->packet_counter;
    s->local_congestion_controller  = frozen_state->local_congestion_controller;
    s->last_in_queue_time           = frozen_state->last_in_queue_time;
    s->predictor_data               = frozen_state->predictor_data;
    s->terminal_id                  = frozen_state->terminal_id;
    s->packet_fin                   = frozen_state->packet_fin;
    s->finished_packets             = frozen_state->finished_packets;
    s->data_size_sample             = frozen_state->data_size_sample;
    s->ross_sample.data_size_sample = frozen_state->ross_sample.data_size_sample;
    s->data_size_ross_sample        = frozen_state->data_size_ross_sample;
    s->total_msg_size               = frozen_state->total_msg_size;
    s->finished_msgs                = frozen_state->finished_msgs;
    s->rank_tbl_pop                 = frozen_state->rank_tbl_pop;
    s->last_packet_sent_id          = frozen_state->last_packet_sent_id;
    s->rank_tbl                     = frozen_state->rank_tbl;
    s->st                           = frozen_state->st;
    memcpy(&s->arrival_of_last_packet, &frozen_state->arrival_of_last_packet, sizeof(s->arrival_of_last_packet));
    memcpy(&s->zombies,              &frozen_state->zombies,              sizeof(s->zombies));
    memcpy(&s->sent_packets,         &frozen_state->sent_packets,         sizeof(s->sent_packets));
    memcpy(&s->is_pending_local_send, &frozen_state->is_pending_local_send, sizeof(s->is_pending_local_send));
    memcpy(&s->remaining_sz_packets, &frozen_state->remaining_sz_packets, sizeof(s->remaining_sz_packets));

    s->frozen_state = frozen_state;
};

// This function never rollsback because it's called at GVT
// Note: this function CANNOT generate any events, because it is to be used in `dragonfly_dally_terminal_final` too
static void dragonfly_dally_terminal_surrogate_to_highdef(
        terminal_state * s, tw_lp * lp, tw_event ** terminal_events) {
    (void) lp;
    (void) terminal_events;
    //printf("Terminal %d (PID: %d) switching back to high-def at %e\n", s->terminal_id, lp->gid, tw_now(lp));

    // Re-instanciating pre-transition (before surrogate was turned on) terminal state
    assert(s->frozen_state != NULL);
    terminal_state * frozen_state = s->frozen_state;
    for (size_t i = 0; i < CATEGORY_MAX; i++) {
        frozen_state->dragonfly_stats_array[i] = s->dragonfly_stats_array[i];
    }
    frozen_state->packet_gen                   = s->packet_gen;
    frozen_state->total_gen_size               = s->total_gen_size;
    frozen_state->params                       = s->params;
    frozen_state->packet_counter               = s->packet_counter;
    frozen_state->local_congestion_controller  = s->local_congestion_controller;
    frozen_state->last_in_queue_time           = s->last_in_queue_time;
    frozen_state->predictor_data               = s->predictor_data;
    frozen_state->terminal_id                  = s->terminal_id;
    frozen_state->packet_fin                   = s->packet_fin;
    frozen_state->finished_packets             = s->finished_packets;
    frozen_state->data_size_sample             = s->data_size_sample;
    frozen_state->ross_sample.data_size_sample = s->ross_sample.data_size_sample;
    frozen_state->data_size_ross_sample        = s->data_size_ross_sample;
    frozen_state->total_msg_size               = s->total_msg_size;
    frozen_state->finished_msgs                = s->finished_msgs;
    frozen_state->rank_tbl_pop                 = s->rank_tbl_pop;
    frozen_state->last_packet_sent_id          = s->last_packet_sent_id;
    frozen_state->rank_tbl                     = s->rank_tbl;
    frozen_state->st                           = s->st;
    memcpy(&frozen_state->arrival_of_last_packet, &s->arrival_of_last_packet, sizeof(s->arrival_of_last_packet));
    memcpy(&frozen_state->zombies,              &s->zombies,              sizeof(s->zombies));
    memcpy(&frozen_state->sent_packets,         &s->sent_packets,         sizeof(s->sent_packets));
    memcpy(&frozen_state->is_pending_local_send, &s->is_pending_local_send, sizeof(s->is_pending_local_send));
    memcpy(&frozen_state->remaining_sz_packets, &s->remaining_sz_packets, sizeof(s->remaining_sz_packets));
    memcpy(s, frozen_state, sizeof(terminal_state));
    memset(frozen_state, 0, sizeof(terminal_state));
    free(frozen_state);
    assert(s->frozen_state == NULL);
};

static bool dragonfly_dally_terminal_should_event_be_frozen(tw_lp * lp, tw_event * event) {
    (void) lp;
    assert(lp->gid == event->dest_lpid);
    // Freeze everything except for MN_BASE_NEW_MSG's
    int const event_types_to_freeze = MN_BASE_SCHED_NEXT | MN_BASE_SAMPLE | MN_BASE_PASS | MN_BASE_END_NOTIF | MN_CONGESTION_EVENT;
    int const event_type = model_net_get_event_type_lp((model_net_wrap_msg *) tw_event_data(event));
    if (event_type & event_types_to_freeze) { // Finding out whether current event type is one of freeze types
        return true;
    }
    return false;
}

// Freezing all events except for R_SNAPSHOT's!!
static bool dragonfly_dally_router_should_event_be_frozen(tw_lp * lp, tw_event * event) {
    assert(lp->gid == event->dest_lpid);
    // Freeze everything except for MN_BASE_PASS!
    int const event_types_to_freeze = MN_BASE_NEW_MSG | MN_BASE_SCHED_NEXT | MN_BASE_SAMPLE | MN_BASE_END_NOTIF | MN_CONGESTION_EVENT;
    int const event_type = model_net_get_event_type_lp((model_net_wrap_msg *) tw_event_data(event));
    if (event_type & event_types_to_freeze) { // Finding out whether current event type is one of freeze types
        return true;
    }

    // Ignore (ie, do not freeze) R_SNAPSHOT's!
    assert(event_type == MN_BASE_PASS);
    terminal_dally_message * msg = (terminal_dally_message *) model_net_method_msg_from_tw_event(lp, (model_net_wrap_msg *) tw_event_data(event));
    assert(msg != NULL);
    if (msg->type == R_SNAPSHOT) { // Snapshots will stay unaltered, never frozen
        return false;
    }

    return true;
}
//
// ==== END OF Surrogate functions definition ====

//Snapshot pattern
//Sends a snapshot event - this wakes the router at the specified time to store its data somewhere
//this storage place could be in the event or elsewehre so long as the data is over-writeable
//in case the event gets rolled back and replayed.
//On commit of the snapshot event, the commit function looks where the data was stored and outputs to lpio
static void router_send_snapshot_events(router_state *s, tw_lp *lp)
{
    int len = sprintf(snapshot_filename, "dragonfly-snapshots.csv");
    snapshot_filename[len] = '\0';
    if (s->router_id == 0) //add header information
    {
        if (OUTPUT_SNAPSHOT)
        {
            // Finding size of snapshot line
            int line_sz = 28;  // This is the size of '#Time of snapshot,Router ID,'
            for (int i = 0, j = 0; i < s->params->radix; ) {
                int sz = snprintf(NULL, 0, "Port %d VC %d,", i, j);
                line_sz += sz;

                j++;
                if(j >= s->params->num_vcs) { i++; j = 0; }
            }

            // Creating snapshot line
            char snapshot_line[line_sz + 1];  // extra space for '\0'
            int offset = 28;
            snprintf(snapshot_line, sizeof(snapshot_line), "#Time of snapshot,Router ID,");
            for (int i = 0, j = 0; i < s->params->radix; ) {
                int sz = snprintf(snapshot_line + offset, sizeof(snapshot_line) - offset, "Port %d VC %d,", i, j);
                offset += sz;

                j++;
                if(j >= s->params->num_vcs) { i++; j = 0; }
            }
            assert(line_sz == offset);
            snapshot_line[line_sz - 1] = '\n';  // replacing last ',' for '\n'
            snapshot_line[line_sz] = '\0';  // just in case it's treated as a null terminating string

            // "Saving" snapshot line
            lp_io_write(lp->gid, snapshot_filename, line_sz, snapshot_line);
        }
    }


    for(int i = 0; i < num_snapshots; i++)
    {
        terminal_dally_message *m;
        tw_event * e = model_net_method_event_new(lp->gid, snapshot_times[i], lp, DRAGONFLY_DALLY, (void**)&m, NULL);
        m->type = R_SNAPSHOT;
        m->magic = router_magic_num;
        m->packet_ID = i; //just borrowing this field for our use
        tw_event_send(e);
    }
    // printf("%d: sending snapshot events\n",s->router_id);
}

static void router_handle_snapshot_event(router_state *s, tw_bf *bf, terminal_dally_message *msg, tw_lp *lp)
{
    if (msg->packet_ID >= num_snapshots) {
        fprintf(stderr, "Warning: packet_ID = %llu will not be saved in the snapshot because there are only %d spaces available.\n", msg->packet_ID, num_snapshots);
        return;
    }

    for(int i = 0; i < s->params->radix; i++)
    {
        for(int j = 0; j < s->params->num_vcs; j++)
        {
            //msg->packet_ID contains which snapshot we're collecting
            int snapshot_array_i = (i * s->params->num_vcs) + j;
            s->snapshot_data[msg->packet_ID][snapshot_array_i] = s->vc_occupancy[i][j];
            // printf("%d: stored snapshot %d at time %.2f\n",s->router_id, msg->packet_ID, tw_now(lp));
        }
    }
}

static void terminal_commit_packet_generate(terminal_state * s, tw_bf * bf, terminal_dally_message * msg, tw_lp * lp) {
    if (!packet_latency_f && !dally_surrogate_configured) {
        return;
    }

    // Storing packet info of sent packet. Once packets arrive back, we can compute
    // the latency of sending the packet
    void * msg_data = malloc(sizeof(terminal_dally_message));
    memcpy(msg_data, msg, sizeof(terminal_dally_message));
    void * remote_data = NULL;
    if (msg->remote_event_size_bytes) {
        remote_data = malloc(msg->remote_event_size_bytes);
        memcpy(remote_data, model_net_method_get_edata(DRAGONFLY_DALLY, msg), msg->remote_event_size_bytes);
    }
    void * local_data = NULL;
    if (msg->local_event_size_bytes) {
        local_data = malloc(msg->local_event_size_bytes);
        memcpy(local_data, (char *) model_net_method_get_edata(DRAGONFLY_DALLY, msg) + msg->remote_event_size_bytes, msg->local_event_size_bytes);
    }
    double const processing_packet_delay = msg->saved_next_packet_delay;

    // TODO (elkin): In the future, this ugly initialization could be done all in a single "line" instead of setting all values one by one. The reason to do it this way is because some old compilers do not understand other ways of initializing
    struct packet_sent sent;
    sent.start.packet_ID = msg->packet_ID;
    sent.start.dest_terminal_lpid = msg->dest_terminal_lpid;
    sent.start.dfdally_dest_terminal_id = msg->dfdally_dest_terminal_id;
    sent.start.travel_start_time = msg->saved_processing_time;
    sent.start.workload_injection_time = msg->msg_start_time;
    sent.start.processing_packet_delay = processing_packet_delay;
    sent.start.packet_size = msg->packet_size;
    sent.start.is_there_another_pckt_in_queue = msg->is_there_another_pckt_in_queue;
    sent.next_packet_delay = -1;
    sent.message_data = msg_data;
    sent.remote_event_data = remote_data;
    sent.local_data = local_data;

    s->sent_packets[msg->packet_ID] = sent;
    if (freeze_network_on_switch && msg->local_event_size_bytes > 0) {
        s->is_pending_local_send.insert(msg->packet_ID);
    }

    // Set next_packet_delay for the last past sent packet
    if (s->sent_packets.count(s->last_packet_sent_id) == 1) {
        assert(s->sent_packets[s->last_packet_sent_id].next_packet_delay == -1);
        s->sent_packets[s->last_packet_sent_id].next_packet_delay = processing_packet_delay;
    }
    
    // If we already received the (previous) last packet latency, we inject it now into the predictor
    if (s->arrival_of_last_packet.packet_ID != -1) {
        assert(s->arrival_of_last_packet.packet_ID == s->last_packet_sent_id);
        assert(s->arrival_of_last_packet.travel_end_time > 0);

        double const travel_end_time = s->arrival_of_last_packet.travel_end_time;
        feed_packet_to_predictor(s, lp, s->arrival_of_last_packet.packet_ID, travel_end_time);
        s->sent_packets.erase(s->arrival_of_last_packet.packet_ID);
        s->arrival_of_last_packet.packet_ID = -1;
    }
}

static void terminal_dally_commit(terminal_state * s,
		tw_bf * bf, 
		terminal_dally_message * msg, 
        tw_lp * lp)
{

    switch (msg->type) {
        case T_GENERATE:
            if(bf->c10) {  // if the packet was sent as a prediction, store the prediction in memory
                assert(dally_surrogate_configured);
                auto start = (struct packet_start) {
                    .packet_ID = msg->packet_ID,
                    .dest_terminal_lpid = msg->dest_terminal_lpid,
                    .dfdally_dest_terminal_id = msg->dfdally_dest_terminal_id,
                    .travel_start_time = msg->travel_start_time,
                    .workload_injection_time = msg->msg_start_time,
                    .processing_packet_delay = -1,
                    .packet_size = msg->packet_size,
                    .is_there_another_pckt_in_queue = msg->is_there_another_pckt_in_queue
                };

                // Saving
                auto end = (struct packet_end) {
                    .travel_end_time = msg->travel_end_time,
                    .next_packet_delay = msg->saved_next_packet_delay,
                };
                packet_latency_save_to_file(s->terminal_id, &start, &end, is_dally_surrogate_on, true);

                // If we had latency info for the last packet transmitted, then we have to store it into memory and clean the variable
                if (s->arrival_of_last_packet.packet_ID != -1) {
                    assert(s->arrival_of_last_packet.packet_ID == s->last_packet_sent_id);
                    assert(s->arrival_of_last_packet.travel_end_time > 0);

                    auto sent = s->sent_packets[s->arrival_of_last_packet.packet_ID];

                    struct packet_end end = {
                        .travel_end_time = s->arrival_of_last_packet.travel_end_time,
                        .next_packet_delay = -1,
                    };

                    packet_latency_save_to_file(s->terminal_id, &sent.start, &end, is_dally_surrogate_on, false);

                    s->sent_packets.erase(s->arrival_of_last_packet.packet_ID);
                    s->arrival_of_last_packet.packet_ID = -1;
                }

            // If the packet info is to be stored in memory to compute terminal delay
            } else {
                terminal_commit_packet_generate(s, bf, msg, lp);
            }

            assert(msg->packet_ID - 1 == s->last_packet_sent_id);
            s->last_packet_sent_id = msg->packet_ID;
        break;

        case T_ARRIVE:
            if (OUTPUT_END_END_LATENCIES) {
                if (msg->message_id % OUTPUT_LATENCY_MODULO == 0) {
                    int written1;
                    char end_end_filename[128];
                    written1 = sprintf(end_end_filename, "end-to-end-latency-hops");
                    end_end_filename[written1] = '\0';

                    char latency[32];
                    int written;
                    tw_stime lat = msg->travel_end_time-msg->travel_start_time;
                    written = sprintf(latency, "%d %.5f %d\n",msg->app_id, msg->travel_end_time-msg->travel_start_time,msg->my_N_hop);
                    lp_io_write(lp->gid, end_end_filename, written, latency);
                }
            }
        break;

        case T_ARRIVE_PREDICTED:
        break;
        
        case T_SEND:
            if (freeze_network_on_switch) {
                if (bf->c16 && s->is_pending_local_send.count(msg->packet_ID) == 1) {
                    s->is_pending_local_send.erase(msg->packet_ID);
                }
            }
        break;
        
        case T_BUFFER:
        break;
    
        case T_BANDWIDTH:
            if(msg->rc_is_qos_set == 1) {
                free(msg->rc_qos_data);
                free(msg->rc_qos_status);
                msg->rc_is_qos_set = 0;
            }
        break;
    
        case T_NOTIFY:
            if(msg->notify_type == NOTIFY_LATENCY) {
                assert(lp->gid == msg->src_terminal_id);
                assert(s->terminal_id == msg->dfdally_src_terminal_id);
                uint64_t packet_ID = msg->packet_ID;

                if (s->sent_packets.count(packet_ID) == 1) { // packet_ID is in s->sent_packets
                    auto sent = s->sent_packets[packet_ID];
                    assert(msg->travel_end_time > sent.start.travel_start_time);
                    if (packet_ID == s->last_packet_sent_id) { // packet_ID is last, we cannot compute the next_packet_delay
                        assert(s->arrival_of_last_packet.packet_ID == -1);
                        s->arrival_of_last_packet.packet_ID = packet_ID;
                        s->arrival_of_last_packet.travel_end_time = msg->travel_end_time;
                    } else {
                        feed_packet_to_predictor(s, lp, packet_ID, msg->travel_end_time);
                        s->sent_packets.erase(packet_ID);
                    }
                }
            }
        break;

#if ALWAYS_DETERMINISTIC_NETWORK
        case T_VACUOUS_EVENT:
        break;
#endif /* ALWAYS_DETERMINISTIC_NETWORK */

        default:
            printf("\n LP %d Terminal message type not supported %d ", (int)lp->gid, msg->type);
            tw_error(TW_LOC, "Msg type not supported");
    }
}

static void router_dally_commit(router_state * s,
		tw_bf * bf, 
		terminal_dally_message * msg, 
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

    if(msg->type == R_SEND)
    {
        if (bf->c1 == 0) { //did the R_SEND event actually complete a send?
            if (OUTPUT_PORT_PORT_LATENCIES)
            {
                if (msg->message_id % OUTPUT_LATENCY_MODULO == 0) {
                    int written1;
                    char port_port_filename[128];
                    written1 = sprintf(port_port_filename, "port-to-port-latencies");
                    port_port_filename[written1] = '\0';

                    char latency[32];
                    int written;
                    written = sprintf(latency, "%d %.5f\n",msg->app_id, msg->this_router_ptp_latency);
                    lp_io_write(lp->gid, port_port_filename, written, latency);
                }
            }
        }
    }

    if (msg->type == R_SNAPSHOT)
    {
        if (OUTPUT_SNAPSHOT == 1 && msg->packet_ID < num_snapshots)
        {
            char snapshot_line[8192];
            int written;

            written = sprintf(snapshot_line, "%.4f, %d, ",snapshot_times[msg->packet_ID],s->router_id);

            for(int i = 0; i < s->params->radix; i++)
            {
                for(int j = 0; j < s->params->num_vcs; j++)
                {
                    int snapshot_array_i = (i * s->params->num_vcs) + j;
                    int this_vc_snapshot_data = s->snapshot_data[msg->packet_ID][snapshot_array_i];
                    written += sprintf(snapshot_line+written, "%d, ", this_vc_snapshot_data);
                }
            }
            assert(written <= 8192);
            assert(snapshot_line[written - 2] == ',');
            snapshot_line[written - 2] = '\n';  // Replacing ',' for new line
            written -= 1;

            lp_io_write(lp->gid, snapshot_filename, written, snapshot_line);
        }
    }
}

/* initialize a dragonfly compute node terminal */
static void terminal_dally_init( terminal_state * s, tw_lp * lp )
{
    s->packet_gen = 0;
    s->packet_fin = 0;
    s->total_gen_size = 0;
    s->is_monitoring_bw = 0;

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

    const dragonfly_param * p = s->params;

    int num_qos_levels = s->params->num_qos_levels;
    int num_lps = codes_mapping_get_lp_count(lp_group_name, 1, LP_CONFIG_NM_TERM,
            s->anno, 0);

    s->terminal_id = codes_mapping_get_lp_relative_id(lp->gid, 0, 0);
    s->router_id = (unsigned int*)calloc(p->num_rails, sizeof(unsigned int));
    s->router_lp = (tw_lpid*)calloc(p->num_rails, sizeof(tw_lpid));

    s->connMan = netMan.get_connection_manager_for_terminal(s->terminal_id);


    for(i = 0; i < p->num_rails; i++)
    {
        s->router_id[i] = dfdally_get_assigned_router_id_from_terminal(s->params,s->terminal_id,i);
        num_routers_per_mgrp = codes_mapping_get_lp_count (lp_group_name, 1, "modelnet_dragonfly_dally_router",
            NULL, 0);
        codes_mapping_get_lp_id(lp_group_name, LP_CONFIG_NM_ROUT, NULL, 1, s->router_id[i] / num_routers_per_mgrp, s->router_id[i] % num_routers_per_mgrp, &s->router_lp[i]);
    }

    s->terminal_available_time = (tw_stime*)calloc(p->num_rails, sizeof(tw_stime));
    s->packet_counter = 0;
    s->min_latency = INT_MAX;
    s->max_latency = 0;  

    s->link_traffic=(uint64_t*)calloc(p->num_rails, sizeof(uint64_t));
    s->finished_msgs = 0;
    s->finished_chunks = 0;
    s->finished_packets = 0;
    s->total_time = 0.0;
    s->total_msg_size = 0;

    s->stalled_chunks = (unsigned long*)calloc(p->num_rails, sizeof(uint64_t));
    s->total_chunks = (unsigned long*)calloc(p->num_rails, sizeof(uint64_t));

    s->injected_chunks = 0;
    s->ejected_chunks = 0;

    s->busy_time = (tw_stime*)calloc(p->num_rails, sizeof(tw_stime));

    s->fwd_events = 0;
    s->rev_events = 0;

    s->workloads_finished_flag = 0;

    rc_stack_create(&s->st);
    rc_stack_create(&s->cc_st);
    s->vc_occupancy = (int**)calloc(p->num_rails, sizeof(int*)); //1 vc times the number of qos levels
    s->last_buf_full = (tw_stime*)calloc(p->num_rails, sizeof(tw_stime));

    s->terminal_length = (int**)calloc(p->num_rails, sizeof(int*)); //1 vc times number of qos levels
    
    for(i = 0; i < p->num_rails; i++)
    {
        s->vc_occupancy[i]= (int*)calloc(num_qos_levels, sizeof(int));
        s->terminal_length[i]= (int*)calloc(num_qos_levels, sizeof(int));
    }

    s->in_send_loop = (int*)calloc(p->num_rails, sizeof(int));
    s->issueIdle = (int*)calloc(p->num_rails, sizeof(int));

    s->rank_tbl = qhash_init(dragonfly_rank_hash_compare, dragonfly_hash_func, DFLY_HASH_TABLE_SIZE);
    s->terminal_msgs = 
        (terminal_dally_message_list***)calloc(p->num_rails, sizeof(terminal_dally_message_list**));
    s->terminal_msgs_tail = 
        (terminal_dally_message_list***)calloc(p->num_rails, sizeof(terminal_dally_message_list**));

    s->qos_status = (int**)calloc(p->num_rails, sizeof(int*));
    s->qos_data = (int**)calloc(p->num_rails, sizeof(int*));

    for(i = 0; i < p->num_rails; i++)
    {
        s->in_send_loop[i] = 0;
        s->terminal_msgs[i] = (terminal_dally_message_list**)calloc(num_qos_levels, sizeof(terminal_dally_message_list*));
        s->terminal_msgs_tail[i] = (terminal_dally_message_list**)calloc(num_qos_levels, sizeof(terminal_dally_message_list*));

        for(int j = 0; j < num_qos_levels; j++)
        {
            s->terminal_msgs[i][j] = NULL;
            s->terminal_msgs_tail[i][j] = NULL;
        }

        /* Whether the virtual channel group is active or over-bw*/
        s->qos_status[i] = (int*)calloc(num_qos_levels, sizeof(int));

        /* How much data has been transmitted on the virtual channel group within
            * the window */
        s->qos_data[i] = (int*)calloc(num_qos_levels, sizeof(int));
        for(int j = 0; j < num_qos_levels; j++)
        {
            s->qos_data[i][j] = 0;
            s->qos_status[i][j] = Q_ACTIVE;
        }
    }
    s->last_qos_lvl = (int*)calloc(p->num_rails, sizeof(int));


    s->ross_sample.busy_time_sample = (tw_stime*)calloc(s->params->num_rails, sizeof(tw_stime));
    s->busy_time_ross_sample = (tw_stime*)calloc(s->params->num_rails, sizeof(tw_stime));
    s->busy_time_sample = (tw_stime*)calloc(s->params->num_rails, sizeof(tw_stime));

        /*if(s->terminal_id == 0)
        {
            char term_bw_log[64];
            sprintf(term_bw_log, "terminal-bw-tracker");
            dragonfly_term_bw_log = fopen(term_bw_log, "w");
            fprintf(dragonfly_term_bw_log, "\n term-id time-stamp port-id busy-time");
        }*/

    s->local_congestion_controller = NULL;
    if (g_congestion_control_enabled) {
        s->local_congestion_controller = (tlc_state*)calloc(1,sizeof(tlc_state));
        cc_terminal_local_controller_init(s->local_congestion_controller, lp, s->terminal_id, &s->workloads_finished_flag);
    }

    // This doesn't allocate any memory, it calls the constructor on the
    // previously allocated memory (by ROSS)
    // In the future calling the constructor could be done with:
    // std::construct_at, for now this syntax suffices and works
    // (see https://en.cppreference.com/w/cpp/memory/construct_at)
    s->last_packet_sent_id = -1;
    s->arrival_of_last_packet.packet_ID = -1;
    s->arrival_of_last_packet.travel_end_time = -1;
    new (&s->sent_packets) map<uint64_t, struct packet_sent>();
    new (&s->is_pending_local_send) set<uint64_t>();
    new (&s->remaining_sz_packets) map<struct packet_id, uint32_t>();
    new (&s->zombies) set<struct packet_id>();
    s->frozen_state = NULL;

    // alloc'ing memory for predictor, calling initiliazer for predictor
    s->predictor_data = NULL;
    if (terminal_predictor != NULL) {
        if (terminal_predictor->predictor_data_sz > 0) {
            s->predictor_data = calloc(1, terminal_predictor->predictor_data_sz);
        }
        terminal_predictor->init(s->predictor_data, lp, s->terminal_id);
    }
    s->last_in_queue_time = 0;
    return;
}

/* sets up the router virtual channels, global channels, 
 * local channels, compute node channels */
static void router_dally_init(router_state * r, tw_lp * lp)
{
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

    num_routers_per_mgrp = codes_mapping_get_lp_count (lp_group_name, 1, "modelnet_dragonfly_dally_router",
            NULL, 0);
    int num_grp_reps = codes_mapping_get_group_reps(lp_group_name);
    if(p->total_routers != num_grp_reps * num_routers_per_mgrp)
        tw_error(TW_LOC, "\n Config error: num_routers specified %d total routers computed in the network %d "
                "does not match with repetitions * dragonfly_router %d  ",
                p->num_routers, p->total_routers, num_grp_reps * num_routers_per_mgrp);

    r->router_id = codes_mapping_get_lp_relative_id(lp->gid, 0, 0);
    r->plane_id = r->router_id / p->num_routers_per_plane;
    r->group_id=r->router_id/p->num_routers;
    
    char rtr_bw_log[128];
    sprintf(rtr_bw_log, "router-bw-tracker-%lu", g_tw_mynode);

    if(dragonfly_rtr_bw_log == NULL && ROUTER_BW_LOG)
    {
        dragonfly_rtr_bw_log = fopen(rtr_bw_log, "w+");

        fprintf(dragonfly_rtr_bw_log, "\n router-id time-stamp port-id qos-level bw-consumed qos-status qos-data busy-time");
    }
   //printf("\n Local router id %d global id %d ", r->router_id, lp->gid);

    r->is_monitoring_bw = 0;
    r->fwd_events = 0;
    r->rev_events = 0;
    r->ross_rsample.fwd_events = 0;
    r->ross_rsample.rev_events = 0;


    int num_qos_levels = p->num_qos_levels;

    r->connMan = netMan.get_connection_manager_for_router(r->router_id);
    
    r->vc_max_sizes = (int*)calloc(p->radix, sizeof(int));
    for(int i = 0; i < p->radix; i++)
    {
        ConnectionType conn_type = r->connMan.get_port_type(i);
        if (conn_type == CONN_LOCAL)
            r->vc_max_sizes[i] = p->local_vc_size;
        else if (conn_type == CONN_GLOBAL)
            r->vc_max_sizes[i] = p->global_vc_size;
        else
            r->vc_max_sizes[i] = p->cn_vc_size;
    }

    r->port_bandwidths = (double*)calloc(p->radix, sizeof(double));
    for(int i = 0; i < p->radix; i++)
    {
        ConnectionType conn_type = r->connMan.get_port_type(i);
        if (conn_type == CONN_LOCAL)
            r->port_bandwidths[i] = p->local_bandwidth;
        else if (conn_type == CONN_GLOBAL)
            r->port_bandwidths[i] = p->global_bandwidth;
        else
            r->port_bandwidths[i] = p->cn_bandwidth;
    }

    r->global_channel = (int*)calloc(p->num_global_channels, sizeof(int));
    r->next_output_available_time = (tw_stime*)calloc(p->radix, sizeof(tw_stime));
    r->link_traffic = (int64_t*)calloc(p->radix, sizeof(int64_t));
    r->link_traffic_sample = (int64_t*)calloc(p->radix, sizeof(int64_t));

    r->stalled_chunks = (unsigned long*)calloc(p->radix, sizeof(unsigned long));
    r->total_chunks = (unsigned long*)calloc(p->radix, sizeof(unsigned long));

    r->workloads_finished_flag = 0;

    r->vc_occupancy = (int**)calloc(p->radix , sizeof(int*));
    r->in_send_loop = (int*)calloc(p->radix, sizeof(int));
    r->qos_data = (int**)calloc(p->radix, sizeof(int*));
    r->last_qos_lvl = (int*)calloc(p->radix, sizeof(int));
    r->qos_status = (int**)calloc(p->radix, sizeof(int*));
    r->pending_msgs = 
        (terminal_dally_message_list***)calloc((p->radix), sizeof(terminal_dally_message_list**));
    r->pending_msgs_tail = 
        (terminal_dally_message_list***)calloc((p->radix), sizeof(terminal_dally_message_list**));
    r->queued_msgs = 
        (terminal_dally_message_list***)calloc(p->radix, sizeof(terminal_dally_message_list**));
    r->queued_msgs_tail = 
        (terminal_dally_message_list***)calloc(p->radix, sizeof(terminal_dally_message_list**));
    r->queued_count = (int*)calloc(p->radix, sizeof(int));
    r->last_buf_full = (tw_stime*)calloc(p->radix, sizeof(tw_stime*));
    r->busy_time = (tw_stime*)calloc(p->radix, sizeof(tw_stime));
    r->busy_time_sample = (tw_stime*)calloc(p->radix, sizeof(tw_stime));

    /* set up for ROSS stats sampling */
    r->link_traffic_ross_sample = (int64_t*)calloc(p->radix, sizeof(int64_t));
    r->busy_time_ross_sample = (tw_stime*)calloc(p->radix, sizeof(tw_stime));
    if (g_st_model_stats)
        lp->model_types->mstat_sz = sizeof(tw_lpid) + (sizeof(int64_t) + sizeof(tw_stime)) * p->radix;
    if (g_st_use_analysis_lps && g_st_model_stats)
        lp->model_types->sample_struct_sz = sizeof(struct dfly_router_sample) + (sizeof(tw_stime) + sizeof(int64_t)) * p->radix;
    r->ross_rsample.busy_time = (tw_stime*)calloc(p->radix, sizeof(tw_stime));
    r->ross_rsample.link_traffic_sample = (int64_t*)calloc(p->radix, sizeof(int64_t));

    rc_stack_create(&r->st);
    rc_stack_create(&r->cc_st);

    for(int i=0; i < p->radix; i++)
    {
       // Set credit & router occupancy
        r->last_buf_full[i] = 0.0;
        r->busy_time[i] = 0.0;
        r->busy_time_sample[i] = 0.0;
        r->next_output_available_time[i]=0;
        r->last_qos_lvl[i] = 0;
        r->link_traffic[i]=0;
        r->link_traffic_sample[i] = 0;
        r->queued_count[i] = 0;    
        r->in_send_loop[i] = 0;
        r->vc_occupancy[i] = (int*)calloc(p->num_vcs, sizeof(int));
    //    printf("\n Number of vcs %d for radix %d ", p->num_vcs, p->radix);
        r->pending_msgs[i] = (terminal_dally_message_list**)calloc(p->num_vcs, 
            sizeof(terminal_dally_message_list*));
        r->pending_msgs_tail[i] = (terminal_dally_message_list**)calloc(p->num_vcs,
            sizeof(terminal_dally_message_list*));
        r->queued_msgs[i] = (terminal_dally_message_list**)calloc(p->num_vcs,
            sizeof(terminal_dally_message_list*));
        r->queued_msgs_tail[i] = (terminal_dally_message_list**)calloc(p->num_vcs,
            sizeof(terminal_dally_message_list*));
        r->qos_status[i] = (int*)calloc(num_qos_levels, sizeof(int));
        r->qos_data[i] = (int*)calloc(num_qos_levels, sizeof(int));
        for(int j = 0; j < num_qos_levels; j++)
        {
            r->qos_status[i][j] = Q_ACTIVE;
            r->qos_data[i][j] = 0;
        }
        for(int j = 0; j < p->num_vcs; j++) 
        {
            r->pending_msgs[i][j] = NULL;
            r->pending_msgs_tail[i][j] = NULL;
            r->queued_msgs[i][j] = NULL;
            r->queued_msgs_tail[i][j] = NULL;
        }
    }

    if (!r->connMan.check_is_solidified())
        tw_error(TW_LOC, "Connection Manager not solidified - should be performed after loading topology");

    if (g_congestion_control_enabled) {
        r->local_congestion_controller = (rlc_state*)calloc(1,sizeof(rlc_state));
        cc_router_local_controller_init(r->local_congestion_controller, lp, p->total_terminals, r->router_id, p->radix, p->num_vcs, r->vc_max_sizes, r->port_bandwidths, &r->workloads_finished_flag);
        
        vector<Connection> terminal_out_conns = r->connMan.get_connections_by_type(CONN_TERMINAL);
        vector<Connection>::iterator it = terminal_out_conns.begin();
        for(; it != terminal_out_conns.end(); it++)
        {
            cc_router_local_controller_add_output_port(r->local_congestion_controller, it->port);
        }

        // vector<Connection> local_conns = r->connMan.get_connections_by_type(CONN_LOCAL);
        // it = local_conns.begin();
        // for(; it != local_conns.end(); it++)
        // {
        //     cc_router_local_controller_add_output_port(r->local_congestion_controller, it->port);
        // }

        // vector<Connection> global_conns = r->connMan.get_connections_by_type(CONN_GLOBAL);
        // it = global_conns.begin();
        // for(; it != global_conns.end(); it++)
        // {
        //     cc_router_local_controller_add_output_port(r->local_congestion_controller, it->port);
        // }

    }

    r->snapshot_data = NULL;
    if (num_snapshots) {
        r->snapshot_data = (int**)calloc(num_snapshots, sizeof(int*));
        for(int i = 0; i < num_snapshots; i++)
        {
            r->snapshot_data[i] = (int*)calloc(r->params->num_vcs * r->params->radix, sizeof(int)); //capturing VC occupancies of each port
        }
        router_send_snapshot_events(r, lp);
    }

    //Xin: msg counters for apps 
    r->agg_link_traffic = NULL;
    r->agg_busy_time = NULL;
    if(p->counting_bool > 0)
    {   
        r->agg_link_traffic = (int64_t **) calloc(p->counting_windows, sizeof(int64_t *));
        r->agg_busy_time = (tw_stime **) malloc (p->counting_windows * sizeof(tw_stime *));

        for (int i = 0; i < p->counting_windows; ++i)
        {
            r->agg_link_traffic[i] = (int64_t*) calloc(p->radix, sizeof(int64_t));
            // r->agg_link_traffic[i] = (int64_t*) calloc(p->radix*p->num_apps, sizeof(int64_t));
            r->agg_busy_time[i] = (tw_stime*) malloc(p->radix * sizeof(tw_stime));
            for(int j = 0; j < p->radix; j++)
              r->agg_busy_time[i][j] = 0.0;
        }
    }

    return;
}	

/* dragonfly packet event reverse handler */
static void dragonfly_dally_packet_event_rc(tw_lp *sender)
{
#if ADD_NOISE == 1
	codes_local_latency_reverse(sender);
#endif
	return;
}

/* dragonfly packet event , generates a dragonfly packet on the compute node */
static tw_stime dragonfly_dally_packet_event(
        model_net_request const * req,
        uint64_t message_offset,
        uint64_t packet_size,
        tw_stime offset,
        mn_sched_params const * sched_params,
        void const * remote_event,
        void const * self_event,
        tw_lp *sender,
        int is_last_pckt,
        bool is_there_another_pckt_in_queue)
{
    (void)message_offset;
    (void)sched_params;
    tw_event * e_new;
    tw_stime xfer_to_nic_time;
    terminal_dally_message * msg;
    char* tmp_ptr;

#if ADD_NOISE == 1
    xfer_to_nic_time = codes_local_latency(sender);
#else
    xfer_to_nic_time = 0;
#endif
    //e_new = tw_event_new(sender->gid, xfer_to_nic_time+offset, sender);
    //msg = tw_event_data(e_new);
    e_new = model_net_method_event_new(sender->gid, xfer_to_nic_time+offset,
            sender, DRAGONFLY_DALLY, (void**)&msg, (void**)&tmp_ptr);
    strcpy(msg->category, req->category);
    msg->final_dest_gid = req->final_dest_lp;
    msg->total_size = req->msg_size;
    msg->sender_lp=req->src_lp;
    msg->sender_mn_lp = sender->gid;
    msg->packet_size = packet_size;
    //msg->travel_start_time = tw_now(sender);
    msg->remote_event_size_bytes = 0;
    msg->local_event_size_bytes = 0;
    msg->type = T_GENERATE;
    msg->dest_terminal_lpid = req->dest_mn_lp;
    msg->dfdally_src_terminal_id = codes_mapping_get_lp_relative_id(msg->sender_mn_lp,0,0);
    msg->dfdally_dest_terminal_id = codes_mapping_get_lp_relative_id(msg->dest_terminal_lpid,0,0);
    msg->message_id = req->msg_id;
    msg->is_pull = req->is_pull;
    msg->pull_size = req->pull_size;
    msg->magic = terminal_magic_num; 
    msg->msg_start_time = req->msg_start_time;
    msg->msg_new_mn_event = req->msg_new_mn_event;
    msg->rail_id = req->queue_offset;
    msg->app_id = req->app_id;
    msg->is_there_another_pckt_in_queue = is_there_another_pckt_in_queue;

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

static void packet_generate_predicted_rc(terminal_state * s, tw_bf * bf, terminal_dally_message * msg, tw_lp * lp)
{
    struct mn_stats * stat = model_net_find_stats(msg->category, s->dragonfly_stats_array);
    stat->send_count--;
    stat->send_bytes -= msg->packet_size;
    stat->send_time -= (1/s->params->cn_bandwidth) * msg->packet_size;

    s->last_in_queue_time = msg->saved_last_in_queue_time;

    terminal_predictor->predict_rc(s->predictor_data, lp);

    s->packet_counter--;
    s->total_gen_size -= msg->packet_size;
    s->packet_gen--;
    packet_gen--;
}

/* generates packet at the current dragonfly compute node */
static void packet_generate_predicted(terminal_state * s, tw_bf * bf, terminal_dally_message * msg, tw_lp * lp) {
    packet_gen++;
    s->packet_gen++;
    s->total_gen_size += msg->packet_size;

    assert(lp->gid != msg->dest_terminal_lpid);
    const dragonfly_param *p = s->params;

    msg->packet_ID = s->packet_counter;
    s->packet_counter++;

    // these actually don't matter because we are bypassing the network
    msg->my_N_hop = -1;
    msg->my_l_hop = -1;
    msg->my_g_hop = -1;
    msg->my_hops_cur_group = -1;

    // Using predictor to find latency
    double const processing_packet_delay = tw_now(lp) - s->last_in_queue_time;
    auto start = (struct packet_start) {
        .packet_ID = msg->packet_ID,
        .dest_terminal_lpid = msg->dest_terminal_lpid,
        .dfdally_dest_terminal_id = msg->dfdally_dest_terminal_id,
        .travel_start_time = tw_now(lp),
        .workload_injection_time = msg->msg_start_time,
        .processing_packet_delay = processing_packet_delay,
        .packet_size = msg->packet_size,
        .is_there_another_pckt_in_queue = msg->is_there_another_pckt_in_queue
    };

    struct packet_end const end = 
        terminal_predictor->predict(s->predictor_data, lp, s->terminal_id, &start);
    double const latency = end.travel_end_time - start.travel_start_time;
    double const arrival = start.travel_start_time + latency; // this is "equivalent" to end.travel_end_time
    assert(arrival >= tw_now(lp));

    // determining injection delay
    //tw_stime injection_ts;
    //if (g_congestion_control_enabled) {
    //    double bandwidth_coef = 1;
    //    if (cc_terminal_is_abatement_active(s->local_congestion_controller)) {
    //         bandwidth_coef = cc_terminal_get_current_injection_bandwidth_coef(s->local_congestion_controller);
    //    }
    //    injection_ts = bytes_to_ns(msg->packet_size, bandwidth_coef * s->params->cn_bandwidth);
    //}
    //else {
    //    injection_ts = bytes_to_ns(msg->packet_size, s->params->cn_bandwidth);
    //}
    //tw_stime const nic_ts = injection_ts;
    // The code above does a good job at limiting the speed in which packets are injected, so it produces good
    // results when running in surrogate. A good model should produce similar `nic`s to what the code above
    // does (the average predictor does just that!)
    double const nic_ts = end.next_packet_delay;
    assert(nic_ts > 0);

    // Scheduling idle event for next packet to be processed
    bool const is_from_remote = false;
    model_net_method_idle_event2(nic_ts, is_from_remote, msg->rail_id, lp);
    msg->saved_last_in_queue_time = s->last_in_queue_time;
    s->last_in_queue_time = tw_now(lp);

    // Info to be used at commit time to save into file
    msg->saved_next_packet_delay = end.next_packet_delay;
    msg->travel_start_time = tw_now(lp);
    msg->travel_end_time = arrival;

    // Sending packet directly to destination terminal
    //tw_stime const ts = 0;
    terminal_dally_message * m;
    void * remote_event;
    void const * const m_data_src = model_net_method_get_edata(DRAGONFLY_DALLY, msg);
    tw_event * const e = model_net_method_event_new(
            msg->dest_terminal_lpid, latency, lp, DRAGONFLY_DALLY, (void**)&m, &remote_event);
    memcpy(m, msg, sizeof(terminal_dally_message));
    if (msg->remote_event_size_bytes) {
        memcpy(remote_event, m_data_src, msg->remote_event_size_bytes);
    }
    m->magic = terminal_magic_num;
    m->type = T_ARRIVE_PREDICTED;
    m->src_terminal_id = lp->gid;
    m->dfdally_src_terminal_id = s->terminal_id; //m->travel_start_time = tw_now(lp);
    //m->rail_id = msg->rail_id;
    //m->vc_index = vcg;
    //m->last_hop = TERMINAL;
    m->path_type = -1;
    m->local_event_size_bytes = 0;
    m->is_intm_visited = 0;
    m->intm_grp_id = -1;
    m->intm_rtr_id = -1; //for legacy prog-adaptive
    tw_event_send(e);

    const int total_event_size = model_net_get_msg_sz(DRAGONFLY_DALLY)
        + msg->remote_event_size_bytes + msg->local_event_size_bytes;
    mn_stats* stat;
    stat = model_net_find_stats(msg->category, s->dragonfly_stats_array);
    stat->send_count++;
    stat->send_bytes += msg->packet_size;
    stat->send_time += (1/p->cn_bandwidth) * msg->packet_size;
    if(stat->max_event_size < total_event_size) {
        stat->max_event_size = total_event_size;
    }

    if(msg->local_event_size_bytes > 0)
    {
        tw_event *e_new = tw_event_new(msg->sender_lp, nic_ts, lp);
        void *m_new = tw_event_data(e_new);
        void *local_event = (char*) model_net_method_get_edata(DRAGONFLY_DALLY, msg) + msg->remote_event_size_bytes;
        memcpy(m_new, local_event, msg->local_event_size_bytes);
        tw_event_send(e_new);
    }
}

static void packet_generate_rc(terminal_state * s, tw_bf * bf, terminal_dally_message * msg, tw_lp * lp)
{
    int num_qos_levels = s->params->num_qos_levels;
    if(bf->c1)
        s->is_monitoring_bw = 0;
    
    s->total_gen_size -= msg->packet_size;
    s->packet_gen--;
    packet_gen--;
    s->packet_counter--;

    s->last_in_queue_time = msg->saved_last_in_queue_time;

    if(bf->c2)
        num_local_packets_sr--;
    if(bf->c3)
        num_local_packets_sg--;
    if(bf->c4)
        num_remote_packets--;

    int num_chunks = msg->packet_size/s->params->chunk_size;
    if(msg->packet_size < s->params->chunk_size)
        num_chunks++;

    int i;
    int vcg = 0;
    if(num_qos_levels > 1)
    {
        vcg = get_vcg_from_category(msg); 
        assert(vcg == Q_HIGH || vcg == Q_MEDIUM);
    }
    assert(vcg < num_qos_levels);

    for(i = 0; i < num_chunks; i++) {
            delete_terminal_dally_message_list(return_tail(s->terminal_msgs[msg->rail_id], s->terminal_msgs_tail[msg->rail_id], vcg));
            s->terminal_length[msg->rail_id][vcg] -= s->params->chunk_size;
    }
    if(bf->c5) {
        s->in_send_loop[msg->rail_id] = 0;
    }

    if (s->params->num_injection_queues > 1) {
        // int* scs = (int*)rc_stack_pop(s->st);
        int* iis = (int*)rc_stack_pop(s->st);
        tw_stime* bts = (tw_stime*)rc_stack_pop(s->st);

        for(int j = 0; j < s->params->num_injection_queues; j++)
        {
            s->last_buf_full[j] = bts[j];
            s->issueIdle[j] = iis[j];
            // s->stalled_chunks[j] = scs[j];
        }
        buff_time_storage_delete(bts);
        int_storage_delete(iis);
        // int_storage_delete(scs);
    }

    if (bf->c11) {
        s->issueIdle[msg->rail_id] = 0;
        if(bf->c8)
            s->last_buf_full[msg->rail_id] = msg->saved_busy_time;
    }

    struct mn_stats* stat;
    stat = model_net_find_stats(msg->category, s->dragonfly_stats_array);
    stat->send_count--;
    stat->send_bytes -= msg->packet_size;
    stat->send_time -= (1/s->params->cn_bandwidth) * msg->packet_size;
}

/* generates packet at the current dragonfly compute node */
static void packet_generate(terminal_state * s, tw_bf * bf, terminal_dally_message * msg, tw_lp * lp) {
    packet_gen++;

    s->packet_gen++;
    s->total_gen_size += msg->packet_size;
    msg->saved_processing_time = tw_now(lp);

    tw_stime ts, injection_ts, nic_ts;

    assert(lp->gid != msg->dest_terminal_lpid);
    const dragonfly_param *p = s->params;

    int total_event_size;
    uint64_t num_chunks = msg->packet_size / p->chunk_size;
    
    double cn_delay = s->params->cn_delay;

    if (msg->packet_size < s->params->chunk_size) 
        num_chunks++;

    if(msg->packet_size < s->params->chunk_size)
        cn_delay = bytes_to_ns(msg->packet_size % s->params->chunk_size, s->params->cn_bandwidth);

    int dest_router_id;
    if (s->params->num_injection_queues > 1 || netMan.is_link_failures_enabled()) {
        //get rails available: should be from rails known to not be failed
        vector< Connection > injection_connections = s->connMan.get_connections_by_type(CONN_INJECTION, false);
        if(injection_connections.size() < 1)
            tw_error(TW_LOC, "Packet Generation Failure: No non-failed injection connections available on terminal %d\n", s->terminal_id);

        vector< Connection > valid_rails;
        if (netMan.is_link_failures_enabled())
        {
            for (int i = 0; i < injection_connections.size(); i++)
            {
                int rail_id = injection_connections[i].rail_or_planar_id;
                int dest_router_id = dfdally_get_assigned_router_id_from_terminal(s->params, msg->dfdally_dest_terminal_id, rail_id);
                int src_router_id = dfdally_get_assigned_router_id_from_terminal(s->params, s->terminal_id, rail_id);
                int src_group_id = src_router_id / s->params->num_routers;
                int dest_group_id = dest_router_id / s->params->num_routers;

                if (isRoutingMinimal(routing)) {
                    if (src_group_id == dest_group_id)
                    {
                        set<Connection> valid_next_stops = netMan.get_valid_next_hops_conns(src_router_id, dest_router_id, max_hops_per_group,0); //max global hops for local group routing == 0
                        if (valid_next_stops.size() > 0)
                            valid_rails.push_back(injection_connections[i]);
                    }
                    else
                    {
                        set<Connection> valid_next_stops = netMan.get_valid_next_hops_conns(src_router_id, dest_router_id, max_hops_per_group,1); //max global hops for local group routing == 0
                        if (valid_next_stops.size() > 0)
                            valid_rails.push_back(injection_connections[i]);
                    }
                }
                else
                {
                    // if (src_group_id == dest_group_id)
                    // {
                    //     set<Connection> valid_next_stops = netMan.get_valid_next_hops_conns(src_router_id, dest_router_id, max_hops_per_group,0); //max global hops for local group routing == 0
                    //     if (valid_next_stops.size() > 0)
                    //         valid_rails.push_back(injection_connections[i]);
                    // }
                    // else
                    // {
                        set<Connection> valid_next_stops = netMan.get_valid_next_hops_conns(src_router_id, dest_router_id, max_hops_per_group,max_global_hops_nonminimal); //max global hops for local group routing == 0
                        if (valid_next_stops.size() > 0)
                            valid_rails.push_back(injection_connections[i]);
                    // }
                }
            }
            if (valid_rails.size() < 1) { 
                tw_error(TW_LOC,"Invalid Connections in Network due to link failures!\n");
                // valid_rails = injection_connections; //TODO will cause problems - deal with
            }
        }
        else {
            valid_rails = injection_connections;
        }
        
        vector< Connection > tied_rails;

        //determine rail
        Connection target_rail_connection = valid_rails[0];
        if (valid_rails.size() > 1)
        {
            bool congestion_fallback = false;

            if (s->params->rail_select == RAIL_DEDICATED) { //then attempt to inject on rail based on the injection queue from the workload
                Connection specific_injection_conn = s->connMan.get_connection_on_port(msg->rail_id, true);
                if (specific_injection_conn.port == - 1)
                    tw_error(TW_LOC, "Packet Generation Failure: No connection on specified rail\n");
                if (specific_injection_conn.is_failed)
                    congestion_fallback = true;
                else
                {
                    target_rail_connection = specific_injection_conn;
                }

            }
            
            if (s->params->rail_select == RAIL_PATH) {
                int path_lens[valid_rails.size()];
                int min_len = 99999;
                int path_tie = 0;
                int index = 0;
                vector< Connection >::iterator it = valid_rails.begin();
                for(; it != valid_rails.end(); it++)
                {
                    int rail_id = it->rail_or_planar_id;
                    int dest_router_id = dfdally_get_assigned_router_id_from_terminal(s->params, msg->dfdally_dest_terminal_id, rail_id);
                    int src_router_id = dfdally_get_assigned_router_id_from_terminal(s->params, s->terminal_id, rail_id);

                    path_lens[index] = netMan.get_shortest_dist_between_routers(src_router_id, dest_router_id);
                    if (path_lens[index] < min_len) {
                        min_len = path_lens[index];
                        target_rail_connection = *it;
                        tied_rails.clear();
                        tied_rails.push_back(*it);
                        path_tie = 0;
                    }
                    else if (path_lens[rail_id] == min_len)
                    {
                        path_tie = 1;
                        tied_rails.push_back(*it);
                    }
                    index++;
                }

                if(path_tie == 1)
                {
                    int rand_sel = tw_rand_integer(lp->rng, 0, tied_rails.size()-1);
                    msg->num_rngs++;
                    target_rail_connection = tied_rails[rand_sel];
                }
            }

            if(s->params->rail_select == RAIL_RAND) {
                int target_rail_sel = tw_rand_integer(lp->rng, 0, valid_rails.size()-1);
                target_rail_connection = valid_rails[target_rail_sel];
                msg->num_rngs++;
            }

            if(s->params->rail_select == RAIL_CONGESTION || congestion_fallback) {
                int min_score = INT_MAX;
                int path_tie = 0;

                vector<Connection>::iterator it = valid_rails.begin();
                for(; it != valid_rails.end(); it++)
                {
                    int sum = 0;
                    for(int j = 0; j < p->num_qos_levels; j++)
                    {
                        int port_no = it->port;
                        sum += s->vc_occupancy[port_no][j];
                    }
                    if (sum < min_score) {
                        min_score = sum;
                        target_rail_connection = *it;
                        tied_rails.clear();
                        tied_rails.push_back(*it);
                        path_tie = 0;
                    }
                    else if (sum == min_score)
                    {
                        path_tie = 1;
                        tied_rails.push_back(*it);
                    }
                }
                if (path_tie == 1)
                {
                    int rand_sel = tw_rand_integer(lp->rng, 0, tied_rails.size() -1);
                    msg->num_rngs++;
                    target_rail_connection = tied_rails[rand_sel];
                }
            }
        }
        msg->rail_id = target_rail_connection.rail_or_planar_id;

        dest_router_id = dfdally_get_assigned_router_id_from_terminal(p, msg->dfdally_dest_terminal_id, msg->rail_id);
    } else {
        dest_router_id = dfdally_get_assigned_router_id_from_terminal(p, msg->dfdally_dest_terminal_id, 0);
    }
    int dest_grp_id = dest_router_id / s->params->num_routers;
    int src_grp_id = s->router_id[msg->rail_id] / s->params->num_routers; 

    if(src_grp_id == dest_grp_id)
    {
        if(dest_router_id == s->router_id[msg->rail_id])
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
    
    msg->packet_ID = s->packet_counter;
    s->packet_counter++;
    msg->my_N_hop = 0;
    msg->my_l_hop = 0;
    msg->my_g_hop = 0;
    msg->my_hops_cur_group = 0;

    //assert(tw_now(lp) == msg->travel_start_time);
    // This in here is NOT next_packet_delay but processing packet delay!!
    msg->saved_next_packet_delay = tw_now(lp) - s->last_in_queue_time;
    msg->saved_last_in_queue_time = s->last_in_queue_time;
    s->last_in_queue_time = tw_now(lp);

    //qos stuff
    int num_qos_levels = s->params->num_qos_levels;
    int vcg = 0;

    if(num_qos_levels > 1)
    {
        if(s->is_monitoring_bw == 0)
        {
            bf->c1 = 1;
            /* Issue an event on both terminal and router to monitor bandwidth */
            tw_stime bw_ts = bw_reset_window + gen_noise(lp, &msg->num_rngs);
            terminal_dally_message * m;
            tw_event * e = model_net_method_event_new(lp->gid, bw_ts, lp, DRAGONFLY_DALLY,
                (void**)&m, NULL);
            m->type = T_BANDWIDTH; 
            m->magic = terminal_magic_num;
            s->is_monitoring_bw = 1;
            tw_event_send(e);
        }
        vcg = get_vcg_from_category(msg);
        assert(vcg == Q_HIGH || vcg == Q_MEDIUM);
    }
    assert(vcg < num_qos_levels);


    for(int i = 0; i < num_chunks; i++)
    {
        terminal_dally_message_list *cur_chunk = (terminal_dally_message_list*)calloc(1,
        sizeof(terminal_dally_message_list));
        msg->origin_router_id = s->router_id[msg->rail_id];
        init_terminal_dally_message_list(cur_chunk, msg);
    
        if(msg->remote_event_size_bytes + msg->local_event_size_bytes > 0) {
        cur_chunk->event_data = (char*)calloc(1,
            msg->remote_event_size_bytes + msg->local_event_size_bytes);
        }
        
        void * m_data_src = model_net_method_get_edata(DRAGONFLY_DALLY, msg);
        if (msg->remote_event_size_bytes){
        memcpy(cur_chunk->event_data, m_data_src, msg->remote_event_size_bytes);
        }
        if (msg->local_event_size_bytes){ 
        m_data_src = (char*)m_data_src + msg->remote_event_size_bytes;
        memcpy((char*)cur_chunk->event_data + msg->remote_event_size_bytes, 
            m_data_src, msg->local_event_size_bytes);
        }

        cur_chunk->msg.rail_id = msg->rail_id;
        cur_chunk->msg.output_chan = vcg;
        cur_chunk->msg.chunk_id = i;
        cur_chunk->msg.origin_router_id = s->router_id[msg->rail_id];
        append_to_terminal_dally_message_list(s->terminal_msgs[msg->rail_id], s->terminal_msgs_tail[msg->rail_id],
        vcg, cur_chunk);
        s->terminal_length[msg->rail_id][vcg] += s->params->chunk_size;
    }
    
    double bandwidth_coef = 1;
    if (g_congestion_control_enabled) {
        if (cc_terminal_is_abatement_active(s->local_congestion_controller)) {
            bandwidth_coef = cc_terminal_get_current_injection_bandwidth_coef(s->local_congestion_controller);
        }
        injection_ts = bytes_to_ns(msg->packet_size, bandwidth_coef * s->params->cn_bandwidth);
    }
    else {
        injection_ts = bytes_to_ns(msg->packet_size, s->params->cn_bandwidth);
    }
    nic_ts = injection_ts;



    if (s->params->num_injection_queues > 1) {
        tw_stime *bts = buff_time_storage_create(s); //mallocs space to push onto the rc stack -- free'd in rc
        int *iis = int_storage_create(s);
        // int *scs = int_storage_create(s);

        //TODO: Inspect this and verify that we should be looking at each port always
        for(int j=0; j<s->params->num_injection_queues; j++){
            bts[j] = s->last_buf_full[j];
            iis[j] = s->issueIdle[j];
            // scs[j] = s->stalled_chunks[j];
            if(s->terminal_length[j][vcg] < s->params->cn_vc_size && s->issueIdle[j] == 0)
            {
                model_net_method_idle_event2(nic_ts, 0, j, lp);
            }
            else
            {
                s->issueIdle[j] = 1;
                // s->stalled_chunks[j]++;
                if(s->last_buf_full[j] == 0.0)
                {
                    s->last_buf_full[j] = tw_now(lp);;
                }
            }
        }
        rc_stack_push(lp, bts, buff_time_storage_delete, s->st);
        rc_stack_push(lp, iis, int_storage_delete, s->st);
        // rc_stack_push(lp, scs, int_storage_delete, s->st);
    }
    else {
        if (s->terminal_length[msg->rail_id][vcg] < s->params->cn_vc_size) {
            model_net_method_idle_event2(nic_ts, 0, msg->rail_id, lp);
        } else {
            bf->c11 = 1;
            s->issueIdle[msg->rail_id] = 1;
            if (s->last_buf_full[msg->rail_id] == 0.0) {
                bf->c8 = 1;
                msg->saved_busy_time = s->last_buf_full[msg->rail_id];
                s->last_buf_full[msg->rail_id] = tw_now(lp);
            }
        }
    }

    // if(s->terminal_length[msg->rail_id][vcg] < s->params->cn_vc_size) {
    //     model_net_method_idle_event2(nic_ts, 0, msg->rail_id, lp);
    // } else {
    //     bf->c11 = 1;
    //     s->issueIdle[msg->rail_id] = 1;
    //     s->stalled_chunks[msg->rail_id]++;

    //     //this block was missing from when QOS was added - readded 5-21-19
    //     if(s->last_buf_full[msg->rail_id] == 0)
    //     {
    //         bf->c8 = 1;
    //         msg->saved_busy_time = s->last_buf_full[msg->rail_id];
    //         s->last_buf_full[msg->rail_id] = tw_now(lp);
    //     }
    // }
    
    if(s->in_send_loop[msg->rail_id] == 0) {
        bf->c5 = 1;
        ts = 0;
        terminal_dally_message *m;
        tw_event* e = model_net_method_event_new(lp->gid, ts + gen_noise(lp, &msg->num_rngs), lp, DRAGONFLY_DALLY, 
        (void**)&m, NULL);
        m->rail_id = msg->rail_id;
        m->type = T_SEND;
        m->magic = terminal_magic_num;
        s->in_send_loop[msg->rail_id] = 1;
        tw_event_send(e);
    }

    total_event_size = model_net_get_msg_sz(DRAGONFLY_DALLY) + 
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

static void packet_send_rc(terminal_state * s, tw_bf * bf, terminal_dally_message * msg, tw_lp * lp)
{
    int num_qos_levels = s->params->num_qos_levels;

    if(msg->qos_reset1)
        s->qos_status[msg->rail_id][0] = Q_ACTIVE;
    if(msg->qos_reset2)
        s->qos_status[msg->rail_id][1] = Q_ACTIVE;
    
    if(msg->last_saved_qos >= 0)
        s->last_qos_lvl[msg->rail_id] = msg->last_saved_qos;

    if(bf->c1) {
        s->in_send_loop[msg->rail_id] = msg->saved_send_loop;
        s->stalled_chunks[msg->rail_id]--;
        if(bf->c3)
            s->last_buf_full[msg->rail_id] = msg->saved_busy_time;
    
        return;
    }
    
    int vcg = msg->saved_vc;
    s->terminal_available_time[msg->rail_id] = msg->saved_available_time;

    s->terminal_length[msg->rail_id][vcg] += s->params->chunk_size;
    /*TODO: MM change this to the vcg */
    s->vc_occupancy[msg->rail_id][vcg] -= s->params->chunk_size;
    s->link_traffic[msg->rail_id]-=s->params->chunk_size;
    s->total_chunks[msg->rail_id]--;
    s->injected_chunks--; 

    terminal_dally_message_list* cur_entry = (terminal_dally_message_list *)rc_stack_pop(s->st);
    
    cur_entry->msg.travel_start_time = msg->saved_avg_time;
    int data_size = s->params->chunk_size;
    if(cur_entry->msg.packet_size < s->params->chunk_size)
        data_size = cur_entry->msg.packet_size % s->params->chunk_size;

    s->qos_data[msg->rail_id][vcg] -= data_size;

    prepend_to_terminal_dally_message_list(s->terminal_msgs[msg->rail_id], 
            s->terminal_msgs_tail[msg->rail_id], vcg, cur_entry);
    
    if(bf->c4) {
        s->in_send_loop[msg->rail_id] = msg->saved_send_loop;
    }
    if(bf->c5)
    {
        s->issueIdle[msg->rail_id] = 1;
        if(bf->c6)
        {
            s->busy_time[msg->rail_id] = msg->saved_total_time;
            s->last_buf_full[msg->rail_id] = msg->saved_busy_time;
            s->busy_time_sample[msg->rail_id] = msg->saved_sample_time;
            s->ross_sample.busy_time_sample[msg->rail_id] = msg->saved_sample_time;
            s->busy_time_ross_sample[msg->rail_id] = msg->saved_busy_time_ross;
        }
    }
    return;
}
/* sends the packet from the current dragonfly compute node to the attached router */
static void packet_send(terminal_state * s, tw_bf * bf, terminal_dally_message * msg, tw_lp * lp) 
{
  
    tw_stime ts;
    tw_event *e;
    terminal_dally_message *m;
    tw_lpid router_id;
    int vcg = 0;
    int num_qos_levels = s->params->num_qos_levels;
    
    msg->last_saved_qos = -1;
    msg->qos_reset1 = -1;
    msg->qos_reset2 = -1;
    msg->saved_send_loop = s->in_send_loop[msg->rail_id];

    vcg = get_next_vcg(s, bf, msg, lp);
    
    /* For a terminal to router connection, there would be as many VCGs as number
    * of VCs*/

    if(vcg == -1) {
        bf->c1 = 1;
        s->in_send_loop[msg->rail_id] = 0;
        s->stalled_chunks[msg->rail_id]++;
        if(!s->last_buf_full[msg->rail_id])
        {
            bf->c3 = 1;
            msg->saved_busy_time = s->last_buf_full[msg->rail_id];
            s->last_buf_full[msg->rail_id] = tw_now(lp); 
        }
        return;
    }

    msg->saved_vc = vcg;
    terminal_dally_message_list* cur_entry = s->terminal_msgs[msg->rail_id][vcg];
    int data_size = s->params->chunk_size;
    uint64_t num_chunks = cur_entry->msg.packet_size/s->params->chunk_size;
    if(cur_entry->msg.packet_size < s->params->chunk_size)
        num_chunks++;
    msg->saved_avg_time = cur_entry->msg.travel_start_time;  // reusing field saved_avg_time. It is only used in another event handler path (arrive). So, no interruptions here
    cur_entry->msg.travel_start_time = tw_now(lp);

    double bandwidth_coef = 1;
    if (g_congestion_control_enabled) {
        if(cc_terminal_is_abatement_active(s->local_congestion_controller)) {
            bandwidth_coef = cc_terminal_get_current_injection_bandwidth_coef(s->local_congestion_controller);
        }
    }
    /* Injection (or transmission) delay: Time taken for the data to be placed on the link/channel
     *    - Based on bandwidth
     * Propagtion delay: Time taken for the data to cross the link and arrive at the reciever
     *    - A physical property of the material of the link (eg. copper, optical fiber)
     */
    tw_stime injection_ts, injection_delay;
    tw_stime propagation_ts, propagation_delay;

    double bandwidth = s->params->cn_bandwidth;
    if (g_congestion_control_enabled)
        bandwidth = bandwidth_coef * bandwidth;
 
    injection_delay = bytes_to_ns(s->params->chunk_size, bandwidth);
    if((cur_entry->msg.packet_size < s->params->chunk_size) && (cur_entry->msg.chunk_id == num_chunks - 1))
    {
        data_size = cur_entry->msg.packet_size % s->params->chunk_size;
        injection_delay = bytes_to_ns(data_size, bandwidth);
    }
    propagation_delay = s->params->cn_delay;

    s->qos_data[msg->rail_id][vcg] += data_size;
  
    // injection_delay += g_tw_lookahead;
    
    msg->saved_available_time = s->terminal_available_time[msg->rail_id];
    s->terminal_available_time[msg->rail_id] = maxd(s->terminal_available_time[msg->rail_id], tw_now(lp));
    s->terminal_available_time[msg->rail_id] += injection_delay;

    injection_ts = s->terminal_available_time[msg->rail_id] - tw_now(lp);
    propagation_ts = injection_ts + propagation_delay;

    router_id = s->router_lp[msg->rail_id];

    void * remote_event;
    e = model_net_method_event_new(router_id, propagation_ts + gen_noise(lp, &msg->num_rngs), lp,
            DRAGONFLY_DALLY_ROUTER, (void**)&m, &remote_event);
    memcpy(m, &cur_entry->msg, sizeof(terminal_dally_message));
    if (m->remote_event_size_bytes){
        memcpy(remote_event, cur_entry->event_data, m->remote_event_size_bytes);
    }

    m->type = R_ARRIVE;
    m->src_terminal_id = lp->gid;
    m->dfdally_src_terminal_id = s->terminal_id;
    m->rail_id = msg->rail_id;
    m->vc_index = vcg;
    m->last_hop = TERMINAL;
    m->magic = router_magic_num;
    m->path_type = -1;
    m->local_event_size_bytes = 0;
    m->is_intm_visited = 0;
    m->intm_grp_id = -1;
    m->intm_rtr_id = -1; //for legacy prog-adaptive
    tw_event_send(e);


#if DEBUG == 1
    if(cur_entry->msg.packet_ID == LLU(TRACK_PKT) && lp->gid == T_ID)
        printf("\n Packet %llu generated at terminal %d dest %llu size %llu num chunks %llu router-id %d %llu", 
                cur_entry->msg.packet_ID, s->terminal_id, LLU(cur_entry->msg.dest_terminal_lpid),
                LLU(cur_entry->msg.packet_size), LLU(num_chunks), s->router_id[msg->rail_id], LLU(router_id));
#endif

    if(cur_entry->msg.chunk_id == num_chunks - 1 && (cur_entry->msg.local_event_size_bytes > 0)) 
    {
        bf->c16 = 1;
        msg->packet_ID = cur_entry->msg.packet_ID;
        tw_stime local_ts = 0;
        tw_event *e_new = tw_event_new(cur_entry->msg.sender_lp, local_ts, lp);
        void * m_new = tw_event_data(e_new);
        void *local_event = (char*)cur_entry->event_data + 
        cur_entry->msg.remote_event_size_bytes;
        memcpy(m_new, local_event, cur_entry->msg.local_event_size_bytes);
        tw_event_send(e_new);
    }
    
    s->vc_occupancy[msg->rail_id][vcg] += s->params->chunk_size;
    cur_entry = return_head(s->terminal_msgs[msg->rail_id], s->terminal_msgs_tail[msg->rail_id], vcg); 
    rc_stack_push(lp, cur_entry, delete_terminal_dally_message_list, s->st);
    s->terminal_length[msg->rail_id][vcg] -= s->params->chunk_size;
    s->link_traffic[msg->rail_id] += s->params->chunk_size;
    s->total_chunks[msg->rail_id]++;
    s->injected_chunks++; //TODO: if a terminal can inject packets from multiple jobs, it might be beneficial to make that matter here

    int next_vcg = 0;

    if(num_qos_levels > 1) //I think this one is OK since the default is that terminals have only 1 VC anyway so leaving vcg as 
        next_vcg = get_next_vcg(s, bf, msg, lp);

    cur_entry = NULL;
    if(next_vcg >= 0)
        cur_entry = s->terminal_msgs[msg->rail_id][next_vcg];

    /* if there is another packet inline then schedule another send event */
    if(cur_entry != NULL && s->vc_occupancy[msg->rail_id][next_vcg] + s->params->chunk_size <= s->params->cn_vc_size) {
        terminal_dally_message *m_new;
        e = model_net_method_event_new(lp->gid, injection_ts + gen_noise(lp, &msg->num_rngs), lp, DRAGONFLY_DALLY, (void**)&m_new, NULL);
        m_new->type = T_SEND;
        m_new->rail_id = msg->rail_id;
        m_new->magic = terminal_magic_num;
        tw_event_send(e);
    } else {
        /* If not then the LP will wait for another credit or packet generation */
        bf->c4 = 1;
        s->in_send_loop[msg->rail_id] = 0;
    }
    // TODO (elkin): The check on vcg only properly works for `num_qos_levels == 1`. Ideally, we should be checking if there is enough
    // space for the next packet in the queue (the packet determines in which queue it's going to be injected), but that is not
    // possible, because we only know the queue at `packet_generate`. This might not present a big problem for most applications but
    // those that are fed at a rate higher than what they can process can see the queue, potentially, grow very large.
    if(s->issueIdle[msg->rail_id] && s->terminal_length[msg->rail_id][vcg] < s->params->cn_vc_size) {
        bf->c5 = 1;
        s->issueIdle[msg->rail_id] = 0;
        model_net_method_idle_event2(injection_ts, 0, msg->rail_id, lp);
    
        if(s->last_buf_full[msg->rail_id] > 0.0)
        {
            bf->c6 = 1;
            msg->saved_total_time = s->busy_time[msg->rail_id];
            msg->saved_busy_time = s->last_buf_full[msg->rail_id];
            msg->saved_sample_time = s->busy_time_sample[msg->rail_id];

            s->busy_time[msg->rail_id] += (tw_now(lp) - s->last_buf_full[msg->rail_id]);
            s->busy_time_sample[msg->rail_id] += (tw_now(lp) - s->last_buf_full[msg->rail_id]);
            s->ross_sample.busy_time_sample[msg->rail_id] += (tw_now(lp) - s->last_buf_full[msg->rail_id]);
            msg->saved_busy_time_ross = s->busy_time_ross_sample[msg->rail_id];
            s->busy_time_ross_sample[msg->rail_id] += (tw_now(lp) - s->last_buf_full[msg->rail_id]);
            s->last_buf_full[msg->rail_id] = 0.0;
        }
    }
    return;
}

static void notify_dest_lp_of(
        terminal_state * s,
        tw_lp * lp,
        terminal_dally_message * msg,
        enum notify_t notification
        ) {
    assert(NOTIFY_ZOMBIE == notification);
    double offset = -1.0;
    switch (notification) {
        case NOTIFY_ZOMBIE:
            offset = 0.0; // Zero-offset events are ugly, but we want to guarantee correctness, so this prevents funky stuff from happening (like in the case of events arriving and being processed before the notification reaches them)
            break;
        default:
            tw_error(TW_LOC, "The notification event with type %d couldn't be created", notification);
    }

    terminal_dally_message * new_msg;
    // Lower value in priority means that it will be processed first
    // This event will be processed before any predicted packet arrives (even if scheduled at the same timestamp)
    tw_event *e = model_net_method_event_new_user_prio(msg->dest_terminal_lpid, offset, lp, DRAGONFLY_DALLY, (void**)&new_msg, NULL, 1);

    memcpy(new_msg, msg, sizeof(terminal_dally_message)); // Just making sure that if the simulation breaks because we didn't set some value below, it breaks in a spectacular manner (~0 can be -1)
    assert(new_msg->dfdally_src_terminal_id == s->terminal_id);
    new_msg->type        = T_NOTIFY;
    new_msg->notify_type = notification;
    new_msg->magic       = terminal_magic_num;
    tw_event_send(e); 
}

static void notify_src_lp_on_total_latency(tw_lp * lp, terminal_dally_message * msg)
{
    terminal_dally_message * new_msg;
    tw_event *e = model_net_method_event_new(
            msg->src_terminal_id, g_tw_lookahead, lp, DRAGONFLY_DALLY, (void**)&new_msg, NULL);

    // We copy all the data from the original message but will change the flags for the message
    // (Some of the data that we care about: packet_ID, src_terminal_lpid, dest_terminal_lpid, dfdally_src_terminal_id, dfdally_dest_terminal_id)
    memcpy(new_msg, msg, sizeof(terminal_dally_message));
    //strcpy(new_msg->category, msg->category);
    new_msg->type                    = T_NOTIFY;
    new_msg->notify_type             = NOTIFY_LATENCY;
    new_msg->magic                   = terminal_magic_num;
    tw_event_send(e); 
}

static void process_terminal_notification_event_rc(terminal_state * s, tw_bf * bf, terminal_dally_message * msg, tw_lp * lp) {
    switch ((enum notify_t) msg->notify_type) {
        case NOTIFY_LATENCY:
            break;

        case NOTIFY_ZOMBIE:
            {
            struct packet_id const zombie = {
                .packet_ID = msg->packet_ID,
                .dfdally_src_terminal_id = msg->dfdally_src_terminal_id};
            assert(s->zombies.count(zombie) == 1);
            s->zombies.erase(zombie);
            }
            break;
    }
}

static void process_terminal_notification_event(terminal_state * s, tw_bf * bf, terminal_dally_message * msg, tw_lp * lp) {
    switch ((enum notify_t) msg->notify_type) {
        case NOTIFY_LATENCY:
            break;

        case NOTIFY_ZOMBIE: {
            // Adding new zombie to LP list of zombies
            assert(lp->gid == msg->dest_terminal_lpid);
            assert(s->terminal_id == msg->dfdally_dest_terminal_id);

            //printf("INSERTING zombie alert: LPID=%d  packet_ID = %d  dfdally_src_terminal_id = %d\n", lp->gid, msg->packet_ID, msg->dfdally_src_terminal_id);
            struct packet_id const zombie = {
                .packet_ID = msg->packet_ID,
                .dfdally_src_terminal_id = msg->dfdally_src_terminal_id};
            assert(s->zombies.count(zombie) == 0);
            s->zombies.insert(zombie);
            }
            break;
    }
}

#if ALWAYS_DETERMINISTIC_NETWORK
// This function triggers an event that is completely ignored when processed later. The number of events produced by a terminal/router DOES alter the simulation results. (The number of events processed by an LP shouldn't be a parameter to the simulation itself, but it is weirdly).
static void vacuous_msg_to_itself(terminal_state * s, terminal_dally_message * msg, tw_lp * lp)
{
    (void) s;
    (void) msg;
    terminal_dally_message * new_msg;
    tw_event *e = model_net_method_event_new(
            lp->gid, g_tw_lookahead, lp, DRAGONFLY_DALLY, (void**)&new_msg, NULL);

    new_msg->type  = T_VACUOUS_EVENT;
    new_msg->magic = terminal_magic_num;
    tw_event_send(e); 
}
#endif /* ALWAYS_DETERMINISTIC_NETWORK */

//used by packet_arrive()
static void send_remote_event(terminal_state * s, terminal_dally_message * msg, tw_lp * lp, tw_bf * bf, char * event_data, int remote_event_size)
{
    (void) s;
    void * tmp_ptr = model_net_method_get_edata(DRAGONFLY_DALLY, msg);
    
    tw_stime ts = 0;

    if (msg->is_pull){
        bf->c4 = 1;
        struct codes_mctx mc_dst =
            codes_mctx_set_global_direct(msg->sender_mn_lp);
        struct codes_mctx mc_src =
            codes_mctx_set_global_direct(lp->gid);
        int net_id = model_net_get_id(LP_METHOD_NM_TERM);

        model_net_set_msg_param(MN_MSG_PARAM_START_TIME, MN_MSG_PARAM_START_TIME_VAL, &(msg->msg_start_time));
        
        msg->event_rc = model_net_event_mctx(net_id, &mc_src, &mc_dst, msg->category,
                msg->sender_lp, msg->pull_size, ts + gen_noise(lp, &msg->num_rngs),
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

static void packet_arrive_predicted_rc(terminal_state * s, tw_bf * bf, terminal_dally_message * msg, tw_lp * lp)
{
    struct dfly_hash_key key = {
        .message_id = msg->message_id,
        .sender_id = msg->sender_lp,
    };
    struct dfly_qhash_entry * tmp = NULL;
    struct qhash_head * hash_link = NULL;

    // If entry was removed from hash
    if(bf->c8) {
        struct dfly_qhash_entry * d_entry_pop = (dfly_qhash_entry *) rc_stack_pop(s->st);
        qhash_add(s->rank_tbl, &key, &(d_entry_pop->hash_link));
        s->rank_tbl_pop++;

        if(s->rank_tbl_pop >= DFLY_HASH_TABLE_SIZE)
            tw_error(TW_LOC, "\n Exceeded allocated qhash size, increase hash size in dragonfly model");

        hash_link = &(d_entry_pop->hash_link);
        tmp = d_entry_pop;
    // In case it was not deleted, and we accessed it
    } else if (bf->c9 || bf->c5) {
        assert(!tmp);
        hash_link = qhash_search(s->rank_tbl, &key);

        tmp = qhash_entry(hash_link, struct dfly_qhash_entry, hash_link);
    }
    assert((bf->c9 || bf->c5) == bf->c6);

    if(bf->c4) {
        model_net_event_rc2(lp, &msg->event_rc);
    }

    if(bf->c7) {
        s->finished_msgs--;
        s->total_msg_size -= msg->total_size;
        total_msg_sz -= msg->total_size;
        N_finished_msgs--;
        s->data_size_ross_sample -= msg->total_size;
        s->ross_sample.data_size_sample -= msg->total_size;
        s->data_size_sample -= msg->total_size;
    }

    if(bf->c6) {
        tmp->remaining_packets++;
    }

    if(bf->c5) {
        qhash_del(hash_link);
        free_tmp(tmp);
        s->rank_tbl_pop--;
    }
}

/* packet arrives at the destination terminal */
static void packet_arrive_predicted(terminal_state * s, tw_bf * bf, terminal_dally_message * msg, tw_lp * lp)
{
    assert(lp->gid == msg->dest_terminal_lpid);
    /* WE do not allow self messages through dragonfly */
    assert(lp->gid != msg->src_terminal_id);

#if DEBUG == 1
    if(msg->packet_ID == LLU(TRACK_PKT) && msg->src_terminal_id == T_ID)
        printf("\n Packet %llu arrived at lp %llu hops %d ", LLU(msg->sender_lp), LLU(lp->gid), msg->my_N_hop);
#endif

    //record for commit_f file IO
    msg->travel_end_time = tw_now(lp);

    // The table has to have been initialized already, if not, what the heck!
    struct dfly_hash_key key = {
        .message_id = msg->message_id,
        .sender_id = msg->sender_lp,
    };

    // Finding out if message is in hash
    struct qhash_head * hash_link = qhash_search(s->rank_tbl, &key);
    struct dfly_qhash_entry * tmp = NULL;
    if(hash_link) {
        bf->c9 = 1;
        tmp = qhash_entry(hash_link, struct dfly_qhash_entry, hash_link);
    // We create an entry into the hash only if it makes sense to do so (ie, only when the message needs multiple packets to be completed)
    } else if (msg->total_size > msg->packet_size) {
        bf->c5 = 1;

        uint64_t const packet_size = s->params->packet_size;
        uint64_t total_packets = msg->total_size / packet_size + (msg->total_size % packet_size ? 1 : 0);
        if (total_packets == 0) { total_packets = 1; }

        struct dfly_qhash_entry * const d_entry = (dfly_qhash_entry *) calloc(1, sizeof (struct dfly_qhash_entry));
        d_entry->num_chunks = 0;
        d_entry->key = key;
        d_entry->remote_event_data = NULL;
        d_entry->remote_event_size = 0;
        d_entry->remaining_packets = total_packets;
        qhash_add(s->rank_tbl, &key, &(d_entry->hash_link));
        s->rank_tbl_pop++;

        if(s->rank_tbl_pop >= DFLY_HASH_TABLE_SIZE) {
            tw_error(TW_LOC, "\n Exceeded allocated qhash size, increase hash size in dragonfly model");
        }

        hash_link = &(d_entry->hash_link);
        tmp = d_entry;
    // Just for completion, checking invariant
    } else {
        // packet sz == message sz
        assert(msg->total_size == msg->packet_size);
    }

    // Decreasing the number of remaining packets
    if (tmp) {
        bf->c6 = 1;
        tmp->remaining_packets--;

        /* retrieve the event data, all chunks from the same packet carry the `remote_event_data` */
        if(msg->remote_event_size_bytes > 0 && !tmp->remote_event_data)
        {
            /* Now retreieve the number of chunks completed from the hash and update them */
            void *m_data_src = model_net_method_get_edata(DRAGONFLY_DALLY, msg);

            /* Retreive the remote event entry */
            tmp->remote_event_data = (char*) calloc(1, msg->remote_event_size_bytes);
            assert(tmp->remote_event_data);
            tmp->remote_event_size = msg->remote_event_size_bytes;
            memcpy(tmp->remote_event_data, m_data_src, msg->remote_event_size_bytes);
        }
    }

    bool const is_msg_completed = tmp ? tmp->remaining_packets == 0 : true;

    if(is_msg_completed) {
        bf->c7 = 1;
        s->data_size_sample += msg->total_size;
        s->ross_sample.data_size_sample += msg->total_size;
        s->data_size_ross_sample += msg->total_size;
        N_finished_msgs++;
        total_msg_sz += msg->total_size;
        s->total_msg_size += msg->total_size;
        s->finished_msgs++;

        if (tmp) {
            // This should always be true. It sends the message to the server/workload or communicates to the model-net layer
            if (tmp->remote_event_data && tmp->remote_event_size > 0) {
                send_remote_event(s, msg, lp, bf, tmp->remote_event_data, tmp->remote_event_size);
            }

            bf->c8 = 1;
            qhash_del(hash_link);
            rc_stack_push(lp, tmp, free_tmp, s->st);
            s->rank_tbl_pop--;
        } else { // packet sz == message sz
            if (msg->remote_event_size_bytes > 0) {
                void *m_data_src = model_net_method_get_edata(DRAGONFLY_DALLY, msg);
                assert(m_data_src);
                send_remote_event(s, msg, lp, bf, (char*) m_data_src, msg->remote_event_size_bytes);
            }
        }
    }
}

static void packet_arrive_rc(terminal_state * s, tw_bf * bf, terminal_dally_message * msg, tw_lp * lp)
{
    if (g_congestion_control_enabled)
        cc_terminal_send_ack_rc(s->local_congestion_controller);

    if(msg->path_type == MINIMAL)
        minimal_count--;
    if(msg->path_type == NON_MINIMAL)
        nonmin_count--;

    N_finished_chunks--;
    s->finished_chunks--;
    s->fin_chunks_sample--;
    s->ross_sample.fin_chunks_sample--;
    s->fin_chunks_ross_sample--;
    s->ejected_chunks--;

    total_hops -= msg->my_N_hop;
    s->total_hops -= msg->my_N_hop;
    s->fin_hops_sample -= msg->my_N_hop;
    s->ross_sample.fin_hops_sample -= msg->my_N_hop;
    s->fin_hops_ross_sample -= msg->my_N_hop;
    s->fin_chunks_time = msg->saved_sample_time;
    s->ross_sample.fin_chunks_time = msg->saved_sample_time;
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
        s->packet_fin--;
        packet_fin--;
        stat->recv_count--;
        stat->recv_bytes -= msg->packet_size;
        N_finished_packets--;
        s->finished_packets--;
    }
    
    if(bf->c21)
        s->min_latency = msg->saved_min_lat;
    if(bf->c22)
	{
          s->max_latency = msg->saved_available_time;
	} 

    struct packet_id const packet_key = {
        .packet_ID = msg->packet_ID,
        .dfdally_src_terminal_id = msg->dfdally_src_terminal_id
    };

    if (bf->c28) {
        if (bf->c29) {
            s->remaining_sz_packets[packet_key] = 0;
        }
        s->remaining_sz_packets[packet_key] += s->params->chunk_size;
    } else {
        if (bf->c29) {
            s->remaining_sz_packets.erase(packet_key);
        }
    }

    if (bf->c14) {
        s->zombies.emplace((struct packet_id) {
            .packet_ID = msg->packet_ID,
            .dfdally_src_terminal_id = msg->dfdally_src_terminal_id
        });
    }
    if (bf->c15) {
        return;
    }

    if(bf->c7) {
        //assert(!hash_link);
        
        N_finished_msgs--;
        s->finished_msgs--;
        total_msg_sz -= msg->total_size;
        s->total_msg_size -= msg->total_size;
        s->data_size_sample -= msg->total_size;
        s->ross_sample.data_size_sample -= msg->total_size;
        s->data_size_ross_sample -= msg->total_size;

        if(bf->c4) {
            model_net_event_rc2(lp, &msg->event_rc);
        }

        struct dfly_qhash_entry * d_entry_pop = (dfly_qhash_entry *)rc_stack_pop(s->st);
        qhash_add(s->rank_tbl, &key, &(d_entry_pop->hash_link));
        s->rank_tbl_pop++; 
        
        if(s->rank_tbl_pop >= DFLY_HASH_TABLE_SIZE)
            tw_error(TW_LOC, "\n Exceeded allocated qhash size, increase hash size in dragonfly model");

        hash_link = &(d_entry_pop->hash_link);
        tmp = d_entry_pop; 

    }
      
    assert(tmp);
    tmp->num_chunks--;

    if (bf->c13) {
        tmp->remaining_packets++;
    }

    if(bf->c5)
    {
        qhash_del(hash_link);
        free_tmp(tmp);	
        s->rank_tbl_pop--;
    }
}

/* packet arrives at the destination terminal */
static void packet_arrive(terminal_state * s, tw_bf * bf, terminal_dally_message * msg, tw_lp * lp) 
{
    // if(isRoutingMinimal(routing) && msg->my_N_hop > 4)
    // {
    //     printf("TERMINAL RECEIVED A NONMINIMAL LENGTH PACKET\n");
    // }
    // if(isRoutingMinimal(routing) && msg->my_g_hop > 1)
    // {
    //     printf("TERMINAL RECEIVED A DOUBLE GLOBAL HOP PACKET\n");
    // }
    // printf("%d\n",msg->my_g_hop);
    if (msg->dfdally_dest_terminal_id != s->terminal_id)
        tw_error(TW_LOC, "Packet arrived at wrong terminal\n");

    if (msg->my_N_hop > s->params->max_hops_notify)
    {
        printf("Terminal received a packet with %d hops! (Notify on > than %d)\n",msg->my_N_hop, s->params->max_hops_notify);
    }


    if (g_congestion_control_enabled)
        cc_terminal_send_ack(s->local_congestion_controller, msg->src_terminal_id);

    assert(lp->gid == msg->dest_terminal_lpid);

#if DEBUG == 1
    if(msg->packet_ID == LLU(TRACK_PKT) && msg->src_terminal_id == T_ID)
        printf("\n Packet %llu arrived at lp %llu hops %d ", LLU(msg->sender_lp), LLU(lp->gid), msg->my_N_hop);
#endif

    tw_stime ts = s->params->cn_credit_delay;

    // no method_event here - message going to router
    tw_event * buf_e;
    terminal_dally_message * buf_msg;
    buf_e = model_net_method_event_new(msg->intm_lp_id, ts + gen_noise(lp, &msg->num_rngs), lp,
            DRAGONFLY_DALLY_ROUTER, (void**)&buf_msg, NULL);
    buf_msg->magic = router_magic_num;
    buf_msg->rail_id = msg->rail_id;
    buf_msg->vc_index = msg->vc_index;
    buf_msg->output_chan = msg->output_chan;
    buf_msg->type = R_BUFFER;
    tw_event_send(buf_e);

    /* Total overall finished chunks in simulation */
    N_finished_chunks++;
    /* Finished chunks on a LP basis */
    s->finished_chunks++;
    /* Finished chunks per sample */
    s->fin_chunks_sample++;
    s->ross_sample.fin_chunks_sample++;
    s->fin_chunks_ross_sample++;
    s->ejected_chunks++;

    /* WE do not allow self messages through dragonfly */
    assert(lp->gid != msg->src_terminal_id);

    // TODO (elkin): this is wrong, this is _not_ finding the number of chunks, consider: chunk_size = 2 and packet_size = 5. There should be 3 chunks, but the code outputs 2!
    uint64_t num_chunks = msg->packet_size / s->params->chunk_size;
    if (msg->packet_size < s->params->chunk_size)
        num_chunks++;

    if(msg->path_type == MINIMAL)
        minimal_count++;   

    if(msg->path_type == NON_MINIMAL)
        nonmin_count++;

    if(msg->path_type != MINIMAL && msg->path_type != NON_MINIMAL)
        printf("\n Wrong message path type %d ", msg->path_type);

    //record for commit_f file IO
    msg->travel_end_time = tw_now(lp);
    tw_stime ete_latency = msg->travel_end_time - msg->travel_start_time;

    /* save the sample time */
    msg->saved_sample_time = s->fin_chunks_time;
    s->fin_chunks_time += ete_latency;
    s->ross_sample.fin_chunks_time += ete_latency;
    msg->saved_fin_chunks_ross = s->fin_chunks_time_ross_sample;
    s->fin_chunks_time_ross_sample += ete_latency;
    
    /* save the total time per LP */
    msg->saved_avg_time = s->total_time;
    s->total_time += ete_latency;
    total_hops += msg->my_N_hop;
    s->total_hops += msg->my_N_hop;
    s->fin_hops_sample += msg->my_N_hop;
    s->ross_sample.fin_hops_sample += msg->my_N_hop;
    s->fin_hops_ross_sample += msg->my_N_hop;

    mn_stats* stat = model_net_find_stats(msg->category, s->dragonfly_stats_array);
    msg->saved_rcv_time = stat->recv_time;
    stat->recv_time += ete_latency;

    // Chunk with the last id has been received (not the last chunk to receive necessarily)
    if(msg->chunk_id == num_chunks - 1)
    {
        bf->c1 = 1;
        s->packet_fin++;
        packet_fin++;

        stat->recv_count++;
        stat->recv_bytes += msg->packet_size;

        N_finished_packets++;
        s->finished_packets++;
    }

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

    if(s->min_latency > ete_latency) {
        bf->c21 = 1;
        msg->saved_min_lat = s->min_latency;
        s->min_latency = ete_latency;
    }

    if(s->max_latency < ete_latency) {
        bf->c22 = 1;
        msg->saved_available_time = s->max_latency;
        s->max_latency = ete_latency;
    }

    struct packet_id const packet_key = {
        .packet_ID = msg->packet_ID,
        .dfdally_src_terminal_id = msg->dfdally_src_terminal_id
    };
    bool const is_zombie = s->zombies.count(packet_key) == 1;
    bool const has_remaining_sz = s->remaining_sz_packets.count(packet_key) == 1;

    // Finding out if the packet is complete
    bool is_packet_completed = false;
    int const chunk_size = s->params->chunk_size;
    if (has_remaining_sz) {
        bf->c28 = 1;
        assert(s->remaining_sz_packets[packet_key] >= chunk_size);
        s->remaining_sz_packets[packet_key] -= chunk_size;

        // if `remaining == 0`, ie, if the packet has been completed
        if (s->remaining_sz_packets[packet_key] == 0) {
            bf->c29 = 1;
            is_packet_completed = true;
            s->remaining_sz_packets.erase(packet_key);
        }
    } else {
        if (chunk_size < msg->packet_size) {
            bf->c29 = 1;
            s->remaining_sz_packets[packet_key] = msg->packet_size - chunk_size;
        } else {
            is_packet_completed = true;
        }
    }

    // Zombies don't generate delay notifications, and they don't modify the state of `s->rank_tbl` (`packet_arrive_predicted` should have removed the msg entry already)
    if (is_zombie) {
        //printf("We got a zombie! LPID=%d  packet_ID = %d  dfdally_src_terminal_id = %d\n", lp->gid, msg->packet_ID, msg->dfdally_src_terminal_id);
        if (is_packet_completed) {
            s->zombies.erase(packet_key);
            bf->c14 = 1;
        }
        bf->c15 = 1;
        return;
    }

    struct dfly_hash_key key = {
        .message_id = msg->message_id,
        .sender_id = msg->sender_lp,
    };

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

    /* If an entry does not exist then create one */
    if(!tmp)
    {
        bf->c5 = 1;
        uint64_t const packet_size = s->params->packet_size;
        uint64_t total_packets = msg->total_size / packet_size + (msg->total_size % packet_size ? 1 : 0);
        if (total_packets == 0) { total_packets = 1; }

        struct dfly_qhash_entry * d_entry = (dfly_qhash_entry *)calloc(1, sizeof (struct dfly_qhash_entry));
        d_entry->num_chunks = 0;
        d_entry->key = key;
        d_entry->remote_event_data = NULL;
        d_entry->remote_event_size = 0;
        d_entry->remaining_packets = total_packets;
        qhash_add(s->rank_tbl, &key, &(d_entry->hash_link));
        s->rank_tbl_pop++;
                
        if(s->rank_tbl_pop >= DFLY_HASH_TABLE_SIZE)
                    tw_error(TW_LOC, "\n Exceeded allocated qhash size, increase hash size in dragonfly model");
        
        hash_link = &(d_entry->hash_link);
        tmp = d_entry;
    }
    
    assert(tmp);
    tmp->num_chunks++;

    /* retrieve the event data, all chunks from the same packet carry the `remote_event_data` */
    if(msg->remote_event_size_bytes > 0 && !tmp->remote_event_data)
    {
        /* Now retreieve the number of chunks completed from the hash and update
            * them */
        void *m_data_src = model_net_method_get_edata(DRAGONFLY_DALLY, msg);

        /* Retreive the remote event entry */
        tmp->remote_event_data = (char*)calloc(1, msg->remote_event_size_bytes);
        assert(tmp->remote_event_data);
        tmp->remote_event_size = msg->remote_event_size_bytes; 
        memcpy(tmp->remote_event_data, m_data_src, msg->remote_event_size_bytes);
    }

    // if the packet is complete (ie, this `msg` is the last piece of the packet)
    if (is_packet_completed) {
        bf->c13 = 1;

        tmp->remaining_packets--;

        //printf("Good day sir, not a zombie! LPID=%d  packet_ID = %d  dfdally_src_terminal_id = %d\n", lp->gid, msg->packet_ID, msg->dfdally_src_terminal_id);
        if (packet_latency_f || dally_surrogate_configured) {
            notify_src_lp_on_total_latency(lp, msg);
#if ALWAYS_DETERMINISTIC_NETWORK
        } else {
            // This vacuous msg is necessary just to keep simulations with and without the latency notification the same. Notifying the latency does not impact
            // the simulation (unless the data is fed to a predictor, later to be used). If the latency notification is deactivated, the simulation will produce
            // the same number of events (a bit wasteful), a parameter that model-net or dragonfly-dally for some reason use :S
            vacuous_msg_to_itself(s, msg, lp);
#endif /* if ALWAYS_DETERMINISTIC_NETWORK */
        }
    }

    // if the message is complete (ie, this `msg` is the last piece of the message)
    /* If all chunks of a message have arrived then send a remote event to the callee */
    //if(tmp->num_chunks >= total_chunks)  // this was the test before, it is a good test assumming the network is never frozen
    if(tmp->remaining_packets == 0)
    {
        bf->c7 = 1;

        assert(tmp->num_chunks <= total_chunks);

        s->data_size_sample += msg->total_size;
        s->ross_sample.data_size_sample += msg->total_size;
        s->data_size_ross_sample += msg->total_size;
        N_finished_msgs++;
        total_msg_sz += msg->total_size;
        s->total_msg_size += msg->total_size;
        s->finished_msgs++;

        //assert(tmp->remote_event_data && tmp->remote_event_size > 0);
        if(tmp->remote_event_data && tmp->remote_event_size > 0) {
            send_remote_event(s, msg, lp, bf, tmp->remote_event_data, tmp->remote_event_size);
        }
        /* Remove the hash entry */
        qhash_del(hash_link);
        rc_stack_push(lp, tmp, free_tmp, s->st);
        s->rank_tbl_pop--;
     }
}

static void terminal_buf_update_rc(terminal_state * s,
		    tw_bf * bf, 
		    terminal_dally_message * msg, 
		    tw_lp * lp)
{
    int vcg = 0;
    int num_qos_levels = s->params->num_qos_levels;

    if(num_qos_levels > 1)
        vcg = get_vcg_from_category(msg);
    
    s->vc_occupancy[msg->rail_id][vcg] += s->params->chunk_size;
    if(bf->c1) {
        s->in_send_loop[msg->rail_id] = 0;
    }

    return;
}
/* update the compute node-router channel buffer */
static void terminal_buf_update(terminal_state * s, 
		    tw_bf * bf, 
		    terminal_dally_message * msg, 
		    tw_lp * lp)
{
    bf->c1 = 0;
    bf->c2 = 0;
    bf->c3 = 0;
    int vcg = 0;
        
    int num_qos_levels = s->params->num_qos_levels;

    if(num_qos_levels > 1)
        vcg = get_vcg_from_category(msg);

    tw_stime ts = 0;
    s->vc_occupancy[msg->rail_id][vcg] -= s->params->chunk_size;
    
    if(s->in_send_loop[msg->rail_id] == 0 && s->terminal_msgs[msg->rail_id][vcg] != NULL) {
        terminal_dally_message *m;
        bf->c1 = 1;
        tw_event* e = model_net_method_event_new(lp->gid, ts + gen_noise(lp, &msg->num_rngs), lp, DRAGONFLY_DALLY, 
            (void**)&m, NULL);
        m->rail_id = msg->rail_id;
        m->type = T_SEND;
        m->magic = terminal_magic_num;
        s->in_send_loop[msg->rail_id] = 1;
        tw_event_send(e);
    }
    return;
}

static void dragonfly_dally_terminal_final( terminal_state * s, 
      tw_lp * lp )
{
    if (freeze_network_on_switch && is_dally_surrogate_on) {
        dragonfly_dally_terminal_surrogate_to_highdef(s, lp, NULL);
    }
    // printf("terminal id %d\n",s->terminal_id);
    dragonfly_total_time += s->total_time; //increment the PE level time counter
    
    if (s->max_latency > dragonfly_max_latency)
        dragonfly_max_latency = s->max_latency; //get maximum latency across all LPs on this PE


	model_net_print_stats(lp->gid, s->dragonfly_stats_array);
    int written = 0;
  
    if(s->terminal_id == 0)
    {
        written += sprintf(s->output_buf + written, "# Format <source_id> <source_type> <dest_id> < dest_type>  <link_type> <link_traffic> <link_saturation> <stalled_chunks>\n");
//        fprintf(fp, "# Format <LP id> <Terminal ID> <Total Data Size> <Avg packet latency> <# Flits/Packets finished> <Avg hops> <Busy Time> <Max packet Latency> <Min packet Latency >\n");
    }
    for(int i = 0; i < s->params->num_rails; i++)
    {
        //since LLU(s->total_msg_size) is total message size a terminal received from a router so source is router and destination is terminal
        written += sprintf(s->output_buf + written, "\n%u %s %u %s %s %llu %lf %lu",
                        s->terminal_id, "T",s->router_id[i], "R","CN", LLU(s->link_traffic[i]), s->busy_time[i], s->stalled_chunks[i]);
    }


    lp_io_write(lp->gid, (char*)"dragonfly-link-stats", written, s->output_buf); 
    
    // if(s->terminal_id == 0)
    // {
    //     //fclose(dragonfly_term_bw_log);
    //     char meta_filename[128];
    //     sprintf(meta_filename, "dragonfly-cn-stats.meta");

    //     FILE * fp = NULL;
    //     fp = fopen(meta_filename, "w");
    //     if(fp)
    //       fprintf(fp, "# Format <LP id> <Terminal ID> <Total Data Size> <Avg packet latency> <# Flits/Packets finished> <Busy Time> <Max packet Latency> <Min packet Latency >\n");
    //     fclose(fp);
    // }
   
    written = 0;
    if(s->terminal_id == 0)
    {
        written += sprintf(s->output_buf2 + written, "# Format <LP id> <Terminal ID> <Total Data Sent> <Total Data Received> <Avg packet latency> <Max packet Latency> <Min packet Latency> <# Packets finished> <Avg Hops> <Avg Busy Time (over rails)>\n");
    }

    tw_stime avg_busy_time = 0;
    for(int i = 0; i < s->params->num_rails; i++)
        avg_busy_time += s->busy_time[i];
    avg_busy_time = avg_busy_time / s->params->num_rails;


    written += sprintf(s->output_buf2 + written, "%llu %u %d %llu %lf %lf %lf %ld %lf %lf\n", 
            LLU(lp->gid), s->terminal_id, s->total_gen_size, LLU(s->total_msg_size), s->total_time/s->finished_chunks, s->max_latency, s->min_latency,
            s->finished_packets, (double)s->total_hops/s->finished_chunks, avg_busy_time);

    for(int i = 0; i < s->params->num_rails; i++)
    {
        if(s->terminal_msgs[i][0] != NULL) 
        printf("[%llu] leftover terminal messages \n", LLU(lp->gid));
    }


    lp_io_write(lp->gid, (char*)"dragonfly-cn-stats", written, s->output_buf2); 

    if (packet_latency_f) {
        // If the last packet transmitted actually received a latency notification (was delievered)
        if (s->arrival_of_last_packet.packet_ID != -1) {
            auto sent = s->sent_packets[s->arrival_of_last_packet.packet_ID];
            assert(s->sent_packets.count(s->arrival_of_last_packet.packet_ID) == 1); // packet_ID is in s->sent_packets
            assert(sent.next_packet_delay < 0); // next_packet_delay is -1

            double const travel_end_time = s->arrival_of_last_packet.travel_end_time;
            struct packet_end end = {
                .travel_end_time = travel_end_time,
                .next_packet_delay = -1,
            };

            packet_latency_save_to_file(s->terminal_id, &sent.start, &end, false, false);

            // Deallocating memory from packet_start
            if (sent.message_data) { free(sent.message_data); }
            if (sent.remote_event_data) { free(sent.remote_event_data); }
            if (sent.local_data) { free(sent.local_data); }

            s->sent_packets.erase(s->arrival_of_last_packet.packet_ID);
            s->arrival_of_last_packet.packet_ID = -1;
        }

        // Storing all other missing packets into io file (deleting all elements from s->sent_packets as we go)
        for (auto it = s->sent_packets.begin(); it != s->sent_packets.end(); it = s->sent_packets.erase(it)) {
            auto& sent = it->second;
            uint64_t packet_ID = it->first;
            assert(sent.message_data);

            struct packet_end end = {
                .travel_end_time = -1,
                .next_packet_delay = sent.next_packet_delay,
            };

            packet_latency_save_to_file(s->terminal_id, &sent.start, &end, false, false);

            // Deallocating memory from packet_start
            if (sent.message_data) { free(sent.message_data); }
            if (sent.remote_event_data) { free(sent.remote_event_data); }
            if (sent.local_data) { free(sent.local_data); }
        }
    }

    //if(s->packet_gen != s->packet_fin)
    //    printf("\n generated %d finished %d ", s->packet_gen, s->packet_fin);
   
    if(s->rank_tbl)
        qhash_finalize(s->rank_tbl);
    
    rc_stack_destroy(s->st);
    rc_stack_destroy(s->cc_st);
    //TODO FREE THESE CORRECTLY
    for(int i = 0; i < s->params->num_rails; i++)
    {
        free(s->vc_occupancy[i]);
        // TODO: terminal_msgs are not properly freed if there are messages left. Correct this!
        free(s->terminal_msgs[i]);
        free(s->terminal_msgs_tail[i]);
    }
    free(s->vc_occupancy);
    free(s->terminal_msgs);
    free(s->terminal_msgs_tail);

    // Calling destructors for data. There is no need to free data, the
    // destructors do it themselves. ROSS allocated space for the datatypes and
    // it doesn't need to be freed
#if 0
    // Checking that there aren't any zombies left in the simulation
    printf("terminal %d - zombies = [", s->terminal_id);
    for (auto&& z: s->zombies) {
        printf("(%d %d) ", z.packet_ID, z.dfdally_src_terminal_id);
    }
    printf("]\n");
#endif
    for (auto&& kv: s->sent_packets) {
        if (kv.second.message_data) { free(kv.second.message_data); }
        if (kv.second.remote_event_data) { free(kv.second.remote_event_data); }
        if (kv.second.local_data) { free(kv.second.local_data); }
    }
    s->sent_packets.~map();
    s->is_pending_local_send.~set();
    s->remaining_sz_packets.~map();

    if (s->predictor_data) {
        free(s->predictor_data);
    }
}

void dragonfly_dally_router_final(router_state * s, tw_lp * lp){

    unsigned long total_stalled_chunks = 0;
    unsigned long total_total_chunks = 0;
    for (int i = 0; i < s->params->radix; i++)
    {
        total_stalled_chunks += s->stalled_chunks[i];
        total_total_chunks += s->total_chunks[i];
    }
    // printf("%d: %.5f\n",s->router_id, s->last_time);
    global_stalled_chunk_counter += total_stalled_chunks;

    // printf("%lu: stalled chunks %lu    total_chunks %lu\n", lp->gid, total_stalled_chunks, total_total_chunks);

    free(s->global_channel);
    int i, j;
    for(i = 0; i < s->params->radix; i++) {
        for(j = 0; j < s->params->num_vcs; j++) {
            if(s->queued_msgs[i][j] != NULL) {
                printf("[%llu] leftover queued messages %d %d %d\n", LLU(lp->gid), i, j,
                s->vc_occupancy[i][j]);
            }
            if(s->pending_msgs[i][j] != NULL) {
                printf("[%llu] lefover pending messages %d %d\n", LLU(lp->gid), i, j);
            }
        }
    }

    if(s->router_id == 0 && ROUTER_BW_LOG)
        fclose(dragonfly_rtr_bw_log);

    rc_stack_destroy(s->st);
    rc_stack_destroy(s->cc_st);
    
    const dragonfly_param *p = s->params;
    int written = 0;
    int src_rel_id = s->router_id % p->num_routers;
    int local_grp_id = s->router_id / p->num_routers;
    for(int d = 0; d <= p->intra_grp_radix; d++) 
    {
        if(d != src_rel_id)
        {
            int dest_ab_id = local_grp_id * p->num_routers + d;
            written += sprintf(s->output_buf + written, "\n%d %s %d %s %s %llu %lf %lu", 
                s->router_id,
                "R",
                dest_ab_id,
                "R",
                "L",
                LLU(s->link_traffic[d]),
                s->busy_time[d],
                s->stalled_chunks[d]);
        }
    }

    vector< Connection > my_global_links = s->connMan.get_connections_by_type(CONN_GLOBAL,true);
    vector< Connection >::iterator it = my_global_links.begin();
    for(; it != my_global_links.end(); it++)
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

    vector< Connection > my_terminal_links = s->connMan.get_connections_by_type(CONN_TERMINAL,true);
    it = my_terminal_links.begin();
    for(; it != my_terminal_links.end(); it++)
    {
        int dest_term_id = it->dest_gid;
        int port_no = it->port;
        written += sprintf(s->output_buf + written, "\n%d %s %d %s %s %llu %lf %lu",
                           s->router_id,
                           "R",
                           dest_term_id,
                           "T",
                           "CN",
                            LLU(s->link_traffic[port_no]),
                            s->busy_time[port_no],
                            s->stalled_chunks[port_no]);
    }

    sprintf(s->output_buf + written, "\n");
    lp_io_write(lp->gid, (char*)"dragonfly-link-stats", written, s->output_buf);

    if(g_congestion_control_enabled)
        cc_router_local_controller_finalize(s->local_congestion_controller);

    /*if(!s->router_id)
    {
        written = sprintf(s->output_buf, "# Format <LP ID> <Group ID> <Router ID> <Link Traffic per router port(s)>");
        written += sprintf(s->output_buf + written, "# Router ports in the order: %d green links, %d black links %d global channels \n", 
                p->num_router_cols * p->num_row_chans, p->num_router_rows * p->num_col_chans, p->num_global_channels);
    }
    written += sprintf(s->output_buf2 + written, "\n %llu %d %d",
        LLU(lp->gid),
        s->router_id / p->num_routers,
        s->router_id % p->num_routers);

    for(int d = 0; d < p->radix; d++) 
        written += sprintf(s->output_buf2 + written, " %lld", LLD(s->link_traffic[d]));

    lp_io_write(lp->gid, (char*)"dragonfly-router-traffic", written, s->output_buf2);
    */
    // if (!g_tw_mynode) {
    //     if (s->router_id == 0) {
    //         if (PRINT_CONFIG) 
    //             dragonfly_print_params(s->params);
    //     }
    // }

    //Xin: output link statistics
    if(p->counting_bool)
    {
      // for link traffic
      if(!s->router_id){
          written = sprintf(s->output_buf5, "# Format <LP ID> <Group ID> <Router ID> <Window ID> <Link Traffic>\n");
          lp_io_write(lp->gid, (char*)"dragonfly-router-traffic-sample", written, s->output_buf5);
      }
      for(int i=0; i < p->counting_windows; i++) {
          written = sprintf(s->output_buf5, "\n %llu %d %d %lf", LLU(lp->gid), s->router_id / p->num_routers, s->router_id , (p->counting_start+(i+1)*p->counting_interval));
          for (int d=0; d < p->radix; d++)
              written += sprintf(s->output_buf5 + written, " %d", (s->agg_link_traffic[i][d]));
          lp_io_write(lp->gid, (char*)"dragonfly-router-traffic-sample", written, s->output_buf5);
      } 

      // for link busy time
      if(!s->router_id){
          written = sprintf(s->output_buf6, "# Format <LP ID> <Group ID> <Router ID> <Window ID> <Link Busy Time>\n");
          lp_io_write(lp->gid, (char*)"dragonfly-router-busytime-sample", written, s->output_buf6);
      }
      for(int i=0; i < p->counting_windows; i++) {
          written = sprintf(s->output_buf6, "\n %llu %d %d %lf", LLU(lp->gid), s->router_id / p->num_routers, s->router_id , (p->counting_start+(i+1)*p->counting_interval));
          for (int d=0; d < p->radix; d++)
              written += sprintf(s->output_buf6 + written, " %lf", (s->agg_busy_time[i][d]));
          lp_io_write(lp->gid, (char*)"dragonfly-router-busytime-sample", written, s->output_buf6);
      }       
    }

}

static Connection do_dfdally_routing(router_state *s, tw_bf *bf, terminal_dally_message *msg, tw_lp *lp, int fdest_router_id)
{
    int my_router_id = s->router_id;
    int my_group_id = s->group_id;
    int fdest_group_id = fdest_router_id / s->params->num_routers;
    int origin_group_id = msg->origin_router_id / s->params->num_routers;
    

    if (!isRoutingSmart(routing))
    {
        // Legacy progressive adaptive routing has its own ways of doing this. These should probably be moved
        // into their own function as "default behavior" that is then called when needed by the other routing
        // functions individually.
        // If link failures are enabled, then this default local destination group routing can't be considered valid
        // as it assumes full connectivity in the local groups.
        // TODO: put the local destination group routing into its own function for "default" behavior.
        // if (routing != PROG_ADAPTIVE_LEGACY && !netMan.is_link_failures_enabled()) //for now when intra and interconnections aren't allowed to be failed, this is OK.
        if (routing != PROG_ADAPTIVE_LEGACY)
        {
            // // Local Destination Group Routing --------------
            if (my_router_id == fdest_router_id) { //destination router reached, next dest = final terminal destination
                vector< Connection > poss_next_stops = s->connMan.get_connections_to_gid(msg->dfdally_dest_terminal_id, CONN_TERMINAL);
                if (poss_next_stops.size() < 1)
                    tw_error(TW_LOC, "Destination Router %d: No connection to destination terminal %d\n", s->router_id, msg->dfdally_dest_terminal_id); //shouldn't happen unless math was wrong
                
                Connection best_min_conn = get_absolute_best_connection_from_conns(s, bf, msg, lp, poss_next_stops);
                return best_min_conn;
            }
            else if (my_group_id == fdest_group_id) { //Then we're already in the destination group and should just route to the fdest router
                vector< Connection > conns_to_fdest = s->connMan.get_connections_to_gid(fdest_router_id, CONN_LOCAL);
                if (conns_to_fdest.size() < 1)
                {
                    vector< Connection > poss_next_stops = get_legal_minimal_stops(s, bf, msg, lp, fdest_router_id);
                    if(poss_next_stops.size() < 1)
                        tw_error(TW_LOC, "Destination Group %d: No connection to destination router %d\n", s->router_id, fdest_router_id); //shouldn't happen unless the connections weren't set up / loaded correctly
                    conns_to_fdest = poss_next_stops;
                }
                    

                if (isRoutingAdaptive(routing)) { // Pick the best connection
                    Connection best_conn = get_absolute_best_connection_from_conns(s, bf, msg, lp, conns_to_fdest);
                    return best_conn;
                }
                else { //Randomize the next legal stop
                    msg->num_rngs++;
                    int rand_sel = tw_rand_integer(lp->rng, 0, conns_to_fdest.size()-1);
                    Connection next_conn = conns_to_fdest[rand_sel];
                    return next_conn;
                }
            }
            // // End Local Destination Group Routing -------------

            if(msg->last_hop == TERMINAL) //This is used by dfdally_nonminimal_routing and dfdally_prog_adaptive_routing
            {
                if (msg->intm_grp_id == -1)
                    dfdally_select_intermediate_group(s, bf, msg, lp, fdest_router_id);
            }
        }
    }

    Connection next_stop_conn;
    switch (routing)
    {
        case MINIMAL:
            next_stop_conn = dfdally_minimal_routing(s, bf, msg, lp, fdest_router_id);
            break;
        case NON_MINIMAL:
            msg->path_type = NON_MINIMAL;            
            next_stop_conn = dfdally_nonminimal_routing(s, bf, msg, lp, fdest_router_id);
            break;
        case ADAPTIVE:
            tw_error(TW_LOC, "Standard Adaptive Routing not implemented - try Progressive Adaptive instead");
            break;
        case PROG_ADAPTIVE:
            next_stop_conn = dfdally_prog_adaptive_routing(s, bf, msg, lp, fdest_router_id);
            break;
        case PROG_ADAPTIVE_LEGACY:
            next_stop_conn = dfdally_prog_adaptive_legacy_routing(s, bf, msg, lp, fdest_router_id);
            break;
        case SMART_PROG_ADAPTIVE:
            next_stop_conn = dfdally_smart_prog_adaptive_routing(s, bf, msg, lp, fdest_router_id);
            break;
        case SMART_MINIMAL:
            next_stop_conn = dfdally_smart_minimal_routing(s, bf, msg, lp, fdest_router_id);
            break;
        case SMART_NON_MINIMAL:
            next_stop_conn = dfdally_smart_nonminimal_routing(s, bf, msg, lp, fdest_router_id);
            break;
        default:
            tw_error(TW_LOC, "Error: No valid routing algorithm specified");
            break;
    }

    return next_stop_conn;
}

static void router_verify_valid_receipt(router_state *s, tw_bf *bf, terminal_dally_message *msg, tw_lp *lp)
{
    if (msg->my_N_hop > s->params->max_hops_notify)
    {
        printf("Router received a packet with %d hops so far! (Notify on > than %d)\n",msg->my_N_hop, s->params->max_hops_notify);
    }

    tw_lpid last_sender_lpid = msg->intm_lp_id;
    int rel_id, src_term_rel_id;

    bool has_valid_connection;
    if (msg->last_hop == TERMINAL) {
        tw_lpid src_term_lpgid = msg->src_terminal_id;

        try {
            src_term_rel_id = codes_mapping_get_lp_relative_id(src_term_lpgid,0,0);
        }
        catch (...) {
            tw_error(TW_LOC, "\nRouter Receipt Verify: Codes Mapping Get LP Rel ID Failure - Terminal");
        }

        // has_valid_connection = (s->router_id == (src_term_rel_id / s->params->num_cn)); //a router can only receive a packet from a terminal if that terminal belongs to it
        has_valid_connection = s->connMan.is_connected_to_by_type(src_term_rel_id, CONN_TERMINAL);

        if (!has_valid_connection) {
            tw_error(TW_LOC, "\nRouter received packet from non-existent connection - Terminal\n");
        }
    
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
        has_valid_connection = s->connMan.is_connected_to_by_type(rel_loc_id, CONN_LOCAL);
    }
    else if (msg->last_hop == GLOBAL)
    {
        try {
            rel_id = codes_mapping_get_lp_relative_id(last_sender_lpid,0,0);
        }
        catch (...) {
            tw_error(TW_LOC, "\nRouter Receipt Verify: Codes Mapping Get LP Rel ID Failure - Global");
        }
        has_valid_connection = s->connMan.is_connected_to_by_type(rel_id, CONN_GLOBAL);
    }
    else {
        tw_error(TW_LOC, "\nUnspecified msg->last_hop when received by a router\n");
    }
    
}

/*When a packet is sent from the current router and a buffer slot becomes available, a credit is sent back to schedule another packet event*/
static void router_credit_send(router_state * s, terminal_dally_message * msg, 
  tw_lp * lp, int sq, short* rng_counter) {
    tw_event * buf_e;
    tw_stime ts;
    terminal_dally_message * buf_msg;

    int dest = 0,  type = R_BUFFER;
    int is_terminal = 0;
    double credit_delay;

    const dragonfly_param *p = s->params;
    
    // Notify sender terminal about available buffer space
    if(msg->last_hop == TERMINAL) {
        dest = msg->src_terminal_id;
        type = T_BUFFER;
        is_terminal = 1;
        credit_delay = p->cn_credit_delay;
    } 
    else if(msg->last_hop == GLOBAL) {
        dest = msg->intm_lp_id;
        credit_delay = p->global_credit_delay;
    }
    else if(msg->last_hop == LOCAL) {
        dest = msg->intm_lp_id;
        credit_delay = p->local_credit_delay;
    } 
    else
        printf("\n Invalid message type");

    /* TODO: Credit delay should really be a combination of credit processing time,
     * the injection delay, and propagation delay of the channel. But this level of
     * granularity _may_ only be necessary for specific credit-based flow control
     * studies. It should certainly be considered for those studies. */
    ts = credit_delay;

    if (is_terminal) {
        buf_e = model_net_method_event_new(dest, ts + gen_noise(lp, &msg->num_rngs), lp, DRAGONFLY_DALLY, 
        (void**)&buf_msg, NULL);
        buf_msg->magic = terminal_magic_num;
    } 
    else {
        buf_e = model_net_method_event_new(dest, ts + gen_noise(lp, &msg->num_rngs), lp, DRAGONFLY_DALLY_ROUTER,
                (void**)&buf_msg, NULL);
        buf_msg->magic = router_magic_num;
    }
    
    buf_msg->rail_id = msg->rail_id;
    buf_msg->origin_router_id = s->router_id;
    if(sq == -1) {
        buf_msg->vc_index = msg->vc_index;
        buf_msg->output_chan = msg->output_chan;
    } else {
        buf_msg->vc_index = msg->saved_vc;
        buf_msg->output_chan = msg->saved_channel;
    }
    strcpy(buf_msg->category, msg->category); 
    buf_msg->type = type;

    tw_event_send(buf_e);
    return;
}

static void router_packet_receive_rc(router_state * s,
        tw_bf * bf,
        terminal_dally_message * msg,
        tw_lp * lp)
{  
    int output_port = msg->saved_vc;
    int output_chan = msg->saved_channel;
    int src_term_id = msg->dfdally_src_terminal_id;
    int app_id = msg->saved_app_id;

    if(bf->c1)
        s->is_monitoring_bw = 0;

    if(bf->c2) {
        terminal_dally_message_list * tail = return_tail(s->pending_msgs[output_port], s->pending_msgs_tail[output_port], output_chan);
        delete_terminal_dally_message_list(tail);
        s->vc_occupancy[output_port][output_chan] -= s->params->chunk_size;
        if(bf->c3) {
            s->in_send_loop[output_port] = 0;
        }
    }
    if(bf->c4) {
        s->stalled_chunks[output_port]--;
        if(bf->c22)
        {
            s->last_buf_full[output_port] = msg->saved_busy_time;
        }
    delete_terminal_dally_message_list(return_tail(s->queued_msgs[output_port], 
        s->queued_msgs_tail[output_port], output_chan));
    s->queued_count[output_port] -= s->params->chunk_size; 
    }

    if (g_congestion_control_enabled) {
        congestion_control_message *cc_msg_rc = (congestion_control_message*)rc_stack_pop(s->cc_st);
        cc_router_received_packet_rc(s->local_congestion_controller, lp, s->params->chunk_size, output_port, output_chan, src_term_id, app_id, cc_msg_rc);
        cc_msg_rc_storage_delete(cc_msg_rc);
    }
}

/* Packet arrives at the router and a credit is sent back to the sending terminal/router */
static void router_packet_receive( router_state * s, 
			tw_bf * bf, 
			terminal_dally_message * msg, 
			tw_lp * lp )
{
    router_verify_valid_receipt(s, bf, msg, lp);

    tw_stime ts;

    int num_qos_levels = s->params->num_qos_levels;
    int vcs_per_qos = s->params->num_vcs / num_qos_levels;

    if(num_qos_levels > 1)
    {
        if(s->is_monitoring_bw == 0)
        {
            bf->c1 = 1;
            tw_stime bw_ts = bw_reset_window;
            terminal_dally_message * m;
            tw_event * e = model_net_method_event_new(lp->gid, bw_ts + gen_noise(lp, &msg->num_rngs), lp, 
                DRAGONFLY_DALLY_ROUTER, (void**)&m, NULL); 
            m->type = R_BANDWIDTH; 
            m->magic = router_magic_num;
            tw_event_send(e);
            s->is_monitoring_bw = 1;
        }
    }
    int vcg = 0;
    if(num_qos_levels > 1)
        vcg = get_vcg_from_category(msg);

    int num_routers = s->params->num_routers;
    int num_groups = s->params->num_groups;
    int total_routers = s->params->total_routers;

    int next_stop = -1, output_port = -1, output_chan = -1;
    int dest_router_id = dfdally_get_assigned_router_id_from_terminal(s->params, msg->dfdally_dest_terminal_id, s->plane_id);
    // int dest_router_id = codes_mapping_get_lp_relative_id(msg->dest_terminal_lpid, 0, 0) / s->params->num_cn;

    msg->this_router_arrival = tw_now(lp);
    terminal_dally_message_list * cur_chunk = (terminal_dally_message_list*)calloc(1, sizeof(terminal_dally_message_list));
    init_terminal_dally_message_list(cur_chunk, msg);
    
    if(cur_chunk->msg.last_hop == TERMINAL) // We are first router in the path
        cur_chunk->msg.path_type = MINIMAL; // Route always starts as minimal


    int num_rngs_before = (cur_chunk->msg).num_rngs;
    Connection next_stop_conn = do_dfdally_routing(s, bf, &(cur_chunk->msg), lp, dest_router_id);
    msg->num_rngs += (cur_chunk->msg).num_rngs - num_rngs_before; //make sure we're counting the rngs called during do_dfdally_routing()
    cur_chunk->msg.num_rngs = num_rngs_before;

    if (s->connMan.is_any_connection_to(next_stop_conn.dest_gid) == false)
        tw_error(TW_LOC, "Router %d does not have a connection to chosen destination %d\n", s->router_id, next_stop_conn.dest_gid);

    output_port = next_stop_conn.port;

    if (next_stop_conn.conn_type != CONN_TERMINAL) {
        int id = next_stop_conn.dest_gid;
        tw_lpid router_dest_id;
        codes_mapping_get_lp_id(lp_group_name, LP_CONFIG_NM_ROUT, s->anno, 0, id / num_routers_per_mgrp, id % num_routers_per_mgrp, &router_dest_id);
        next_stop = router_dest_id;
    }
    else {
        next_stop = cur_chunk->msg.dest_terminal_lpid;
    }

    //From here the output port is known and output_chan is determined shortly
    assert(output_port >= 0);
    cur_chunk->msg.vc_index = output_port;
    cur_chunk->msg.next_stop = next_stop;

    // printf("Router %d: Output Port = %d      next stop = %d\n",s->router_id, output_port, next_stop);

    int max_vc_size = s->params->cn_vc_size;

    int my_group_id = s->group_id;
    int src_group_id = msg->origin_router_id / num_routers;
    int dest_group_id = dest_router_id / num_routers;

    output_chan = 0;
    if (my_group_id == src_group_id)
    {
        output_chan = cur_chunk->msg.my_l_hop;
        if(msg->path_type == NON_MINIMAL)
            output_chan = 1;
    }
    else if (my_group_id != src_group_id && my_group_id != dest_group_id)
    {
        assert(msg->path_type == NON_MINIMAL);
        output_chan = 2;
    }
    else if (my_group_id == dest_group_id)
    {
        output_chan = 3;
    }

    if (next_stop_conn.conn_type == CONN_LOCAL)
    {
        max_vc_size = s->params->local_vc_size;
        cur_chunk->msg.my_l_hop++;
        cur_chunk->msg.my_hops_cur_group++;
    }
    if (next_stop_conn.conn_type == CONN_GLOBAL)
    {
        max_vc_size = s->params->global_vc_size;
        cur_chunk->msg.my_hops_cur_group = 0; //reset this as it's going to a new group
        cur_chunk->msg.my_g_hop++;
    }

    //this seemed outdated with current literature and was replaced with the scheme above.
    // output_chan = 0;
    // if(output_port < s->params->intra_grp_radix) {
    //     if(cur_chunk->msg.my_g_hop == 1 && cur_chunk->msg.last_hop == GLOBAL) {
    //         output_chan = 1;
    //     }
    //     else if(cur_chunk->msg.my_g_hop == 1 && cur_chunk->msg.last_hop == LOCAL) {
    //         output_chan = 2;
    //     }
    //     else if (cur_chunk->msg.my_g_hop == 2) {
    //         output_chan = 3;
    //     }

    //     max_vc_size = s->params->local_vc_size;
    //     cur_chunk->msg.my_l_hop++;
    //     cur_chunk->msg.my_hops_cur_group++;
    // }
    // else if(output_port < (s->params->intra_grp_radix +
    //     s->params->num_global_channels))
    // {
    //     output_chan = cur_chunk->msg.my_g_hop;
    //     max_vc_size = s->params->global_vc_size;
    //     cur_chunk->msg.my_hops_cur_group = 0; //reset this as it's going to a new group
    //     cur_chunk->msg.my_g_hop++;
    // }
        
    assert(output_chan < vcs_per_qos);
    output_chan = output_chan + (vcg * vcs_per_qos);
    assert(output_chan < s->params->num_vcs && output_chan >= 0);

    cur_chunk->msg.output_chan = output_chan;
    cur_chunk->msg.my_N_hop++;

    if(output_port >= s->params->radix)
        tw_error(TW_LOC, "\n Output port greater than router radix %d ", output_port);
    
    if(output_chan >= s->params->num_vcs || output_chan < 0)
        tw_error(TW_LOC, "\n Output channel %d great than available VCs %d", output_chan, s->params->num_vcs - 1);
                //cur_chunk->msg.packet_ID, output_chan, output_port, s->router_id, dest_router_id, cur_chunk->msg.path_type, src_grp_id, dest_grp_id, msg->src_terminal_id);

#if DEBUG == 1
    if(cur_chunk->msg.packet_ID == LLU(TRACK_PKT) && cur_chunk->msg.src_terminal_id == T_ID)
            printf("\n Packet %llu arrived at router %u next stop %d final stop %d local hops %d global hops %d", cur_chunk->msg.packet_ID, s->router_id, next_stop, dest_router_id, cur_chunk->msg.my_l_hop, cur_chunk->msg.my_g_hop);
#endif

    if(msg->remote_event_size_bytes > 0) {
        void *m_data_src = model_net_method_get_edata(DRAGONFLY_DALLY_ROUTER, msg);
        cur_chunk->event_data = (char*)calloc(1, msg->remote_event_size_bytes);
        memcpy(cur_chunk->event_data, m_data_src, msg->remote_event_size_bytes);
    }

    if(s->vc_occupancy[output_port][output_chan] + s->params->chunk_size  <= max_vc_size) {
        bf->c2 = 1;
        assert(output_chan < s->params->num_vcs && output_port < s->params->radix);
        router_credit_send(s, msg, lp, -1, &(msg->num_rngs));
    
        append_to_terminal_dally_message_list(s->pending_msgs[output_port], s->pending_msgs_tail[output_port],
                                            output_chan, cur_chunk);
        s->vc_occupancy[output_port][output_chan] += s->params->chunk_size;
        if(s->in_send_loop[output_port] == 0) {
            bf->c3 = 1;
            terminal_dally_message *m;
            ts = maxd(s->next_output_available_time[output_port], tw_now(lp)) - tw_now(lp);
            tw_event *e = model_net_method_event_new(lp->gid, ts + gen_noise(lp, &msg->num_rngs), lp,
                    DRAGONFLY_DALLY_ROUTER, (void**)&m, NULL);
            m->rail_id = msg->rail_id;
            m->type = R_SEND;
            m->magic = router_magic_num;
            m->vc_index = output_port;
            
            tw_event_send(e);
            s->in_send_loop[output_port] = 1;
        }
    } 
    else 
    {
        bf->c4 = 1;
        s->stalled_chunks[output_port]++;
        cur_chunk->msg.saved_vc = msg->vc_index;
        cur_chunk->msg.saved_channel = msg->output_chan;
        assert(output_chan < s->params->num_vcs && output_port < s->params->radix);
        append_to_terminal_dally_message_list( s->queued_msgs[output_port], 
        s->queued_msgs_tail[output_port], output_chan, cur_chunk);
        s->queued_count[output_port] += s->params->chunk_size;


        //THIS WAS REMOVED WHEN QOS WAS INSTITUTED - READDED 5/20/19
        /* a check for pending msgs is non-empty then we dont set anything. If
        * that is empty then we check if last_buf_full is set or not. If already
        * set then we don't overwrite it. If two packets arrive next to each other
        * then the first person should be setting it. */
        if(s->last_buf_full[output_port] == 0.0)
        {
            bf->c22 = 1;
            msg->saved_busy_time = s->last_buf_full[output_port];
            s->last_buf_full[output_port] = tw_now(lp);
        }
    }

    if (g_congestion_control_enabled) {
            congestion_control_message *cc_msg_rc = cc_msg_rc_storage_create();
            cc_router_received_packet(s->local_congestion_controller, lp, s->params->chunk_size, output_port, output_chan, cur_chunk->msg.dfdally_src_terminal_id, cur_chunk->msg.app_id, cc_msg_rc);
            rc_stack_push(lp, cc_msg_rc, cc_msg_rc_storage_delete, s->cc_st);
    }

    msg->saved_app_id = cur_chunk->msg.app_id;
    msg->dfdally_src_terminal_id = cur_chunk->msg.dfdally_src_terminal_id;
    msg->saved_vc = output_port;
    msg->saved_channel = output_chan;
    return;
}

static void router_packet_send_rc(router_state * s, tw_bf * bf, terminal_dally_message * msg, tw_lp * lp)
{
    int num_qos_levels = s->params->num_qos_levels;
   
    int output_port = msg->saved_vc;
    int src_term_id = msg->dfdally_src_terminal_id;
    int app_id = msg->saved_app_id;
      
    if(msg->qos_reset1)
        s->qos_status[output_port][0] = Q_ACTIVE;
    if(msg->qos_reset2)
        s->qos_status[output_port][1] = Q_ACTIVE;
    
    if(msg->last_saved_qos)
       s->last_qos_lvl[output_port] = msg->last_saved_qos; 

    //Xin: target window to rollback
    bool rolback = false;
    int current_window = -1;
    const dragonfly_param *p = s->params;
    if(s->params->counting_bool>0 && msg->last_sent_time >= s->params->counting_start) {
        current_window = (int) ((msg->last_sent_time-s->params->counting_start)/s->params->counting_interval);
        if(current_window < s->params->counting_windows) {
          rolback = true;
        }
    }      

    if(bf->c1) {
        s->in_send_loop[output_port] = msg->saved_send_loop;
        if(bf->c2) {
            s->last_buf_full[output_port] = msg->saved_busy_time;
        }
        return;  
    }
    s->last_qos_lvl[output_port] = msg->last_saved_qos;

    int output_chan = msg->saved_channel;
    if(bf->c8)
    {
        s->busy_time[output_port] = msg->saved_rcv_time;
        s->busy_time_sample[output_port] = msg->saved_sample_time;
        s->ross_rsample.busy_time[output_port] = msg->saved_sample_time;
        s->last_buf_full[output_port] = msg->saved_busy_time;

        //Xin: reverse busy time
        if(rolback && current_window >= 0){
          s->agg_busy_time[current_window][output_port] = msg->saved_rcv_time;
        }
    }
      
    terminal_dally_message_list * cur_entry = (terminal_dally_message_list *)rc_stack_pop(s->st);
    assert(cur_entry);
 
    int vcg = 0;
    if(num_qos_levels > 1)
        vcg = get_vcg_from_category(&(cur_entry->msg));

    int msg_size = s->params->chunk_size;
    if(cur_entry->msg.packet_size < s->params->chunk_size)
        msg_size = cur_entry->msg.packet_size;

    s->qos_data[output_port][vcg] -= msg_size;
    s->next_output_available_time[output_port] = msg->saved_available_time;

    if(bf->c11)
    {
        s->link_traffic[output_port] -= cur_entry->msg.packet_size % s->params->chunk_size;
        s->link_traffic_sample[output_port] -= cur_entry->msg.packet_size % s->params->chunk_size; 
        s->ross_rsample.link_traffic_sample[output_port] -= cur_entry->msg.packet_size % s->params->chunk_size; 
        s->link_traffic_ross_sample[output_port] -= cur_entry->msg.packet_size % s->params->chunk_size; 

        //Xin: reverse link traffic
        if(rolback && current_window >= 0){
          s->agg_link_traffic[current_window][output_port] -= cur_entry->msg.packet_size % s->params->chunk_size;
        }
    }
    if(bf->c12)
    {
        s->link_traffic[output_port] -= s->params->chunk_size;
        s->link_traffic_sample[output_port] -= s->params->chunk_size;
        s->ross_rsample.link_traffic_sample[output_port] -= s->params->chunk_size;
        s->link_traffic_ross_sample[output_port] -= s->params->chunk_size;

        //Xin: reverse link traffic
        if(rolback && current_window >= 0){
          s->agg_link_traffic[current_window][output_port] -= s->params->chunk_size;
        }
    }

    s->total_chunks[output_port]--;

    prepend_to_terminal_dally_message_list(s->pending_msgs[output_port],
            s->pending_msgs_tail[output_port], output_chan, cur_entry);

    if (g_congestion_control_enabled) {
        congestion_control_message *cc_msg_rc = (congestion_control_message*)rc_stack_pop(s->cc_st);
        cc_router_forwarded_packet_rc(s->local_congestion_controller, lp, s->params->chunk_size, output_port, output_chan, src_term_id, app_id, cc_msg_rc);
        cc_msg_rc_storage_delete(cc_msg_rc);
    }

    if(bf->c4) {
        s->in_send_loop[output_port] = msg->saved_send_loop;
    }
}
/* routes the current packet to the next stop */
static void router_packet_send( router_state * s, tw_bf * bf, terminal_dally_message * msg, tw_lp * lp)
{
    tw_event *e;
    terminal_dally_message *m;
    int output_port = msg->vc_index;
    int is_local = 0;
    terminal_dally_message_list *cur_entry = NULL;

    /* reset qos rc handler before incrementing it */
    msg->last_saved_qos = -1;
    msg->qos_reset1 = -1;
    msg->qos_reset2 = -1;
    msg->saved_send_loop = s->in_send_loop[output_port];

    int num_qos_levels = s->params->num_qos_levels;
    int output_chan = get_next_router_vcg(s, bf, msg, lp); //includes default output_chan setting functionality if qos not enabled
    
    msg->saved_vc = output_port;
    msg->saved_channel = output_chan;

    //Xin: target window to update link traffic 
    msg->last_sent_time = tw_now(lp);
    bool update = false;
    int current_window = -1;
    const dragonfly_param *p = s->params;
    if(s->params->counting_bool>0 && msg->last_sent_time >= s->params->counting_start) {
      current_window = (int) ((msg->last_sent_time - s->params->counting_start)/s->params->counting_interval);
      if(current_window < s->params->counting_windows) {
          update = true;
        }
    }
    
    if(output_chan < 0) 
    {
        bf->c1 = 1;
        s->in_send_loop[output_port] = 0;
        if(s->queued_count[output_port] && !s->last_buf_full[output_port])  //5-21-19, not sure why this was added here with the qos stuff
        {
            bf->c2 = 1; 
            msg->saved_busy_time = s->last_buf_full[output_port];
            s->last_buf_full[output_port] = tw_now(lp);
        }
        return;
    }

    cur_entry = s->pending_msgs[output_port][output_chan];
    
    msg->dfdally_src_terminal_id = cur_entry->msg.dfdally_src_terminal_id;

    assert(cur_entry != NULL);

    if(s->last_buf_full[output_port]) //5-12-19, same here as above comment
    {
        bf->c8 = 1;
        msg->saved_rcv_time = s->busy_time[output_port]; 
        msg->saved_busy_time = s->last_buf_full[output_port]; 
        msg->saved_sample_time = s->busy_time_sample[output_port];  
        s->busy_time[output_port] += (tw_now(lp) - s->last_buf_full[output_port]); 
        s->busy_time_sample[output_port] += (tw_now(lp) - s->last_buf_full[output_port]);
        s->ross_rsample.busy_time[output_port] += (tw_now(lp) - s->last_buf_full[output_port]);
        s->last_buf_full[output_port] = 0.0;

        //Xin: update link busy time
        if(update && current_window >= 0){
            s->agg_busy_time[current_window][output_port] += (tw_now(lp) - s->last_buf_full[output_port]); 
        }
    }

    int vcg = 0;
    if(num_qos_levels > 1)
        vcg = get_vcg_from_category(&(cur_entry->msg));
        
    int to_terminal = 1, global = 0;
    double delay = s->params->cn_delay;
    double bandwidth = s->params->cn_bandwidth;

    if(output_port < s->params->intra_grp_radix) {
        to_terminal = 0;
        delay = s->params->local_delay;
        bandwidth = s->params->local_bandwidth;
    } 
    else if(output_port < s->params->intra_grp_radix + 
            s->params->num_global_channels) {
        to_terminal = 0;
        global = 1;
        delay = s->params->global_delay;
        bandwidth = s->params->global_bandwidth;
    }

    uint64_t num_chunks = cur_entry->msg.packet_size / s->params->chunk_size;
    if(cur_entry->msg.packet_size < s->params->chunk_size)
        num_chunks++;

    /* Injection delay: Time taken for the data to be placed on the link/channel
     *  - Based on bandwidth
     * Propagtion delay: Time taken for the data to cross the link and arrive at the reciever
     *  - A physical property of the material of teh link (eg. copper, optical fiber)
     */
    tw_stime injection_ts, injection_delay;
    tw_stime propagation_ts, propagation_delay;

    propagation_delay = delay;
    injection_delay = bytes_to_ns(s->params->chunk_size, bandwidth);

    if(cur_entry->msg.packet_size == 0)
        injection_delay = bytes_to_ns(s->params->credit_size, bandwidth);

    if((cur_entry->msg.packet_size < s->params->chunk_size) && (cur_entry->msg.chunk_id == num_chunks - 1))
        injection_delay = bytes_to_ns(cur_entry->msg.packet_size % s->params->chunk_size, bandwidth);

    injection_delay += s->params->router_delay;

    msg->saved_available_time = s->next_output_available_time[output_port];
    s->next_output_available_time[output_port] = 
        maxd(s->next_output_available_time[output_port], tw_now(lp));
    s->next_output_available_time[output_port] += injection_delay;

    injection_ts = s->next_output_available_time[output_port] - tw_now(lp);
    propagation_ts = injection_ts + propagation_delay;

    cur_entry->msg.this_router_ptp_latency = s->next_output_available_time[output_port] - cur_entry->msg.this_router_arrival;
    msg->this_router_ptp_latency = cur_entry->msg.this_router_ptp_latency;

    // dest can be a router or a terminal, so we must check
    void * m_data;
    if (to_terminal) {
        // printf("\n next stop %d dest term id %d ", cur_entry->msg.next_stop, cur_entry->msg.dest_terminal_lpid);
        if(cur_entry->msg.next_stop != cur_entry->msg.dest_terminal_lpid)
        printf("\n intra-group radix %d output port %d next stop %d", s->params->intra_grp_radix, output_port, cur_entry->msg.next_stop);
        assert(cur_entry->msg.next_stop == cur_entry->msg.dest_terminal_lpid);
        e = model_net_method_event_new(cur_entry->msg.next_stop, 
            propagation_ts + gen_noise(lp, &msg->num_rngs), lp, DRAGONFLY_DALLY, (void**)&m, &m_data);
    }
    else {
        e = model_net_method_event_new(cur_entry->msg.next_stop,
                propagation_ts + gen_noise(lp, &msg->num_rngs), lp, DRAGONFLY_DALLY_ROUTER, (void**)&m, &m_data);
    }
    memcpy(m, &cur_entry->msg, sizeof(terminal_dally_message));
    if (m->remote_event_size_bytes) {
        memcpy(m_data, cur_entry->event_data, m->remote_event_size_bytes);
    }

    if(global)
        m->last_hop = GLOBAL;
    else
        m->last_hop = LOCAL;

    m->intm_lp_id = lp->gid;
    m->magic = router_magic_num;

    int msg_size = s->params->chunk_size;
    if(((cur_entry->msg.packet_size % s->params->chunk_size) || cur_entry->msg.packet_size == 0) && (cur_entry->msg.chunk_id == num_chunks - 1)) {
        bf->c11 = 1;
        s->link_traffic[output_port] +=  (cur_entry->msg.packet_size % s->params->chunk_size); 
        s->link_traffic_sample[output_port] += (cur_entry->msg.packet_size % s->params->chunk_size);
        s->ross_rsample.link_traffic_sample[output_port] += (cur_entry->msg.packet_size % s->params->chunk_size);
        s->link_traffic_ross_sample[output_port] += (cur_entry->msg.packet_size % s->params->chunk_size);
        msg_size = cur_entry->msg.packet_size % s->params->chunk_size;

        //Xin: update link traffic
        if(update && current_window >= 0){
            s->agg_link_traffic[current_window][output_port] += (cur_entry->msg.packet_size % s->params->chunk_size);           
        }
    } 
    else {
        bf->c12 = 1;
        s->link_traffic[output_port] += s->params->chunk_size;
        s->link_traffic_sample[output_port] += s->params->chunk_size;
        s->ross_rsample.link_traffic_sample[output_port] += s->params->chunk_size;
        s->link_traffic_ross_sample[output_port] += s->params->chunk_size;

        //Xin: update link traffic 
        if(update && current_window >= 0){
            s->agg_link_traffic[current_window][output_port] += s->params->chunk_size;
        }
    }

    s->total_chunks[output_port]++;

#if DEBUG == 1
    if(cur_entry->msg.packet_ID == LLU(TRACK_PKT) && cur_entry->msg.src_terminal_id == T_ID)
        printf("\n Queuing at the router %d ", s->router_id);
#endif

    m->rail_id = msg->rail_id;

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

    msg->saved_app_id = cur_entry->msg.app_id;
    if (g_congestion_control_enabled) {
        congestion_control_message *cc_msg_rc = cc_msg_rc_storage_create();
        cc_router_forwarded_packet(s->local_congestion_controller, lp, s->params->chunk_size, output_port, output_chan, cur_entry->msg.dfdally_src_terminal_id, cur_entry->msg.app_id, cc_msg_rc);
        rc_stack_push(lp, cc_msg_rc, cc_msg_rc_storage_delete, s->cc_st);
    }

    cur_entry = return_head(s->pending_msgs[output_port], 
        s->pending_msgs_tail[output_port], output_chan);
    rc_stack_push(lp, cur_entry, delete_terminal_dally_message_list, s->st);

    s->qos_data[output_port][vcg] += msg_size; 
    s->next_output_available_time[output_port] -= s->params->router_delay;
    injection_ts -= s->params->router_delay;

    int next_output_chan = -1;
    int base_limit = 0;
    int vcs_per_qos = s->params->num_vcs / num_qos_levels;
    for(int i = 0; i < num_qos_levels; i++)
    {
        base_limit = i * vcs_per_qos;
        for(int k = base_limit; k < base_limit + vcs_per_qos; k ++)
        {
            if(s->pending_msgs[output_port][k] != NULL)
            {
                next_output_chan = k;
                break;
            }
        }
        if(next_output_chan >= 0)
            break;
    }

    if(next_output_chan < 0)
    {
        bf->c4 = 1;
        s->in_send_loop[output_port] = 0;
        return;
    }
    cur_entry = s->pending_msgs[output_port][next_output_chan];
    assert(cur_entry != NULL); 

    terminal_dally_message *m_new;
    e = model_net_method_event_new(lp->gid, injection_ts + gen_noise(lp, &msg->num_rngs), lp, DRAGONFLY_DALLY_ROUTER,
                (void**)&m_new, NULL);
    m_new->type = R_SEND;
    m_new->rail_id = msg->rail_id;
    m_new->magic = router_magic_num;
    m_new->vc_index = output_port;
    tw_event_send(e);
    return;
}

static void router_buf_update_rc(router_state * s,
        tw_bf * bf,
        terminal_dally_message * msg,
        tw_lp * lp)
{
    int indx = msg->vc_index;
    int output_chan = msg->output_chan;
    s->vc_occupancy[indx][output_chan] += s->params->chunk_size;

    if(bf->c3)
    {
        s->busy_time[indx] = msg->saved_rcv_time;
        s->busy_time_sample[indx] = msg->saved_sample_time;
        s->ross_rsample.busy_time[indx] = msg->saved_sample_time;
        s->busy_time_ross_sample[indx] = msg->saved_busy_time_ross;
        s->last_buf_full[indx] = msg->saved_busy_time;

        //Xin: reverse agg busytime (not working for cross window reverse)
        const dragonfly_param *p = s->params;
        if(s->params->counting_bool>0 && msg->last_bufupdate_time >= s->params->counting_start) {
            int current_window = (int) ((msg->last_bufupdate_time - s->params->counting_start)/s->params->counting_interval);
            if(current_window < s->params->counting_windows) {
              s->agg_busy_time[current_window][indx] = msg->saved_rcv_time;
            }
        }
    }
    if(bf->c1) {
        terminal_dally_message_list* head = return_tail(s->pending_msgs[indx],
            s->pending_msgs_tail[indx], output_chan);
        prepend_to_terminal_dally_message_list(s->queued_msgs[indx], 
            s->queued_msgs_tail[indx], output_chan, head);
        s->vc_occupancy[indx][output_chan] -= s->params->chunk_size;
        s->queued_count[indx] += s->params->chunk_size;
    }
    if(bf->c2) {
        s->in_send_loop[indx] = 0;
    }
}
/* Update the buffer space associated with this router LP */
static void router_buf_update(router_state * s, tw_bf * bf, terminal_dally_message * msg, tw_lp * lp)
{

    int indx = msg->vc_index;
    int output_chan = msg->output_chan;
    s->vc_occupancy[indx][output_chan] -= s->params->chunk_size;

    if(s->last_buf_full[indx] > 0.0)
    {
        bf->c3 = 1;
        msg->saved_rcv_time = s->busy_time[indx];
        msg->saved_busy_time = s->last_buf_full[indx];
        msg->saved_sample_time = s->busy_time_sample[indx];
        msg->saved_busy_time_ross = s->busy_time_ross_sample[indx];
        s->busy_time[indx] += (tw_now(lp) - s->last_buf_full[indx]);
        s->busy_time_sample[indx] += (tw_now(lp) - s->last_buf_full[indx]);
        s->ross_rsample.busy_time[indx] += (tw_now(lp) - s->last_buf_full[indx]);
        s->busy_time_ross_sample[indx] += (tw_now(lp) - s->last_buf_full[indx]);

        //Xin: update link busy time
        const dragonfly_param *p = s->params;
        msg->last_bufupdate_time = tw_now(lp);
        if(s->params->counting_bool>0 && msg->last_bufupdate_time >= s->params->counting_start) {
            int current_window = (int) ((msg->last_bufupdate_time - s->params->counting_start)/s->params->counting_interval);
            if(current_window < s->params->counting_windows) {
                int full_window = (int) ((s->last_buf_full[indx] - s->params->counting_start)/s->params->counting_interval);
                if(full_window==current_window) {
                    s->agg_busy_time[current_window][indx] += (tw_now(lp) - s->last_buf_full[indx]);
                } else {
                    s->agg_busy_time[current_window][indx] += (tw_now(lp) - (s->params->counting_start+current_window*s->params->counting_interval));
                    s->agg_busy_time[full_window][indx] += ((s->params->counting_start+current_window*s->params->counting_interval) - s->last_buf_full[indx]);
                }
            }
        }

        s->last_buf_full[indx] = 0.0;
    }

    if(s->queued_msgs[indx][output_chan] != NULL) {
        bf->c1 = 1;
        assert(indx < s->params->radix);
        assert(output_chan < s->params->num_vcs);
        terminal_dally_message_list *head = return_head(s->queued_msgs[indx],
            s->queued_msgs_tail[indx], output_chan);
        /*if(strcmp(head->msg.category, "medium") == 0)
        {
        if(head->msg.saved_channel < 4 || head->msg.saved_channel >= 8)
        {
                tw_error(TW_LOC, "\n invalid output chan %d last-hop %d", head->msg.saved_channel, head->msg.last_hop);
        }
        }*/
        router_credit_send(s, &head->msg, lp, 1, &(msg->num_rngs)); 
        append_to_terminal_dally_message_list(s->pending_msgs[indx], 
        s->pending_msgs_tail[indx], output_chan, head);
        s->vc_occupancy[indx][output_chan] += s->params->chunk_size;
        s->queued_count[indx] -= s->params->chunk_size; 
    }

    if(s->in_send_loop[indx] == 0 && s->pending_msgs[indx][output_chan] != NULL) {
        bf->c2 = 1;
        terminal_dally_message *m;
        tw_stime ts = maxd(s->next_output_available_time[indx], tw_now(lp)) - tw_now(lp);
        tw_event *e = model_net_method_event_new(lp->gid, ts + gen_noise(lp, &msg->num_rngs), lp, DRAGONFLY_DALLY_ROUTER,
                (void**)&m, NULL);
        m->type = R_SEND;
        m->rail_id = msg->rail_id;
        m->vc_index = indx;
        m->magic = router_magic_num;
        s->in_send_loop[indx] = 1;
        tw_event_send(e);
    }
    return;
}

static void 
terminal_dally_event( terminal_state * s, 
		tw_bf * bf, 
		terminal_dally_message * msg, 
		tw_lp * lp )
{
    msg->num_cll = 0;
    msg->num_rngs = 0;

    s->fwd_events++;
    s->ross_sample.fwd_events++;
    //*(int *)bf = (int)0;
    assert(msg->magic == terminal_magic_num);
    //printf("LPID: %llu Event type %d processed at %f\n", lp->gid, msg->type, tw_now(lp));

    if (is_dally_surrogate_on && freeze_network_on_switch) {
        // This event will be reversed. It comes from the past, it has been forwarded to the future
        // by the surrogate freezing the network procedure and should not be taken into account
        if (! (msg->type == T_GENERATE || msg->type == T_ARRIVE_PREDICTED || msg->type == T_NOTIFY)) {
            bf->c20 = 1;
            printf("This shouldn't happen! :( (time stamp = %e)\n", tw_now(lp));
            return;
        }
    } else {
        rc_stack_gc(lp, s->st);
        rc_stack_gc(lp, s->cc_st);
    }
    switch(msg->type)
        {
        case T_GENERATE:
            if (is_dally_surrogate_on) {
                bf->c10 = 1;
                packet_generate_predicted(s,bf,msg,lp);
            } else {
                packet_generate(s,bf,msg,lp);
            }
        break;
        
        case T_ARRIVE:
            packet_arrive(s,bf,msg,lp);
        break;

        case T_ARRIVE_PREDICTED:
            packet_arrive_predicted(s,bf,msg,lp);
        break;
        
        case T_SEND:
            packet_send(s,bf,msg,lp);
        break;
        
        case T_BUFFER:
            terminal_buf_update(s, bf, msg, lp);
        break;
    
        case T_BANDWIDTH:
            issue_bw_monitor_event(s, bf, msg, lp);
        break;
    
        case T_NOTIFY:
            process_terminal_notification_event(s, bf, msg, lp);
        break;

#if ALWAYS_DETERMINISTIC_NETWORK
        case T_VACUOUS_EVENT:
        break;
#endif /* ALWAYS_DETERMINISTIC_NETWORK */

        default:
            printf("\n LP %d Terminal message type not supported %d ", (int)lp->gid, msg->type);
            tw_error(TW_LOC, "Msg type not supported");
        }
}

static void router_dally_event(router_state * s, tw_bf * bf, terminal_dally_message * msg, 
    tw_lp * lp) 
{
    msg->num_cll = 0;
    msg->num_rngs = 0;

    s->fwd_events++;
    s->ross_rsample.fwd_events++;
    rc_stack_gc(lp, s->st);
    rc_stack_gc(lp, s->cc_st);

    msg->last_received_time = s->last_time;
    s->last_time = tw_now(lp);
    
    assert(msg->magic == router_magic_num);
    switch(msg->type)
    {
        case R_SEND: // Router has sent a packet to an intra-group router (local channel)
            // printf("%d: router packet send\n", s->router_id);
            router_packet_send(s, bf, msg, lp);
        break;

        case R_ARRIVE: // Router has received a packet from an intra-group router (local channel)
            // printf("%d: router packet recv\n", s->router_id);
            router_packet_receive(s, bf, msg, lp);
        break;

        case R_BUFFER:
            // printf("%d: router buf update\n", s->router_id);
            router_buf_update(s, bf, msg, lp);
        break;

        case R_BANDWIDTH:
            // printf("%d: router bandwidth monitor event\n", s->router_id);
            issue_rtr_bw_monitor_event(s, bf, msg, lp);
        break;

        case R_SNAPSHOT:
            router_handle_snapshot_event(s, bf, msg, lp);
        break;
        
        default:
            printf("\n (%lf) [Router %d] Router Message type not supported %d dest " 
                "terminal id %d packet ID %d ", tw_now(lp), (int)lp->gid, msg->type, 
                (int)msg->dest_terminal_lpid, (int)msg->packet_ID);
            tw_error(TW_LOC, "Msg type not supported");
        break;
    }	   
}

/* Reverse computation handler for a terminal event */
static void terminal_dally_rc_event_handler(terminal_state * s, tw_bf * bf, terminal_dally_message * msg, tw_lp * lp) 
{
    s->rev_events++;
    s->ross_sample.rev_events++;

    // In case the event was skipped above, skip now
    if (bf->c20) {
        bf->c20 = 0;
        printf("Has been rolledback! :)\n");
        return;
    }

    for(int i = 0; i < msg->num_rngs; i++)
        tw_rand_reverse_unif(lp->rng);

    for(int i = 0; i < msg->num_cll; i++)
        codes_local_latency_reverse(lp);

    switch((enum event_t) msg->type)
    {
        case T_GENERATE:
            if (bf->c10) {
                packet_generate_predicted_rc(s,bf,msg,lp);
            } else {
                packet_generate_rc(s, bf, msg, lp); 
            }
            break;

        case T_SEND:
            packet_send_rc(s, bf, msg, lp);
            break;

        case T_ARRIVE:
            packet_arrive_rc(s, bf, msg, lp);
            break;

        case T_ARRIVE_PREDICTED:
            packet_arrive_predicted_rc(s, bf, msg, lp);
            break;

        case T_BUFFER:
            terminal_buf_update_rc(s, bf, msg, lp); 
            break;

        case T_BANDWIDTH:
            issue_bw_monitor_event_rc(s,bf, msg, lp);
            break;
    
        case T_NOTIFY:
            process_terminal_notification_event_rc(s, bf, msg, lp);
        break;

#if ALWAYS_DETERMINISTIC_NETWORK
        case T_VACUOUS_EVENT:
        break;
#endif /* ALWAYS_DETERMINISTIC_NETWORK */

        default:
            tw_error(TW_LOC, "\n Invalid terminal event type %d ", msg->type);
    }
    msg->num_cll = 0;
    msg->num_rngs = 0;
    bf->c10 = 0;
}

/* Reverse computation handler for a router event */
static void router_dally_rc_event_handler(router_state * s, tw_bf * bf, 
  terminal_dally_message * msg, tw_lp * lp) 
{
    s->last_time = msg->last_received_time;

    for(int i = 0; i < msg->num_rngs; i++)
        tw_rand_reverse_unif(lp->rng);

    for(int i = 0; i < msg->num_cll; i++)
        codes_local_latency_reverse(lp);

    s->rev_events++;
    s->ross_rsample.rev_events++;

    switch((enum event_t) msg->type) {
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
    msg->num_cll = 0;
    msg->num_rngs = 0;
}

//*** ---------- START OF reverse handler checking functions ---------- ***
static void copy_rank_tbl(struct qhash_table * into, struct qhash_table const * from) {
  // YES! This function is very, very slow and so are all the others. This is
  // the simplest implementation we could come up with without changing how
  // qhash_table works or replacing it altogether. Both options would need
  // substantial changes to the dragonfly model
  for (int i = 0; i < from->table_size; i++) {
    struct dfly_qhash_entry *entry;
    qlist_for_each_entry(entry, &from->array[i], hash_link) {
      struct dfly_qhash_entry *new_entry =
          (struct dfly_qhash_entry *)malloc(sizeof(struct dfly_qhash_entry));
      *new_entry = *entry; // There is no need to copy contents of pointer because we don't check it
      qlist_add(&new_entry->hash_link, &into->array[i]);
    }
    }
}

static void clean_rank_tbl(struct qhash_table * rank_tbl) {
    for (int i=0; i < rank_tbl->table_size; i++) {
        while(!qlist_empty(&rank_tbl->array[i])) {
            struct qlist_head *item = qlist_pop(&rank_tbl->array[i]);
            struct dfly_qhash_entry * entry = qlist_entry(item, struct dfly_qhash_entry, hash_link);
            free(entry);
        }
    }
}

static bool check_dfly_qhash_entry(struct dfly_qhash_entry * before, struct dfly_qhash_entry * after) {
    // We ignore the remote data fields because they won't be needed:
    // - remote_event_size
    // - remote_event_data

    if (before->key.sender_id != after->key.sender_id ||
        before->key.message_id != after->key.message_id ||
        before->num_chunks != after->num_chunks ||
        before->remaining_packets != after->remaining_packets) {
        return false;
    }

    return true;
}

static bool check_rank_tbl(qhash_table const * before, struct qhash_table const * after) {
    for (int i=0; i < before->table_size; i++) {
        if (qlist_count(&before->array[i]) != qlist_count(&before->array[i])) {
            return false;
        }
        struct dfly_qhash_entry * before_entry;
        struct dfly_qhash_entry * after_entry;
        qlist_for_each_entry(before_entry, &before->array[i], hash_link) {
            // Yes, this is slow if there are many collisions, but often there won't be any
            bool found_entry = false;
            qlist_for_each_entry(after_entry, &after->array[i], hash_link) {
                if (check_dfly_qhash_entry(before_entry, after_entry)) {
                    found_entry = true;
                    break;
                }
            }
            if (!found_entry) {
                return false;
            }
        }
    }
    return true;
}

static void print_rank_tbl(FILE * out, char const * prefix, struct qhash_table * rank_tbl) {
    fprintf(out, "%stable_size = %d\n", prefix, rank_tbl->table_size);
    fprintf(out, "%s   compare = %p\n", prefix, rank_tbl->compare);
    fprintf(out, "%s      hash = %p\n", prefix, rank_tbl->hash);
    fprintf(out, "%s     array = %p\n", prefix, rank_tbl->array);

    char addprefix[] = "     |  | ";
    int len_subprefix = snprintf(NULL, 0, "%s%s", prefix, addprefix) + 1;
    char * subprefix = (char *) malloc(len_subprefix * sizeof(char));
    snprintf(subprefix, len_subprefix, "%s%s", prefix, addprefix);

    for (int i=0; i < rank_tbl->table_size; i++) {
        struct dfly_qhash_entry * entry;
        qlist_for_each_entry(entry, &rank_tbl->array[i], hash_link) {
            fprintf(out, "%s     | {\n", prefix);
            fprintf(out, "%s     |      key.message_id = %lu\n", prefix, entry->key.message_id);
            fprintf(out, "%s     |       key.sender_id = %lu\n", prefix, entry->key.sender_id);
            fprintf(out, "%s     |          num_chunks = %d\n", prefix, entry->num_chunks);
            fprintf(out, "%s     |   remaining_packets = %d\n", prefix, entry->remaining_packets);
            fprintf(out, "%s     |   remote_event_size = %d\n", prefix, entry->remote_event_size);
            fprintf(out, "%s     | * remote_event_data = %p\n", prefix, entry->remote_event_data);
            if (entry->remote_event_size) {
                tw_fprint_binary_array(out, subprefix, entry->remote_event_data, entry->remote_event_size);
            }
            fprintf(out, "%s     | },\n", prefix);
        }
    }

    free(subprefix);
}

static void save_terminal_state(terminal_state *into, terminal_state const *from) {
    // These should be deep-cloned/compared/printed if we want to run the functionality they are activated at
    // from->predictor_data
    // from->sample_stat
    // from->ross_sample
    // from->busy_time_ross_sample
    // from->busy_time_sample

    memcpy(into, from, sizeof(terminal_state));

    dragonfly_param const * p = into->params;
    int const num_qos_levels = p->num_qos_levels;
    int const num_rails = p->num_rails;

    if (!is_dally_surrogate_on) {
        into->vc_occupancy = (int **) malloc(num_rails * sizeof(int*));
        into->terminal_length = (int**) malloc(num_rails * sizeof(int*));
        into->last_buf_full = (tw_stime*) malloc(num_rails * sizeof(tw_stime));
        into->in_send_loop = (int*) malloc(num_rails * sizeof(int));
        into->issueIdle = (int*) malloc(num_rails * sizeof(int));
        into->qos_status = (int**) malloc(num_rails * sizeof(int*));
        into->qos_data = (int**) malloc(num_rails * sizeof(int*));
        into->last_qos_lvl = (int*) malloc(num_rails * sizeof(int));
        into->terminal_available_time = (tw_stime*) malloc(num_rails * sizeof(tw_stime));
        into->stalled_chunks = (unsigned long*) malloc(num_rails * sizeof(uint64_t));
        into->total_chunks = (unsigned long*) malloc(num_rails * sizeof(uint64_t));
        into->busy_time = (tw_stime*) malloc(num_rails * sizeof(tw_stime));
        into->terminal_msgs = (terminal_dally_message_list***) malloc(num_rails * sizeof(terminal_dally_message_list**));
        into->link_traffic = (uint64_t*) malloc(num_rails * sizeof(uint64_t));

        for(int i = 0; i < num_rails; i++) {
            into->vc_occupancy[i] = (int*) malloc(num_qos_levels * sizeof(int));
            into->terminal_length[i] = (int*) malloc(num_qos_levels * sizeof(int));
            into->qos_status[i] = (int*) malloc(num_qos_levels * sizeof(int));
            into->qos_data[i] = (int*) malloc(num_qos_levels * sizeof(int));
            into->terminal_msgs[i] = (terminal_dally_message_list**) malloc(num_qos_levels * sizeof(terminal_dally_message_list*));
            for (int j = 0; j<num_qos_levels; j++) {
                into->vc_occupancy[i][j] = from->vc_occupancy[i][j];
                into->terminal_length[i][j] = from->terminal_length[i][j];
                into->qos_data[i][j] = from->qos_data[i][j];
                into->qos_status[i][j] = from->qos_status[i][j];
                copy_terminal_dally_message_list(&into->terminal_msgs[i][j], from->terminal_msgs[i][j]);
            }
            into->last_buf_full[i] = from->last_buf_full[i];
            into->in_send_loop[i] = from->in_send_loop[i];
            into->issueIdle[i] = from->issueIdle[i];
            into->last_qos_lvl[i] = from->last_qos_lvl[i];
            into->terminal_available_time[i] = from->terminal_available_time[i];
            into->stalled_chunks[i] = from->stalled_chunks[i];
            into->total_chunks[i] = from->total_chunks[i];
            into->busy_time[i] = from->busy_time[i];
            into->link_traffic[i] = from->link_traffic[i];
        }
    }

    if (from->local_congestion_controller != NULL) {
        assert(g_congestion_control_enabled);
        into->local_congestion_controller = (tlc_state*) malloc(sizeof(tlc_state));
        save_tlc_state(into->local_congestion_controller, from->local_congestion_controller);
    }

    into->rank_tbl = qhash_init(dragonfly_rank_hash_compare, dragonfly_hash_func, DFLY_HASH_TABLE_SIZE);
    copy_rank_tbl(into->rank_tbl, from->rank_tbl);

    // I would use the C++ amgic to copy these containers but they don't work as well :S
    new (&into->remaining_sz_packets) map<struct packet_id, uint32_t>();
    new (&into->zombies) set<struct packet_id>();

    // Sorry const, I promise not to change the state of remaining_sz_packets
    map<struct packet_id, uint32_t> * from_remaining_sz_packets = (map<struct packet_id, uint32_t> *) &from->remaining_sz_packets;
    set<struct packet_id> * from_zombies = (set<struct packet_id> *) &from->zombies;

    std::map<struct packet_id, uint32_t>::iterator it_map;
    for (it_map = from_remaining_sz_packets->begin(); it_map != from_remaining_sz_packets->end(); ++it_map) {
        into->remaining_sz_packets[it_map->first] = it_map->second;
    }

    std::set<struct packet_id>::iterator it_set;
    for (it_set = from_zombies->begin(); it_set != from_zombies->end(); ++it_set) {
        struct packet_id const zombie = {
            .packet_ID = it_set->packet_ID,
            .dfdally_src_terminal_id = it_set->dfdally_src_terminal_id};
        into->zombies.insert(zombie);
    }
}

// Partially written by Claude
static void clean_terminal_state(terminal_state *state) {
    dragonfly_param const * p = state->params;
    int const num_rails = p->num_rails;
    int const num_qos_levels = p->num_qos_levels;

    if (!is_dally_surrogate_on) {
        for (int i = 0; i < num_rails; i++) {
            free(state->vc_occupancy[i]);
            free(state->terminal_length[i]);
            free(state->qos_status[i]);
            free(state->qos_data[i]);
            for (int j = 0; j<num_qos_levels; j++) {
                clean_terminal_dally_message_list(state->terminal_msgs[i][j]);
            }
            free(state->terminal_msgs[i]);
        }

        free(state->vc_occupancy);
        free(state->terminal_length);
        free(state->last_buf_full);
        free(state->in_send_loop);
        free(state->issueIdle);
        free(state->qos_status);
        free(state->qos_data);
        free(state->last_qos_lvl);
        free(state->terminal_available_time);
        free(state->stalled_chunks);
        free(state->total_chunks);
        free(state->busy_time);
        free(state->link_traffic);
        free(state->terminal_msgs);
    }

    if (state->local_congestion_controller != NULL) {
        clean_tlc_state(state->local_congestion_controller);
        free(state->local_congestion_controller);
    }

    clean_rank_tbl(state->rank_tbl);
    qhash_finalize(state->rank_tbl);

    state->remaining_sz_packets.~map();
    state->zombies.~set();
}

static bool check_terminal_state(terminal_state *before, terminal_state *after) {
    bool is_same = true;

    // There is no need to deep-copy the following. They're never modified
    assert(before->params == after->params);
    assert(before->router_lp == after->router_lp);
    assert(before->router_id == after->router_id);

    // We ignore the comparison of the following. They are not meant to be rolled-back
    // before->fwd_events
    // before->rev_events
    // before->sent_packets
    // before->last_packet_sent_id
    // before->arrival_of_last_packet
    // before->anno
    assert(before->frozen_state == after->frozen_state);

    // Comparing all other elements of the struct
    is_same &= (before->packet_counter == after->packet_counter);
    is_same &= (before->packet_gen == after->packet_gen);
    is_same &= (before->packet_fin == after->packet_fin);
    is_same &= (before->total_gen_size == after->total_gen_size);
    is_same &= (before->terminal_id == after->terminal_id);
    is_same &= (before->workloads_finished_flag == after->workloads_finished_flag);
    is_same &= (before->is_monitoring_bw == after->is_monitoring_bw);
    is_same &= (before->rank_tbl_pop == after->rank_tbl_pop);
    is_same &= (before->total_time == after->total_time);
    is_same &= (before->total_msg_size == after->total_msg_size);
    is_same &= (before->total_hops == after->total_hops);
    is_same &= (before->finished_msgs == after->finished_msgs);
    is_same &= (before->finished_chunks == after->finished_chunks);
    is_same &= (before->finished_packets == after->finished_packets);
    is_same &= (before->injected_chunks == after->injected_chunks);
    is_same &= (before->ejected_chunks == after->ejected_chunks);
    is_same &= (before->max_latency == after->max_latency);
    is_same &= (before->min_latency == after->min_latency);
    is_same &= (before->fin_chunks_sample == after->fin_chunks_sample);
    is_same &= (before->data_size_sample == after->data_size_sample);
    is_same &= (before->fin_hops_sample == after->fin_hops_sample);
    is_same &= (before->fin_chunks_time == after->fin_chunks_time);
    is_same &= (before->op_arr_size == after->op_arr_size);
    is_same &= (before->max_arr_size == after->max_arr_size);
    is_same &= (before->fin_chunks_ross_sample == after->fin_chunks_ross_sample);
    is_same &= (before->data_size_ross_sample == after->data_size_ross_sample);
    is_same &= (before->fin_hops_ross_sample == after->fin_hops_ross_sample);
    is_same &= (before->fin_chunks_time_ross_sample == after->fin_chunks_time_ross_sample);
    is_same &= (before->last_in_queue_time == after->last_in_queue_time);

    // Compare string buffers
    is_same &= (strncmp(before->output_buf, after->output_buf, 4096) == 0);
    is_same &= (strncmp(before->output_buf2, after->output_buf2, 4096) == 0);
    is_same &= (strncmp(before->sample_buf, after->sample_buf, 4096) == 0);

    // Compare anno strings (handling NULL case)
    if (before->anno && after->anno) {
        is_same &= (strcmp(before->anno, after->anno) == 0);
    } else {
        is_same &= (before->anno == after->anno);
    }

    if (!is_dally_surrogate_on) {
        dragonfly_param const * p = before->params;
        int const num_qos_levels = p->num_qos_levels;
        int const num_rails = p->num_rails;

        for (int i = 0; i < num_rails; i++) {
            for (int j = 0; j < num_qos_levels; j++) {
                is_same &= (before->vc_occupancy[i][j] == after->vc_occupancy[i][j]);
                is_same &= (before->terminal_length[i][j] == after->terminal_length[i][j]);
                is_same &= (before->qos_status[i][j] == after->qos_status[i][j]);
                is_same &= (before->qos_data[i][j] == after->qos_data[i][j]);
                is_same &= check_terminal_dally_message_list(before->terminal_msgs[i][j], after->terminal_msgs[i][j]);
            }

            is_same &= (before->last_buf_full[i] == after->last_buf_full[i]);
            is_same &= (before->in_send_loop[i] == after->in_send_loop[i]);
            is_same &= (before->issueIdle[i] == after->issueIdle[i]);
            is_same &= (before->last_qos_lvl[i] == after->last_qos_lvl[i]);
            is_same &= (before->terminal_available_time[i] == after->terminal_available_time[i]);
            is_same &= (before->stalled_chunks[i] == after->stalled_chunks[i]);
            is_same &= (before->total_chunks[i] == after->total_chunks[i]);
            is_same &= (before->busy_time[i] == after->busy_time[i]);
            is_same &= (before->link_traffic[i] == after->link_traffic[i]);
        }
    }

    // Ignoring model statistics. In general, we don't care if there are errors in the statistics, as they are only approximate. The stastistics don't interferee with the state of the model. There is a bug within the statistics when rolbacking though. A parameters is never reversed properly
    //for (size_t i = 0; i < CATEGORY_MAX; i++) {
    //    is_same &= check_mn_stats(&before->dragonfly_stats_array[i], &after->dragonfly_stats_array[i]);
    //}

    if (after->local_congestion_controller != NULL) {
        is_same &= check_tlc_state(before->local_congestion_controller, after->local_congestion_controller);
    }

    is_same &= check_rank_tbl(before->rank_tbl, after->rank_tbl);

    is_same &= before->remaining_sz_packets == after->remaining_sz_packets;
    is_same &= before->zombies == after->zombies;

    return is_same;
}

// Print fuction originally constructed with help from Claude.ai
static void print_terminal_state(FILE * out, char const * prefix, terminal_state * state) {
    fprintf(out, "%sterminal_state (dragonfly-dally) ->\n", prefix);
    fprintf(out, "%s  |           packet_counter = %ld\n", prefix, state->packet_counter);
    fprintf(out, "%s  |               packet_gen = %d\n", prefix, state->packet_gen);
    fprintf(out, "%s  |               packet_fin = %d\n", prefix, state->packet_fin);
    fprintf(out, "%s  |           total_gen_size = %d\n", prefix, state->total_gen_size);

    fprintf(out, "%s  | *          router_lp[%d] = [", prefix, state->params->num_rails);
    for (int i=0; i<state->params->num_rails; i++) {
        fprintf(out, "%s%lu", i ? ", " : "", state->router_lp[i]);
    }
    fprintf(out, "]\n");

    fprintf(out, "%s  | *          router_id[%d] = [", prefix, state->params->num_rails);
    for (int i=0; i<state->params->num_rails; i++) {
        fprintf(out, "%s%u", i ? ", " : "", state->router_id[i]);
    }
    fprintf(out, "]\n");

    fprintf(out, "%s  |              terminal_id = %u\n", prefix, state->terminal_id);
    fprintf(out, "%s  |                  connMan = <DragonflyConnectionManager object>\n", prefix);

    char addprefix[] = "  | ";
    int len_subprefix = snprintf(NULL, 0, "%s%s", prefix, addprefix) + 1;
    char * subprefix = (char *) malloc(len_subprefix * sizeof(char));
    fprintf(out, "%s  | *local_congestion_controller = %p\n", prefix, state->local_congestion_controller);
    if (state->local_congestion_controller != NULL) {
        print_tlc_state(out, subprefix, state->local_congestion_controller);
    }
    free(subprefix);

    fprintf(out, "%s  |  workloads_finished_flag = %d\n", prefix, state->workloads_finished_flag);

    if (is_dally_surrogate_on) {
        fprintf(out, "%s  | **          vc_occupancy = %p\n", prefix, state->vc_occupancy);
        fprintf(out, "%s  | *terminal_available_time = %p\n", prefix, state->terminal_available_time);
        fprintf(out, "%s  | ***        terminal_msgs = %p\n", prefix, state->terminal_msgs);
    } else {
        fprintf(out, "%s  | **  vc_occupancy[%d][%d] = [\n", prefix, state->params->num_rails, state->params->num_qos_levels);
        for (int i=0; i<state->params->num_rails; i++) {
            fprintf(out, "%s  |        rail %d: [", prefix, i);
            for (int j=0; j<state->params->num_qos_levels; j++) {
                fprintf(out, "%s%d", j ? ", " : "", state->vc_occupancy[i][j]);
            }
            fprintf(out, "]\n");
        }
        fprintf(out, "%s  |     ]\n", prefix);

        fprintf(out, "%s  | *terminal_available_time[%d] = [", prefix, state->params->num_rails);
        for (int i=0; i<state->params->num_rails; i++) {
            fprintf(out, "%s%g", i ? ", " : "", state->terminal_available_time[i]);
        }
        fprintf(out, "]\n");

        char addprefix_2[] = "  |    |  | ";
        len_subprefix = snprintf(NULL, 0, "%s%s", prefix, addprefix_2) + 1;
        subprefix = (char *) malloc(len_subprefix * sizeof(char));
        snprintf(subprefix, len_subprefix, "%s%s", prefix, addprefix_2);
        fprintf(out, "%s  | ***        terminal_msgs[%d][%d] = [\n", prefix, state->params->num_rails, state->params->num_qos_levels);
        for (int i=0; i<state->params->num_rails; i++) {
            fprintf(out, "%s  |   rail %d: [\n", prefix, i);
            for (int j=0; j<state->params->num_qos_levels; j++) {
                fprintf(out, "%s  |    | qos level %d\n", prefix, j);
                print_terminal_dally_message_list(out, subprefix, state, state->terminal_msgs[i][j]);
            }
        }
        fprintf(out, "%s  | ]\n", prefix);
        free(subprefix);
    }

    fprintf(out, "%s  | ***   terminal_msgs_tail = %p\n", prefix, state->terminal_msgs_tail);

    if (is_dally_surrogate_on) {
        fprintf(out, "%s  | *          in_send_loop = %p\n", prefix, state->in_send_loop);
    } else {
        fprintf(out, "%s  | *       in_send_loop[%d] = [", prefix, state->params->num_rails);
        for (int i=0; i<state->params->num_rails; i++) {
            fprintf(out, "%s%d", i ? ", " : "", state->in_send_loop[i]);
        }
        fprintf(out, "]\n");
    }

    char addprefix_3[] = "  |    | ";
    len_subprefix = snprintf(NULL, 0, "%s%s", prefix, addprefix_3) + 1;
    subprefix = (char *) malloc(len_subprefix * sizeof(char));
    snprintf(subprefix, len_subprefix, "%s%s", prefix, addprefix_3);
    fprintf(out, "%s  |    dragonfly_stats_array = [\n", prefix);
    for (int i = 0; i < CATEGORY_MAX; i++) {
        fprintf(out, "%s  |    %d:\n", prefix, i);
        print_mn_stats(out, subprefix, &state->dragonfly_stats_array[i]);
    }
    fprintf(out, "%s  |    ]\n", prefix);
    free(subprefix);

    if (is_dally_surrogate_on) {
        fprintf(out, "%s  | **           qos_status = %p\n", prefix, state->qos_status);
        fprintf(out, "%s  | **             qos_data = %p\n", prefix, state->qos_data);
        fprintf(out, "%s  | *          last_qos_lvl = %p\n", prefix, state->last_qos_lvl);
    } else {
        fprintf(out, "%s  | **      qos_status[%d][%d] = [\n", prefix, state->params->num_rails, state->params->num_qos_levels);
        for (int i=0; i<state->params->num_rails; i++) {
            fprintf(out, "%s  |          rail %d: [", prefix, i);
            for (int j=0; j<state->params->num_qos_levels; j++) {
                fprintf(out, "%s%d", j ? ", " : "", state->qos_status[i][j]);
            }
            fprintf(out, "]\n");
        }
        fprintf(out, "%s  |       ]\n", prefix);

        fprintf(out, "%s  | **        qos_data[%d][%d] = [\n", prefix, state->params->num_rails, state->params->num_qos_levels);
        for (int i=0; i<state->params->num_rails; i++) {
            fprintf(out, "%s  |            rail %d: [", prefix, i);
            for (int j=0; j<state->params->num_qos_levels; j++) {
                fprintf(out, "%s%d", j ? ", " : "", state->qos_data[i][j]);
            }
            fprintf(out, "]\n");
        }
        fprintf(out, "%s  |         ]\n", prefix);

        fprintf(out, "%s  | *        last_qos_lvl[%d] = [", prefix, state->params->num_rails);
        for (int i=0; i<state->params->num_rails; i++) {
            fprintf(out, "%s%d", i ? ", " : "", state->last_qos_lvl[i]);
        }
        fprintf(out, "]\n");
    }

    fprintf(out, "%s  |         is_monitoring_bw = %d\n", prefix, state->is_monitoring_bw);
    fprintf(out, "%s  | *                     st = %p\n", prefix, state->st);
    fprintf(out, "%s  | *                  cc_st = %p\n", prefix, state->cc_st);

    if (is_dally_surrogate_on) {
        fprintf(out, "%s  | *             issueIdle = %p\n", prefix, state->issueIdle);
        fprintf(out, "%s  | **      terminal_length = %p\n", prefix, state->terminal_length);
    } else {
        fprintf(out, "%s  | *           issueIdle[%d] = [", prefix, state->params->num_rails);
        for (int i=0; i<state->params->num_rails; i++) {
            fprintf(out, "%s%d", i ? ", " : "", state->issueIdle[i]);
        }
        fprintf(out, "]\n");

        fprintf(out, "%s  | ** terminal_length[%d][%d] = [\n", prefix, state->params->num_rails, state->params->num_qos_levels);
        for (int i=0; i<state->params->num_rails; i++) {
            fprintf(out, "%s  |       rail %d: [", prefix, i);
            for (int j=0; j<state->params->num_qos_levels; j++) {
                fprintf(out, "%s%d", j ? ", " : "", state->terminal_length[i][j]);
            }
            fprintf(out, "]\n");
        }
        fprintf(out, "%s  |    ]\n", prefix);
    }

    fprintf(out, "%s  | *                   anno = %s\n", prefix, state->anno ? state->anno : "(nil)");
    fprintf(out, "%s  | *                 params = %p\n", prefix, state->params);

    fprintf(out, "%s  | *               rank_tbl = {\n", prefix);
    char addprefix_4[] = "  |     ";
    len_subprefix = snprintf(NULL, 0, "%s%s", prefix, addprefix_4) + 1;
    subprefix = (char *) malloc(len_subprefix * sizeof(char));
    snprintf(subprefix, len_subprefix, "%s%s", prefix, addprefix_4);
    print_rank_tbl(out, subprefix, state->rank_tbl);
    free(subprefix);
    fprintf(out, "%s  | }\n", prefix);

    fprintf(out, "%s  |             rank_tbl_pop = %lu\n", prefix, state->rank_tbl_pop);
    fprintf(out, "%s  |               total_time = %g\n", prefix, state->total_time);
    fprintf(out, "%s  |           total_msg_size = %lu\n", prefix, state->total_msg_size);
    fprintf(out, "%s  |               total_hops = %g\n", prefix, state->total_hops);
    fprintf(out, "%s  |            finished_msgs = %ld\n", prefix, state->finished_msgs);
    fprintf(out, "%s  |          finished_chunks = %ld\n", prefix, state->finished_chunks);
    fprintf(out, "%s  |         finished_packets = %ld\n", prefix, state->finished_packets);

    if (is_dally_surrogate_on) {
        fprintf(out, "%s  | **      terminal_length = %p\n", prefix, state->terminal_length);
        fprintf(out, "%s  | *         last_buf_full = %p\n", prefix, state->last_buf_full);
        fprintf(out, "%s  | *             busy_time = %p\n", prefix, state->busy_time);
        fprintf(out, "%s  | *          link_traffic = %p\n", prefix, state->link_traffic);
        fprintf(out, "%s  | *          total_chunks = %p\n", prefix, state->total_chunks);
        fprintf(out, "%s  | *        stalled_chunks = %p\n", prefix, state->stalled_chunks);
    } else {
        fprintf(out, "%s  | *       last_buf_full[%d] = [", prefix, state->params->num_rails);
        for (int i=0; i<state->params->num_rails; i++) {
            fprintf(out, "%s%g", i ? ", " : "", state->last_buf_full[i]);
        }
        fprintf(out, "]\n");

        fprintf(out, "%s  | *           busy_time[%d] = [", prefix, state->params->num_rails);
        for (int i=0; i<state->params->num_rails; i++) {
            fprintf(out, "%s%g", i ? ", " : "", state->busy_time[i]);
        }
        fprintf(out, "]\n");

        fprintf(out, "%s  | *        link_traffic[%d] = [", prefix, state->params->num_rails);
        for (int i=0; i<state->params->num_rails; i++) {
            fprintf(out, "%s%lu", i ? ", " : "", state->link_traffic[i]);
        }
        fprintf(out, "]\n");

        fprintf(out, "%s  | *        total_chunks[%d] = [", prefix, state->params->num_rails);
        for (int i=0; i<state->params->num_rails; i++) {
            fprintf(out, "%s%lu", i ? ", " : "", state->total_chunks[i]);
        }
        fprintf(out, "]\n");

        fprintf(out, "%s  | *      stalled_chunks[%d] = [", prefix, state->params->num_rails);
        for (int i=0; i<state->params->num_rails; i++) {
            fprintf(out, "%s%lu", i ? ", " : "", state->stalled_chunks[i]);
        }
        fprintf(out, "]\n");
    }

    fprintf(out, "%s  |          injected_chunks = %lu\n", prefix, state->injected_chunks);
    fprintf(out, "%s  |           ejected_chunks = %lu\n", prefix, state->ejected_chunks);
    fprintf(out, "%s  |              max_latency = %g\n", prefix, state->max_latency);
    fprintf(out, "%s  |              min_latency = %g\n", prefix, state->min_latency);
    fprintf(out, "%s  |               output_buf = '%.4096s'\n", prefix, state->output_buf);
    fprintf(out, "%s  |              output_buf2 = '%.4096s'\n", prefix, state->output_buf2);
    fprintf(out, "%s  |        fin_chunks_sample = %ld\n", prefix, state->fin_chunks_sample);
    fprintf(out, "%s  |         data_size_sample = %ld\n", prefix, state->data_size_sample);
    fprintf(out, "%s  |          fin_hops_sample = %g\n", prefix, state->fin_hops_sample);
    fprintf(out, "%s  |          fin_chunks_time = %g\n", prefix, state->fin_chunks_time);
    fprintf(out, "%s  | *       busy_time_sample = %p\n", prefix, state->busy_time_sample);
    fprintf(out, "%s  |               sample_buf = '%.4096s'\n", prefix, state->sample_buf);
    fprintf(out, "%s  | *            sample_stat = %p\n", prefix, state->sample_stat);  // ingnoring as this part of the code is never used. Originally part of instrumentation
    fprintf(out, "%s  |              op_arr_size = %d\n", prefix, state->op_arr_size);
    fprintf(out, "%s  |             max_arr_size = %d\n", prefix, state->max_arr_size);
    fprintf(out, "%s  |               fwd_events = %ld\n", prefix, state->fwd_events);
    fprintf(out, "%s  |               rev_events = %ld\n", prefix, state->rev_events);
    fprintf(out, "%s  |   fin_chunks_ross_sample = %ld\n", prefix, state->fin_chunks_ross_sample);
    fprintf(out, "%s  |    data_size_ross_sample = %ld\n", prefix, state->data_size_ross_sample);
    fprintf(out, "%s  |     fin_hops_ross_sample = %ld\n", prefix, state->fin_hops_ross_sample);
    fprintf(out, "%s  | fin_chunks_time_ross_sample = %g\n", prefix, state->fin_chunks_time_ross_sample);
    fprintf(out, "%s  | *  busy_time_ross_sample = %p\n", prefix, state->busy_time_ross_sample);  // ingnoring as this part of the code is never used. Originally part of instrumentation
    fprintf(out, "%s  |              ross_sample = <dfly_cn_sample object>\n", prefix);  // ingnoring as this part of the code is never used. Originally part of instrumentation

    // modified outside of process and reverse computation (at commit and at surrogate change)
    fprintf(out, "%s  |             sent_packets = <map object>\n", prefix);

    fprintf(out, "%s  |      last_packet_sent_id = %ld\n", prefix, state->last_packet_sent_id);
    fprintf(out, "%s  |   arrival_of_last_packet = {packet_ID: %ld, travel_end_time: %g}\n", prefix, state->arrival_of_last_packet.packet_ID, state->arrival_of_last_packet.travel_end_time);

    fprintf(out, "%s  |     remaining_sz_packets = {\n", prefix);
    std::map<struct packet_id, uint32_t>::iterator it_map;
    for (it_map = state->remaining_sz_packets.begin(); it_map != state->remaining_sz_packets.end(); ++it_map) {
        fprintf(out, "%s  |         {packet_ID: %lu, dfdally_src_terminal_id: %u} -> %d,\n", prefix, it_map->first.packet_ID, it_map->first.dfdally_src_terminal_id, it_map->second);
    }
    fprintf(out, "%s  |     }\n", prefix);

    fprintf(out, "%s  |       last_in_queue_time = %g\n", prefix, state->last_in_queue_time);
    fprintf(out, "%s  | *         predictor_data = %p\n", prefix, state->predictor_data);

    fprintf(out, "%s  |                  zombies = [\n", prefix);
    std::set<struct packet_id>::iterator it;
    for (it = state->zombies.begin(); it != state->zombies.end(); ++it) {
        fprintf(out, "%s  |                    {packet_ID: %lu, dfdally_src_terminal_id: %u},\n", prefix, it->packet_ID, it->dfdally_src_terminal_id);
    }
    fprintf(out, "%s  |                  ]\n", prefix);

    fprintf(out, "%s  | *           frozen_state = %p\n", prefix, state->frozen_state);
}

// Original function implemented by Claude
static void save_router_state(router_state *into, router_state const *from) {
    // Missing deep-clone/comparison/print members. These members are always accessed, so it is possible to discover some bugs if we print their contents
    // from->local_congestion_controller

    // Missing deep-clone/comparison/print members.
    // from->rsamples
    // from->ross_rsample
    // from->busy_time_sample
    // from->link_traffic_sample
    // from->link_traffic_ross_sample

    memcpy(into, from, sizeof(router_state));

    dragonfly_param const * p = into->params;
    int const radix = p->radix;
    int const num_qos_levels = p->num_qos_levels;

    into->global_channel = (int*) malloc(p->num_global_channels * sizeof(int));

    for (int i = 0; i < p->num_global_channels; i++) {
        into->global_channel[i] = from->global_channel[i];
    }

    into->next_output_available_time = (tw_stime*) malloc(radix * sizeof(tw_stime));
    into->last_buf_full = (tw_stime*) malloc(radix * sizeof(tw_stime));
    into->busy_time = (tw_stime*) malloc(radix * sizeof(tw_stime));
    into->stalled_chunks = (unsigned long*) malloc(radix * sizeof(unsigned long));
    into->total_chunks = (unsigned long*) malloc(radix * sizeof(unsigned long));
    into->in_send_loop = (int*) malloc(radix * sizeof(int));
    into->queued_count = (int*) malloc(radix * sizeof(int));
    into->port_bandwidths = (double*) malloc(radix * sizeof(double));
    into->vc_max_sizes = (int*) malloc(radix * sizeof(int));
    into->link_traffic = (int64_t*) malloc(radix * sizeof(int64_t));
    into->last_qos_lvl = (int*) malloc(radix * sizeof(int));
    into->vc_occupancy = (int**) malloc(radix * sizeof(int*));
    into->qos_status = (int**) malloc(radix * sizeof(int*));
    into->qos_data = (int**) malloc(radix * sizeof(int*));
    into->pending_msgs = (terminal_dally_message_list***) malloc(radix * sizeof(terminal_dally_message_list**));
    into->queued_msgs = (terminal_dally_message_list***) malloc(radix * sizeof(terminal_dally_message_list**));

    for (int i = 0; i < radix; i++) {
        into->next_output_available_time[i] = from->next_output_available_time[i];
        into->last_buf_full[i] = from->last_buf_full[i];
        into->busy_time[i] = from->busy_time[i];
        into->stalled_chunks[i] = from->stalled_chunks[i];
        into->total_chunks[i] = from->total_chunks[i];
        into->in_send_loop[i] = from->in_send_loop[i];
        into->queued_count[i] = from->queued_count[i];
        into->port_bandwidths[i] = from->port_bandwidths[i];
        into->vc_max_sizes[i] = from->vc_max_sizes[i];
        into->link_traffic[i] = from->link_traffic[i];
        into->last_qos_lvl[i] = from->last_qos_lvl[i];

        into->vc_occupancy[i] = (int*) malloc(p->num_vcs * sizeof(int));
        into->qos_status[i] = (int*) malloc(num_qos_levels * sizeof(int));
        into->qos_data[i] = (int*) malloc(num_qos_levels * sizeof(int));

        into->pending_msgs[i] = (terminal_dally_message_list**) malloc(p->num_vcs * sizeof(terminal_dally_message_list*));
        into->queued_msgs[i] = (terminal_dally_message_list**) malloc(p->num_vcs * sizeof(terminal_dally_message_list*));

        for (int j = 0; j < p->num_vcs; j++) {
            into->vc_occupancy[i][j] = from->vc_occupancy[i][j];
            copy_terminal_dally_message_list(&into->pending_msgs[i][j], from->pending_msgs[i][j]);
            copy_terminal_dally_message_list(&into->queued_msgs[i][j], from->queued_msgs[i][j]);
        }
        for (int j = 0; j < num_qos_levels; j++) {
            into->qos_status[i][j] = from->qos_status[i][j];
            into->qos_data[i][j] = from->qos_data[i][j];
        }
    }

    if (p->counting_bool > 0) {
        assert(from->agg_busy_time != NULL);
        assert(from->agg_link_traffic != NULL);
        into->agg_busy_time = (tw_stime**) malloc(p->counting_windows * sizeof(tw_stime*));
        into->agg_link_traffic = (int64_t**) malloc(p->counting_windows * sizeof(int64_t*));

        for (int i = 0; i < p->counting_windows; i++) {
            into->agg_busy_time[i] = (tw_stime*) malloc(radix * sizeof(tw_stime));
            into->agg_link_traffic[i] = (int64_t*) malloc(radix * sizeof(int64_t));
            memcpy(into->agg_busy_time[i], from->agg_busy_time[i], radix * sizeof(tw_stime));
            memcpy(into->agg_link_traffic[i], from->agg_link_traffic[i], radix * sizeof(int64_t));
        }
    }

    //if (from->local_congestion_controller != NULL) {
    //    assert(g_congestion_control_enabled);
    //    into->local_congestion_controller = (rlc_state*) malloc(sizeof(rlc_state));
    //    save_rlc_state(into->local_congestion_controller, from->local_congestion_controller);
    //}
}

// Original function implemented by Claude
static void clean_router_state(router_state *state) {
    dragonfly_param const * p = state->params;
    int const radix = p->radix;

    // Free simple arrays
    free(state->global_channel);
    free(state->next_output_available_time);
    free(state->last_buf_full);
    free(state->busy_time);
    free(state->stalled_chunks);
    free(state->total_chunks);
    free(state->in_send_loop);
    free(state->queued_count);
    free(state->port_bandwidths);
    free(state->vc_max_sizes);
    free(state->link_traffic);
    free(state->last_qos_lvl);

    // Clean and free 2D arrays
    for (int i = 0; i < radix; i++) {
        free(state->vc_occupancy[i]);
        free(state->qos_status[i]);
        free(state->qos_data[i]);

        for (int j = 0; j < p->num_vcs; j++) {
            clean_terminal_dally_message_list(state->pending_msgs[i][j]);
            clean_terminal_dally_message_list(state->queued_msgs[i][j]);
        }

        free(state->pending_msgs[i]);
        free(state->queued_msgs[i]);
    }

    free(state->vc_occupancy);
    free(state->qos_status);
    free(state->qos_data);
    free(state->pending_msgs);
    free(state->queued_msgs);

    if (p->counting_bool > 0) {
        for (int i = 0; i < p->counting_windows; i++) {
            free(state->agg_busy_time[i]);
            free(state->agg_link_traffic[i]);
        }
        free(state->agg_busy_time);
        free(state->agg_link_traffic);
    }

    //if (state->local_congestion_controller != NULL) {
    //    clean_rlc_state(state->local_congestion_controller);
    //    free(state->local_congestion_controller);
    //}
}

// Original function implemented by Claude
static bool check_router_state(router_state const *before, router_state const *after) {
    // The following are not checked because they don't influence any other
    // components of the router state, ie, they are never used to change
    // the simulation behavior.
    // - snapshot_data
    // - fwd_events
    // - rev_events

    dragonfly_param const * p = before->params;
    int const radix = p->radix;
    int const num_qos_levels = p->num_qos_levels;

    if (before->router_id != after->router_id ||
        before->group_id != after->group_id ||
        before->plane_id != after->plane_id ||
        before->op_arr_size != after->op_arr_size ||
        before->max_arr_size != after->max_arr_size ||
        before->workloads_finished_flag != after->workloads_finished_flag ||
        before->is_monitoring_bw != after->is_monitoring_bw ||
        before->last_time != after->last_time) {
        return false;
    }

    for (int i = 0; i < p->num_global_channels; i++) {
        if (before->global_channel[i] != after->global_channel[i]) {
            return false;
        }
    }

    for (int i = 0; i < radix; i++) {
        if (before->next_output_available_time[i] != after->next_output_available_time[i] ||
            before->last_buf_full[i] != after->last_buf_full[i] ||
            before->busy_time[i] != after->busy_time[i] ||
            before->stalled_chunks[i] != after->stalled_chunks[i] ||
            before->total_chunks[i] != after->total_chunks[i] ||
            before->in_send_loop[i] != after->in_send_loop[i] ||
            before->queued_count[i] != after->queued_count[i] ||
            before->port_bandwidths[i] != after->port_bandwidths[i] ||
            before->vc_max_sizes[i] != after->vc_max_sizes[i] ||
            before->link_traffic[i] != after->link_traffic[i] ||
            before->last_qos_lvl[i] != after->last_qos_lvl[i]) {
            return false;
        }

        for (int j = 0; j < p->num_vcs; j++) {
            if (before->vc_occupancy[i][j] != after->vc_occupancy[i][j]) {
                return false;
            }

            if (!check_terminal_dally_message_list(before->pending_msgs[i][j], after->pending_msgs[i][j]) ||
                !check_terminal_dally_message_list(before->queued_msgs[i][j], after->queued_msgs[i][j])) {
                return false;
            }
        }

        for (int j = 0; j < num_qos_levels; j++) {
            if (before->qos_status[i][j] != after->qos_status[i][j] ||
                before->qos_data[i][j] != after->qos_data[i][j]) {
                return false;
            }
        }
    }

    if ((before->agg_busy_time == NULL) != (after->agg_busy_time == NULL)) {
        return false;
    }
    if ((before->agg_link_traffic == NULL) != (after->agg_link_traffic == NULL)) {
        return false;
    }

    if (p->counting_bool > 0) {
        assert(before->agg_busy_time != NULL && after->agg_busy_time);
        assert(before->agg_link_traffic != NULL && after->agg_link_traffic);
        for (int i = 0; i < p->counting_windows; i++) {
            for (int j = 0; j < radix; j++) {
                if (before->agg_busy_time[i][j] != after->agg_busy_time[i][j] ||
                    before->agg_link_traffic[i][j] != after->agg_link_traffic[i][j]) {
                    return false;
                }
            }
        }
    }

    //if (before->local_congestion_controller != NULL) {
    //    if (!check_rlc_state(before->local_congestion_controller, after->local_congestion_controller)) {
    //        return false;
    //    }
    //}

    // Check strings
    if (strncmp(before->output_buf, after->output_buf, 4096) != 0 ||
        strncmp(before->output_buf5, after->output_buf5, 4096) != 0 ||
        strncmp(before->output_buf6, after->output_buf6, 4096) != 0) {
        return false;
    }

    // All checks passed
    return true;
}

// Original function implemented by Claude
static void print_router_state(FILE * out, char const * prefix, router_state * state) {
    dragonfly_param const * p = state->params;
    int const radix = p->radix;
    int const num_qos_levels = p->num_qos_levels;

    fprintf(out, "%srouter_state (dragonfly) ->\n", prefix);
    fprintf(out, "%s  |              router_id = %u\n", prefix, state->router_id);
    fprintf(out, "%s  |               group_id = %d\n", prefix, state->group_id);
    fprintf(out, "%s  |               plane_id = %d\n", prefix, state->plane_id);
    fprintf(out, "%s  |            op_arr_size = %d\n", prefix, state->op_arr_size);
    fprintf(out, "%s  |           max_arr_size = %d\n", prefix, state->max_arr_size);

    fprintf(out, "%s  | *       global_channel[%d] = [", prefix, radix);
    for (int i = 0; i < p->num_global_channels; i++) {
        fprintf(out, "%s%d", i ? ", " : "", state->global_channel[i]);
    }
    fprintf(out, "]\n");

    fprintf(out, "%s  |                connMan = <DragonflyConnectionManager object>\n", prefix);

    char addprefix[] = "  | ";
    int len_subprefix = snprintf(NULL, 0, "%s%s", prefix, addprefix) + 1;
    char * subprefix = (char *) malloc(len_subprefix * sizeof(char));
    fprintf(out, "%s  | *local_congestion_controller = %p\n", prefix, state->local_congestion_controller);
    //if (state->local_congestion_controller != NULL) {
    //    snprintf(subprefix, len_subprefix, "%s%s", prefix, addprefix);
    //    print_rlc_state(out, subprefix, state->local_congestion_controller);
    //}
    free(subprefix);

    fprintf(out, "%s  | *next_output_available_time[%d] = [", prefix, radix);
    for (int i = 0; i < radix; i++) {
        fprintf(out, "%s%g", i ? ", " : "", state->next_output_available_time[i]);
    }
    fprintf(out, "]\n");

    fprintf(out, "%s  | *     last_buf_full[%d] = [", prefix, radix);
    for (int i = 0; i < radix; i++) {
        fprintf(out, "%s%g", i ? ", " : "", state->last_buf_full[i]);
    }
    fprintf(out, "]\n");

    fprintf(out, "%s  | *         busy_time[%d] = [", prefix, radix);
    for (int i = 0; i < radix; i++) {
        fprintf(out, "%s%g", i ? ", " : "", state->busy_time[i]);
    }
    fprintf(out, "]\n");

    fprintf(out, "%s  | *     busy_time_sample = %p\n", prefix, state->busy_time_sample);

    fprintf(out, "%s  | *    stalled_chunks[%d] = [", prefix, radix);
    for (int i = 0; i < radix; i++) {
        fprintf(out, "%s%lu", i ? ", " : "", state->stalled_chunks[i]);
    }
    fprintf(out, "]\n");

    fprintf(out, "%s  | *      total_chunks[%d] = [", prefix, radix);
    for (int i = 0; i < radix; i++) {
        fprintf(out, "%s%lu", i ? ", " : "", state->total_chunks[i]);
    }
    fprintf(out, "]\n");

    char addprefix_2[] = "  |   |  |  ";
    len_subprefix = snprintf(NULL, 0, "%s%s", prefix, addprefix_2) + 1;
    subprefix = (char *) malloc(len_subprefix * sizeof(char));
    snprintf(subprefix, len_subprefix, "%s%s", prefix, addprefix_2);

    fprintf(out, "%s  | ***   pending_msgs[%d][%d] = [\n", prefix, radix, p->num_vcs);
    for (int i = 0; i < radix; i++) {
        fprintf(out, "%s  |   port %d: [\n", prefix, i);
        for (int j = 0; j < p->num_vcs; j++) {
            fprintf(out, "%s  |   |  vcs # %d\n", prefix, j);
            print_terminal_dally_message_list(out, subprefix, NULL, state->pending_msgs[i][j]);
        }
        fprintf(out, "%s  |   ]\n", prefix);
    }
    fprintf(out, "%s  | ]\n", prefix);

    fprintf(out, "%s  | ***    pending_msgs_tail = %p\n", prefix, state->pending_msgs_tail);

    fprintf(out, "%s  | ***  queued_msgs[%d][%d] = [\n", prefix, radix, p->num_vcs);
    for (int i = 0; i < radix; i++) {
        fprintf(out, "%s  |   port %d: [\n", prefix, i);
        for (int j = 0; j < p->num_vcs; j++) {
            fprintf(out, "%s  |   |  vcs # %d\n", prefix, j);
            print_terminal_dally_message_list(out, subprefix, NULL, state->queued_msgs[i][j]);
        }
        fprintf(out, "%s  |   ]\n", prefix);
    }
    fprintf(out, "%s  | ]\n", prefix);
    free(subprefix);

    fprintf(out, "%s  | ***     queued_msgs_tail = %p\n", prefix, state->queued_msgs_tail);

    fprintf(out, "%s  | *        in_send_loop[%d] = [", prefix, radix);
    for (int i = 0; i < radix; i++) {
        fprintf(out, "%s%d", i ? ", " : "", state->in_send_loop[i]);
    }
    fprintf(out, "]\n");

    fprintf(out, "%s  | *        queued_count[%d] = [", prefix, radix);
    for (int i = 0; i < radix; i++) {
        fprintf(out, "%s%d", i ? ", " : "", state->queued_count[i]);
    }
    fprintf(out, "]\n");

    fprintf(out, "%s  | *                     st = %p\n", prefix, state->st);
    fprintf(out, "%s  | *                  cc_st = %p\n", prefix, state->cc_st);
    fprintf(out, "%s  |  workloads_finished_flag = %d\n", prefix, state->workloads_finished_flag);

    fprintf(out, "%s  | *     port_bandwidths[%d] = [", prefix, radix);
    for (int i = 0; i < radix; i++) {
        fprintf(out, "%s%g", i ? ", " : "", state->port_bandwidths[i]);
    }
    fprintf(out, "]\n");

    fprintf(out, "%s  | *        vc_max_sizes[%d] = [", prefix, radix);
    for (int i = 0; i < radix; i++) {
        fprintf(out, "%s%d", i ? ", " : "", state->vc_max_sizes[i]);
    }
    fprintf(out, "]\n");

    fprintf(out, "%s  | **    vc_occupancy[%d][%d] = [\n", prefix, radix, p->num_vcs);
    for (int i = 0; i < radix; i++) {
        fprintf(out, "%s  |         port %d: [", prefix, i);
        for (int j = 0; j < p->num_vcs; j++) {
            fprintf(out, "%s%d", j ? ", " : "", state->vc_occupancy[i][j]);
        }
        fprintf(out, "]\n");
    }
    fprintf(out, "%s  |      ]\n", prefix);

    fprintf(out, "%s  | *        link_traffic[%d] = [", prefix, radix);
    for (int i = 0; i < radix; i++) {
        fprintf(out, "%s%ld", i ? ", " : "", state->link_traffic[i]);
    }
    fprintf(out, "]\n");

    fprintf(out, "%s  | *    link_traffic_sample = %p\n", prefix, state->link_traffic_sample);

    fprintf(out, "%s  |         is_monitoring_bw = %d\n", prefix, state->is_monitoring_bw);

    fprintf(out, "%s  | *        last_qos_lvl[%d] = [", prefix, radix);
    for (int i = 0; i < radix; i++) {
        fprintf(out, "%s%d", i ? ", " : "", state->last_qos_lvl[i]);
    }
    fprintf(out, "]\n");

    fprintf(out, "%s  | **      qos_status[%d][%d] = [\n", prefix, radix, num_qos_levels);
    for (int i = 0; i < radix; i++) {
        fprintf(out, "%s  |          port %d: [", prefix, i);
        for (int j = 0; j < num_qos_levels; j++) {
            fprintf(out, "%s%d", j ? ", " : "", state->qos_status[i][j]);
        }
        fprintf(out, "]\n");
    }
    fprintf(out, "%s  |       ]\n", prefix);

    fprintf(out, "%s  | **        qos_data[%d][%d] = [\n", prefix, radix, num_qos_levels);
    for (int i = 0; i < radix; i++) {
        fprintf(out, "%s  |            port %d: [", prefix, i);
        for (int j = 0; j < num_qos_levels; j++) {
            fprintf(out, "%s%d", j ? ", " : "", state->qos_data[i][j]);
        }
        fprintf(out, "]\n");
    }
    fprintf(out, "%s  |         ]\n", prefix);

    fprintf(out, "%s  | *                   anno = %s\n", prefix, state->anno ? state->anno : "(nil)");
    fprintf(out, "%s  | *                 params = %p\n", prefix, state->params);

    fprintf(out, "%s  | **         snapshot_data = %p\n", prefix, state->snapshot_data);

    fprintf(out, "%s  |               output_buf = '%.4096s'\n", prefix, state->output_buf);
    fprintf(out, "%s  | *               rsamples = %p\n", prefix, state->rsamples);
    fprintf(out, "%s  |               fwd_events = %ld\n", prefix, state->fwd_events);
    fprintf(out, "%s  |               rev_events = %ld\n", prefix, state->rev_events);
    fprintf(out, "%s  |              output_buf5 = '%.4096s'\n", prefix, state->output_buf5);
    fprintf(out, "%s  |              output_buf6 = '%.4096s'\n", prefix, state->output_buf6);

    if(p->counting_bool <= 0)
    {
        fprintf(out, "%s  | **         agg_busy_time = %p\n", prefix, state->agg_busy_time);
        fprintf(out, "%s  | **      agg_link_traffic = %p\n", prefix, state->agg_link_traffic);
    } else {
        assert(state->agg_busy_time != NULL);
        assert(state->agg_link_traffic != NULL);
        fprintf(out, "%s  | **   agg_busy_time[%d][%d] = [\n", prefix, p->counting_windows, radix);
        for (int i = 0; i < p->counting_windows; i++) {
            fprintf(out, "%s  |  window %d: [", prefix, i);
            for (int j = 0; j < radix; j++) {
                fprintf(out, "%s%g", j ? ", " : "", state->agg_busy_time[i][j]);
            }
            fprintf(out, "]\n");
        }
        fprintf(out, "%s  |  ]\n", prefix);

        fprintf(out, "%s  | ** agg_link_traffic[%d][%d] = [\n", prefix, p->counting_windows, radix);
        for (int i = 0; i < p->counting_windows; i++) {
            fprintf(out, "%s  |  window %d: [", prefix, i);
            for (int j = 0; j < radix; j++) {
                fprintf(out, "%s%lu", j ? ", " : "", state->agg_link_traffic[i][j]);
            }
            fprintf(out, "]\n");
        }
        fprintf(out, "%s  |  ]\n", prefix);
    }

    fprintf(out, "%s  |             ross_rsample = <dfly_router_sample object>\n", prefix);
    fprintf(out, "%s  |                last_time = %g\n", prefix, state->last_time);
}

char const * const string_event_t(enum event_t type) {
    switch (type) {
        case T_GENERATE:         return "T_GENERATE";
        case T_ARRIVE:           return "T_ARRIVE";
        case T_SEND:             return "T_SEND";
        case T_BUFFER:           return "T_BUFFER";
        case R_SEND:             return "R_SEND";
        case R_ARRIVE:           return "R_ARRIVE";
        case R_BUFFER:           return "R_BUFFER";
        case R_BANDWIDTH:        return "R_BANDWIDTH";
        case R_BW_HALT:          return "R_BW_HALT";
        case T_BANDWIDTH:        return "T_BANDWIDTH";
        case R_SNAPSHOT:         return "R_SNAPSHOT";
        case T_NOTIFY:           return "T_NOTIFY";
        case T_ARRIVE_PREDICTED: return "T_ARRIVE_PREDICTED";
#if ALWAYS_DETERMINISTIC_NETWORK
        case T_VACUOUS_EVENT:    return "T_VACUOUS_EVENT";
#endif /* ALWAYS_DETERMINISTIC_NETWORK */
        default:                 return "UNKNOWN TYPE!!";
    }
}

// Built with help of Claude
bool check_terminal_dally_message(struct terminal_dally_message * before, struct terminal_dally_message * after) {
    bool is_same = true;

    // Fields that have no effects in the simulation
    // before->this_router_ptp_latency

    // Compare all fields
    is_same &= before->magic == after->magic;
    is_same &= before->travel_start_time == after->travel_start_time;
    is_same &= before->travel_end_time == after->travel_end_time;
    is_same &= before->packet_ID == after->packet_ID;
    is_same &= before->type == after->type;
    is_same &= before->notify_type == after->notify_type;
    is_same &= strncmp(before->category, after->category, CATEGORY_NAME_MAX) == 0;
    is_same &= before->final_dest_gid == after->final_dest_gid;
    is_same &= before->sender_lp == after->sender_lp;
    is_same &= before->sender_mn_lp == after->sender_mn_lp;
    is_same &= before->dest_terminal_lpid == after->dest_terminal_lpid;
    is_same &= before->dfdally_src_terminal_id == after->dfdally_src_terminal_id;
    is_same &= before->dfdally_dest_terminal_id == after->dfdally_dest_terminal_id;
    is_same &= before->src_terminal_id == after->src_terminal_id;
    is_same &= before->origin_router_id == after->origin_router_id;
    is_same &= before->app_id == after->app_id;
    is_same &= before->my_N_hop == after->my_N_hop;
    is_same &= before->my_l_hop == after->my_l_hop;
    is_same &= before->my_g_hop == after->my_g_hop;
    is_same &= before->my_hops_cur_group == after->my_hops_cur_group;
    is_same &= before->next_stop == after->next_stop;
    is_same &= before->this_router_arrival == after->this_router_arrival;
    is_same &= before->intm_lp_id == after->intm_lp_id;
    is_same &= before->last_hop == after->last_hop;
    is_same &= before->is_intm_visited == after->is_intm_visited;
    is_same &= before->intm_rtr_id == after->intm_rtr_id;
    is_same &= before->intm_grp_id == after->intm_grp_id;
    is_same &= before->chunk_id == after->chunk_id;
    is_same &= before->packet_size == after->packet_size;
    is_same &= before->message_id == after->message_id;
    is_same &= before->total_size == after->total_size;
    is_same &= before->remote_event_size_bytes == after->remote_event_size_bytes;
    is_same &= before->local_event_size_bytes == after->local_event_size_bytes;
    is_same &= before->vc_index == after->vc_index;
    is_same &= before->rail_id == after->rail_id;
    is_same &= before->output_chan == after->output_chan;
    is_same &= before->is_pull == after->is_pull;
    is_same &= before->pull_size == after->pull_size;
    is_same &= before->path_type == after->path_type;
    is_same &= before->is_there_another_pckt_in_queue == after->is_there_another_pckt_in_queue;
    is_same &= before->qos_reset1 == after->qos_reset1;
    is_same &= before->qos_reset2 == after->qos_reset2;

    return is_same;
}

// Print fuction originally constructed with help from Claude.ai
void print_terminal_dally_message(FILE * out, char const * prefix, void * s, struct terminal_dally_message * msg) {
    //terminal_state * ns = (terminal_state *) s;
    //router_state * ns = (router_state *) s;

    fprintf(out, "%sterminal_dally_message ->\n", prefix);
    fprintf(out, "%s  |                      magic = %d\n", prefix, msg->magic);
    fprintf(out, "%s  |          travel_start_time = %g\n", prefix, msg->travel_start_time);
    fprintf(out, "%s  |            travel_end_time = %g\n", prefix, msg->travel_end_time);
    fprintf(out, "%s  |                  packet_ID = %llu\n", prefix, msg->packet_ID);
    fprintf(out, "%s  |                       type = %d (%s)\n", prefix, msg->type, string_event_t((enum event_t) msg->type));
    fprintf(out, "%s  |                notify_type = %d\n", prefix, msg->notify_type);
    fprintf(out, "%s  |                   category = %.16s\n", prefix, msg->category);
    fprintf(out, "%s  |             final_dest_gid = %lu\n", prefix, msg->final_dest_gid);
    fprintf(out, "%s  |                  sender_lp = %lu\n", prefix, msg->sender_lp);
    fprintf(out, "%s  |               sender_mn_lp = %lu\n", prefix, msg->sender_mn_lp);
    fprintf(out, "%s  |         dest_terminal_lpid = %lu\n", prefix, msg->dest_terminal_lpid);
    fprintf(out, "%s  |    dfdally_src_terminal_id = %u\n", prefix, msg->dfdally_src_terminal_id);
    fprintf(out, "%s  |   dfdally_dest_terminal_id = %u\n", prefix, msg->dfdally_dest_terminal_id);
    fprintf(out, "%s  |            src_terminal_id = %u\n", prefix, msg->src_terminal_id);
    fprintf(out, "%s  |           origin_router_id = %u\n", prefix, msg->origin_router_id);
    fprintf(out, "%s  |                     app_id = %d\n", prefix, msg->app_id);
    fprintf(out, "%s  |                   my_N_hop = %d\n", prefix, msg->my_N_hop);
    fprintf(out, "%s  |                   my_l_hop = %d\n", prefix, msg->my_l_hop);
    fprintf(out, "%s  |                   my_g_hop = %d\n", prefix, msg->my_g_hop);
    fprintf(out, "%s  |          my_hops_cur_group = %d\n", prefix, msg->my_hops_cur_group);
    fprintf(out, "%s  |              saved_channel = %d\n", prefix, msg->saved_channel);
    fprintf(out, "%s  |                   saved_vc = %d\n", prefix, msg->saved_vc);
    fprintf(out, "%s  |                  next_stop = %d\n", prefix, msg->next_stop);
    fprintf(out, "%s  |        this_router_arrival = %g\n", prefix, msg->this_router_arrival);
    fprintf(out, "%s  |    this_router_ptp_latency = %g\n", prefix, msg->this_router_ptp_latency);
    fprintf(out, "%s  |                 intm_lp_id = %u\n", prefix, msg->intm_lp_id);
    fprintf(out, "%s  |                   last_hop = %d\n", prefix, msg->last_hop);
    fprintf(out, "%s  |            is_intm_visited = %d\n", prefix, msg->is_intm_visited);
    fprintf(out, "%s  |                intm_rtr_id = %d\n", prefix, msg->intm_rtr_id);
    fprintf(out, "%s  |                intm_grp_id = %d\n", prefix, msg->intm_grp_id);
    fprintf(out, "%s  |             saved_src_dest = %d\n", prefix, msg->saved_src_dest);
    fprintf(out, "%s  |             saved_src_chan = %d\n", prefix, msg->saved_src_chan);
    fprintf(out, "%s  |                   chunk_id = %u\n", prefix, msg->chunk_id);
    fprintf(out, "%s  |                packet_size = %u\n", prefix, msg->packet_size);
    fprintf(out, "%s  |                 message_id = %u\n", prefix, msg->message_id);
    fprintf(out, "%s  |                 total_size = %u\n", prefix, msg->total_size);
    fprintf(out, "%s  |    remote_event_size_bytes = %d\n", prefix, msg->remote_event_size_bytes);
    fprintf(out, "%s  |     local_event_size_bytes = %d\n", prefix, msg->local_event_size_bytes);
    fprintf(out, "%s  |                   vc_index = %d\n", prefix, msg->vc_index);
    fprintf(out, "%s  |                    rail_id = %d\n", prefix, msg->rail_id);
    fprintf(out, "%s  |                output_chan = %d\n", prefix, msg->output_chan);
    fprintf(out, "%s  |                   event_rc = %d\n", prefix, msg->event_rc);
    fprintf(out, "%s  |                    is_pull = %d\n", prefix, msg->is_pull);
    fprintf(out, "%s  |                  pull_size = %u\n", prefix, msg->pull_size);
    fprintf(out, "%s  |                  path_type = %d\n", prefix, msg->path_type);
    fprintf(out, "%s  |               saved_app_id = %d\n", prefix, msg->saved_app_id);
    fprintf(out, "%s  | is_there_another_pckt_in_queue = %d\n", prefix, msg->is_there_another_pckt_in_queue);
    fprintf(out, "%s  |                   num_rngs = %d\n", prefix, msg->num_rngs);
    fprintf(out, "%s  |                    num_cll = %d\n", prefix, msg->num_cll);
    fprintf(out, "%s  |             last_saved_qos = %d\n", prefix, msg->last_saved_qos);
    fprintf(out, "%s  |                 qos_reset1 = %d\n", prefix, msg->qos_reset1);
    fprintf(out, "%s  |                 qos_reset2 = %d\n", prefix, msg->qos_reset2);
    fprintf(out, "%s  |              rc_is_qos_set = %d\n", prefix, msg->rc_is_qos_set);
    fprintf(out, "%s  | *              rc_qos_data = %p\n", prefix, msg->rc_qos_data);
    fprintf(out, "%s  | *            rc_qos_status = %p\n", prefix, msg->rc_qos_status);
    fprintf(out, "%s  |            saved_send_loop = %d\n", prefix, msg->saved_send_loop);
    fprintf(out, "%s  |       saved_available_time = %g\n", prefix, msg->saved_available_time);
    fprintf(out, "%s  |              saved_min_lat = %g\n", prefix, msg->saved_min_lat);
    fprintf(out, "%s  |             saved_avg_time = %g\n", prefix, msg->saved_avg_time);
    fprintf(out, "%s  |             saved_rcv_time = %g\n", prefix, msg->saved_rcv_time);
    fprintf(out, "%s  |            saved_busy_time = %g\n", prefix, msg->saved_busy_time);
    fprintf(out, "%s  |           saved_total_time = %g\n", prefix, msg->saved_total_time);
    fprintf(out, "%s  |          saved_sample_time = %g\n", prefix, msg->saved_sample_time);
    fprintf(out, "%s  |             msg_start_time = %g\n", prefix, msg->msg_start_time);
    fprintf(out, "%s  |       saved_busy_time_ross = %g\n", prefix, msg->saved_busy_time_ross);
    fprintf(out, "%s  |      saved_fin_chunks_ross = %g\n", prefix, msg->saved_fin_chunks_ross);
    fprintf(out, "%s  |   saved_last_in_queue_time = %g\n", prefix, msg->saved_last_in_queue_time);
    fprintf(out, "%s  |    saved_next_packet_delay = %g\n", prefix, msg->saved_next_packet_delay);
    fprintf(out, "%s  |      saved_processing_time = %g\n", prefix, msg->saved_processing_time);
    fprintf(out, "%s  |           msg_new_mn_event = %g\n", prefix, msg->msg_new_mn_event);
    fprintf(out, "%s  |         last_received_time = %g\n", prefix, msg->last_received_time);
    fprintf(out, "%s  |             last_sent_time = %g\n", prefix, msg->last_sent_time);
    fprintf(out, "%s  |        last_bufupdate_time = %g\n", prefix, msg->last_bufupdate_time);
}
//*** ---------- END OF reverse handler checking functions ---------- ***

/* dragonfly compute node and router LP types */
tw_lptype dragonfly_dally_lps[] =
{
    // Terminal handling functions
    {
        (init_f)terminal_dally_init,
        (pre_run_f) NULL,
        (event_f) terminal_dally_event,
        (revent_f) terminal_dally_rc_event_handler,
        (commit_f) terminal_dally_commit,
        (final_f) dragonfly_dally_terminal_final,
        (map_f) codes_mapping,
        sizeof(terminal_state)
    },
    {
        (init_f) router_dally_init,
        (pre_run_f) NULL,
        (event_f) router_dally_event,
        (revent_f) router_dally_rc_event_handler,
        (commit_f) router_dally_commit,
        (final_f) dragonfly_dally_router_final,
        (map_f) codes_mapping,
        sizeof(router_state),
    },
    {NULL, NULL, NULL, NULL, NULL, NULL, NULL, 0},
};

crv_checkpointer dragonfly_dally_checkpointers[] = {
    {
        &dragonfly_dally_lps[0],
        sizeof(terminal_state),
        (save_checkpoint_state_f) save_terminal_state,
        (clean_checkpoint_state_f) clean_terminal_state,
        (check_states_f) check_terminal_state,
        (print_lpstate_f) print_terminal_state,
        (print_checkpoint_state_f) print_terminal_state,
        (print_event_f) print_terminal_dally_message,
    },
    {
        &dragonfly_dally_lps[1],
        sizeof(router_state),
        (save_checkpoint_state_f) save_router_state,
        (clean_checkpoint_state_f) clean_router_state,
        (check_states_f) check_router_state,
        (print_lpstate_f) print_router_state,
        (print_checkpoint_state_f) print_router_state,
        (print_event_f) print_terminal_dally_message,
    },
};

/* returns the dragonfly lp type for lp registration */
static const tw_lptype* dragonfly_dally_get_cn_lp_type(void)
{
	   return(&dragonfly_dally_lps[0]);
}
static const tw_lptype* router_dally_get_lp_type(void)
{
    return (&dragonfly_dally_lps[1]);
}

static void dragonfly_dally_register(tw_lptype *base_type) {
    lp_type_register(LP_CONFIG_NM_TERM, base_type);
}

static void router_dally_register(tw_lptype *base_type) {
    lp_type_register(LP_CONFIG_NM_ROUT, base_type);
}

/* Routing Functions */
static void dfdally_select_intermediate_group(router_state *s, tw_bf *bf, terminal_dally_message *msg, tw_lp *lp, int fdest_router_id)
{
    int fdest_group_id = fdest_router_id / s->params->num_routers;
    int origin_group_id = msg->origin_router_id / s->params->num_routers;

    // Has an intermediate group been chosen yet? (Should happen at first router)
    if (msg->intm_grp_id == -1) { // Intermediate group hasn't been chosen yet, choose one randomly and route toward it
        assert(s->router_id == msg->origin_router_id);
        msg->num_rngs++;
        int rand_group_id;
        if (NONMIN_INCLUDE_SOURCE_DEST) //then any group is a valid intermediate group
            rand_group_id = tw_rand_integer(lp->rng, s->params->num_groups*s->plane_id, (s->params->num_groups*(s->plane_id+1))-1);
        else { //then we don't consider source or dest groups as valid intermediate groups
            vector<int> group_list;
            for (int i = s->params->num_groups*s->plane_id; i < s->params->num_groups*(s->plane_id+1); i++)
            {
                if ((i != origin_group_id) && (i != fdest_group_id)) {
                    group_list.push_back(i);
                }
            }
            int rand_sel = tw_rand_integer(lp->rng, 0, group_list.size()-1);
            rand_group_id = group_list[rand_sel];
        }
        msg->intm_grp_id = rand_group_id;
    }
    else { //the only time that it is re-set is when a router didn't have a direct connection to the intermediate group but had no other options
        // so we need to pick an intm group that the current router DOES have a connection to.
        assert(s->router_id != msg->origin_router_id);

        set< int > valid_intm_groups;
        vector< Connection > global_conns = s->connMan.get_connections_by_type(CONN_GLOBAL);
        for (vector<Connection>::iterator it = global_conns.begin(); it != global_conns.end(); it ++) {
            Connection conn = *it;
            if (NONMIN_INCLUDE_SOURCE_DEST) //then any group I connect to is valid
            {
                valid_intm_groups.insert(conn.dest_group_id);
            }
            else
            {
                if ((conn.dest_group_id != fdest_group_id) && (conn.dest_group_id != origin_group_id))
                    valid_intm_groups.insert(conn.dest_group_id);
            }
        }

        int rand_sel = tw_rand_integer(lp->rng, 0, valid_intm_groups.size()-1);
        msg->num_rngs++;
        set< int >::iterator it = valid_intm_groups.begin();
        advance(it, rand_sel); //you can't just use [] to access a set
        msg->intm_grp_id = *it;
    }
}

//when using this function, you should assume that the self router is NOT the destination. That should be handled elsewhere.
static vector< Connection > get_legal_minimal_stops(router_state *s, tw_bf *bf, terminal_dally_message *msg, tw_lp *lp, int fdest_router_id)
{
    int my_router_id = s->router_id;
    int my_group_id = s->group_id;
    int origin_group_id = msg->origin_router_id / s->params->num_routers;
    int fdest_group_id = fdest_router_id / s->params->num_routers;

    if (my_group_id != fdest_group_id) { //we're in origin group or intermediate group - either way we need to route to fdest group minimally
        vector< Connection > conns_to_dest_group = s->connMan.get_connections_to_group(fdest_group_id);
        if (conns_to_dest_group.size() > 0) { //then we have a direct connection to dest group
            return conns_to_dest_group; // --------- return direct connection
        }
        // else { //we don't have a direct connection to group and need list of routers in our group that do
        //     return s->connMan.get_routed_connections_to_group(fdest_group_id, true); //get list of connections that I have that go to a router in my group that go to dest group
        //     // --------- return non-direct connection (still minimal though)        
        // }
        else { //we don't have a direct connection to group and need list of routers in our group that do
            vector<Connection> poss_next_conns_to_group = s->connMan.get_routed_connections_to_group(fdest_group_id, true);

            // vector< Connection > poss_next_conns_to_group;
            // set< int > poss_router_id_set_to_group; //TODO this might be a source of non-determinism(?)
            // for(int i = 0; i < connectionList[my_group_id][fdest_group_id].size(); i++)
            // {
            //     int poss_router_id = connectionList[my_group_id][fdest_group_id][i];
            //     if (poss_router_id_set_to_group.count(poss_router_id) == 0) { //we only want to consider a single router id once (we look at all connections to it using the conn man)
            //         vector< Connection > conns = s->connMan.get_connections_to_gid(poss_router_id, CONN_LOCAL);
            //         poss_router_id_set_to_group.insert(poss_router_id);
            //         poss_next_conns_to_group.insert(poss_next_conns_to_group.end(), conns.begin(), conns.end());
            //     }
            // }
            return poss_next_conns_to_group; 
        }
    }
    else { //then we're in the final destination group, also we assume that we're not the fdest router
        assert(my_group_id == fdest_group_id);
        assert(my_router_id != fdest_router_id); //this should be handled outside of this function

        // if(netMan.is_link_failures_enabled()) {
        //     vector<Connection> conns = s->connMan.get_connections_by_type(CONN_LOCAL);
        //     vector<Connection> good_conns;
        //     for(int i = 0; i < conns.size(); i++)
        //     {
        //         int next_src_gid = conns[i].dest_gid;
        //         if(netMan.get_connection_manager_for_router(next_src_gid).is_any_connection_to(fdest_router_id))
        //             good_conns.push_back(conns[i]);
        //     }
        //     return good_conns;
        // }

        vector< Connection > conns_to_fdest_router = s->connMan.get_connections_to_gid(fdest_router_id, CONN_LOCAL);
        return conns_to_fdest_router;
    }
}

//Note that this is different than Dragonfly Plus's implementation, this isn't the converse of minimal, these are any
//connections that could lead to the intermediate group or a new one if necessary
static vector< Connection > get_legal_nonminimal_stops(router_state *s, tw_bf *bf, terminal_dally_message *msg, tw_lp *lp, int fdest_router_id)
{
    int my_router_id = s->router_id;
    int my_group_id = s->group_id;
    int origin_group_id = msg->origin_router_id / s->params->num_routers;
    int fdest_group_id = fdest_router_id / s->params->num_routers;
    bool in_intermediate_group = (my_group_id != origin_group_id) && (my_group_id != fdest_group_id);
    int preset_intm_group_id = msg->intm_grp_id;

    if (my_group_id == origin_group_id) {
        vector< Connection > conns_to_intm_group = s->connMan.get_connections_to_group(preset_intm_group_id);
        //are we the originating router
        if (my_router_id == msg->origin_router_id) { //then we are able to route within our own group if necessary
            // Do we have direct connection to intermediate group?
            if (conns_to_intm_group.size() > 0) { //yes
                return conns_to_intm_group;
            }
            else { //no - route within group to router that DOES have a connection to intm group
                vector<Connection> conns_to_connecting_routers = s->connMan.get_routed_connections_to_group(preset_intm_group_id, true);
                // vector<int> connecting_router_ids = connectionList[my_group_id][preset_intm_group_id];
                // vector< Connection > conns_to_connecting_routers;
                // for (int i = 0; i < connecting_router_ids.size(); i++)
                // {
                //     int poss_router_id = connecting_router_ids[i];
                //     vector< Connection > candidate_conns = s->connMan.get_connections_to_gid(poss_router_id, CONN_LOCAL);
                //     conns_to_connecting_routers.insert(conns_to_connecting_routers.end(), candidate_conns.begin(), candidate_conns.end());
                // }
                return conns_to_connecting_routers;
            }
        }
        else { //then we can't afford to reroute within our group, we must route to the int group if possible - pick a new one if not
            if (conns_to_intm_group.size() > 0) {
                return conns_to_intm_group; //route there directly
            }
            else { //pick a new one!
                dfdally_select_intermediate_group(s, bf, msg, lp, fdest_router_id);
                conns_to_intm_group = s->connMan.get_connections_to_group(msg->intm_grp_id); //new intm group id
                return conns_to_intm_group;
            }
        }
    }
    else if (in_intermediate_group) {
        //if we're in the intermediate group then we're just going to default to routing minimally, return an empty vector.
        vector< Connection > empty;
        return empty;
    }
    else if (my_group_id == fdest_group_id)
    {
        //same as intermediate, force minimal choices
        vector< Connection > empty;
        return empty;
    }
    else
    {
        tw_error(TW_LOC, "Invalid group somehow: not origin, not intermediate, and not fdest group\n");
        vector< Connection > empty;
        return empty;
    }
}

//TODO - make some way to configure whether there's any form of adaptiveness to this scheme
static Connection dfdally_minimal_routing(router_state *s, tw_bf *bf, terminal_dally_message *msg, tw_lp *lp, int fdest_router_id)
{
    vector< Connection > poss_next_stops = get_legal_minimal_stops(s, bf, msg, lp, fdest_router_id);
    if (poss_next_stops.size() < 1)
        tw_error(TW_LOC, "%d group %d: MINIMAL DEAD END to %d in group %d - (s%d -> d%d)\n", s->router_id, s->group_id, fdest_router_id, fdest_router_id / s->params->num_routers, msg->origin_router_id, fdest_router_id);

    // ConnectionType conn_type = poss_next_stops[0].conn_type; //TODO this assumes that all possible next stops are of same type - OK for now, but remember this
    // if (conn_type == CONN_GLOBAL) { //TOOD should we really only randomize global and not local? should we really do light adaptive for nonglobal?
        msg->num_rngs++;
        int rand_sel = tw_rand_integer(lp->rng, 0, poss_next_stops.size() - 1);
        return poss_next_stops[rand_sel];
    // }
    // else
    // {
    //     Connection best_min_conn = get_absolute_best_connection_from_conns(s, bf, msg, lp, poss_next_stops);
    //     return best_min_conn;
    // }
}

// Coloquially: "Valiant Group Routing"
// This follows the randomized indirect routing algorithm detailed in "Cost-Efficient Dragonfly topology for Large-Scale Systems" and
// "Technology-Driven, Highly-Scalable Dragonfly Topology" by Kim, Dally, Scott, and Abts
// They sourced it from "A scheme for fast parallel communication" by L.G. Valiant
// It differs from true valiant routing in that it randomly selects a GROUP and routes to it - not a random intermediate router
static Connection dfdally_nonminimal_routing(router_state *s, tw_bf *bf, terminal_dally_message *msg, tw_lp *lp, int fdest_router_id)
{
    int my_router_id = s->router_id;
    int my_group_id = s->group_id;
    int fdest_group_id = fdest_router_id / s->params->num_routers;
    int origin_group_id = msg->origin_router_id / s->params->num_routers;

    assert(msg->intm_grp_id != -1); // This needs to have already been set.
    // //The setting of intm_grp_id is kept out of this function so that other routing algorithms can utilize this routing function
    // //and set it themselves based on the context. This helps avoid an instance like: prog-adaptive routing, second router in path decides
    // //to take non-minimal routing but the pre-selected intermediate group ID isn't accessible to it and would require re-routing and 
    // //thus take extra hops. Possible illegal move and could cause deadlock.

    if (my_group_id == msg->intm_grp_id) //then we've visited the intermediate group by definition
        msg->is_intm_visited = 1;

    int next_dest_group_id; //The ID of the group that we are aiming for next - either intermediate group or fdest group
    if (msg->is_intm_visited == 1) // Then we need to route to the fdest group
        next_dest_group_id = fdest_group_id;
    else // Then we haven't visited the intermediate group yet and need to route there first
        next_dest_group_id = msg->intm_grp_id;

    // Do I have a direct connection to the next_dest group?
    vector< Connection > conns_to_next_group = s->connMan.get_connections_to_group(next_dest_group_id);
    if (conns_to_next_group.size() > 0) { //Then yes I do
        msg->num_rngs++;
        int rand_sel = tw_rand_integer(lp->rng, 0, conns_to_next_group.size()-1);
        Connection next_conn = conns_to_next_group[rand_sel];
        return next_conn;
    }
    else { // I need to route to a router in my group that does have a direct connection to the intermediate group
        vector<Connection> connections_toward_next_group = s->connMan.get_routed_connections_to_group(next_dest_group_id, true);
        // vector<int> connecting_router_ids = connectionList[my_group_id][next_dest_group_id];
        // assert(connecting_router_ids.size() > 0);
        // msg->num_rngs++;
        // int rand_sel = tw_rand_integer(lp->rng, 0, connecting_router_ids.size()-1);
        // int conn_router_id = connecting_router_ids[rand_sel];

        // //There may be parallel connections to the same router - randomly select from them
        // vector< Connection > conns_to_next_router = s->connMan.get_connections_to_gid(conn_router_id, CONN_LOCAL);
        // assert(conns_to_next_router.size() > 0);
        // msg->num_rngs++;
        // rand_sel = tw_rand_integer(lp->rng, 0, conns_to_next_router.size()-1);
        // Connection next_conn = conns_to_next_router[rand_sel];
        
        int rand_sel = tw_rand_integer(lp->rng, 0, connections_toward_next_group.size()-1);
        msg->num_rngs++;
        Connection next_conn = connections_toward_next_group[rand_sel];
        
        return next_conn;
    }
}

//Uses PAR algorithm
static Connection dfdally_prog_adaptive_routing(router_state *s, tw_bf *bf, terminal_dally_message *msg, tw_lp *lp, int fdest_router_id)
{
    int my_router_id = s->router_id;
    int my_group_id = s->group_id;
    int fdest_group_id = fdest_router_id / s->params->num_routers;
    int origin_group_id = msg->origin_router_id / s->params->num_routers;
    int adaptive_threshold = s->params->adaptive_threshold;
    
    // The check for detination group local routing has already been completed - we can assume we're not in the destination group

    // are we in the intermediate group?
    if (my_group_id == msg->intm_grp_id)
        msg->is_intm_visited = 1;

    Connection nextStopConn;
    vector< Connection > poss_min_next_stops = get_legal_minimal_stops(s, bf, msg, lp, fdest_router_id);
    vector< Connection > poss_nonmin_next_stops = get_legal_nonminimal_stops(s, bf, msg, lp, fdest_router_id);

    Connection best_min_conn, best_nonmin_conn;
    ConnectionType conn_type_of_mins, conn_type_of_nonmins;

    if (poss_min_next_stops.size() > 0)
    {
        conn_type_of_mins = poss_min_next_stops[0].conn_type; // All of these in this vector should be the same...
    }
    if (poss_nonmin_next_stops.size() > 0)
    {
        conn_type_of_nonmins = poss_nonmin_next_stops[0].conn_type;
    }

    if (conn_type_of_mins == CONN_GLOBAL)
        best_min_conn = dfdally_get_best_from_k_connections(s, bf, msg, lp, poss_min_next_stops, s->params->global_k_picks);
    else
        best_min_conn = get_absolute_best_connection_from_conns(s, bf, msg, lp, poss_min_next_stops); //could use from_k_connections function but that's very expensive when k == size of input connections
    
    if (conn_type_of_nonmins == CONN_GLOBAL)
        best_nonmin_conn = dfdally_get_best_from_k_connections(s, bf, msg, lp, poss_nonmin_next_stops, s->params->global_k_picks);
    else
        best_nonmin_conn = get_absolute_best_connection_from_conns(s, bf, msg, lp, poss_nonmin_next_stops);

    int min_score = dfdally_score_connection(s, bf, msg, lp, best_min_conn, C_MIN);
    int nonmin_score = dfdally_score_connection(s, bf, msg, lp, best_nonmin_conn, C_NONMIN);

    if ((msg->path_type == NON_MINIMAL) && (msg->is_intm_visited != 1)) { //if we're nonminimal and haven't reached the intermediate group yet
        //must pick non-minimal (if we have visited, we can pick minimal then as nonminimal will be an empty vector)
        return best_nonmin_conn;
    }

    if (min_score <= adaptive_threshold)
        return best_min_conn;
    else if (min_score <= nonmin_score)
        return best_min_conn;
    else {
        msg->path_type = NON_MINIMAL;
        return best_nonmin_conn;
    }
}

//SMART ROUTING: Smart routing is my term for failure aware routing. Leverages the network manager's ability to filter out invalid connections and to return only valid paths between endpoints.
//It's significantly slower in runtime, typically, and is very experimental at this point. Use at own risk.

//when using this function, you should assume that the self router is NOT the destination. That should be handled elsewhere.
static set< Connection> get_smart_legal_minimal_stops(router_state *s, tw_bf *bf, terminal_dally_message *msg, tw_lp *lp, int fdest_router_id, int max_global_hops_in_path)
{
    int my_router_id = s->router_id;
    int my_group_id = s->group_id;
    int origin_group_id = msg->origin_router_id / s->params->num_routers;
    int fdest_group_id = fdest_router_id / s->params->num_routers;

    set<Connection> possible_next_dests;

    if (my_group_id == fdest_group_id) { //we're in origin group or intermediate group - either way we need to route to fdest group minimally
        vector< Connection > conns_to_fdest_router = s->connMan.get_connections_to_gid(fdest_router_id, CONN_LOCAL);
        if(conns_to_fdest_router.size() > 0) {
            possible_next_dests.insert(conns_to_fdest_router.begin(),conns_to_fdest_router.end());
            return possible_next_dests;
        }
    }
    // vector<int> shortest_path_next_gids = netMan.get_shortest_nexts(s->router_id, fdest_router_id);
    // for(vector<int>::iterator it = shortest_path_next_gids.begin(); it != shortest_path_next_gids.end(); it++)
    // {   
    //     vector<Connection> local_conns;
    //     vector<Connection> global_conns;
    //     if(msg->my_hops_cur_group < max_hops_per_group)
    //         local_conns = s->connMan.get_connections_to_gid(*it,CONN_LOCAL);
    //     if(msg->my_g_hop < max_global_hops_in_path)
    //         global_conns = s->connMan.get_connections_to_gid(*it,CONN_GLOBAL);
        
    //     vector<Connection>::iterator it2;
    //     for(it2 = local_conns.begin(); it2 != local_conns.end(); it2++)
    //     {
    //         if (netMan.get_valid_next_hops_conns(it2->dest_gid,fdest_router_id,(max_hops_per_group-(msg->my_hops_cur_group+1)), (max_global_hops_in_path-msg->my_g_hop)).size())
    //             possible_next_dests.insert(*it2);
    //     }
    //     for(it2 = global_conns.begin(); it2 != global_conns.end(); it2++)
    //     {
    //         if (netMan.get_valid_next_hops_conns(it2->dest_gid,fdest_router_id,(max_hops_per_group-msg->my_hops_cur_group), (max_global_hops_in_path-(msg->my_g_hop+1))).size())
    //             possible_next_dests.insert(*it2);
    //     }
    // }
    // if (possible_next_dests.size() < 1)
    // {
        if(s->group_id == fdest_router_id / s->params->num_routers)
        {
            possible_next_dests = netMan.get_valid_next_hops_conns(s->router_id, fdest_router_id, (max_hops_per_group-msg->my_hops_cur_group), 0);
        }
        else {
            possible_next_dests = netMan.get_valid_next_hops_conns(s->router_id, fdest_router_id, (max_hops_per_group-msg->my_hops_cur_group), (max_global_hops_in_path-msg->my_g_hop));
        }

        // return possible_next_dests;
    // }
    return possible_next_dests;
}

//when using this function, you should assume that the self router is NOT the destination. That should be handled elsewhere.
static set< Connection> get_smart_legal_minimal_stops(router_state *s, tw_bf *bf, terminal_dally_message *msg, tw_lp *lp, int fdest_router_id)
{
    return get_smart_legal_minimal_stops(s, bf, msg, lp, fdest_router_id, max_global_hops_minimal);
}

static set<Connection> get_smart_legal_nonminimal_stops(router_state *s, tw_bf *bf, terminal_dally_message *msg, tw_lp *lp, int fdest_router_id)
{
    set<Connection> possible_next_dests;
    if(s->group_id == fdest_router_id / s->params->num_routers)
    {
        possible_next_dests = netMan.get_valid_next_hops_conns(s->router_id, fdest_router_id, (max_hops_per_group-msg->my_hops_cur_group), 0);
        if (possible_next_dests.size() == 0)
            possible_next_dests = netMan.get_valid_next_hops_conns(s->router_id, fdest_router_id, (max_hops_per_group-msg->my_hops_cur_group), (max_global_hops_nonminimal-msg->my_g_hop));
    }
    else {
        possible_next_dests = netMan.get_valid_next_hops_conns(s->router_id, fdest_router_id, (max_hops_per_group-msg->my_hops_cur_group), (max_global_hops_nonminimal-msg->my_g_hop));
    }

    return possible_next_dests;
}

static Connection dfdally_smart_minimal_routing(router_state *s, tw_bf *bf, terminal_dally_message *msg, tw_lp *lp, int fdest_router_id)
{
    int my_router_id = s->router_id;
    int my_group_id = s->group_id;
    int fdest_group_id = fdest_router_id / s->params->num_routers;
    int origin_group_id = msg->origin_router_id / s->params->num_routers;

    if(s->router_id == fdest_router_id)
    {
        vector< Connection > poss_next_stops = s->connMan.get_connections_to_gid(msg->dfdally_dest_terminal_id, CONN_TERMINAL);
            if (poss_next_stops.size() < 1)
                tw_error(TW_LOC, "Destination Router %d: No connection to destination terminal %d\n", s->router_id, msg->dfdally_dest_terminal_id); //shouldn't happen unless math was wrong
        Connection best_conn = get_absolute_best_connection_from_conns(s, bf, msg, lp, poss_next_stops);
        return best_conn;
    }
    //do i have a direct connection to fdest by any chance?
    vector<Connection> direct_conns;
    if(my_group_id != fdest_group_id)
        direct_conns = s->connMan.get_connections_to_gid(fdest_router_id,CONN_GLOBAL);
    else
        direct_conns = s->connMan.get_connections_to_gid(fdest_router_id,CONN_LOCAL);
    if (direct_conns.size() > 0) {
        msg->num_rngs++;
        int offset = tw_rand_integer(lp->rng,0,direct_conns.size()-1);
        return direct_conns[offset];
    }
    else
    {
        set<Connection> possible_conns = get_smart_legal_minimal_stops(s, bf, msg, lp, fdest_router_id);

        if (possible_conns.size() < 1)
            tw_error(TW_LOC, "Smart Routing: Pathfinder messed up\n");


        msg->num_rngs++;
        int offset = tw_rand_integer(lp->rng,0,possible_conns.size()-1);
        set<Connection>::iterator it = possible_conns.begin();
        advance(it, offset);
        return *(it);
    }    
}

static Connection dfdally_smart_nonminimal_routing(router_state *s, tw_bf *bf, terminal_dally_message *msg, tw_lp *lp, int fdest_router_id)
{
    int my_router_id = s->router_id;
    int my_group_id = s->group_id;
    int fdest_group_id = fdest_router_id / s->params->num_routers;
    int origin_group_id = msg->origin_router_id / s->params->num_routers;

    if(s->router_id == fdest_router_id)
    {
        vector< Connection > poss_next_stops = s->connMan.get_connections_to_gid(msg->dfdally_dest_terminal_id, CONN_TERMINAL);
            if (poss_next_stops.size() < 1)
                tw_error(TW_LOC, "Destination Router %d: No connection to destination terminal %d\n", s->router_id, msg->dfdally_dest_terminal_id); //shouldn't happen unless math was wrong
        Connection best_conn = get_absolute_best_connection_from_conns(s, bf, msg, lp, poss_next_stops);
        return best_conn;
    }
    
    if (my_group_id != origin_group_id && my_group_id != fdest_group_id)
        msg->path_type = NON_MINIMAL;
        
    set<Connection> possible_conns = get_smart_legal_nonminimal_stops(s, bf, msg, lp, fdest_router_id);

    if (possible_conns.size() < 1)
        tw_error(TW_LOC, "Smart Routing: Pathfinder messed up\n");

    msg->num_rngs++;
    int offset = tw_rand_integer(lp->rng,0,possible_conns.size()-1);
    set<Connection>::iterator it = possible_conns.begin();
    advance(it, offset);
    return *(it);
}

static Connection dfdally_smart_prog_adaptive_routing(router_state *s, tw_bf *bf, terminal_dally_message *msg, tw_lp *lp, int fdest_router_id)
{
    int my_router_id = s->router_id;
    int my_group_id = s->group_id;
    int fdest_group_id = fdest_router_id / s->params->num_routers;
    int origin_group_id = msg->origin_router_id / s->params->num_routers;
    int adaptive_threshold = s->params->adaptive_threshold;

    if (my_router_id == fdest_router_id) // we're the destination, send to connected terminal
    {
        if(s->router_id == fdest_router_id)
        {
            vector< Connection > poss_next_stops = s->connMan.get_connections_to_gid(msg->dfdally_dest_terminal_id, CONN_TERMINAL);
                if (poss_next_stops.size() < 1)
                    tw_error(TW_LOC, "Destination Router %d: No connection to destination terminal %d\n", s->router_id, msg->dfdally_dest_terminal_id); //shouldn't happen unless math was wrong
            Connection best_conn = get_absolute_best_connection_from_conns(s, bf, msg, lp, poss_next_stops);
            return best_conn;
        }
    }

    if (my_group_id != origin_group_id && my_group_id != fdest_group_id)
        msg->is_intm_visited = 1;

    if (msg->is_intm_visited == 1) //then we route minimally the rest of the way
    {
        set<Connection> poss_min_next_stops = get_smart_legal_minimal_stops(s, bf, msg, lp, fdest_router_id, max_global_hops_nonminimal);
        return get_absolute_best_connection_from_conn_set(s, bf, msg, lp, poss_min_next_stops);
    }
    else
    {
        if(msg->path_type == NON_MINIMAL)
        {
            set<Connection> poss_next_stops = get_smart_legal_nonminimal_stops(s, bf, msg, lp, fdest_router_id);
            if (poss_next_stops.size() < 1)
                tw_error(TW_LOC, "Smart Prog Adaptive Routing: intm can't reach intm rtr");
            Connection best_conn = dfdally_get_best_from_k_connection_set(s,bf,msg,lp,poss_next_stops,s->params->global_k_picks);
            return best_conn;
        }
        else {
            if (my_group_id == origin_group_id)
            {
                set<Connection> poss_min_next_stops = get_smart_legal_minimal_stops(s, bf, msg, lp, fdest_router_id);
                set<Connection> poss_non_min_next_stops = get_smart_legal_nonminimal_stops(s, bf, msg, lp, fdest_router_id);

                Connection best_min_conn = dfdally_get_best_from_k_connection_set(s,bf,msg,lp,poss_min_next_stops,s->params->global_k_picks);
                Connection best_nonmin_conn = dfdally_get_best_from_k_connection_set(s,bf,msg,lp,poss_non_min_next_stops,s->params->global_k_picks);

                int min_score = dfdally_score_connection(s, bf, msg, lp, best_min_conn, C_MIN);
                int nonmin_score = dfdally_score_connection(s, bf, msg, lp, best_nonmin_conn, C_NONMIN);

                if (min_score <= adaptive_threshold)
                    return best_min_conn;
                else if (min_score <= nonmin_score)
                    return best_min_conn;
                else {
                    msg->path_type = NON_MINIMAL;
                    return best_nonmin_conn;
                }
            } else 
            {
                set<Connection> poss_min_next_stops = get_smart_legal_minimal_stops(s, bf, msg, lp, fdest_router_id, max_global_hops_nonminimal);
                return dfdally_get_best_from_k_connection_set(s, bf, msg, lp, poss_min_next_stops,s->params->global_k_picks);
            }
        }
    }
}

/*
LEGACY DRAGONFLY DALLY ROUTING - Sept. 2019, the state of dragonfly routing became difficult to maintain.
There is little documentation to justify decisions in the code and consequently it is near impossible to tell
the difference between intended behavior and a bug. This is not sustainable and lowers confidence in the model.
Getting the routing functions all up to date with the latest updates to the model was very challenging and
the decision was made to rewrite the routing from the ground up. We didn't want to completely scrap what had
been done previously as we wanted to still be able to recreate experiments with the same routing strategy as
before. So the old routing functions have been preserved with minimal modifications to work with the latest
data structures used by the model. Specify: routing="prog-adaptive-legacy" in the configuration file to use.
USE AT OWN RISK. One should consider support for this routing algorithm ended. From thorough analysis, I believe
that there are bugs and unintended behavior in this code but I do not have proper documentation to say for
certain. Thanks, -NM

Link to last version prior to this port: https://github.com/codes-org/codes/releases/tag/old-dfdally
(See git tag: old-dfdally)
*/

//This is utilized by prog_adaptive_legacy routing - returns the first channel number from the conneciton list taht features a given router to the specified group
int find_chan_legacy(int router_id, int dest_grp_id, int num_routers)
{
    int my_grp_id = router_id / num_routers;
    for(int i = 0; i < connectionList[my_grp_id][dest_grp_id].size(); i++)
    {
        if(connectionList[my_grp_id][dest_grp_id][i] == router_id)
            return i;
    }
    return -1;
}

static int get_port_score_legacy(router_state * s,
        int port,
        int bias)
{
    int port_count = 0;

    if(port <= 0)
       return INT_MAX;
    
    for(int k = 0; k < s->params->num_vcs; k++)
    {
        port_count += s->vc_occupancy[port][k];
    }
    port_count += s->queued_count[port];

    if(bias)
        port_count = port_count * 2;
    return port_count;
}


static void do_local_adaptive_routing_legacy(router_state * s,
        tw_lp * lp,
        terminal_dally_message * msg,
        tw_bf * bf,
        int dest_router_id,
        int intm_router_id,
        short* rng_counter)
{

        //this is an adaptation of the do_local_adaptive_routing() from the original dragonfly-dally model found below   
        int next_min_stop = dest_router_id; //was a vector of one in original - this is equivalent to what it did
        int next_nonmin_stop = intm_router_id; //was a vector of one
        
        Connection min_conn = s->connMan.get_connections_to_gid(next_min_stop, CONN_LOCAL)[0];
        Connection nonmin_conn = s->connMan.get_connections_to_gid(next_nonmin_stop, CONN_LOCAL)[0];

        int min_score = dfdally_score_connection(s, bf, msg, lp, min_conn, C_MIN);
        int nonmin_score = dfdally_score_connection(s, bf, msg, lp, nonmin_conn, C_NONMIN);

        if (min_score > s->params->adaptive_threshold && min_score > nonmin_score)
        {
            msg->path_type = NON_MINIMAL;
        }
        else
            msg->path_type = MINIMAL;
        
// Old version
//     tw_lpid min_rtr_id, nonmin_rtr_id; 
//     int min_port, nonmin_port;

//     int dest_grp_id = dest_router_id / s->params->num_routers;
//     int intm_grp_id = intm_router_id / s->params->num_routers;
//     int my_grp_id = s->router_id / s->params->num_routers;

//     if(my_grp_id != dest_grp_id || my_grp_id != intm_grp_id)
//         tw_error(TW_LOC, "\n Invalid local routing my grp id %d dest_gid %d intm_gid %d intm rid %d",
//                 my_grp_id, dest_grp_id, intm_grp_id, intm_router_id);

//     int min_chan=-1, nonmin_chan=-1;
//     vector<int> next_min_stops = get_intra_router(s, s->router_id, dest_router_id, s->params->num_routers);
//     vector<int> next_nonmin_stops = get_intra_router(s, s->router_id, intm_router_id, s->params->num_routers);

//     (*rng_counter)++;
//     min_chan = tw_rand_integer(lp->rng, 0, next_min_stops.size() - 1);
//     (*rng_counter)++;
//     nonmin_chan = tw_rand_integer(lp->rng, 0, next_nonmin_stops.size() - 1);
  
//     codes_mapping_get_lp_id(lp_group_name, LP_CONFIG_NM_ROUT, s->anno, 0, next_min_stops[min_chan] / num_routers_per_mgrp,
//           next_min_stops[min_chan] % num_routers_per_mgrp, &min_rtr_id);
//     codes_mapping_get_lp_id(lp_group_name, LP_CONFIG_NM_ROUT, s->anno, 0, next_nonmin_stops[nonmin_chan] / num_routers_per_mgrp,
//           next_nonmin_stops[nonmin_chan] % num_routers_per_mgrp, &nonmin_rtr_id);

//     min_port = get_output_port(s, msg, lp, bf, min_rtr_id, rng_counter);
//     nonmin_port = get_output_port(s, msg, lp, bf, nonmin_rtr_id, rng_counter);

//     int min_port_count = 0, nonmin_port_count = 0;

//     for(int k = 0; k < s->params->num_vcs; k++)
//         min_port_count += s->vc_occupancy[min_port][k];
//     min_port_count += s->queued_count[min_port];

//     for(int k = 0; k < s->params->num_vcs; k++)
//         nonmin_port_count += s->vc_occupancy[nonmin_port][k];
//     nonmin_port_count += s->queued_count[nonmin_port];

//     int local_stop = -1;
//     tw_lpid global_stop;

//     if(BIAS_MIN == 1)
//     {
//         nonmin_port_count = nonmin_port_count * 2;
//     }

//     msg->path_type = MINIMAL;

// //  if(nonmin_port_count * num_intra_nonmin_hops > min_port_count * num_intra_min_hops)
//     if(min_port_count > adaptive_threshold && min_port_count > nonmin_port_count)
//     {
//         msg->path_type = NON_MINIMAL;
//     }
// end old version
}


static int get_output_port_legacy(router_state *s, terminal_dally_message *msg, tw_lp *lp, tw_bf *bf, int next_stop, short *rng_counter)
{
    int output_port = -1;
    int rand_offset = -1;
    int terminal_id = codes_mapping_get_lp_relative_id(msg->dest_terminal_lpid, 0, 0);
    const dragonfly_param *p = s->params;
        
    int local_router_id = codes_mapping_get_lp_relative_id(next_stop, 0, 0);
    int src_router = s->router_id;

    if((tw_lpid)next_stop == msg->dest_terminal_lpid)
    {
        Connection term_conn = s->connMan.get_connections_to_gid(msg->dfdally_dest_terminal_id, CONN_TERMINAL)[0];
        output_port = term_conn.port;
    }
    else
    {
        int intm_grp_id = local_router_id / p->num_routers;
        int rand_offset = -1;

        if(intm_grp_id != s->group_id)
        {
            /* traversing a global channel */
            vector< Connection > conns_to_intm_grp = s->connMan.get_connections_to_group(intm_grp_id);

            if (conns_to_intm_grp.size() == 0)
                printf("\n Source router %d intm_grp_id %d ", src_router, intm_grp_id);

            assert(conns_to_intm_grp.size() > 0);

            (*rng_counter)++;
            rand_offset = tw_rand_integer(lp->rng, 0, conns_to_intm_grp.size()-1);

            assert(rand_offset >= 0 && rand_offset < s->params->num_global_channels);

            Connection global_conn = conns_to_intm_grp[rand_offset];

            output_port = global_conn.port;
        }
        else
        {
            vector< Connection > conns_to_local_router = s->connMan.get_connections_to_gid(local_router_id, CONN_LOCAL);
            
            (*rng_counter)++;
            rand_offset = tw_rand_integer(lp->rng, 0, conns_to_local_router.size()-1);

            Connection local_conn = conns_to_local_router[rand_offset];

            output_port = local_conn.port;
        }
    }
    return output_port;
}

//This is a 1:1 port of the get_next_stop from the old dfdally model. All bugs left as is, use at own risk.
static tw_lpid get_next_stop_legacy(router_state *s, tw_lp *lp, tw_bf *bf, terminal_dally_message *msg, int dest_router_id, int adap_chan, int do_chan_selection, int get_direct_con, short* rng_counter)
{
    int dest_lp;
    tw_lpid router_dest_id;
    int dest_group_id;

    int local_router_id = s->router_id;
    int my_grp_id = s->router_id / s->params->num_routers;
    dest_group_id = dest_router_id / s->params->num_routers;
    int origin_grp_id = msg->origin_router_id / s->params->num_routers;

    int select_chan = -1;
    /* If the packet has arrived at the destination router */
    if(dest_router_id == local_router_id)
    {
        dest_lp = msg->dest_terminal_lpid;
        return dest_lp;
    }
    /* If the packet has arrived at the destination group */
    if(s->group_id == dest_group_id)
    {
        int next_stop = dest_router_id; //trimmed down from old as the old had a lot of superflouous code to poll randomly from a vector of one.
        
        codes_mapping_get_lp_id(lp_group_name, LP_CONFIG_NM_ROUT, s->anno, 0, next_stop / num_routers_per_mgrp,
            next_stop % num_routers_per_mgrp, &router_dest_id);
    
#if DEBUG == 1
        if(msg->packet_ID == LLU(TRACK_PKT) && msg->src_terminal_id == T_ID)
                printf("\n Next stop is %d ", next_stop);
#endif
        
        return router_dest_id;
    }

    /* If the packet is at the source router then select a global channel among
        * the many global channels available (unless one already specified by
        * adaptive routing). do_chan_selection is turned on in case prog-adaptive
        * routing has just decided to take a non-minimal route. */
    //modified a little to work with connection manager
    if(msg->last_hop == TERMINAL 
            || s->router_id == msg->intm_rtr_id
            || (routing == PROG_ADAPTIVE_LEGACY && do_chan_selection))
    {
        if(adap_chan >= 0) //
            select_chan = adap_chan;
        else
        {
            /* Only for non-minimal routes, direct connections are preferred
            * (global ports) */
            if(get_direct_con)
            {
                if(s->connMan.get_connections_to_group(dest_group_id).size() > 1) //NTODO - this looks like maybe it should be >= 1
                    select_chan = find_chan_legacy(s->router_id, dest_group_id, s->params->num_routers);
                else
                {
                    printf("Trying to find channel to group %d from router %d in group %d\n", dest_group_id, s->router_id, my_grp_id);
                    assert(select_chan >= 0);
                }
            }
            else
            {
                (*rng_counter)++;
                select_chan = tw_rand_integer(lp->rng, 0, connectionList[my_grp_id][dest_group_id].size() - 1);
            }
        }
        dest_lp = connectionList[my_grp_id][dest_group_id][select_chan];
        //printf("\n my grp %d dest router %d dest_lp %d rid %d chunk id %d", my_grp_id, dest_router_id, dest_lp, s->router_id, msg->chunk_id);
        msg->saved_src_dest = dest_lp;
    }

    if(s->router_id == msg->saved_src_dest)
    {
        vector< Connection > conns_to_dest_group = s->connMan.get_connections_to_group(dest_group_id);
        (*rng_counter)++;
        select_chan = tw_rand_integer(lp->rng, 0, conns_to_dest_group.size() - 1);
        dest_lp = conns_to_dest_group[select_chan].dest_gid;
    }
    else
    {
        /* Connection within the group */
        int dests = msg->saved_src_dest; //trimmed down from old as the old had a lot of superflouous code to poll randomly from a vector of one.

        /* If there is a direct connection */
        dest_lp = dests;
    }

#if DEBUG == 1
    if(msg->packet_ID == LLU(TRACK_PKT) && msg->src_terminal_id == T_ID)
        printf("\n Next stop is %d ", dest_lp);
#endif
    codes_mapping_get_lp_id(lp_group_name, LP_CONFIG_NM_ROUT, s->anno, 0, dest_lp / num_routers_per_mgrp,
        dest_lp % num_routers_per_mgrp, &router_dest_id);


   return router_dest_id;
}

static int do_global_adaptive_routing_legacy(router_state *s, tw_lp *lp, terminal_dally_message *msg, tw_bf *bf, int dest_router_id, int intm_id_a, int intm_id_b, short* rng_counter)
{
    int next_chan = -1;
    // decide which routing to take
    // get the queue occupancy of both the minimal and non-minimal output ports 

    bool local_min = false;
    int num_routers = s->params->num_routers;
    int dest_grp_id = dest_router_id / num_routers;
    int intm_grp_id_a = intm_id_a / num_routers;
    int intm_grp_id_b = intm_id_b / num_routers;
    
    assert(intm_grp_id_a >= 0 && intm_grp_id_b >=0);

    int my_grp_id = s->router_id / num_routers;

    int num_min_chans;
    vector<int> direct_intra;
    if(my_grp_id == dest_grp_id)
    {
        local_min = true;
        direct_intra.push_back(dest_router_id); //Shortened from original but equivalent
        num_min_chans = direct_intra.size();
    }
    else
    {
        num_min_chans = connectionList[my_grp_id][dest_grp_id].size();
    }
    int num_nonmin_chans_a = connectionList[my_grp_id][intm_grp_id_a].size();
    int num_nonmin_chans_b = connectionList[my_grp_id][intm_grp_id_b].size();
    int min_chan_a = -1, min_chan_b = -1, nonmin_chan_a = -1, nonmin_chan_b = -1;
    int min_rtr_a, min_rtr_b, nonmin_rtr_a, nonmin_rtr_b;
    vector<int> dest_rtr_as, dest_rtr_bs;
    int min_port_a, min_port_b, nonmin_port_a, nonmin_port_b;
    tw_lpid min_rtr_a_id, min_rtr_b_id, nonmin_rtr_a_id, nonmin_rtr_b_id;
    bool noIntraA, noIntraB;



    /* two possible routes to minimal destination */
    (*rng_counter) += 2;
    min_chan_a = tw_rand_integer(lp->rng, 0, num_min_chans - 1);
    min_chan_b = tw_rand_integer(lp->rng, 0, num_min_chans - 1);

    if(min_chan_a == min_chan_b && num_min_chans > 1)
        min_chan_b = (min_chan_a + 1) % num_min_chans;

    int chana1 = 0;

    assert(min_chan_a >= 0);
    if(!local_min)
    {
        min_rtr_a = connectionList[my_grp_id][dest_grp_id][min_chan_a];
        noIntraA = false;
        if(min_rtr_a == s->router_id) {
            noIntraA = true;
            min_rtr_a = s->connMan.get_connections_to_group(dest_grp_id)[0].dest_gid;
        }
        if(num_min_chans > 1) {
            assert(min_chan_b >= 0);
            noIntraB = false;
            min_rtr_b = connectionList[my_grp_id][dest_grp_id][min_chan_b];
        
            if(min_rtr_b == s->router_id) {
                noIntraB = true;
                min_rtr_b = s->connMan.get_connections_to_group(dest_grp_id)[0].dest_gid;
            }
        }

        dest_rtr_as.push_back(min_rtr_a); //shortened from original but equivalent
    }
    else
    {
        noIntraA = true;
        noIntraB = true;

        assert(direct_intra.size() > 0);
        min_rtr_a = direct_intra[min_chan_a]; 
        dest_rtr_as.push_back(min_rtr_a);

        if(num_min_chans > 1)
            min_rtr_b = direct_intra[min_chan_b];
    }
        int dest_rtr_b_sel;
    (*rng_counter)++;
    int dest_rtr_a_sel = tw_rand_integer(lp->rng, 0, dest_rtr_as.size() - 1);

    codes_mapping_get_lp_id(lp_group_name, LP_CONFIG_NM_ROUT, s->anno, 0, dest_rtr_as[dest_rtr_a_sel] / num_routers_per_mgrp,
            dest_rtr_as[dest_rtr_a_sel] % num_routers_per_mgrp, &min_rtr_a_id); 

    min_port_a = get_output_port_legacy(s, msg, lp, bf, min_rtr_a_id, rng_counter);

    if(num_min_chans > 1)
    {
        dest_rtr_bs.push_back(min_rtr_b); //shortened but equivalent

        (*rng_counter)++;
        dest_rtr_b_sel = tw_rand_integer(lp->rng, 0, dest_rtr_bs.size() - 1);
        codes_mapping_get_lp_id(lp_group_name, LP_CONFIG_NM_ROUT, s->anno, 0, dest_rtr_bs[dest_rtr_b_sel] / num_routers_per_mgrp,
            dest_rtr_bs[dest_rtr_b_sel] % num_routers_per_mgrp, &min_rtr_b_id); 
        min_port_b = get_output_port_legacy(s, msg, lp, bf, min_rtr_b_id, rng_counter);
    }

    /* if a direct global channel exists for non-minimal route in the source group then give a priority to that. */
    if(msg->my_l_hop == max_lvc_src_g)
    {
        assert(routing == PROG_ADAPTIVE_LEGACY);
        nonmin_chan_a = find_chan_legacy(s->router_id, intm_grp_id_a, num_routers);
        nonmin_chan_b = find_chan_legacy(s->router_id, intm_grp_id_b, num_routers);
        assert(nonmin_chan_a >= 0 && nonmin_chan_b >= 0);
    }
    /* two possible nonminimal routes */
    (*rng_counter) += 2;
    int rand_a = tw_rand_integer(lp->rng, 0, num_nonmin_chans_a - 1);
    int rand_b = tw_rand_integer(lp->rng, 0, num_nonmin_chans_b - 1);

    noIntraA = false;
    if(nonmin_chan_a != -1) {
        /* TODO: For a 2-D dragonfly, this can be more than one link. */
        noIntraA = true;
        nonmin_rtr_a = s->connMan.get_connections_to_group(intm_grp_id_a)[0].dest_gid;
    }
    else
    {
        assert(rand_a >= 0);
        nonmin_chan_a = rand_a;
        nonmin_rtr_a = connectionList[my_grp_id][intm_grp_id_a][rand_a];
        if(nonmin_rtr_a == s->router_id) 
        {
            noIntraA = true;
            nonmin_rtr_a = s->connMan.get_connections_to_group(intm_grp_id_a)[0].dest_gid; //NOTE: This line did not exist in the original but I believe this was what was supposed to happen and it won't work without it, otherwise nonmin_rtr_* is THIS ROUTER
        }

    }
    assert(nonmin_chan_a >= 0);
    
    if(num_nonmin_chans_b > 0) {
        noIntraB = false;
        if(nonmin_chan_b != -1) {
            bf->c26=1;
            noIntraB = true;
            nonmin_rtr_b = s->connMan.get_connections_to_group(intm_grp_id_b)[0].dest_gid;
        }
        else
        {
            assert(rand_b >= 0);
            nonmin_chan_b = rand_b;
            nonmin_rtr_b = connectionList[my_grp_id][intm_grp_id_b][rand_b];
            if(nonmin_rtr_b == s->router_id)
            {
                noIntraB = true;
                nonmin_rtr_b = s->connMan.get_connections_to_group(intm_grp_id_b)[0].dest_gid; //NOTE: This line did not exist in the original but I believe this was what was supposed to happen and it won't work without it, otherwise nonmin_rtr_* is THIS ROUTER
            }
        }
        assert(nonmin_chan_b >= 0);
    }

    dest_rtr_as.clear();
    dest_rtr_as.push_back(nonmin_rtr_a); //shortened from original but equivalent

    (*rng_counter)++;
    dest_rtr_a_sel = tw_rand_integer(lp->rng, 0, dest_rtr_as.size() - 1);
  
    codes_mapping_get_lp_id(lp_group_name, LP_CONFIG_NM_ROUT, s->anno, 0, dest_rtr_as[dest_rtr_a_sel] / num_routers_per_mgrp,
            dest_rtr_as[dest_rtr_a_sel] % num_routers_per_mgrp, &nonmin_rtr_a_id); 
    nonmin_port_a = get_output_port_legacy(s, msg, lp, bf, nonmin_rtr_a_id, rng_counter);

    assert(nonmin_port_a >= 0);

    if(num_nonmin_chans_b > 0)
    {
        dest_rtr_bs.clear();
        dest_rtr_bs.push_back(nonmin_rtr_b); //shortened from original but equvalent

        (*rng_counter)++;
        dest_rtr_b_sel = tw_rand_integer(lp->rng, 0, dest_rtr_bs.size() - 1);
        codes_mapping_get_lp_id(lp_group_name, LP_CONFIG_NM_ROUT, s->anno, 0, dest_rtr_bs[dest_rtr_b_sel] / num_routers_per_mgrp,
            dest_rtr_bs[dest_rtr_b_sel] % num_routers_per_mgrp, &nonmin_rtr_b_id); 
        nonmin_port_b = get_output_port_legacy(s, msg, lp, bf, nonmin_rtr_b_id, rng_counter);
        assert(nonmin_port_b >= 0);
    }

    int min_port_a_count = 0, min_port_b_count = 0;
    int nonmin_port_a_count = 0, nonmin_port_b_count = 0;

    min_port_a_count = get_port_score_legacy(s, min_port_a, 0);
    
    if(num_min_chans > 1)
    {
        min_port_b_count = get_port_score_legacy(s, min_port_b, 0);
    }
    
    nonmin_port_a_count = get_port_score_legacy(s, nonmin_port_a, 1);

    if(num_nonmin_chans_b > 0)
    {
        assert(nonmin_port_b >= 0);
        nonmin_port_b_count += get_port_score_legacy(s, nonmin_port_b, 1);
    }
    int next_min_stop = -1, next_nonmin_stop = -1;
    int next_min_count = -1, next_nonmin_count = -1;

    /* First compare which of the nonminimal ports has less congestions */
    int sel_nonmin = 0;
    if(num_nonmin_chans_b > 0 && nonmin_port_a_count > nonmin_port_b_count)
    {
        next_nonmin_count = nonmin_port_b_count;
        next_nonmin_stop = nonmin_chan_b;
        sel_nonmin = 1;
    }
    else
    {
        next_nonmin_count = nonmin_port_a_count;
        next_nonmin_stop = nonmin_chan_a;
    }
    /* do the same for minimal ports */
    if(num_min_chans > 1 && min_port_a_count > min_port_b_count)
    {
        next_min_count = min_port_b_count;
        next_min_stop = min_chan_b;
    }
    else
    {
        next_min_count = min_port_a_count;
        next_min_stop = min_chan_a;
    }

    /* Now compare the least congested minimal and non-minimal routes */
    if(next_min_count > s->params->adaptive_threshold && next_min_count > next_nonmin_count)
    {
    //      printf("\n Minimal chan %d occupancy %d non-min %d occupancy %d ", next_min_stop, next_min_count, next_nonmin_stop, next_nonmin_count);
        next_chan = next_nonmin_stop;
        msg->path_type = NON_MINIMAL;

        if(sel_nonmin)
            msg->intm_rtr_id = intm_id_b;
        else
            msg->intm_rtr_id = intm_id_a;
    }
    else
    {
        next_chan = next_min_stop;
        msg->path_type = MINIMAL;
    }
    return next_chan;


}

static Connection dfdally_prog_adaptive_legacy_routing(router_state *s, tw_bf *bf, terminal_dally_message *msg, tw_lp *lp, int fdest_router_id)
{
    int num_routers = s->params->num_routers;
    int num_groups = s->params->num_groups;
    int total_routers = s->params->total_routers;
    int next_stop = -1, output_port = -1, output_chan = -1, adap_chan = -1;
    int dest_router_id = codes_mapping_get_lp_relative_id(msg->dest_terminal_lpid, 0, 0) / s->params->num_cn;
    int my_group_id = s->router_id / num_routers;
    int src_grp_id = msg->origin_router_id / num_routers;
    int dest_grp_id = dest_router_id / num_routers;
    int intm_rtr_a, intm_rtr_b;

    short prev_path_type = 0, next_path_type = 0;

    /* for prog-adaptive routing, record the current route of packet */
    int get_direct_con = 0;
    prev_path_type = msg->path_type;

    /* Here we check for local or global adaptive routing. If destination router
    * is in the same group then we do a local adaptive routing by selecting an
    * intermediate router ID which is in the same group. */
    if(src_grp_id != dest_grp_id)
    {
        if(msg->my_l_hop == max_lvc_src_g)
        {
            vector<int> direct_rtrs;
            int dest_idx = tw_rand_integer(lp->rng, 0, num_routers - 1); //local intra id of routers
            msg->num_rngs++;
            vector<int> groups_i_connect_to = s->connMan.get_connected_group_ids();
            vector<int>::iterator it = groups_i_connect_to.begin();
            for (; it != groups_i_connect_to.end(); it++)
            {
                int begin = *it * num_routers; //first router index of this group
                int gid = begin + dest_idx;

                int num_conns_to_group = s->connMan.get_connections_to_group(*it).size();
                for (int i = 0; i < num_conns_to_group; i++) //old version adds same router the order of the number of connections to said group //NTODO this is probably an error in the original
                {
                    direct_rtrs.push_back(gid);
                }
            }
            assert(direct_rtrs.size() > 0);
            int indxa = tw_rand_integer(lp->rng, 0, direct_rtrs.size() - 1);
            int indxb = tw_rand_integer(lp->rng, 0, direct_rtrs.size() - 1);
            msg->num_rngs += 2;

            intm_rtr_a = direct_rtrs[indxa];
            intm_rtr_b = direct_rtrs[indxb];
            assert(intm_rtr_a / num_routers != my_group_id);
            assert(intm_rtr_b / num_routers != my_group_id);
        }
        else
        {
            msg->num_rngs += 2;
            intm_rtr_a = tw_rand_integer(lp->rng, 0, total_routers -1);
            intm_rtr_b = tw_rand_integer(lp->rng, 0, total_routers -1);

            if ((intm_rtr_a/num_routers) == my_group_id)
                intm_rtr_a = (intm_rtr_a + num_routers) % total_routers;
            if ((intm_rtr_b/num_routers) == my_group_id)
                intm_rtr_b = (intm_rtr_b + num_routers) % total_routers;
            
            assert(intm_rtr_a / num_routers != my_group_id);
            assert(intm_rtr_b / num_routers != my_group_id);
        }
    }
    else
    {
        msg->num_rngs++;
        intm_rtr_a = (src_grp_id * num_routers) + 
                        (((s->router_id % num_routers) + 
                        tw_rand_integer(lp->rng, 1, num_routers - 1)) % num_routers);
    }
    
    if(routing == NON_MINIMAL)
        msg->path_type = NON_MINIMAL;
    
    /* progressive adaptive routing is only triggered when packet has to traverse a
    * global channel. It doesn't make sense to use it within a group */
    if(dest_grp_id != src_grp_id && 
            ((msg->last_hop == TERMINAL 
                && routing == ADAPTIVE) 
            || (msg->path_type == MINIMAL 
                && routing == PROG_ADAPTIVE_LEGACY
             // && s->router_id != dest_router_id)))
                && my_group_id == src_grp_id))) 
    {
        adap_chan = do_global_adaptive_routing_legacy(s, lp, msg, bf, dest_router_id, intm_rtr_a, intm_rtr_b, &(msg->num_rngs));
    }
    
    /* If destination router is in the same group then local adaptive routing is
    * triggered */
    if(msg->origin_router_id == dest_router_id)
        msg->path_type = MINIMAL;

    if(dest_grp_id == src_grp_id &&
            dest_router_id != s->router_id &&
            (routing == ADAPTIVE || routing == PROG_ADAPTIVE_LEGACY) 
            && msg->last_hop == TERMINAL) 
    {
            do_local_adaptive_routing_legacy(s, lp, msg, bf, dest_router_id, intm_rtr_a, &(msg->num_rngs));
    }

    next_path_type = msg->path_type;

    if(msg->path_type != MINIMAL && msg->path_type != NON_MINIMAL)
        tw_error(TW_LOC, "\n packet src %d dest %d intm %d src grp %d dest grp %d", s->router_id, dest_router_id, intm_rtr_a, src_grp_id, dest_grp_id);

    assert(msg->path_type == MINIMAL || msg->path_type == NON_MINIMAL);
    
    /* If non-minimal, set the random destination */
    if(msg->last_hop == TERMINAL 
            && msg->path_type == NON_MINIMAL
            && msg->intm_rtr_id == -1)
    {
        msg->is_intm_visited = 0;
        msg->intm_rtr_id = intm_rtr_a;
    }

    if(msg->path_type == NON_MINIMAL)
    {
        /* If non-minimal route has completed, mark the packet.
        * If not, set the non-minimal destination.*/
        if(s->router_id == msg->intm_rtr_id)
        {
            msg->is_intm_visited = 1;
        }
        else if(msg->is_intm_visited == 0)
        {
            //printf("\n Setting intm router id to %d %d", dest_router_id, msg->intm_rtr_id);
            dest_router_id = msg->intm_rtr_id;
        }
    }

    if(msg->path_type == NON_MINIMAL)
    {
        if((msg->my_l_hop == max_lvc_src_g && msg->my_g_hop == min_gvc_src_g)
    || (msg->my_l_hop == max_lvc_intm_g && msg->my_g_hop == min_gvc_intm_g))
        get_direct_con = 1;
    }
    
    /* If the packet route has just changed to non-minimal with prog-adaptive
    * routing, we have to compute the next stop based on that */
    int do_chan_selection = 0;
    if(routing == PROG_ADAPTIVE_LEGACY && prev_path_type != next_path_type && s->group_id == src_grp_id)
        do_chan_selection = 1;
  
    next_stop = get_next_stop_legacy(s, lp, bf, msg, dest_router_id, adap_chan, do_chan_selection, get_direct_con, &(msg->num_rngs));

#if DEBUG == 1
    if(msg->packet_ID == LLU(TRACK_PKT) && msg->src_terminal_id == T_ID)
        printf("\n Packet %llu arrived at router %u next stop %d final stop %d local hops %d global hops %d", msg->packet_ID, s->router_id, next_stop, dest_router_id, msg->my_l_hop, msg->my_g_hop);
#endif

    output_port = get_output_port_legacy(s, msg, lp, bf, next_stop, &(msg->num_rngs)); 
    assert(output_port >= 0);

    Connection return_conn = s->connMan.get_connection_on_port(output_port);

    return return_conn;
}

extern "C" {
/* data structure for dragonfly statistics */
struct model_net_method dragonfly_dally_method =
{
    0,
    dragonfly_dally_configure,
    dragonfly_dally_register,
    dragonfly_dally_packet_event,
    dragonfly_dally_packet_event_rc,
    NULL,
    NULL,
    dragonfly_dally_get_cn_lp_type,
    dragonfly_dally_get_msg_sz,
    dragonfly_dally_report_stats,
    NULL,
    NULL,   
    NULL,//(event_f)dragonfly_dally_sample_fn,    
    NULL,//(revent_f)dragonfly_dally_sample_rc_fn,
    (init_f)dragonfly_dally_sample_init,
    NULL,//(final_f)dragonfly_dally_sample_fin
    custom_dally_dragonfly_register_model_types,
    custom_dally_dragonfly_get_model_types,
    (event_f)dragonfly_dally_terminal_end_sim_notif,
    (revent_f)dragonfly_dally_terminal_end_sim_notif_rc,
    (event_f)dragonfly_dally_terminal_congestion_event,
    (revent_f)dragonfly_dally_terminal_congestion_event_rc,
    (commit_f)dragonfly_dally_terminal_congestion_event_commit,
    &dragonfly_dally_checkpointers[0],
};

struct model_net_method dragonfly_dally_router_method =
{
    0,
    NULL, // handled by dragonfly_configure
    router_dally_register,
    NULL,
    NULL,
    NULL,
    NULL,
    router_dally_get_lp_type,
    dragonfly_dally_get_msg_sz,
    NULL, // not yet supported
    NULL,
    NULL,
    NULL,//(event_f)dragonfly_dally_rsample_fn,
    NULL,//(revent_f)dragonfly_dally_rsample_rc_fn,
    (init_f)dragonfly_dally_rsample_init,
    NULL,//(final_f)dragonfly_dally_rsample_fin
    custom_dally_router_register_model_types,
    custom_dally_dfly_router_get_model_types,
    (event_f)dragonfly_dally_router_end_sim_notif,
    (revent_f)dragonfly_dally_router_end_sim_notif_rc,
    (event_f)dragonfly_dally_router_congestion_event,
    (revent_f)dragonfly_dally_router_congestion_event_rc,
    (commit_f)dragonfly_dally_router_congestion_event_commit,
    &dragonfly_dally_checkpointers[1],
};

// #ifdef ENABLE_CORTEX

// static int dragonfly_dally_get_number_of_compute_nodes(void* topo) {
    
//     const dragonfly_param * params = &all_params[num_params-1];
//     if(!params)
//         return -1.0;

//     return params->total_terminals;
// }

// static int dragonfly_dally_get_number_of_routers(void* topo) {
//     // TODO
//     const dragonfly_param * params = &all_params[num_params-1];
//     if(!params)
//         return -1.0;

//     return params->total_routers;
// }

// static double dragonfly_dally_get_router_link_bandwidth(void* topo, router_id_t r1, router_id_t r2) {
//         // TODO: handle this function for multiple cables between the routers.
//         // Right now it returns the bandwidth of a single cable only. 
// 	// Given two router ids r1 and r2, this function should return the bandwidth (double)
// 	// of the link between the two routers, or 0 of such a link does not exist in the topology.
// 	// The function should return -1 if one of the router id is invalid.
//     const dragonfly_param * params = &all_params[num_params-1];
//     if(!params)
//         return -1.0;

//     if(r1 > params->total_routers || r2 > params->total_routers)
//         return -1.0;

//     if(r1 < 0 || r2 < 0)
//         return -1.0;

//     int gid_r1 = r1 / params->num_routers;
//     int gid_r2 = r2 / params->num_routers;

//     if(gid_r1 == gid_r2)
//     {
//         int lid_r1 = r1 % params->num_routers;
//         int lid_r2 = r2 % params->num_routers;

//         /* The connection will be there if the router is in the same row or
//          * same column */
//         int src_row_r1 = lid_r1 / params->num_router_cols;
//         int src_row_r2 = lid_r2 / params->num_router_cols;

//         int src_col_r1 = lid_r1 % params->num_router_cols;
//         int src_col_r2 = lid_r2 % params->num_router_cols;

//         if(src_row_r1 == src_row_r2 || src_col_r1 == src_col_r2)
//             return params->local_bandwidth;
//         else
//             return 0.0;
//     }
//     else
//     {
//         vector<bLink> &curVec = interGroupLinks[r1][gid_r2];
//         vector<bLink>::iterator it = curVec.begin();

//         for(; it != curVec.end(); it++)
//         {
//             bLink bl = *it;
//             if(bl.dest == r2)
//                 return params->global_bandwidth;
//         }
        
//         return 0.0;
//     }
//     return 0.0;
// }

// static double dragonfly_dally_get_compute_node_bandwidth(void* topo, cn_id_t node) {
//         // TODO
// 	// Given the id of a compute node, this function should return the bandwidth of the
// 	// link connecting this compute node to its router.
// 	// The function should return -1 if the compute node id is invalid.
//     const dragonfly_param * params = &all_params[num_params-1];
//     if(!params)
//         return -1.0;
   
//     if(node < 0 || node >= params->total_terminals)
//         return -1.0;
    
//     return params->cn_bandwidth;
// }

// static int dragonfly_dally_get_router_neighbor_count(void* topo, router_id_t r) {
//         // TODO
// 	// Given the id of a router, this function should return the number of routers
// 	// (not compute nodes) connected to it. It should return -1 if the router id
// 	// is not valid.
//     const dragonfly_param * params = &all_params[num_params-1];
//     if(!params)
//         return -1.0;

//     if(r < 0 || r >= params->total_routers)
//         return -1.0;

//     /* Now count the global channels */
//     set<router_id_t> g_neighbors;

//     map< int, vector<bLink> > &curMap = interGroupLinks[r];
//     map< int, vector<bLink> >::iterator it = curMap.begin(); 
//     for(; it != curMap.end(); it++) {   
//         for(int l = 0; l < it->second.size(); l++) {
//             g_neighbors.insert(it->second[l].dest);
//         }
//     }
//     return (params->num_router_cols - 1) + (params->num_router_rows - 1) + g_neighbors.size();
// }

// static void dragonfly_dally_get_router_neighbor_list(void* topo, router_id_t r, router_id_t* neighbors) {
// 	// Given a router id r, this function fills the "neighbors" array with the ids of routers
// 	// directly connected to r. It is assumed that enough memory has been allocated to "neighbors"
// 	// (using get_router_neighbor_count to know the required size).
//     const dragonfly_param * params = &all_params[num_params-1];

//     int gid = r / params->num_routers;
//     int local_rid = r - (gid * params->num_routers);
//     int src_row = local_rid / params->num_router_cols;
//     int src_col = local_rid % params->num_router_cols;

//     /* First the routers in the same row */
//     int i = 0;
//     int offset = 0;
//     while(i < params->num_router_cols)
//     {
//         int neighbor = gid * params->num_routers + (src_row * params->num_router_cols) + i;
//         if(neighbor != r)
//         {
//             neighbors[offset] = neighbor;
//             offset++;
//         }
//         i++;
//     }

//     /* Now the routers in the same column. */
//     offset = 0;
//     i = 0;
//     while(i <  params->num_router_rows)
//     {
//         int neighbor = gid * params->num_routers + src_col + (i * params->num_router_cols);

//         if(neighbor != r)
//         {
//             neighbors[offset+params->num_router_cols-1] = neighbor;
//             offset++;
//         }
//         i++;
//     }
//     int g_offset = params->num_router_cols + params->num_router_rows - 2;
    
//     /* Now fill up global channels */
//     set<router_id_t> g_neighbors;

//     map< int, vector<bLink> > &curMap = interGroupLinks[r];
//     map< int, vector<bLink> >::iterator it = curMap.begin(); 
//     for(; it != curMap.end(); it++) {   
//         for(int l = 0; l < it->second.size(); l++) {
//             g_neighbors.insert(it->second[l].dest);
//         }
//     }
//     /* Now transfer the content of the sets to the array */
//     set<router_id_t>::iterator it_set;
//     int count = 0;

//     for(it_set = g_neighbors.begin(); it_set != g_neighbors.end(); it_set++)
//     {
//         neighbors[g_offset+count] = *it_set;
//         ++count;
//     }
// }

// static int dragonfly_dally_get_router_location(void* topo, router_id_t r, int32_t* location, int size) {
//         // TODO
// 	// Given a router id r, this function should fill the "location" array (of maximum size "size")
// 	// with information providing the location of the router in the topology. In a Dragonfly network,
// 	// for instance, this can be the array [ group_id, router_id ] where group_id is the id of the
// 	// group in which the router is, and router_id is the id of the router inside this group (as opposed
// 	// to "r" which is its global id). For a torus network, this would be the dimensions.
// 	// If the "size" is sufficient to hold the information, the function should return the size 
// 	// effectively used (e.g. 2 in the above example). If however the function did not manage to use
// 	// the provided buffer, it should return -1.
//     const dragonfly_param * params = &all_params[num_params-1];
//     if(!params)
//         return -1;

//     if(r < 0 || r >= params->total_terminals)
//         return -1;

//     if(size < 2)
//         return -1;

//     int rid = r % params->num_routers;
//     int gid = r / params->num_routers;
//     location[0] = gid;
//     location[1] = rid;
//     return 2;
// }

// static int dragonfly_dally_get_compute_node_location(void* topo, cn_id_t node, int32_t* location, int size) {
//         // TODO
// 	// This function does the same as dragonfly_dally_get_router_location but for a compute node instead
// 	// of a router. E.g., for a dragonfly network, the location could be expressed as the array
// 	// [ group_id, router_id, terminal_id ]
//     const dragonfly_param * params = &all_params[num_params-1];
//     if(!params)
//         return -1;

//     if(node < 0 || node >= params->total_terminals)
//         return -1;
  
//     if(size < 3)
//         return -1;

//     int rid = (node / params->num_cn) % params->num_routers;
//     int rid_global = node / params->num_cn;
//     int gid = rid_global / params->num_routers;
//     int lid = node % params->num_cn;
   
//     location[0] = gid;
//     location[1] = rid;
//     location[2] = lid;

//     return 3;
// }

// static router_id_t dragonfly_dally_get_router_from_compute_node(void* topo, cn_id_t node) {
//         // TODO
// 	// Given a node id, this function returns the id of the router connected to the node,
// 	// or -1 if the node id is not valid.
//     const dragonfly_param * params = &all_params[num_params-1];
//     if(!params)
//         return -1;

//     if(node < 0 || node >= params->total_terminals)
//         return -1;
    
//     router_id_t rid = node / params->num_cn;
//     return rid;
// }

// static int dragonfly_dally_get_router_compute_node_count(void* topo, router_id_t r) {
// 	// Given the id of a router, returns the number of compute nodes connected to this
// 	// router, or -1 if the router id is not valid.
//     const dragonfly_param * params = &all_params[num_params-1];
//     if(!params)
//         return -1;

//     if(r < 0 || r >= params->total_routers)
//         return -1;
    
//     return params->num_cn;
// }

// static void dragonfly_dally_get_router_compute_node_list(void* topo, router_id_t r, cn_id_t* nodes) {
//         // TODO: What if there is an invalid router ID?
// 	// Given the id of a router, fills the "nodes" array with the list of ids of compute nodes
// 	// connected to this router. It is assumed that enough memory has been allocated for the
// 	// "nodes" variable to hold all the ids.
//     const dragonfly_param * params = &all_params[num_params-1];

//     for(int i = 0; i < params->num_cn; i++)
//         nodes[i] = r * params->num_cn + i;
// }

// extern "C" {

// cortex_topology dragonfly_dally_cortex_topology = {
// //        .internal = 
// 			NULL,
// //		  .get_number_of_routers          = 
// 			dragonfly_dally_get_number_of_routers,
// //		  .get_number_of_compute_nodes	  = 
// 			dragonfly_dally_get_number_of_compute_nodes,
// //        .get_router_link_bandwidth      = 
// 			dragonfly_dally_get_router_link_bandwidth,
// //        .get_compute_node_bandwidth     = 
// 			dragonfly_dally_get_compute_node_bandwidth,
// //        .get_router_neighbor_count      = 
// 			dragonfly_dally_get_router_neighbor_count,
// //        .get_router_neighbor_list       = 
// 			dragonfly_dally_get_router_neighbor_list,
// //        .get_router_location            = 
// 			dragonfly_dally_get_router_location,
// //        .get_compute_node_location      = 
// 			dragonfly_dally_get_compute_node_location,
// //        .get_router_from_compute_node   = 
// 			dragonfly_dally_get_router_from_compute_node,
// //        .get_router_compute_node_count  = 
// 			dragonfly_dally_get_router_compute_node_count,
// //        .get_router_compute_node_list   = dragonfly_dally_get_router_compute_node_list,
//             dragonfly_dally_get_router_compute_node_list
// };

// }
// #endif

}


/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
