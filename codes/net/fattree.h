#ifndef FATTREE_H
#define FATTREE_H

#include <ross.h>

typedef struct fattree_message fattree_message;

/* this message is used for both dragonfly compute nodes and routers */
struct fattree_message
{
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
 /* destination terminal ID of the message */
  int dest_num;
  /* source terminal ID of the fattree */
  unsigned int src_terminal_id;
  /* Intermediate LP ID from which this message is coming */
  unsigned int intm_lp_id;
  short saved_vc;
  short saved_off;
  int last_hop;
  int intm_id; //to find which port I connect to sender with

  // For buffer message
  short vc_index;
  short vc_off;

  /* for reverse computation */   
  tw_stime saved_available_time;
  tw_stime saved_credit_time;
  uint64_t packet_size;
   
  /* meta data to aggregate packets into a message at receiver */
  uint64_t msg_size;
  uint64_t src_nic;
  uint64_t uniq_id;
  uint64_t saved_size;

  int remote_event_size_bytes;
  int local_event_size_bytes;

};

#endif /* end of include guard: FATTREE_H */

