#include "codes/net/common-net.h"
#include "codes/quickhash.h"
#include "assert.h"

void append_to_message_list(  
        message_list ** thisq,
        message_list ** thistail,
        int index, 
        message_list *msg) {
    if(thisq[index] == NULL) {
        thisq[index] = msg;
    } else {
        thistail[index]->next = msg;
        msg->prev = thistail[index];
    } 
    thistail[index] = msg;
}

void prepend_to_message_list(  
        message_list ** thisq,
        message_list ** thistail,
        int index, 
        message_list *msg) {
    if(thisq[index] == NULL) {
        thistail[index] = msg;
    } else {
        thisq[index]->prev = msg;
        msg->next = thisq[index];
    } 
    thisq[index] = msg;
}

message_list* return_head(
        message_list ** thisq,
        message_list ** thistail,
        int index) {
    message_list *head = thisq[index];
    if(head != NULL) {
        thisq[index] = head->next;
        if(head->next != NULL) {
            head->next->prev = NULL;
            head->next = NULL;
        } else {
            thistail[index] = NULL;
        }
    }
    return head;
}

message_list* return_tail(
        message_list ** thisq,
        message_list ** thistail,
        int index) {
    message_list *tail = thistail[index];
    assert(tail);
    if(tail->prev != NULL) {
        tail->prev->next = NULL;
        thistail[index] = tail->prev;
        tail->prev = NULL;
    } else {
        thistail[index] = NULL;
        thisq[index] = NULL;
    }
    return tail;
}

void delete_from_message_list(message_list *** allq, message_list *** alltail,
        message_list *msg) {
    message_list ** thisq = allq[msg->port];
    message_list ** thistail = alltail[msg->port];
    if(thisq[msg->index] == msg) {
      thisq[msg->index] = msg->next;
    } 
    if(thistail[msg->index] == msg) {
      thistail[msg->index] = msg->prev;
    }
    if(msg->prev != NULL) {
      msg->prev->next = msg->next;
    }
    if(msg->next != NULL) {
      msg->next->prev = msg->prev;
    }
}

void add_to_message_list(message_list *** allq, message_list *** alltail,
        message_list *msg) {
    message_list ** thisq = allq[msg->port];
    message_list ** thistail = alltail[msg->port];
    if(thisq[msg->index] == msg->next) {
      thisq[msg->index] = msg;
    } 
    if(thistail[msg->index] == msg->prev) {
      thistail[msg->index] = msg;
    }
    if(msg->prev != NULL) {
      msg->prev->next = msg;
    }
    if(msg->next != NULL) {
      msg->next->prev = msg;
    }
}

void altq_append_to_message_list(  
        message_list ** thisq,
        message_list ** thistail,
        int index, 
        message_list *msg) {
    assert(index == 0);
    if(thisq[index] == NULL) {
        thisq[index] = msg;
    } else {
        thistail[index]->altq_next = msg;
        msg->altq_prev = thistail[index];
    } 
    thistail[index] = msg;
}

void altq_prepend_to_message_list(  
        message_list ** thisq,
        message_list ** thistail,
        int index, 
        message_list *msg) {
    assert(index == 0);
    if(thisq[index] == NULL) {
        thistail[index] = msg;
    } else {
        thisq[index]->altq_prev = msg;
        msg->altq_next = thisq[index];
    } 
    thisq[index] = msg;
}

message_list* altq_return_head(
        message_list ** thisq,
        message_list ** thistail,
        int index) {
    assert(index == 0);
    message_list *head = thisq[index];
    if(head != NULL) {
        thisq[index] = head->altq_next;
        if(head->altq_next != NULL) {
            head->altq_next->altq_prev = NULL;
            head->altq_next = NULL;
        } else {
            thistail[index] = NULL;
        }
    }
    return head;
}

message_list* altq_return_tail(
        message_list ** thisq,
        message_list ** thistail,
        int index) {
    assert(index == 0);
    message_list *tail = thistail[index];
    assert(tail);
    if(tail->altq_prev != NULL) {
        tail->altq_prev->altq_next = NULL;
        thistail[index] = tail->altq_prev;
        tail->altq_prev = NULL;
    } else {
        thistail[index] = NULL;
        thisq[index] = NULL;
    }
    return tail;
}

void altq_delete_from_message_list(message_list *** allq, message_list *** alltail,
        message_list *msg) {
    message_list ** thisq = allq[msg->altq_port];
    message_list ** thistail = alltail[msg->altq_port];
    if(thisq[0] == msg) {
      thisq[0] = msg->altq_next;
    } 
    if(thistail[0] == msg) {
      thistail[0] = msg->altq_prev;
    }
    if(msg->altq_prev != NULL) {
      msg->altq_prev->altq_next = msg->altq_next;
    }
    if(msg->altq_next != NULL) {
      msg->altq_next->altq_prev = msg->altq_prev;
    }
}

void altq_add_to_message_list(message_list *** allq, message_list *** alltail,
        message_list *msg) {
    message_list ** thisq = allq[msg->altq_port];
    message_list ** thistail = alltail[msg->altq_port];
    if(thisq[0] == msg->altq_next) {
      thisq[0] = msg;
    } 
    if(thistail[0] == msg->altq_prev) {
      thistail[0] = msg;
    }
    if(msg->altq_prev != NULL) {
      msg->altq_prev->altq_next = msg;
    }
    if(msg->altq_next != NULL) {
      msg->altq_next->altq_prev = msg;
    }
}

void delete_message_list(void *thisM) {
    message_list *thism = (message_list *)thisM;
    if(thism->event_data != NULL) free(thism->event_data);
    free(thism);
}

int mn_rank_hash_compare(void *key, struct qhash_head *link)
{
  struct mn_hash_key *message_key = (struct mn_hash_key *)key;
  struct mn_qhash_entry *tmp = NULL;

  tmp = qhash_entry(link, struct mn_qhash_entry, hash_link);

  if (tmp->key.message_id == message_key->message_id
      && tmp->key.sender_id == message_key->sender_id)
    return 1;

  return 0;
}

int mn_hash_func(void *k, int table_size)
{
  struct mn_hash_key *tmp = (struct mn_hash_key *)k;
  uint64_t key = (~tmp->message_id) + (tmp->message_id << 18);
  key = key * 21;
  key = ~key ^ (tmp->sender_id >> 4);
  key = key * tmp->sender_id; 
  return (int)(key & (table_size - 1));
}

void free_tmp(void * ptr)
{
  struct mn_qhash_entry * msg = ptr; 

  if(msg->remote_event_data)
    free(msg->remote_event_data);

  if(msg)
    free(msg);
}

/* convert GiB/s and bytes to ns */
tw_stime bytes_to_ns(uint64_t bytes, double GB_p_s)
{
  tw_stime time;

  /* bytes to GB */
  time = ((double)bytes)/(1024.0*1024.0*1024.0);
  /* GiB to s */
  time = time / GB_p_s;
  /* s to ns */
  time = time * 1000.0 * 1000.0 * 1000.0;
  return(time);
}


