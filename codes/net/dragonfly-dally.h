/*
 * Copyright (C) 2014 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#ifndef DRAGONFLY_DALLY_H
#define DRAGONFLY_DALLY_H

#ifdef __cplusplus
extern "C" {
#endif

#include <ross.h>

typedef struct terminal_dally_message terminal_dally_message;

/* this message is used for both dragonfly compute nodes and routers */
struct terminal_dally_message
{
  /* magic number */
  int magic;
  /* flit travel start time*/
  tw_stime travel_start_time;
  /* flit travel end time*/
  tw_stime travel_end_time;
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
  tw_lpid dest_terminal_lpid;
  unsigned int dfdally_src_terminal_id;
  unsigned int dfdally_dest_terminal_id; //this is the terminal id in the dfdally network in range [0-total_num_terminals)
  /* source terminal ID of the dragonfly */
  unsigned int src_terminal_id;
  /* message originating router id. MM: Can we calculate it through
   * sender_mn_lp??*/
  unsigned int origin_router_id;

  int app_id; //id of the job associated with this terminal TODO - this will cause a problem if multiple job workload LPs are mapped to one terminal

  /* number of hops traversed by the packet */
  short my_N_hop;
  short my_l_hop, my_g_hop;
  short my_hops_cur_group;
  short saved_channel;
  short saved_vc;

  int next_stop;

  //encoded time when received at a router
  tw_stime this_router_arrival;
  //encoded time when departed from router
  tw_stime this_router_ptp_latency;
  short ptp_latency_set;

  /* Intermediate LP ID from which this message is coming */
  unsigned int intm_lp_id;
  /* last hop of the message, can be a terminal, local router or global router */
  short last_hop;
   /* For routing */
  short is_intm_visited;
  int minimal_intermediate_flag; //flag for when the minimal route will be equivalent to a valiant one
  int intm_rtr_id;
  int intm_grp_id;
  int saved_src_dest;
  int saved_src_chan;

   uint32_t chunk_id;
   uint32_t packet_size;
   uint32_t message_id;
   uint32_t total_size;

   int remote_event_size_bytes;
   int local_event_size_bytes;

  // For buffer message
   short vc_index;
   short rail_id;
   int output_chan;
   model_net_event_return event_rc;
   int is_pull;
   uint32_t pull_size;
   int path_type;
   int saved_app_id;

   /* for reverse computation */   
   short num_rngs;
   short num_cll;

   /* qos related attributes */
   short last_saved_qos;
   short qos_reset1;
   short qos_reset2;

   /* new qos rc - These are calloced in forward events, free'd in RC or commit_f */
   /* note: dynamic memory here is OK since it's only accessed by the LP that alloced it in the first place. */
   short rc_is_qos_set;
   unsigned long long * rc_qos_data;
   int * rc_qos_status;

   short saved_send_loop;
   tw_stime saved_available_time;
   tw_stime saved_min_lat;
   tw_stime saved_avg_time;
   tw_stime saved_rcv_time;
   tw_stime saved_busy_time; 
   tw_stime saved_total_time;
   tw_stime saved_sample_time;
   tw_stime msg_start_time;
   tw_stime saved_busy_time_ross;
   tw_stime saved_fin_chunks_ross;
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
