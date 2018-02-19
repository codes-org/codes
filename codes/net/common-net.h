#ifndef COMMON_NET_H
#define COMMON_NET_H
#include "codes/model-net-lp.h"
#include "codes/quickhash.h"

#ifdef __cplusplus
extern "C" {
#endif


struct mn_hash_key
{
  uint64_t message_id;
  tw_lpid sender_id;
};

struct mn_qhash_entry
{
  struct mn_hash_key key;
  char * remote_event_data;
  int num_chunks;
  int remote_event_size;
  struct qhash_head hash_link;
};

extern int mn_rank_hash_compare(void *key, struct qhash_head *link);

extern int mn_hash_func(void *k, int table_size);

extern void free_tmp(void * ptr);

typedef struct message_list message_list;

struct message_list {
  //CHANGE: add message types for new networks here
  union {
    terminal_message dfly_msg;
    em_message em_msg;
  };
  char* event_data;
  message_list *next;
  message_list *prev;
  int port, index;
  message_list *altq_next, *altq_prev;
  int in_alt_q, altq_port;
};

extern void append_to_message_list(  
    message_list ** thisq,
    message_list ** thistail,
    int index, 
    message_list *msg);

extern void prepend_to_message_list(  
    message_list ** thisq,
    message_list ** thistail,
    int index, 
    message_list *msg);

extern message_list* return_head(
    message_list ** thisq,
    message_list ** thistail,
    int index);

extern message_list* return_tail(
    message_list ** thisq,
    message_list ** thistail,
    int index);

extern void delete_from_message_list(
    message_list *** allq, 
    message_list *** alltail,
    message_list *msg);

extern void add_to_message_list(
    message_list *** allq, 
    message_list *** alltail,
    message_list *msg);

extern void altq_append_to_message_list(  
    message_list ** thisq,
    message_list ** thistail,
    int index, 
    message_list *msg);

extern void altq_prepend_to_message_list(  
    message_list ** thisq,
    message_list ** thistail,
    int index, 
    message_list *msg);

extern message_list* altq_return_head(
    message_list ** thisq,
    message_list ** thistail,
    int index);

extern message_list* altq_return_tail(
    message_list ** thisq,
    message_list ** thistail,
    int index);

extern void altq_delete_from_message_list(
    message_list *** allq, 
    message_list *** alltail,
    message_list *msg);

extern void altq_add_to_message_list(
    message_list *** allq, 
    message_list *** alltail,
    message_list *msg);

extern void delete_message_list(void *thism);
extern tw_stime bytes_to_ns(uint64_t bytes, double GB_p_s);

#ifdef __cplusplus
}
#endif

#endif
