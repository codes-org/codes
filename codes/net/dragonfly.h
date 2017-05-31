/*
 * Copyright (C) 2014 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#ifndef DRAGONFLY_H
#define DRAGONFLY_H

#ifdef __cplusplus
extern "C" {
#endif

#include <ross.h>

extern void dragonfly_register_evcol();
extern void router_register_evcol();

typedef struct terminal_message terminal_message;

/* this message is used for both dragonfly compute nodes and routers */
struct terminal_message
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

  /* store category hash in the event */
  uint32_t category_hash;

  /* final destination LP ID, this comes from codes can be a server or any other LP type*/
  tw_lpid final_dest_gid;
  /*sending LP ID from CODES, can be a server or any other LP type */
  tw_lpid sender_lp;
  tw_lpid sender_mn_lp; // source modelnet id
 /* destination terminal ID of the dragonfly */
  tw_lpid dest_terminal_id;
  /* source terminal ID of the dragonfly */
  unsigned int src_terminal_id;
  /* local LP ID to calculate the radix of the sender node/router */
  unsigned int local_id;
  /* message originating router id */
  unsigned int origin_router_id;

  /* number of hops traversed by the packet */
  short my_N_hop;
  short my_l_hop, my_g_hop;
  short saved_channel;

  /* Intermediate LP ID from which this message is coming */
  unsigned int intm_lp_id;
  short new_vc;
  short saved_vc;
  
  int saved_src_dest;
  /* last hop of the message, can be a terminal, local router or global router */
  int last_hop;
   /* For routing */
   int intm_group_id;
   uint64_t chunk_id;
   uint64_t packet_size;
   uint64_t message_id;
   uint64_t total_size;

   int saved_remote_esize;
   int remote_event_size_bytes;
   int local_event_size_bytes;

  // For buffer message
   int vc_index;
   int sender_radix;
   int output_chan;
   model_net_event_return event_rc;
   int is_pull;
   uint64_t pull_size;

   /* for reverse computation */   
   int path_type;
   tw_stime saved_available_time;
   tw_stime saved_avg_time;
   tw_stime saved_rcv_time;
   tw_stime saved_busy_time; 
   tw_stime saved_total_time;
   tw_stime saved_hist_start_time;
   tw_stime saved_sample_time;
   tw_stime msg_start_time;
   tw_stime saved_busy_time_ross;
   tw_stime saved_fin_chunks_ross;

   int saved_hist_num;
   int saved_occupancy;


   /* for reverse computation of a node's fan in*/
   int saved_fan_nodes;
   tw_lpid sender_svr;

  /* LP ID of the sending node, has to be a network node in the dragonfly */
   tw_lpid sender_node;
   tw_lpid next_stop;
};

#ifdef __cplusplus
}
#endif

#endif /* end of include guard: DRAGONFLY_H */

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
