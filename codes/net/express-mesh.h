/*
 * Copyright (C) 2014 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#ifndef EXPRESS_MESH_H
#define EXPRESS_MESH_H

#ifdef __cplusplus
extern "C" {
#endif

#include <ross.h>

typedef struct em_message em_message;

struct em_message
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
 /* destination terminal ID */
  tw_lpid dest_terminal_id;
  int dest_terminal;
  /* source terminal ID */
  tw_lpid src_terminal_id;

  short saved_channel;
  short my_N_hop;
  short hops[8];

  /* Intermediate LP ID from which this message is coming */
  unsigned int intm_lp_id;
  short saved_vc;
  short dim_change;
  /* last hop of the message, can be a terminal, local router or global router */
  int last_hop;
  /* For routing */
  uint64_t chunk_id;
  uint64_t packet_size;
  uint64_t message_id;
  uint64_t total_size;

  int saved_remote_esize;
  int remote_event_size_bytes;
  int local_event_size_bytes;

  // For buffer message
  int vc_index;
  int output_chan;
  model_net_event_return event_rc;
  int is_pull;
  uint64_t pull_size;

  /* for reverse computation */   
  tw_stime saved_available_time;
  tw_stime saved_avg_time;
  tw_stime saved_rcv_time;
  tw_stime saved_busy_time; 
  tw_stime saved_total_time;
  tw_stime saved_hist_start_time;
  tw_stime saved_sample_time;
  tw_stime msg_start_time;

  int saved_hist_num;
  int saved_occupancy;
};

#ifdef __cplusplus
}
#endif

#endif 

