/*
 * Copyright (C) 2014 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#ifndef DRAGONFLY_H
#define DRAGONFLY_H

#include <ross.h>

typedef struct terminal_message terminal_message;

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
  char category[CATEGORY_NAME_MAX];
  /* final destination LP ID, this comes from codes can be a server or any other LP type*/
  tw_lpid final_dest_gid;
  /*sending LP ID from CODES, can be a server or any other LP type */
  tw_lpid sender_lp;
 /* destination terminal ID of the dragonfly */
  unsigned int dest_terminal_id;
  /* source terminal ID of the dragonfly */
  unsigned int src_terminal_id;
  /* local LP ID to calculate the radix of the sender node/router */
  unsigned int local_id;
  /* number of hops traversed by the packet */
  short my_N_hop;
  /* Intermediate LP ID from which this message is coming */
  unsigned int intm_lp_id;
  short old_vc;
  short saved_vc;
  /* last hop of the message, can be a terminal, local router or global router */
  short last_hop;

  /* for reverse computation */
  short path_type;
  // For buffer message
   short vc_index;
   int input_chan;
   int output_chan;
    int is_pull;
    uint64_t pull_size;

   /* for reverse computation */   
   tw_stime saved_available_time;
   tw_stime saved_credit_time;
   tw_stime saved_collective_init_time;  

   int intm_group_id;
   short chunk_id;
   uint64_t packet_size;
   int remote_event_size_bytes;
   int local_event_size_bytes;

   /* for reverse computation of a node's fan in*/
   int saved_fan_nodes;
   tw_lpid sender_svr;

  /* LP ID of the sending node, has to be a network node in the dragonfly */
   tw_lpid sender_node;
};

#endif /* end of include guard: DRAGONFLY_H */

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
