#ifndef FATTREE_H
#define FATTREE_H

#include <ross.h>

/* Functions used for ROSS event tracing */
extern void fattree_register_evtrace();

/* Global variable for modelnet output directory name */
extern char *modelnet_stats_dir;

typedef struct fattree_message fattree_message;

/* this message is used for both fattree compute nodes and routers */
struct fattree_message
{
  /* magic number */
  int magic;
  /* flit travel start time*/
  tw_stime travel_start_time;
 /* packet ID of the flit  */
  unsigned long long packet_ID;
  /* event type of the flit */
  short  type;
  /* category: comes from codes */
  char category[CATEGORY_NAME_MAX];
  /* final destination LP ID, this comes from codes can be a server or any other LP type*/
  tw_lpid final_dest_gid;
  /*sending LP ID from CODES, can be a server or any other LP type */
  tw_lpid sender_lp;
  tw_lpid sender_mn_lp; // source modelnet id
 /* destination terminal ID of the message */
//  int dest_num; replaced with dest_terminal_id
  tw_lpid dest_terminal_id;
  /* source terminal ID of the fattree */
  unsigned int src_terminal_id;
  /* Intermediate LP ID from which this message is coming */
  unsigned int intm_lp_id;
  short saved_vc;
  short saved_off;
  int last_hop;
  int intm_id; //to find which port I connect to sender with

  /* message originating router id */
  unsigned int origin_switch_id;

  /* number of hops traversed by the packet */
  short my_N_hop;

  // For buffer message
  short vc_index;
  short vc_off;
  int is_pull;
  model_net_event_return event_rc;
  uint64_t pull_size;

  /* for reverse computation */    
  int path_type; 
  tw_stime saved_available_time;
  tw_stime saved_credit_time;
  uint64_t packet_size;
  tw_stime msg_start_time;
  tw_stime saved_busy_time;
  tw_stime saved_sample_time;
  tw_stime saved_avg_time;
  tw_stime saved_rcv_time;
  tw_stime saved_total_time;

  /* For routing */
  uint64_t chunk_id;
  uint64_t total_size;
  uint64_t message_id;
   
  /* meta data to aggregate packets into a message at receiver */
  uint64_t msg_size;
  uint64_t src_nic;
  uint64_t uniq_id;
  uint64_t saved_size;

  int remote_event_size_bytes;
  int local_event_size_bytes;

};

#endif /* end of include guard: FATTREE_H */

