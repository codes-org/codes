/*
 * * Copyright (C) 2011, University of Chicago
 * *
 * * See COPYRIGHT notice in top-level directory.
 * */

#ifndef INC_torus_h
#define INC_torus_h

#include <ross.h>
#include <assert.h>

#include "codes/codes_mapping.h"
#include "codes/codes.h"
#include "codes/model-net.h"
#include "codes/model-net-method.h"

#define CHUNK_SIZE 32
#define DEBUG 1
#define MEAN_INTERVAL 100
#define CATEGORY_NAME_MAX 16
#define MAX_NAME_LENGTH 256
#define TRACE -1 

/* Torus network model implementation of codes, implements the modelnet API */

// Total number of nodes in torus, calculate in main
int N_nodes = 1;
/* Link bandwidth for each torus link, configurable from the config file */
double link_bandwidth;
/* buffer size of each torus link, configurable */
int buffer_size;
/* number of virtual channels for each torus link, configurable */
int num_vc;
/* number of torus dimensions, configurable */
int n_dims;
/* length of each torus dimension, configurable */
int * dim_length;
/* factor, used in torus coordinate calculation */
int * factor;
/* half length of each dimension, used in torus coordinates calculation */
int * half_length;

/* codes mapping group name, lp type name */
char grp_name[MAX_NAME_LENGTH], type_name[MAX_NAME_LENGTH];
/* codes mapping group id, lp type id, repetition id and offset */
int grp_id, lp_type_id, rep_id, offset;

/* nodes event enumeration, packet generation, send, receive and buffer event types */
typedef enum nodes_event_t nodes_event_t;
/* state of a torus compute node (come up with a better name instead of compute node?)*/
typedef struct nodes_state nodes_state;
/* torus message--- can be a packet or a flit */
typedef struct nodes_message nodes_message;

/* Issues a torus packet event call */
static void torus_packet_event(
		       char* category,
		       tw_lpid final_dest_lp,
		       int packet_size,
		       int remote_event_size,
		       const void* remote_event,
		       int self_event_size,
		       const void* self_event,
		       tw_lp *sender,
		       int is_last_pckt);
/* torus reverse event handler */
static void torus_packet_event_rc(tw_lp *sender);
/* torus setup function, sets up configurable parameters like torus dimensions,
 * length of each dimension, channel bandwidth, buffer size etc. */
static void torus_setup(const void* net_params);
/* returns size of the torus message */
static int torus_get_msg_sz(void);
/* returns torus lp type */
static const tw_lptype* torus_get_lp_type(void);
/* reports torus statistics */
static void torus_report_stats(void);

/* data structure for torus statistics */
struct model_net_method torus_method =
{
   .method_name = "torus",
   .mn_setup = torus_setup,
   .model_net_method_packet_event = torus_packet_event,
   .model_net_method_packet_event_rc = torus_packet_event_rc,
   .mn_get_lp_type = torus_get_lp_type,
   .mn_get_msg_sz = torus_get_msg_sz,
   .mn_report_stats = torus_report_stats,
};

/* event type of each torus message, can be packet generate, flit arrival, flit send or credit */
enum nodes_event_t
{
  GENERATE = 1,
  ARRIVAL, 
  SEND,
  CREDIT,
};

/* state of a torus node */
struct nodes_state
{
  /* counts the number of packets sent from this compute node */
  unsigned long long packet_counter;            
  /* availability time of each torus link */
  tw_stime** next_link_available_time; 
  /* availability of each torus credit link */
  tw_stime** next_credit_available_time;
  /* next flit generate time */
  tw_stime** next_flit_generate_time;
  /* buffer size for each torus virtual channel */
  int** buffer;
  /* coordinates of the current torus node */
  int* dim_position;
  /* neighbor LP ids for this torus node */
  int* neighbour_minus_lpID;
  int* neighbour_plus_lpID;
};

struct nodes_message
{
  /* category: comes from codes message */
  char category[CATEGORY_NAME_MAX];
  /* time the packet was generated */
  tw_stime travel_start_time;
  /* for reverse event computation*/
  tw_stime saved_available_time;

  /* packet ID */
  unsigned long long packet_ID;
  /* event type of the message */
  nodes_event_t	 type;

  /* for reverse computation */
  int saved_src_dim;
  int saved_src_dir;

  /* coordinates of the destination torus nodes */
  int* dest;

  /* final destination LP ID, comes from codes, can be a server or any other I/O LP type */
  tw_lpid final_dest_gid;
  /* destination torus node of the message */
  tw_lpid dest_lp;
  /* LP ID of the sender, comes from codes, can be a server or any other I/O LP type */
  tw_lpid sender_lp;

  /* number of hops traversed by the packet */
  int my_N_hop;
  /* source dimension of the message */
  int source_dim;
  /* source direction of the message */
  int source_direction;
  /* next torus hop that the packet will traverse */
  int next_stop;
  /* size of the torus packet */
  int packet_size;
  /* chunk id of the flit (distinguishes flits) */
  short chunk_id;

  /* for codes local and remote events, only carried by the last packet of the message */
  int local_event_size_bytes;
  int remote_event_size_bytes;
};

/* for calculating torus model statistics, average and maximum travel time of a packet */
tw_stime         average_travel_time = 0;
tw_stime         total_time = 0;
tw_stime         max_latency = 0;

/* indicates delays calculated through the bandwidth calculation of the torus link */
float head_delay=0.0;
float credit_delay = 0.0;

/* number of finished packets on each PE */
static long long       N_finished_packets = 0;
/* total number of hops traversed by a message on each PE */
static long long       total_hops = 0;

/* number of chunks/flits in each torus packet, calculated through the size of each flit (32 bytes by default) */
int num_chunks;

#endif
