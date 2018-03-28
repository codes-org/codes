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

#define DUMP_CONNECTIONS 0
#define CREDIT_SIZE 8
#define DFLY_HASH_TABLE_SIZE 4999

// debugging parameters
#define TRACK -1
#define TRACK_PKT -1
#define TRACK_MSG -1
#define DEBUG 0
#define MAX_STATS 65536

#define LP_CONFIG_NM_TERM (model_net_lp_config_names[DRAGONFLY_PLUS])
#define LP_METHOD_NM_TERM (model_net_method_names[DRAGONFLY_PLUS])
#define LP_CONFIG_NM_ROUT (model_net_lp_config_names[DRAGONFLY_PLUS_ROUTER])
#define LP_METHOD_NM_ROUT (model_net_method_names[DRAGONFLY_PLUS_ROUTER])

using namespace std;
struct Link
{
    int offset;
};
struct bLink
{
    int offset, dest;
};

/*MM: Maintains a list of routers connecting the source and destination groups */
static vector< vector< vector< int > > > connectionList;

static vector< ConnectionManager > connManagerList;

struct IntraGroupLink
{
    int src, dest;
};

struct InterGroupLink
{
    int src, dest;
};

#ifdef ENABLE_CORTEX
/* This structure is defined at the end of the file */
extern "C" {
extern cortex_topology dragonfly_plus_cortex_topology;
}
#endif

static int debug_slot_count = 0;
static long term_ecount, router_ecount, term_rev_ecount, router_rev_ecount;
static long packet_gen = 0, packet_fin = 0;

static double maxd(double a, double b)
{
    return a < b ? b : a;
}

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

    // dfp params start
    int num_level_chans;   // number of channels between levels of the group(?)
    int num_router_spine;  // number of spine routers (top level)
    int num_router_leaf;   // number of leaf routers (bottom level)
    int queue_threshold;   // predefined queue length threshold T before a packet is routed through a lower priority queue
    // dfp params end

    int num_groups;
    int radix;
    int total_routers;
    int total_terminals;
    int num_global_connections;
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

/* handles terminal and router events like packet generate/send/receive/buffer */
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
    int *vc_occupancy;  // NUM_VC
    int num_vcs;
    tw_stime terminal_available_time;
    terminal_plus_message_list **terminal_msgs;
    terminal_plus_message_list **terminal_msgs_tail;
    int in_send_loop;
    struct mn_stats dragonfly_stats_array[CATEGORY_MAX];

    struct rc_stack *st;
    int issueIdle;
    int terminal_length;

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

    tw_stime *last_buf_full;
    tw_stime busy_time;

    tw_stime max_latency;
    tw_stime min_latency;

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
    struct dfly_cn_sample *sample_stat;
    int op_arr_size;
    int max_arr_size;

    /* for logging forward and reverse events */
    long fwd_events;
    long rev_events;
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
} event_t;

/* whether the last hop of a packet was global, local or a terminal */
enum last_hop
{
    GLOBAL = 1,
    LOCAL,
    TERMINAL,
};

typedef enum routing_alg_t
{
    MINIMAL = 1, //will always follow the minimal route from host to host
    NON_MINIMAL_SPINE, //will always route through an intermediate spine in an intermediate group for inter group traffic
    NON_MINIMAL_LEAF, //will always route through an intermediate leaf in an intermediate group for inter group traffic
    ON_THE_FLY_ADAPTIVE, //Choose between Minimal, Nonmin spine, and nonmin leaf at the router level based on own congestion
    FULLY_PROG_ADAPTIVE //OTFA with ARNs
} routing_alg_t;

typedef enum route_scoring_metric_t
{
    ALPHA = 1, //Count queue lengths and pending messages for a port
    BETA //Expected Hops to Destination * (Count queue lengths and pending messages) for a port
} route_scoring_metric_t;

bool isRoutingAdaptive(int alg)
{
    if (alg == ON_THE_FLY_ADAPTIVE || alg == FULLY_PROG_ADAPTIVE)
        return true;
    else
        return false;
}

bool isRoutingMinimal(int alg)
{
    if (alg == MINIMAL)
        return true;
    else
        return false;
}

enum LINK_TYPE
{
    GREEN,
    BLACK,
};

enum router_type
{
    SPINE = 1,
    LEAF
};

struct router_state
{
    unsigned int router_id;
    int group_id;
    int op_arr_size;
    int max_arr_size;

    router_type dfp_router_type;  // Enum to specify whether this router is a spine or a leaf

    ConnectionManager *connMan;

    int *global_channel;

    tw_stime *next_output_available_time;
    tw_stime **last_buf_full;

    tw_stime *busy_time;
    tw_stime *busy_time_sample;

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
};

int dragonfly_plus_get_assigned_router_id(int terminal_id, const dragonfly_plus_param *p);

static short routing = MINIMAL;
static short scoring = ALPHA;

static tw_stime dragonfly_total_time = 0;
static tw_stime dragonfly_max_latency = 0;


