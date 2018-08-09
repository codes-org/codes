/*
 * Copyright (C) 2014 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

//CHANGE: modify to match you header file name
#ifndef NETWORK_GRAPH_H
#define NETWORK_GRAPH_H

#ifdef __cplusplus
extern "C" {
#endif

#include <ross.h>

//CHANGE: modify to match the struct
typedef struct network_graph_message network_graph_message;

//CHANGE: modify the struct name - add to message_list union in common-net.h
struct network_graph_message
{
  //common entries:
  int magic; /* magic number */
  short  type; /* event type of the flit */

  tw_stime travel_start_time; /* flit travel start time*/
  unsigned long long packet_ID; /* packet ID of the flit  */
  char category[CATEGORY_NAME_MAX]; /* category: comes from codes */

  tw_lpid final_dest_gid; /* final destination LP ID, this comes from codes can be a server or any other LP type*/
  tw_lpid sender_lp; /*sending LP ID from CODES, can be a server or any other LP type */
  tw_lpid sender_mn_lp; // source modelnet id (think NIC)
  tw_lpid src_terminal_id; /* source terminal ID - mostly same as sender_mn_lp */
  tw_lpid dest_terminal_id; /* destination modelnet id */
  int dest_terminal; /* logical id of destination modelnet id */

  /* packet/message identifier and status */
  uint64_t chunk_id; //which chunk of packet I am
  uint64_t packet_size; //what is the size of my packet
  uint64_t message_id; //seq number at message level - NIC specified
  uint64_t total_size; //total size of the message
  int remote_event_size_bytes; // data size for target event at destination
  int local_event_size_bytes; // data size for event at source
  int is_pull;
  uint64_t pull_size;
  tw_stime msg_start_time;

  //info for path traversal
  short my_N_hop; /* hops traversed so far */
  unsigned int intm_lp_id; /* Intermediate LP ID that sent this packet */
  int last_hop; /* last hop of the message, can be a terminal, local router or global router */
  int vc_index; /* stores port info */
  int output_chan; /* virtual channel within port */

  //info for reverse computation
  short saved_vc;
  short saved_port;
  model_net_event_return event_rc;
  tw_stime saved_available_time;
  tw_stime saved_avg_time;
  tw_stime saved_rcv_time;
  tw_stime saved_busy_time;
  tw_stime saved_total_time;
  tw_stime saved_hist_start_time;
  tw_stime saved_sample_time;

};

#ifdef __cplusplus
}
#endif

#endif

