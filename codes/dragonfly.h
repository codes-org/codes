/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#ifndef INC_dragonfly_h
#define INC_dragonfly_h

#include <ross.h>

#include "codes/codes_mapping.h"
#include "codes/codes.h"
#include "codes/model-net.h"
#include "codes/model-net-method.h"

#define CHUNK_SIZE 32.0
#define CREDIT_SIZE 8
#define MEAN_PROCESS 1.0
#define MAX_NAME_LENGTH 256

// debugging parameters
#define TRACK 235221
#define PRINT_ROUTER_TABLE 1
#define DEBUG 1

// arrival rate
static double MEAN_INTERVAL=200.0;
/* radix of a dragonfly router = number of global channels + number of
 * compute node channels + number of local router channels */
static int radix=0;

/* configurable parameters, coming from the codes config file*/
/* number of virtual channels, number of routers comes from the
 * config file, number of compute nodes, global channels and group
 * is calculated from these configurable parameters */
int num_vcs, num_routers, num_cn, num_global_channels, num_groups;

/* configurable parameters, global channel, local channel and 
 * compute node bandwidth */
double global_bandwidth, local_bandwidth, cn_bandwidth;

/*configurable parameters, global virtual channel size, local
 * virtual channel size and compute node channel size */
int global_vc_size, local_vc_size, cn_vc_size;

/* global variables for codes mapping */
char lp_group_name[MAX_NAME_LENGTH], lp_type_name[MAX_NAME_LENGTH];
int mapping_grp_id, mapping_type_id, mapping_rep_id, mapping_offset;

/* sets up the dragonfly initial network parameters like number of dragonfly groups, 
 * number of compute nodes, number of global channels, bandwidth etc. */
static void dragonfly_setup(const void* net_params);

/* reports dragonfly statistics like average packet delay, average number of hops
 * traversed and maximum packet latency */
static void dragonfly_report_stats();

/* dragonfly packet event method called by modelnet, this method triggers the packet
 * generate event of dragonfly and attached remote and local events to the last packet
 * of the message */
static void dragonfly_packet_event(char* category, tw_lpid final_dest_lp, int packet_size, int remote_event_size, const void* remote_event, int self_event_size, const void* self_event, tw_lp *sender, int is_last_pckt);

/* returns dragonfly message size */
static int dragonfly_get_msg_sz(void);

/* reverse handler for dragonfly packet event */
static void dragonfly_packet_event_rc(tw_lp *sender);

/* returns the lp type of dragonfly compute node (terminal) */
static const tw_lptype* dragonfly_get_cn_lp_type(void);

/* returns the lp type of dragonfly router */
static const tw_lptype* dragonfly_get_router_lp_type(void);

/* data structure for dragonfly statistics */
struct model_net_method dragonfly_method =
{
    .method_name = "dragonfly",
    .mn_setup = dragonfly_setup,
    .model_net_method_packet_event = dragonfly_packet_event,
    .model_net_method_packet_event_rc = dragonfly_packet_event_rc,
    .mn_get_lp_type = dragonfly_get_cn_lp_type,
    .mn_get_msg_sz = dragonfly_get_msg_sz,
    .mn_report_stats = dragonfly_report_stats,
};

/* handles terminal and router events like packet generate/send/receive/buffer */
typedef enum event_t event_t;

typedef struct terminal_state terminal_state;
typedef struct terminal_message terminal_message;
typedef struct router_state router_state;

/* dragonfly compute node data structure */
struct terminal_state
{
   unsigned long long packet_counter;

   // Dragonfly specific parameters
   unsigned int router_id;
   unsigned int terminal_id;

   // Each terminal will have an input and output channel with the router
   int* vc_occupancy; // NUM_VC
   int* output_vc_state;
   tw_stime terminal_available_time;
   tw_stime next_credit_available_time;
// Terminal generate, sends and arrival T_SEND, T_ARRIVAL, T_GENERATE
// Router-Router Intra-group sends and receives RR_LSEND, RR_LARRIVE
// Router-Router Inter-group sends and receives RR_GSEND, RR_GARRIVE
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
   MINIMAL,
   NON_MINIMAL,
   ADAPTIVE
};

/* this message is used for both dragonfly compute nodes and routers */
struct terminal_message
{
  /* flit travel start time*/
  tw_stime travel_start_time;
 /* packet ID of the flit  */
  unsigned long long packet_ID;
  /* event type of the flit */
  short  type;
  /* category: comes from codes */
  char category[MAX_NAME_LENGTH];
  /* final destination LP ID, this comes from codes can be a server or any other LP type*/
  tw_lpid final_dest_gid;
  /*sending LP ID from CODES, can be a server or any other LP type */
  tw_lpid sender_lp;
 /* destination terminal ID of the dragonfly */
  unsigned int dest_terminal_id;
  /* source terminal ID of the dragonfly */
  unsigned int src_terminal_id;
  /* number of hops traversed by the packet */
  short my_N_hop;
  /* Intermediate LP ID from which this message is coming */
  unsigned int intm_lp_id;
  short old_vc;
  short saved_vc;
  /* last hop of the message, can be a terminal, local router or global router */
  short last_hop;
  // For buffer message
   short vc_index;
   int input_chan;
   int output_chan;
   
   tw_stime saved_available_time;
   tw_stime saved_credit_time;

   int intm_group_id;
   short chunk_id;
   int packet_size;
   int remote_event_size_bytes;
   int local_event_size_bytes;
};

struct router_state
{
   unsigned int router_id;
   unsigned int group_id;
  
   int* global_channel; 
   tw_stime* next_output_available_time;
   tw_stime* next_credit_available_time;
   int* vc_occupancy;
   int* output_vc_state;
};

short routing = MINIMAL;

int minimal_count, nonmin_count;
int adaptive_threshold;
int head_delay;
int num_packets;
int num_chunks;

static tw_stime         dragonfly_total_time = 0;
static tw_stime         dragonfly_max_latency = 0;

int num_vc;
int num_terminal=0, num_router=0;
int total_routers, total_terminals, total_mpi_procs;

static long long       total_hops = 0;
static long long       N_finished_packets = 0;

#endif

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