static long long total_hops = 0;
static long long N_finished_packets = 0;
static long long total_msg_sz = 0;
static long long N_finished_msgs = 0;
static long long N_finished_chunks = 0;

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
        fprintf(stderr, "Buffer size of local channels not specified, setting to %d\n", p->local_vc_size);
    }

    rc = configuration_get_value_int(&config, "PARAMS", "global_vc_size", anno, &p->global_vc_size);
    if (rc) {
        p->global_vc_size = 2048;
        fprintf(stderr, "Buffer size of global channels not specified, setting to %d\n", p->global_vc_size);
    }

    rc = configuration_get_value_int(&config, "PARAMS", "cn_vc_size", anno, &p->cn_vc_size);
    if (rc) {
        p->cn_vc_size = 1024;
        fprintf(stderr, "Buffer size of compute node channels not specified, setting to %d\n", p->cn_vc_size);
    }

    rc = configuration_get_value_int(&config, "PARAMS", "chunk_size", anno, &p->chunk_size);
    if (rc) {
        p->chunk_size = 512;
        fprintf(stderr, "Chunk size for packets is specified, setting to %d\n", p->chunk_size);
    }

    rc = configuration_get_value_double(&config, "PARAMS", "local_bandwidth", anno, &p->local_bandwidth);
    if (rc) {
        p->local_bandwidth = 5.25;
        fprintf(stderr, "Bandwidth of local channels not specified, setting to %lf\n", p->local_bandwidth);
    }

    rc = configuration_get_value_double(&config, "PARAMS", "global_bandwidth", anno, &p->global_bandwidth);
    if (rc) {
        p->global_bandwidth = 4.7;
        fprintf(stderr, "Bandwidth of global channels not specified, setting to %lf\n", p->global_bandwidth);
    }

    rc = configuration_get_value_double(&config, "PARAMS", "cn_bandwidth", anno, &p->cn_bandwidth);
    if (rc) {
        p->cn_bandwidth = 5.25;
        fprintf(stderr, "Bandwidth of compute node channels not specified, setting to %lf\n",
                p->cn_bandwidth);
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
    else if (strcmp(routing_str, "on-the-fly-adaptive") == 0)
        routing = ON_THE_FLY_ADAPTIVE;
    else if (strcmp(routing_str, "fully-prog-adaptive") == 0)
        routing = FULLY_PROG_ADAPTIVE;
    else {
        fprintf(stderr, "No routing protocol specified, setting to minimal routing\n");
        routing = MINIMAL;
    }

    char scoring_str[MAX_NAME_LENGTH];
    configuration_get_value(&config, "PARAMS", "route_scoring_metric", anno, scoring_str, MAX_NAME_LENGTH);
    if (strcmp(scoring_str, "alpha") == 0)
        scoring = ALPHA;
    else if (strcmp(scoring_str, "beta") == 0)
        scoring = BETA;
    else {
        fprintf(stderr, "No route scoring protocol specified, setting to alpha scoring\n");
        scoring = ALPHA;
    }

    /* MM: This should be 2 for dragonfly plus*/
    p->num_vcs = 2;

    rc = configuration_get_value_int(&config, "PARAMS", "num_groups", anno, &p->num_groups);
    if (rc) {
        printf("Number of groups not specified. Aborting");
        MPI_Abort(MPI_COMM_CODES, 1);
    }
    rc = configuration_get_value_int(&config, "PARAMS", "num_router_spine", anno, &p->num_router_spine);
    if (rc) {
        p->num_router_spine = 1;
    }
    rc = configuration_get_value_int(&config, "PARAMS", "num_router_leaf", anno, &p->num_router_leaf);
    if (rc) {
        p->num_router_leaf = 1;
    }
    rc = configuration_get_value_int(&config, "PARAMS", "num_level_chans", anno, &p->num_level_chans);
    if (rc) {
        printf("\n Number of links connecting chassis not specified, setting to default value 1 ");
        p->num_level_chans = 1;
    }


    p->num_routers = p->num_router_spine + p->num_router_leaf;  // num routers per group
    p->intra_grp_radix = (p->num_routers / 2) * p->num_level_chans;


    rc = configuration_get_value_int(&config, "PARAMS", "num_cns_per_router", anno, &p->num_cn);
    if (rc) {
        printf("\n Number of cns per router not specified, setting to %d ", 4);
        p->num_cn = 4;
    }

    rc = configuration_get_value_int(&config, "PARAMS", "num_global_connections", anno, &p->num_global_connections);
    if (rc) {
        printf("\n Number of global channels per router not specified, setting to 10 ");
        p->num_global_connections = 10;
    }
    p->radix = p->intra_grp_radix + p->num_global_connections +
               p->num_cn;  // TODO this may not be sufficient, radix isn't same for leaf and spine routers
    p->total_routers = p->num_groups * p->num_routers;
    p->total_terminals = (p->num_groups * p->num_router_leaf) * p->num_cn;

    // read intra group connections, store from a router's perspective
    // all links to the same router form a vector
    char intraFile[MAX_NAME_LENGTH];
    configuration_get_value(&config, "PARAMS", "intra-group-connections", anno, intraFile, MAX_NAME_LENGTH);
    if (strlen(intraFile) <= 0) {
        tw_error(TW_LOC, "Intra group connections file not specified. Aborting");
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
        tw_error(TW_LOC, "intra-group file not found ");

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
        tw_error(TW_LOC, "Inter group connections file not specified. Aborting");
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

    if (!myRank) {
        printf("\n Total nodes %d routers %d groups %d routers per group %d radix %d\n",
               p->num_cn * p->num_router_leaf * p->num_groups, p->total_routers, p->num_groups, p->num_routers, p->radix);
    }

    p->cn_delay = bytes_to_ns(p->chunk_size, p->cn_bandwidth);
    p->local_delay = bytes_to_ns(p->chunk_size, p->local_bandwidth);
    p->global_delay = bytes_to_ns(p->chunk_size, p->global_bandwidth);
    p->credit_delay = bytes_to_ns(CREDIT_SIZE, p->local_bandwidth);  // assume 8 bytes packet
}

void dragonfly_plus_configure()
{
    anno_map = codes_mapping_get_lp_anno_map(LP_CONFIG_NM_TERM);
    assert(anno_map);
    num_params = anno_map->num_annos + (anno_map->has_unanno_lp > 0);
    all_params = (dragonfly_plus_param *) malloc(num_params * sizeof(*all_params));

    for (int i = 0; i < anno_map->num_annos; i++) {
        const char *anno = anno_map->annotations[i].ptr;
        dragonfly_read_config(anno, &all_params[i]);
    }
    if (anno_map->has_unanno_lp > 0) {
        dragonfly_read_config(NULL, &all_params[anno_map->num_annos]);
    }
#ifdef ENABLE_CORTEX
    model_net_topology = dragonfly_plus_cortex_topology;
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

    MPI_Reduce(&total_hops, &avg_hops, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_CODES);
    MPI_Reduce(&N_finished_packets, &total_finished_packets, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_CODES);
    MPI_Reduce(&N_finished_msgs, &total_finished_msgs, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_CODES);
    MPI_Reduce(&N_finished_chunks, &total_finished_chunks, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_CODES);
    MPI_Reduce(&total_msg_sz, &final_msg_sz, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_CODES);
    MPI_Reduce(&dragonfly_total_time, &avg_time, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_CODES);
    MPI_Reduce(&dragonfly_max_latency, &max_time, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_CODES);

    MPI_Reduce(&packet_gen, &total_gen, 1, MPI_LONG, MPI_SUM, 0, MPI_COMM_CODES);
    MPI_Reduce(&packet_fin, &total_fin, 1, MPI_LONG, MPI_SUM, 0, MPI_COMM_CODES);
    // if (routing == ADAPTIVE || routing == PROG_ADAPTIVE) {
    //     MPI_Reduce(&minimal_count, &total_minimal_packets, 1, MPI_INT, MPI_SUM, 0, MPI_COMM_CODES);
    //     MPI_Reduce(&nonmin_count, &total_nonmin_packets, 1, MPI_INT, MPI_SUM, 0, MPI_COMM_CODES);
    // }

    /* print statistics */
    if (!g_tw_mynode) {
        printf(
            " Average number of router hops traversed: %f; average chunk latency: %lf us; maximum chunk latency: %lf us; "
            "avg message size: %lf bytes; finished messages: %lld; finished chunks: %lld \n",
            (float) avg_hops / total_finished_chunks, avg_time / (total_finished_chunks * 1000),
            max_time / 1000, (float) final_msg_sz / total_finished_msgs, total_finished_msgs,
            total_finished_chunks);
        // if (routing == ADAPTIVE || routing == PROG_ADAPTIVE)
        //     printf(
        //         "\n ADAPTIVE ROUTING STATS: %d chunks routed minimally %d chunks routed non-minimally "
        //         "completed packets %lld \n",
        //         total_minimal_packets, total_nonmin_packets, total_finished_chunks);

        printf("\n Total packets generated: %ld; finished: %ld \n", total_gen, total_fin);
    }
    return;
}

int dragonfly_plus_get_router_type(int router_id, const dragonfly_plus_param *p)
{
    int num_groups = p->num_groups;
    int num_routers = p->num_routers;
    int num_router_leaf = p->num_router_leaf;

    int group_id = router_id / num_groups;
    int router_local_id = router_id % num_routers;

    if (router_local_id > num_router_leaf)
        return SPINE;
    else
        return LEAF;
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

    int group_id = terminal_id / (num_router_leaf * num_cn);
    int local_router_id = terminal_id % (num_router_leaf * num_cn) / num_router_leaf;

    int router_id = (group_id * num_routers) + local_router_id;
    // printf("Terminal %d: assigned to group %d, local router id %d, global router id %d\n",terminal_id,
    // group_id, local_router_id, router_id);

    return router_id;
}

/* initialize a dragonfly compute node terminal */
void terminal_plus_init(terminal_state *s, tw_lp *lp)
{
    // printf("%d: Terminal Init()\n",lp->gid);
    s->packet_gen = 0;
    s->packet_fin = 0;

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

    s->busy_time = 0.0;

    s->fwd_events = 0;
    s->rev_events = 0;

    rc_stack_create(&s->st);
    s->num_vcs = 1;
    s->vc_occupancy = (int *) malloc(s->num_vcs * sizeof(int));
    s->last_buf_full = (tw_stime *) malloc(s->num_vcs * sizeof(tw_stime));

    for (i = 0; i < s->num_vcs; i++) {
        s->last_buf_full[i] = 0.0;
        s->vc_occupancy[i] = 0;
    }


    s->rank_tbl = NULL;
    s->terminal_msgs =
        (terminal_plus_message_list **) malloc(s->num_vcs * sizeof(terminal_plus_message_list *));
    s->terminal_msgs_tail =
        (terminal_plus_message_list **) malloc(s->num_vcs * sizeof(terminal_plus_message_list *));
    s->terminal_msgs[0] = NULL;
    s->terminal_msgs_tail[0] = NULL;
    s->terminal_length = 0;
    s->in_send_loop = 0;
    s->issueIdle = 0;

    return;
}

/* sets up the router virtual channels, global channels,
 * local channels, compute node channels */
void router_plus_setup(router_state *r, tw_lp *lp)
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

    r->fwd_events = 0;
    r->rev_events = 0;

    // Determine if router is a spine or a leaf
    int intra_group_id = r->router_id % p->num_routers;
    if (intra_group_id >= (p->num_routers / 2)) { //TODO this assumes symmetric spine and leafs
        r->dfp_router_type = SPINE;
        // printf("%lu: %i is a SPINE\n",lp->gid, r->router_id);
    }
    else {
        r->dfp_router_type = LEAF;
        // printf("%lu: %i is a LEAF\n",lp->gid, r->router_id);
    }

    r->connMan = &connManagerList[r->router_id];

    r->global_channel = (int *) malloc(p->num_global_connections * sizeof(int));
    r->next_output_available_time = (tw_stime *) malloc(p->radix * sizeof(tw_stime));
    r->link_traffic = (int64_t *) malloc(p->radix * sizeof(int64_t));
    r->link_traffic_sample = (int64_t *) malloc(p->radix * sizeof(int64_t));

    r->vc_occupancy = (int **) malloc(p->radix * sizeof(int *));
    r->in_send_loop = (int *) malloc(p->radix * sizeof(int));
    r->pending_msgs =
        (terminal_plus_message_list ***) malloc(p->radix * sizeof(terminal_plus_message_list **));
    r->pending_msgs_tail =
        (terminal_plus_message_list ***) malloc(p->radix * sizeof(terminal_plus_message_list **));
    r->queued_msgs =
        (terminal_plus_message_list ***) malloc(p->radix * sizeof(terminal_plus_message_list **));
    r->queued_msgs_tail =
        (terminal_plus_message_list ***) malloc(p->radix * sizeof(terminal_plus_message_list **));
    r->queued_count = (int *) malloc(p->radix * sizeof(int));
    r->last_buf_full = (tw_stime **) malloc(p->radix * sizeof(tw_stime *));
    r->busy_time = (tw_stime *) malloc(p->radix * sizeof(tw_stime));
    r->busy_time_sample = (tw_stime *) malloc(p->radix * sizeof(tw_stime));

    rc_stack_create(&r->st);

    for (int i = 0; i < p->radix; i++) {
        // Set credit & router occupancy
        r->busy_time[i] = 0.0;
        r->busy_time_sample[i] = 0.0;
        r->next_output_available_time[i] = 0;
        r->link_traffic[i] = 0;
        r->link_traffic_sample[i] = 0;
        r->queued_count[i] = 0;
        r->in_send_loop[i] = 0;
        r->vc_occupancy[i] = (int *) malloc(p->num_vcs * sizeof(int));
        r->pending_msgs[i] =
            (terminal_plus_message_list **) malloc(p->num_vcs * sizeof(terminal_plus_message_list *));
        r->last_buf_full[i] = (tw_stime *) malloc(p->num_vcs * sizeof(tw_stime));
        r->pending_msgs_tail[i] =
            (terminal_plus_message_list **) malloc(p->num_vcs * sizeof(terminal_plus_message_list *));
        r->queued_msgs[i] =
            (terminal_plus_message_list **) malloc(p->num_vcs * sizeof(terminal_plus_message_list *));
        r->queued_msgs_tail[i] =
            (terminal_plus_message_list **) malloc(p->num_vcs * sizeof(terminal_plus_message_list *));
        for (int j = 0; j < p->num_vcs; j++) {
            r->last_buf_full[i][j] = 0.0;
            r->vc_occupancy[i][j] = 0;
            r->pending_msgs[i][j] = NULL;
            r->pending_msgs_tail[i][j] = NULL;
            r->queued_msgs[i][j] = NULL;
            r->queued_msgs_tail[i][j] = NULL;
        }
    }

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

/* dragonfly packet event reverse handler */
static void dragonfly_plus_packet_event_rc(tw_lp *sender)
{
    codes_local_latency_reverse(sender);
    return;
}

/*MM: This will also be used as is. This is meant to sent a credit back to the
 * sending router. */
/*When a packet is sent from the current router and a buffer slot becomes available, a credit is sent back to
 * schedule another packet event*/
static void router_credit_send(router_state *s, terminal_plus_message *msg, tw_lp *lp, int sq)
{
    tw_event *buf_e;
    tw_stime ts;
    terminal_plus_message *buf_msg;

    int dest = 0, type = R_BUFFER;
    int is_terminal = 0;

    const dragonfly_plus_param *p = s->params;

    // Notify sender terminal about available buffer space
    if (msg->last_hop == TERMINAL) {
        dest = msg->src_terminal_id;
        type = T_BUFFER;
        is_terminal = 1;
    }
    else if (msg->last_hop == GLOBAL || msg->last_hop == LOCAL) {
        dest = msg->intm_lp_id;
    }
    else
        printf("\n Invalid message type");

    ts = g_tw_lookahead + p->credit_delay + tw_rand_unif(lp->rng);

    if (is_terminal) {
        buf_e = model_net_method_event_new(dest, ts, lp, DRAGONFLY_PLUS, (void **) &buf_msg, NULL);
        buf_msg->magic = terminal_magic_num;
    }
    else {
        buf_e = model_net_method_event_new(dest, ts, lp, DRAGONFLY_PLUS_ROUTER, (void **) &buf_msg, NULL);
        buf_msg->magic = router_magic_num;
    }

    if (sq == -1) {
        buf_msg->vc_index = msg->vc_index;
        buf_msg->output_chan = msg->output_chan;
    }
    else {
        buf_msg->vc_index = msg->saved_vc;
        buf_msg->output_chan = msg->saved_channel;
    }

    buf_msg->type = type;

    tw_event_send(buf_e);
    return;
}

static void packet_generate_rc(terminal_state *s, tw_bf *bf, terminal_plus_message *msg, tw_lp *lp)
{
    s->packet_gen--;
    packet_gen--;

    tw_rand_reverse_unif(lp->rng);

    int num_chunks = msg->packet_size / s->params->chunk_size;
    if (msg->packet_size < s->params->chunk_size)
        num_chunks++;

    int i;
    for (i = 0; i < num_chunks; i++) {
        delete_terminal_plus_message_list(return_tail(s->terminal_msgs, s->terminal_msgs_tail, 0));
        s->terminal_length -= s->params->chunk_size;
    }
    if (bf->c5) {
        codes_local_latency_reverse(lp);
        s->in_send_loop = 0;
    }
    if (bf->c11) {
        s->issueIdle = 0;
        if (bf->c8) {
            s->last_buf_full[0] = msg->saved_busy_time;
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
    packet_gen++;
    s->packet_gen++;

    tw_stime ts, nic_ts;

    assert(lp->gid != msg->dest_terminal_id);
    const dragonfly_plus_param *p = s->params;

    int total_event_size;
    uint64_t num_chunks = msg->packet_size / p->chunk_size;
    double cn_delay = s->params->cn_delay;

    if (msg->packet_size < s->params->chunk_size)
        num_chunks++;

    if (msg->packet_size < s->params->chunk_size)
        cn_delay = bytes_to_ns(msg->packet_size % s->params->chunk_size, s->params->cn_bandwidth);

    nic_ts = g_tw_lookahead + (num_chunks * cn_delay) + tw_rand_unif(lp->rng);

    msg->packet_ID = lp->gid + g_tw_nlp * s->packet_counter;
    msg->my_N_hop = 0;
    msg->my_l_hop = 0;
    msg->my_g_hop = 0;

    // if(msg->dest_terminal_id == TRACK)
    if (msg->packet_ID == LLU(TRACK_PKT))
        printf("\n Packet %llu generated at terminal %d dest %llu size %llu num chunks %llu ", msg->packet_ID,
               s->terminal_id, LLU(msg->dest_terminal_id), LLU(msg->packet_size), LLU(num_chunks));

    for (int i = 0; i < num_chunks; i++) {
        terminal_plus_message_list *cur_chunk =
            (terminal_plus_message_list *) malloc(sizeof(terminal_plus_message_list));
        msg->origin_router_id = s->router_id;
        init_terminal_plus_message_list(cur_chunk, msg);

        if (msg->remote_event_size_bytes + msg->local_event_size_bytes > 0) {
            cur_chunk->event_data =
                (char *) malloc(msg->remote_event_size_bytes + msg->local_event_size_bytes);
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

        cur_chunk->msg.chunk_id = i;
        cur_chunk->msg.origin_router_id = s->router_id;
        append_to_terminal_plus_message_list(s->terminal_msgs, s->terminal_msgs_tail, 0, cur_chunk);
        s->terminal_length += s->params->chunk_size;
    }

    if (s->terminal_length < 2 * s->params->cn_vc_size) {
        model_net_method_idle_event(nic_ts, 0, lp);
    }
    else {
        bf->c11 = 1;
        s->issueIdle = 1;

        if (s->last_buf_full[0] == 0.0) {
            bf->c8 = 1;
            msg->saved_busy_time = s->last_buf_full[0];
            /* TODO: Assumes a single vc from terminal to router */
            s->last_buf_full[0] = tw_now(lp);
        }
    }

    if (s->in_send_loop == 0) {
        bf->c5 = 1;
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

static void packet_send_rc(terminal_state *s, tw_bf *bf, terminal_plus_message *msg, tw_lp *lp)
{
    if (bf->c1) {
        s->in_send_loop = 1;

        if (bf->c10)
            s->last_buf_full[0] = msg->saved_busy_time;

        return;
    }

    tw_rand_reverse_unif(lp->rng);
    s->terminal_available_time = msg->saved_available_time;
    if (bf->c2) {
        codes_local_latency_reverse(lp);
    }

    s->terminal_length += s->params->chunk_size;
    s->packet_counter--;
    s->vc_occupancy[0] -= s->params->chunk_size;

    terminal_plus_message_list *cur_entry = (terminal_plus_message_list *) rc_stack_pop(s->st);

    prepend_to_terminal_plus_message_list(s->terminal_msgs, s->terminal_msgs_tail, 0, cur_entry);
    if (bf->c3) {
        tw_rand_reverse_unif(lp->rng);
    }
    if (bf->c4) {
        s->in_send_loop = 1;
    }
    if (bf->c5) {
        tw_rand_reverse_unif(lp->rng);
        s->issueIdle = 1;
        if (bf->c6) {
            s->busy_time = msg->saved_total_time;
            s->last_buf_full[0] = msg->saved_busy_time;
            s->busy_time_sample = msg->saved_sample_time;
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

    terminal_plus_message_list *cur_entry = s->terminal_msgs[0];

    if (s->vc_occupancy[0] + s->params->chunk_size > s->params->cn_vc_size) {
        if (s->last_buf_full[0] == 0.0) {
            bf->c10 = 1;
            msg->saved_busy_time = s->last_buf_full[0];
            s->last_buf_full[0] = tw_now(lp);
        }
    }

    if (s->vc_occupancy[0] + s->params->chunk_size > s->params->cn_vc_size || cur_entry == NULL) {
        bf->c1 = 1;
        s->in_send_loop = 0;
        return;
    }

    uint64_t num_chunks = cur_entry->msg.packet_size / s->params->chunk_size;
    if (cur_entry->msg.packet_size < s->params->chunk_size)
        num_chunks++;

    tw_stime delay = s->params->cn_delay;
    if ((cur_entry->msg.packet_size < s->params->chunk_size) && (cur_entry->msg.chunk_id == num_chunks - 1))
        delay = bytes_to_ns(cur_entry->msg.packet_size % s->params->chunk_size, s->params->cn_bandwidth);

    msg->saved_available_time = s->terminal_available_time;
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
    m->vc_index = 0;
    m->last_hop = TERMINAL;
    m->magic = router_magic_num;
    m->path_type = -1;
    m->local_event_size_bytes = 0;
    m->intm_rtr_id = -1;
    m->intm_group_id = -1;
    tw_event_send(e);


    if (cur_entry->msg.chunk_id == num_chunks - 1 && (cur_entry->msg.local_event_size_bytes > 0)) {
        bf->c2 = 1;
        tw_stime local_ts = codes_local_latency(lp);
        tw_event *e_new = tw_event_new(cur_entry->msg.sender_lp, local_ts, lp);
        void *m_new = tw_event_data(e_new);
        void *local_event = (char *) cur_entry->event_data + cur_entry->msg.remote_event_size_bytes;
        memcpy(m_new, local_event, cur_entry->msg.local_event_size_bytes);
        tw_event_send(e_new);
    }
    s->packet_counter++;
    s->vc_occupancy[0] += s->params->chunk_size;
    cur_entry = return_head(s->terminal_msgs, s->terminal_msgs_tail, 0);
    rc_stack_push(lp, cur_entry, delete_terminal_plus_message_list, s->st);
    s->terminal_length -= s->params->chunk_size;

    cur_entry = s->terminal_msgs[0];

    /* if there is another packet inline then schedule another send event */
    if (cur_entry != NULL && s->vc_occupancy[0] + s->params->chunk_size <= s->params->cn_vc_size) {
        bf->c3 = 1;
        terminal_plus_message *m_new;
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
        ts += tw_rand_unif(lp->rng);
        model_net_method_idle_event(ts, 0, lp);

        if (s->last_buf_full[0] > 0.0) {
            bf->c6 = 1;
            msg->saved_total_time = s->busy_time;
            msg->saved_busy_time = s->last_buf_full[0];
            msg->saved_sample_time = s->busy_time_sample;

            s->busy_time += (tw_now(lp) - s->last_buf_full[0]);
            s->busy_time_sample += (tw_now(lp) - s->last_buf_full[0]);
            s->last_buf_full[0] = 0.0;
        }
    }
    return;
}

static void packet_arrive_rc(terminal_state *s, tw_bf *bf, terminal_plus_message *msg, tw_lp *lp)
{
    if (bf->c31) {
        s->packet_fin--;
        packet_fin--;
    }
    tw_rand_reverse_unif(lp->rng);
    if (msg->path_type == MINIMAL)
        minimal_count--;
    // if (msg->path_type == NON_MINIMAL)
    //     nonmin_count--;

    N_finished_chunks--;
    s->finished_chunks--;
    s->fin_chunks_sample--;

    total_hops -= msg->my_N_hop;
    s->total_hops -= msg->my_N_hop;
    s->fin_hops_sample -= msg->my_N_hop;
    dragonfly_total_time = msg->saved_total_time;
    s->fin_chunks_time = msg->saved_sample_time;
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
    if (bf->c3) {
        dragonfly_max_latency = msg->saved_available_time;
    }

    if (bf->c22) {
        s->max_latency = msg->saved_available_time;
    }
    if (bf->c7) {
        // assert(!hash_link);
        if (bf->c8)
            tw_rand_reverse_unif(lp->rng);
        N_finished_msgs--;
        s->finished_msgs--;
        total_msg_sz -= msg->total_size;
        s->total_msg_size -= msg->total_size;
        s->data_size_sample -= msg->total_size;

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
static void send_remote_event(terminal_state *s, terminal_plus_message *msg, tw_lp *lp, tw_bf *bf,
                              char *event_data, int remote_event_size)
{
    void *tmp_ptr = model_net_method_get_edata(DRAGONFLY_PLUS, msg);
    // tw_stime ts = g_tw_lookahead + bytes_to_ns(msg->remote_event_size_bytes, (1/s->params->cn_bandwidth));
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
/* packet arrives at the destination terminal */
static void packet_arrive(terminal_state *s, tw_bf *bf, terminal_plus_message *msg, tw_lp *lp)
{
    // NIC aggregation - should this be a separate function?
    // Trigger an event on receiving server

    // printf("Packet Arrived: %d hops\n",msg->my_N_hop);

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

    if (msg->packet_ID == LLU(TRACK_PKT))
        printf("\n Packet %llu arrived at lp %llu hops %d ", msg->packet_ID, LLU(lp->gid), msg->my_N_hop);

    tw_stime ts = g_tw_lookahead + s->params->credit_delay + tw_rand_unif(lp->rng);

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

    /* WE do not allow self messages through dragonfly */
    assert(lp->gid != msg->src_terminal_id);

    uint64_t num_chunks = msg->packet_size / s->params->chunk_size;
    if (msg->packet_size < s->params->chunk_size)
        num_chunks++;

    if (msg->path_type == MINIMAL)
        minimal_count++;

    // if (msg->path_type == NON_MINIMAL)
    //     nonmin_count++;

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

    /* save the total time per LP */
    msg->saved_avg_time = s->total_time;
    s->total_time += (tw_now(lp) - msg->travel_start_time);

    msg->saved_total_time = dragonfly_total_time;
    dragonfly_total_time += tw_now(lp) - msg->travel_start_time;
    total_hops += msg->my_N_hop;
    s->total_hops += msg->my_N_hop;
    s->fin_hops_sample += msg->my_N_hop;

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
        struct dfly_qhash_entry *d_entry = (dfly_qhash_entry *) malloc(sizeof(struct dfly_qhash_entry));
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
        tmp->remote_event_data = (char *) malloc(msg->remote_event_size_bytes);
        assert(tmp->remote_event_data);
        tmp->remote_event_size = msg->remote_event_size_bytes;
        memcpy(tmp->remote_event_data, m_data_src, msg->remote_event_size_bytes);
    }
    if (s->min_latency > tw_now(lp) - msg->travel_start_time) {
        s->min_latency = tw_now(lp) - msg->travel_start_time;
    }
    if (dragonfly_max_latency < tw_now(lp) - msg->travel_start_time) {
        bf->c3 = 1;
        msg->saved_available_time = dragonfly_max_latency;
        dragonfly_max_latency = tw_now(lp) - msg->travel_start_time;
        s->max_latency = tw_now(lp) - msg->travel_start_time;
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

void dragonfly_plus_rsample_init(router_state *s, tw_lp *lp)
{
    (void) lp;
    int i = 0;
    const dragonfly_plus_param *p = s->params;

    assert(p->radix);

    s->max_arr_size = MAX_STATS;
    s->rsamples = (struct dfly_router_sample *) malloc(MAX_STATS * sizeof(struct dfly_router_sample));
    for (; i < s->max_arr_size; i++) {
        s->rsamples[i].busy_time = (tw_stime *) malloc(sizeof(tw_stime) * p->radix);
        s->rsamples[i].link_traffic_sample = (int64_t *) malloc(sizeof(int64_t) * p->radix);
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
            (dfly_router_sample *) malloc((MAX_STATS + s->max_arr_size) * sizeof(struct dfly_router_sample));
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
        sprintf(meta_fname, "dragonfly-plus-router-sampling.meta");

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
        sprintf(rt_fn, "dragonfly-plus-router-sampling-%ld.bin", g_tw_mynode);
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

    s->sample_stat = (dfly_cn_sample *) malloc(MAX_STATS * sizeof(struct dfly_cn_sample));
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
            (dfly_cn_sample *) malloc((MAX_STATS + s->max_arr_size) * sizeof(struct dfly_cn_sample));
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

static void terminal_buf_update_rc(terminal_state *s, tw_bf *bf, terminal_plus_message *msg, tw_lp *lp)
{
    s->vc_occupancy[0] += s->params->chunk_size;
    codes_local_latency_reverse(lp);
    if (bf->c1) {
        s->in_send_loop = 0;
    }

    return;
}
/* update the compute node-router channel buffer */
static void terminal_buf_update(terminal_state *s, tw_bf *bf, terminal_plus_message *msg, tw_lp *lp)
{
    bf->c1 = 0;
    bf->c2 = 0;
    bf->c3 = 0;

    tw_stime ts = codes_local_latency(lp);
    s->vc_occupancy[0] -= s->params->chunk_size;

    if (s->in_send_loop == 0 && s->terminal_msgs[0] != NULL) {
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

        default:
            printf("\n LP %d Terminal message type not supported %d ", (int) lp->gid, msg->type);
            tw_error(TW_LOC, "Msg type not supported");
    }
}

void dragonfly_plus_terminal_final(terminal_state *s, tw_lp *lp)
{
    model_net_print_stats(lp->gid, s->dragonfly_stats_array);

    if (s->terminal_id == 0) {
        char meta_filename[64];
        sprintf(meta_filename, "dragonfly-msg-stats.meta");

        FILE *fp = fopen(meta_filename, "w+");
        fprintf(fp,
                "# Format <LP id> <Terminal ID> <Total Data Size> <Avg packet latency> <# Flits/Packets "
                "finished> <Avg hops> <Busy Time> <Max packet Latency> <Min packet Latency >\n");
    }
    int written = 0;

    written += sprintf(s->output_buf + written, "%llu %u %llu %lf %ld %lf %lf %lf %lf\n", LLU(lp->gid),
                       s->terminal_id, LLU(s->total_msg_size), s->total_time / s->finished_chunks,
                       s->finished_packets, (double) s->total_hops / s->finished_chunks, s->busy_time,
                       s->max_latency, s->min_latency);

    lp_io_write(lp->gid, (char *) "dragonfly-msg-stats", written, s->output_buf);

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
    free(s->global_channel);
    int i, j;
    for (i = 0; i < s->params->radix; i++) {
        for (j = 0; j < s->params->num_vcs; j++) {
            if (s->queued_msgs[i][j] != NULL) {
                printf("[%llu] leftover queued messages %d %d %d\n", LLU(lp->gid), i, j,
                       s->vc_occupancy[i][j]);
            }
            if (s->pending_msgs[i][j] != NULL) {
                printf("[%llu] lefover pending messages %d %d\n", LLU(lp->gid), i, j);
            }
        }
    }

    rc_stack_destroy(s->st);

    /*MM: These statistics will need to be updated for dragonfly plus.
     * Especially the meta file information on router ports still have green
     * and black links. */
    const dragonfly_plus_param *p = s->params;
    int written = 0;
    if (!s->router_id) {
        written = sprintf(s->output_buf, "DRAGONFLY-PLUS STATS OUTPUT NOT IMPLEMENTED\n");
        written =
            sprintf(s->output_buf, "# Format <LP ID> <Group ID> <Router ID> <Busy time per router port(s)>");
    }
    written += sprintf(s->output_buf + written, "\n %llu %d %d", LLU(lp->gid), s->router_id / p->num_routers,
                       s->router_id % p->num_routers);
    for (int d = 0; d < p->radix; d++)
        written += sprintf(s->output_buf + written, " %lf", s->busy_time[d]);

    sprintf(s->output_buf + written, "\n");
    lp_io_write(lp->gid, (char *) "dragonfly-plus-router-stats", written, s->output_buf);

    written = 0;
    if (!s->router_id) {
        written = sprintf(s->output_buf2, "DRAGONFLY-PLUS TRAFFIC OUTPUT NOT IMPLEMENTED\n");
        written =
            sprintf(s->output_buf2, "# Format <LP ID> <Group ID> <Router ID> <Busy time per router port(s)>");
    }
    written += sprintf(s->output_buf2 + written, "\n %llu %d %d", LLU(lp->gid), s->router_id / p->num_routers,
                       s->router_id % p->num_routers);

    for (int d = 0; d < p->radix; d++)
        written += sprintf(s->output_buf2 + written, " %lld", LLD(s->link_traffic[d]));

    lp_io_write(lp->gid, (char *) "dragonfly-plus-router-traffic", written, s->output_buf2);
}

//Returns a vector of possible next stops that follow a minimal route to the destination
static vector< Connection > get_possible_minimal_next_stops(router_state *s,
                                            tw_bf *bf,
                                            terminal_plus_message *msg,
                                            tw_lp *lp,
                                            int fdest_router_id)
{
    int local_router_id = s->router_id;
    int my_group_id = s->router_id / s->params->num_routers;
    int fdest_group_id = fdest_router_id / s->params->num_routers;
    int origin_grp_id = msg->origin_router_id / s->params->num_routers;

    vector< Connection > possible_next_conns = vector< Connection >();

    if (fdest_router_id == local_router_id) {
        assert(s->dfp_router_type == LEAF);
        int term_id = msg->dfp_dest_terminal_id;
        possible_next_conns = s->connMan->get_connections_to_gid(term_id, CONN_TERMINAL);
    }
    else if (s->group_id == fdest_group_id) { //we need to forward to the assigned router for the destination terminal
        if (s->dfp_router_type == LEAF) {
            //we need to send to a spine who can send it back down to the dest leaf in our group
            possible_next_conns = s->connMan->get_connections_by_type(CONN_LOCAL);
        }
        else {
            assert(s->dfp_router_type == SPINE);
            int assigned_router = dragonfly_plus_get_assigned_router_id(msg->dfp_dest_terminal_id, s->params);
            possible_next_conns = s->connMan->get_connections_to_gid(assigned_router, CONN_LOCAL);
        }
    }
    else {
        //Then packet direction is upward to another group
        if (s->dfp_router_type == LEAF) {
            //we need to get a list of connections to spine routers in our group that can connect to the fdest group

            set<int> poss_router_id_set;
            for(int i = 0; i < connectionList[my_group_id][fdest_group_id].size(); i++)
            {
                int poss_router_id = connectionList[my_group_id][fdest_group_id][i];
                if (poss_router_id_set.count(poss_router_id) == 0) { //if we haven't added the connections from poss_router_id yet
                    vector< Connection > conns = s->connMan->get_connections_to_gid(poss_router_id, CONN_LOCAL);
                    possible_next_conns.insert(possible_next_conns.end(), conns.begin(), conns.end());
                    poss_router_id_set.insert(poss_router_id);
                }
            }
        }
        else {
            assert(s->dfp_router_type == SPINE);
            possible_next_conns = s->connMan->get_connections_to_group(fdest_group_id); //gets connections to fdest group
        }
    }
    return possible_next_conns;
}

//Returns a vector of possible next stops that follow a nonminimal route -
static vector< Connection > get_possible_nonminimal_next_stops(router_state *s,
                                            tw_bf *bf,
                                            terminal_plus_message *msg,
                                            tw_lp *lp,
                                            int fdest_router_id)
{
    int local_router_id = s->router_id;
    int my_group_id = s->router_id / s->params->num_routers;
    int fdest_group_id = fdest_router_id / s->params->num_routers;
    int origin_grp_id = msg->origin_router_id / s->params->num_routers;

    vector< Connection > possible_next_conns;

    if (s->group_id == fdest_group_id) {
        return possible_next_conns; //empty because there is only minimal connections within the destination group
    }
    else {
        if(s->dfp_router_type == LEAF) {
            possible_next_conns = s->connMan->get_connections_by_type(CONN_LOCAL); //I am a leaf, my local connections are all to spine routers in my group
        }
        else { //SPINE
            assert(s->dfp_router_type == SPINE);
            bool in_intermediate_group = (my_group_id != origin_grp_id) && (my_group_id != fdest_group_id);

            if (in_intermediate_group) { //then the nonminimal paths are those that go to leafs in my group
                possible_next_conns = s->connMan->get_connections_by_type(CONN_LOCAL);
            }
            else { // Then we have to be in source group - nonminimal paths are those that go to any other group other than destination

                for(int i = 0; i < s->params->num_groups; i++)
                {
                    if (i != fdest_group_id) {
                        vector< Connection > conns_to_group = s->connMan->get_connections_to_group(i);
                        possible_next_conns.insert(possible_next_conns.end(),conns_to_group.begin(),conns_to_group.end());
                    }
                }
            }
        }
    }
    return possible_next_conns;
}

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
        if (next_hops_type == SPINE)
            return 3; //Next Spine -> Spine -> Leaf -> dest_term
        else {
            assert(next_hops_type == LEAF);
            return 4; //Next Leaf -> Spine -> Spine -> Leaf -> dest_term
        }
    }
}

/**
 * Scores a connection based on the metric provided in the function, lower score is better
 */
static int dfp_score_connection(router_state *s, tw_bf *bf, terminal_plus_message *msg, tw_lp *lp, Connection conn)
{
    //Currently just looking at VC Occupancy and message queue length like dragonfly custom
    int score = 0;
    int port = conn.port;

    switch(scoring) {
        case ALPHA:
        {
            for(int k=0; k < s->params->num_vcs; k++)
            {
                score += s->vc_occupancy[port][k];
            }
            score += s->queued_count[port];
            break;
        }
        case BETA:
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
    }
    return score;
}

static vector< Connection > dfp_select_two_connections(router_state *s, tw_bf *bf, terminal_plus_message *msg, tw_lp *lp, vector< Connection > conns)
{
    if(conns.size() < 2) {
        tw_rand_integer(lp->rng,0,2);
        tw_rand_integer(lp->rng,0,2);
        if(conns.size() == 1)
            return conns;
        if(conns.size() == 0)
            return vector< Connection>();
    }


    int rand_sel_1, rand_sel_2;
    vector<int> indices;
    vector<int>::iterator it;

    for(int i = 0; i < conns.size(); i++)
    {
        indices.push_back(i);
    }

    // printf("Selecting 1st from %d\n",indices.size());
    int pi = tw_rand_integer(lp->rng, 0, indices.size()-1);
    rand_sel_1 = indices[pi];
    for(it = indices.begin(); it != indices.end(); it++) //because std::find wasn't working :/
    {
        if (*it == rand_sel_1)
            break;
    }
    indices.erase(it); //erases the number located at pos pi

    // printf("Selecting 2nd from %d\n",indices.size());
    pi = tw_rand_integer(lp->rng, 0, indices.size()-1);
    rand_sel_2 = indices[pi];

    for(it = indices.begin(); it != indices.end(); it++) //because std::find wasn't working :/
    {
        if (*it == rand_sel_2)
            break;
    }
    indices.erase(it); //erases the number located at pos pi

    vector< Connection > selected_conns;
    selected_conns.push_back(conns[rand_sel_1]);
    selected_conns.push_back(conns[rand_sel_2]);
    return selected_conns;
}

static Connection do_dfp_routing(router_state *s,
                                tw_bf *bf,
                                terminal_plus_message *msg,
                                tw_lp *lp,
                                int fdest_router_id)
{

    int local_router_id = s->router_id;
    int my_group_id = s->router_id / s->params->num_routers;
    int fdest_group_id = fdest_router_id / s->params->num_routers;
    int origin_grp_id = msg->origin_router_id / s->params->num_routers;
    bool in_intermediate_group = (my_group_id != origin_grp_id) && (my_group_id != fdest_group_id);


    vector< Connection > poss_min_next_stops = get_possible_minimal_next_stops(s, bf, msg, lp, fdest_router_id);
    vector< Connection > poss_non_min_next_stops = get_possible_nonminimal_next_stops(s, bf, msg, lp, fdest_router_id);

    Connection theConn;

    if (isRoutingAdaptive(routing)) {
        bool choose_minimal = false;
        bool turning_upward = false;

        int num_min_poss = poss_min_next_stops.size();
        int num_nonmin_poss = poss_non_min_next_stops.size();

        if (msg->dfp_upward_channel_flag == 1 || (my_group_id == fdest_group_id))
            choose_minimal = true; //in both of these cases minimal should always be chosen


        vector< Connection > two_minimal_conns = dfp_select_two_connections(s, bf, msg, lp, poss_min_next_stops);
        vector< Connection > two_nonminimal_conns = dfp_select_two_connections(s, bf, msg, lp, poss_non_min_next_stops);
        int num_conns = two_minimal_conns.size() + two_nonminimal_conns.size(); //we selected two from each //TODO maybe make this configurable


        //calculate the best connections for minimal and nonminimal
        int scores[num_conns];
        for(int i = 0; i < num_conns; i++)
        {
            if(i < (two_minimal_conns.size()) )
                scores[i] = dfp_score_connection(s, bf, msg, lp, two_minimal_conns[i]);
            else
                scores[i] = dfp_score_connection(s, bf, msg, lp, two_nonminimal_conns[i-two_minimal_conns.size()]);
        }

        int best_min_score = INT_MAX;
        int best_min_score_index = 0;
        for(int i = 0; i < two_minimal_conns.size(); i++)
        {
            if (scores[i] < best_min_score) {
                best_min_score = scores[i];
                best_min_score_index = i;
            }
        }

        int best_non_min_score = INT_MAX;
        int best_non_min_score_index = 0;
        for(int i = two_minimal_conns.size(); i < num_conns; i++)
        {
            if (scores[i] < best_non_min_score) {
                best_non_min_score = scores[i];
                best_non_min_score_index = i-two_minimal_conns.size();
            }
        }
        //end score calculation

        if (best_min_score <= best_non_min_score) { //ties go to minimal
            choose_minimal = true;
        }

        //intermediate groups have restrictions on how they have to route to avoid livelock
        if (in_intermediate_group) {
            if (s->dfp_router_type == SPINE) { //intermediate spine
                if (choose_minimal)
                    msg->dfp_upward_channel_flag = 1; //only if it chose minimal should this be flipped on
            }
            else {
                assert(s->dfp_router_type == LEAF); //intermediate leaf always sends
                choose_minimal = true;
                msg->dfp_upward_channel_flag = 1;
            }
        }

        if (choose_minimal) {
            theConn = two_minimal_conns[best_min_score_index];
        }
        else {
            theConn = two_nonminimal_conns[best_non_min_score_index];
        }
    }

    else { //routing algorithm is specified in msg->path_type
        bool choose_minimal = false;
        if ( msg->path_type == MINIMAL || msg->dfp_upward_channel_flag == 1 || (my_group_id == fdest_group_id) )
            choose_minimal = true;
        else {
            if (in_intermediate_group) {
                if (s->dfp_router_type == SPINE) { //INTERMEDIATE SPINE
                    if (msg->path_type == NON_MINIMAL_SPINE) {
                        //we then follow minimal
                        choose_minimal = true;
                        msg->dfp_upward_channel_flag = 1;
                    }
                    else {
                        assert(msg->path_type == NON_MINIMAL_LEAF);
                        choose_minimal = false;
                    }
                }
                else { //INTERMEDIATE LEAF
                    choose_minimal = true;
                    msg->dfp_upward_channel_flag = 1;
                }
            }
            else { //must be in source group
                assert(my_group_id == origin_grp_id);
                choose_minimal = false;
            }
        }

        if (choose_minimal == true) {
            assert(poss_min_next_stops.size() > 0);
            int rand_sel = tw_rand_integer(lp->rng, 0, poss_min_next_stops.size()-1);
            theConn = poss_min_next_stops[rand_sel];
        }
        else {
            assert(poss_non_min_next_stops.size() > 0);
            int rand_sel = tw_rand_integer(lp->rng, 0, poss_non_min_next_stops.size()-1);
            theConn =  poss_non_min_next_stops[rand_sel];
        }
    }

    return theConn;
}

static void router_packet_receive_rc(router_state *s, tw_bf *bf, terminal_plus_message *msg, tw_lp *lp)
{
    router_rev_ecount++;
    router_ecount--;

    int output_port = msg->saved_vc;
    int output_chan = msg->saved_channel;

    if( isRoutingAdaptive(routing) ) {
        //we need to reverse 4
        for(int i =0; i < 4; i++)
            tw_rand_reverse_unif(lp->rng);
    }
    else {
        tw_rand_reverse_unif(lp->rng);
    }

    tw_rand_reverse_unif(lp->rng);
    if (bf->c2) {
        tw_rand_reverse_unif(lp->rng);
        terminal_plus_message_list *tail =
            return_tail(s->pending_msgs[output_port], s->pending_msgs_tail[output_port], output_chan);
        delete_terminal_plus_message_list(tail);
        s->vc_occupancy[output_port][output_chan] -= s->params->chunk_size;
        if (bf->c3) {
            codes_local_latency_reverse(lp);
            s->in_send_loop[output_port] = 0;
        }
    }
    if (bf->c4) {
        if (bf->c22) {
            s->last_buf_full[output_port][output_chan] = msg->saved_busy_time;
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
    router_ecount++;

    tw_stime ts;

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
        (terminal_plus_message_list *) malloc(sizeof(terminal_plus_message_list));
    init_terminal_plus_message_list(cur_chunk, msg);

    cur_chunk->msg.path_type = MINIMAL;
    if ( !isRoutingMinimal(routing) )
    {
        if (!cur_chunk->msg.dfp_upward_channel_flag)
        {
            cur_chunk->msg.path_type = routing; //if we're on an upward channel then we follow minimal routing
        }
    }

    Connection next_stop_conn = do_dfp_routing(s, bf, &(cur_chunk->msg), lp, dest_router_id);

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

    if(cur_chunk->msg.path_type == MINIMAL)
    {
        output_chan = 0;
    }
    if(cur_chunk->msg.dfp_upward_channel_flag)
    {
        output_chan = 1;
    }

    ConnectionType port_type = s->connMan.get_port_type(output_port);
    int max_vc_size = 0;
    if (port_type == CONN_GLOBAL) {
        max_vc_size = s->params->global_vc_size;
    }
    else if (port_type == CONN_LOCAL) {
        max_vc_size = s->params->local_vc_size;
    }
    else {
        assert(port_type == CONN_TERMINAL)
        max_vc_size = s->param->cn_vc_size;
    }

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

    if (msg->remote_event_size_bytes > 0) {
        void *m_data_src = model_net_method_get_edata(DRAGONFLY_PLUS_ROUTER, msg);
        cur_chunk->event_data = (char *) malloc(msg->remote_event_size_bytes);
        memcpy(cur_chunk->event_data, m_data_src, msg->remote_event_size_bytes);
    }

    if (s->vc_occupancy[output_port][output_chan] + s->params->chunk_size <= max_vc_size) {
        bf->c2 = 1;
        router_credit_send(s, msg, lp, -1);
        append_to_terminal_plus_message_list(s->pending_msgs[output_port], s->pending_msgs_tail[output_port],
                                             output_chan, cur_chunk);
        s->vc_occupancy[output_port][output_chan] += s->params->chunk_size;
        if (s->in_send_loop[output_port] == 0) {
            bf->c3 = 1;
            terminal_plus_message *m;
            ts = codes_local_latency(lp);
            tw_event *e =
                model_net_method_event_new(lp->gid, ts, lp, DRAGONFLY_PLUS_ROUTER, (void **) &m, NULL);
            m->type = R_SEND;
            m->magic = router_magic_num;
            m->vc_index = output_port;
            m->dfp_upward_channel_flag = msg->dfp_upward_channel_flag;

            tw_event_send(e);
            s->in_send_loop[output_port] = 1;
        }
    }
    else {
        bf->c4 = 1;
        cur_chunk->msg.saved_vc = msg->vc_index;
        cur_chunk->msg.saved_channel = msg->output_chan;
        append_to_terminal_plus_message_list(s->queued_msgs[output_port], s->queued_msgs_tail[output_port],
                                             output_chan, cur_chunk);
        s->queued_count[output_port] += s->params->chunk_size;

        /* a check for pending msgs is non-empty then we dont set anything. If
         * that is empty then we check if last_buf_full is set or not. If already
         * set then we don't overwrite it. If two packets arrive next to each other
         * then the first person should be setting it. */
        if (s->pending_msgs[output_port][output_chan] == NULL &&
            s->last_buf_full[output_port][output_chan] == 0.0) {
            bf->c22 = 1;
            msg->saved_busy_time = s->last_buf_full[output_port][output_chan];
            s->last_buf_full[output_port][output_chan] = tw_now(lp);
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

    int output_port = msg->saved_vc;
    int output_chan = msg->saved_channel;
    if (bf->c1) {
        s->in_send_loop[output_port] = 1;
        return;
    }

    tw_rand_reverse_unif(lp->rng);

    terminal_plus_message_list *cur_entry = (terminal_plus_message_list *) rc_stack_pop(s->st);
    assert(cur_entry);

    if (bf->c11) {
        s->link_traffic[output_port] -= cur_entry->msg.packet_size % s->params->chunk_size;
        s->link_traffic_sample[output_port] -= cur_entry->msg.packet_size % s->params->chunk_size;
    }
    if (bf->c12) {
        s->link_traffic[output_port] -= s->params->chunk_size;
        s->link_traffic_sample[output_port] -= s->params->chunk_size;
    }
    s->next_output_available_time[output_port] = msg->saved_available_time;

    prepend_to_terminal_plus_message_list(s->pending_msgs[output_port], s->pending_msgs_tail[output_port],
                                          output_chan, cur_entry);

    if (bf->c3) {
        tw_rand_reverse_unif(lp->rng);
    }

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

    int output_chan = s->params->num_vcs - 1;
    for (int k = s->params->num_vcs - 1; k >= 0; k--) {
        cur_entry = s->pending_msgs[output_port][k];
        if (cur_entry != NULL) {
            output_chan = k;
            break;
        }
    }

    msg->saved_vc = output_port;
    msg->saved_channel = output_chan;

    if (cur_entry == NULL) {
        bf->c1 = 1;
        s->in_send_loop[output_port] = 0;
        return;
    }

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
    if (msg->packet_size < s->params->chunk_size)
        num_chunks++;

    double bytetime = delay;

    if (cur_entry->msg.packet_size == 0)
        bytetime = bytes_to_ns(CREDIT_SIZE, bandwidth);

    if ((cur_entry->msg.packet_size < s->params->chunk_size) && (cur_entry->msg.chunk_id == num_chunks - 1))
        bytetime = bytes_to_ns(cur_entry->msg.packet_size % s->params->chunk_size, bandwidth);

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

    if ((cur_entry->msg.packet_size % s->params->chunk_size) && (cur_entry->msg.chunk_id == num_chunks - 1)) {
        bf->c11 = 1;
        s->link_traffic[output_port] += (cur_entry->msg.packet_size % s->params->chunk_size);
        s->link_traffic_sample[output_port] += (cur_entry->msg.packet_size % s->params->chunk_size);
    }
    else {
        bf->c12 = 1;
        s->link_traffic[output_port] += s->params->chunk_size;
        s->link_traffic_sample[output_port] += s->params->chunk_size;
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

    s->next_output_available_time[output_port] -= s->params->router_delay;
    ts -= s->params->router_delay;

    cur_entry = NULL;

    for (int k = s->params->num_vcs - 1; k >= 0; k--) {
        cur_entry = s->pending_msgs[output_port][k];
        if (cur_entry != NULL)
            break;
    }
    if (cur_entry != NULL) {
        bf->c3 = 1;
        terminal_plus_message *m_new;
        ts += g_tw_lookahead + tw_rand_unif(lp->rng);
        e = model_net_method_event_new(lp->gid, ts, lp, DRAGONFLY_PLUS_ROUTER, (void **) &m_new, NULL);
        m_new->type = R_SEND;
        m_new->magic = router_magic_num;
        m_new->vc_index = output_port;
        tw_event_send(e);
    }
    else {
        bf->c4 = 1;
        s->in_send_loop[output_port] = 0;
    }
    return;
}

static void router_buf_update_rc(router_state *s, tw_bf *bf, terminal_plus_message *msg, tw_lp *lp)
{
    int indx = msg->vc_index;
    int output_chan = msg->output_chan;
    s->vc_occupancy[indx][output_chan] += s->params->chunk_size;
    if (bf->c3) {
        s->busy_time[indx] = msg->saved_rcv_time;
        s->busy_time_sample[indx] = msg->saved_sample_time;
        s->last_buf_full[indx][output_chan] = msg->saved_busy_time;
    }
    if (bf->c1) {
        terminal_plus_message_list *head =
            return_tail(s->pending_msgs[indx], s->pending_msgs_tail[indx], output_chan);
        tw_rand_reverse_unif(lp->rng);
        prepend_to_terminal_plus_message_list(s->queued_msgs[indx], s->queued_msgs_tail[indx], output_chan,
                                              head);
        s->vc_occupancy[indx][output_chan] -= s->params->chunk_size;
        s->queued_count[indx] += s->params->chunk_size;
    }
    if (bf->c2) {
        codes_local_latency_reverse(lp);
        s->in_send_loop[indx] = 0;
    }
}
/* Update the buffer space associated with this router LP */
static void router_buf_update(router_state *s, tw_bf *bf, terminal_plus_message *msg, tw_lp *lp)
{
    int indx = msg->vc_index;
    int output_chan = msg->output_chan;
    s->vc_occupancy[indx][output_chan] -= s->params->chunk_size;

    if (s->last_buf_full[indx][output_chan] > 0.0) {
        bf->c3 = 1;
        msg->saved_rcv_time = s->busy_time[indx];
        msg->saved_busy_time = s->last_buf_full[indx][output_chan];
        msg->saved_sample_time = s->busy_time_sample[indx];
        s->busy_time[indx] += (tw_now(lp) - s->last_buf_full[indx][output_chan]);
        s->busy_time_sample[indx] += (tw_now(lp) - s->last_buf_full[indx][output_chan]);
        s->last_buf_full[indx][output_chan] = 0.0;
    }
    if (s->queued_msgs[indx][output_chan] != NULL) {
        bf->c1 = 1;
        terminal_plus_message_list *head =
            return_head(s->queued_msgs[indx], s->queued_msgs_tail[indx], output_chan);
        router_credit_send(s, &head->msg, lp, 1);
        append_to_terminal_plus_message_list(s->pending_msgs[indx], s->pending_msgs_tail[indx], output_chan,
                                             head);
        s->vc_occupancy[indx][output_chan] += s->params->chunk_size;
        s->queued_count[indx] -= s->params->chunk_size;
    }
    if (s->in_send_loop[indx] == 0 && s->pending_msgs[indx][output_chan] != NULL) {
        bf->c2 = 1;
        terminal_plus_message *m;
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
        (commit_f) NULL,
        (final_f) dragonfly_plus_terminal_final,
        (map_f) codes_mapping,
        sizeof(terminal_state),
    },
    {
        (init_f) router_plus_setup,
        (pre_run_f) NULL,
        (event_f) router_plus_event,
        (revent_f) router_plus_rc_event_handler,
        (commit_f) NULL,
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
    NULL,  //(event_f)dragonfly_plus_sample_fn,
    NULL,  //(revent_f)dragonfly_plus_sample_rc_fn,
    (init_f) dragonfly_plus_sample_init,
    NULL,  //(final_f)dragonfly_plus_sample_fin
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
    NULL,  //(event_f)dragonfly_plus_rsample_fn,
    NULL,  //(revent_f)dragonfly_plus_rsample_rc_fn,
    (init_f) dragonfly_plus_rsample_init,
    NULL,  //(final_f)dragonfly_plus_rsample_fin
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
