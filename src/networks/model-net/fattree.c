#include <ross.h>

#include "codes/codes_mapping.h"
#include "codes/codes.h"
#include "codes/model-net.h"
#include "codes/model-net-method.h"
#include "codes/model-net-lp.h"
#include "codes/net/fattree.h"
#include "sys/file.h"
#include "codes/quickhash.h"
//#include "codes/map_messages.h"

#define CREDIT_SIZE 8
#define MEAN_PROCESS 1.0

#define FTREE_HASH_TABLE_SIZE 262144

// debugging parameters
//#define TRACK_PKT -1
#define TRACK_PKT 2820
#define FATTREE_HELLO 0
#define FATTREE_DEBUG 0
#define FATTREE_CONNECTIONS 0
#define FATTREE_MSG 0

#define LP_CONFIG_NM (model_net_lp_config_names[FATTREE])
#define LP_METHOD_NM (model_net_method_names[FATTREE])

long fattree_packet_gen = 0, fattree_packet_fin = 0;

static double maxd(double a, double b) { return a < b ? b : a; }

// arrival rate
static double MEAN_INTERVAL=200.0;

typedef struct fattree_param fattree_param;
/* annotation-specific parameters (unannotated entry occurs at the 
 * last index) */
static uint64_t                  num_params = 0;
static fattree_param           * all_params = NULL;
static config_anno_map_t * anno_map   = NULL;

/* global variables for codes mapping */
static char lp_group_name[MAX_NAME_LENGTH];
static char def_group_name[MAX_NAME_LENGTH];
static int def_gname_set = 0;
static int mapping_grp_id, mapping_type_id, mapping_rep_id, mapping_offset;

/* switch magic number */
int switch_magic_num = 0;

/* terminal magic number */
int fattree_terminal_magic_num = 0;

typedef struct fattree_message_list fattree_message_list;
struct fattree_message_list {
    fattree_message msg;
    char* event_data;
    fattree_message_list *next;
    fattree_message_list *prev;
};

void init_fattree_message_list(fattree_message_list *this, 
  fattree_message *inmsg) {
    this->msg = *inmsg;
    this->event_data = NULL;
    this->next = NULL;
    this->prev = NULL;
}

void delete_fattree_message_list(fattree_message_list *this) {
    if(this->event_data != NULL) free(this->event_data);
    free(this);
}

struct fattree_param
{
  int ft_type;
  // configuration parameters
  int num_levels;
  int *num_switches; //switches at various levels
  int *switch_radix; //radix of switches are various levels
  double link_bandwidth;/* bandwidth of a wire connecting switches */
  double cn_bandwidth;/* bandwidth of the compute node channels 
                        connected to switch */
  int vc_size; /* buffer size of the link channels */
  int cn_vc_size; /* buffer size of the compute node channels */
  int chunk_size; /* full-sized packets are broken into smaller chunks.*/
  int packet_size; 
  int num_terminals;
  int l1_set_size;
  int l1_term_size;
  double cn_delay;
  double head_delay;
  double credit_delay;
  double router_delay;
  double soft_delay;
};

struct ftree_hash_key
{
    uint64_t message_id;
    tw_lpid sender_id;
};

struct ftree_qhash_entry
{
   struct ftree_hash_key key;
   char * remote_event_data;
   int num_chunks;
   int remote_event_size;
   struct qhash_head hash_link;
};

/* convert GiB/s and bytes to ns */
 static tw_stime bytes_to_ns(uint64_t bytes, double GB_p_s)
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

/* handles terminal and switch events like packet generate/send/receive/buffer */
typedef enum event_t event_t;
typedef struct ft_terminal_state ft_terminal_state;
typedef struct switch_state switch_state;

/* fattree compute node data structure */
struct ft_terminal_state
{
  unsigned long long packet_counter;
  int packet_gen;
  int packet_fin;
  // Fattree specific parameters
  unsigned int terminal_id;
  unsigned int switch_id;
  tw_lpid switch_lp;

  // Each terminal will have an input and output channel with the switch
  int vc_occupancy; // NUM_VC
  tw_stime terminal_available_time;
  tw_stime next_credit_available_time;
  
  struct mn_stats fattree_stats_array[CATEGORY_MAX];

  fattree_message_list **terminal_msgs;
  fattree_message_list **terminal_msgs_tail;
  int terminal_length;
  int in_send_loop;
  int issueIdle;

   struct rc_stack * st;

  char * anno;
  fattree_param *params;

  struct qhash_table *rank_tbl;
  uint64_t rank_tbl_pop;

  tw_stime   total_time;
  uint64_t total_msg_size;
  double total_hops;
  long finished_msgs;
  long finished_chunks;
  long finished_packets;

  tw_stime last_buf_full;

  /* For sampling */
  long fin_chunks_sample;
  long data_size_sample;
  double fin_hops_sample;
  tw_stime fin_chunks_time;
};

/* terminal event type (1-4) */
enum event_t
{
  T_GENERATE=1,
  T_ARRIVE,
  T_SEND,
  T_BUFFER,
  S_SEND,
  S_ARRIVE,
  S_BUFFER,
};

enum last_hop
{
   LINK,
   TERMINAL
};

struct switch_state
{
  unsigned int switch_id;
  int switch_level;
  int radix;
  int num_cons;
  int num_lcons;
  int con_per_lneigh;
  int con_per_uneigh;;
  int start_lneigh;
  int end_lneigh;
  int start_uneigh;
  int unused;

  tw_stime* next_output_available_time;
  tw_stime* next_credit_available_time;
  fattree_message_list **pending_msgs;
  fattree_message_list **pending_msgs_tail;
  fattree_message_list **queued_msgs;
  fattree_message_list **queued_msgs_tail;
  int *queued_length;
  int *in_send_loop;
  int* vc_occupancy;
  int64_t* link_traffic;
  tw_lpid *port_connections;

  struct rc_stack * st;

  char * anno;
  fattree_param *params;
};

static tw_stime         fattree_total_time = 0;
static tw_stime         fattree_max_latency = 0;
static tw_stime         max_collective = 0;


static long long       total_hops = 0;
static long long       N_finished_packets = 0;
static long long       total_msg_sz = 0;
static long long       N_finished_msgs = 0;
static long long       N_finished_chunks = 0;

static int fattree_rank_hash_compare(
void *key, struct qhash_head *link)
{
    struct ftree_hash_key *message_key = (struct ftree_hash_key *)key;
    struct ftree_qhash_entry *tmp = NULL;

    tmp = qhash_entry(link, struct ftree_qhash_entry, hash_link);

    if (tmp->key.message_id == message_key->message_id
            && tmp->key.sender_id == message_key->sender_id)
        return 1;

    return 0;
}
static int fattree_hash_func(void *k, int table_size)
{
    struct ftree_hash_key *tmp = (struct ftree_hash_key *)k;
    //uint32_t pc = 0, pb = 0;
    //bj_hashlittle2(tmp, sizeof(*tmp), &pc, &pb);
    uint64_t key = (~tmp->message_id) + (tmp->message_id << 18);
    key = key * 21;
    key = ~key ^ (tmp->sender_id >> 4);
    key = key * tmp->sender_id;
    return (int)(key & (table_size - 1));
    //return (int)(pc % (table_size - 1));
}

static void free_tmp(void * ptr)
{
    struct ftree_qhash_entry * ftree = ptr; 
    free(ftree->remote_event_data);
    free(ftree);
}

static void append_to_fattree_message_list(  
        fattree_message_list ** thisq,
        fattree_message_list ** thistail,
        int index, 
        fattree_message_list *msg) {
    if(thisq[index] == NULL) {
        thisq[index] = msg;
    } else {
        thistail[index]->next = msg;
        msg->prev = thistail[index];
    } 
    thistail[index] = msg;
}

static void prepend_to_fattree_message_list(  
        fattree_message_list ** thisq,
        fattree_message_list ** thistail,
        int index, 
        fattree_message_list *msg) {
    if(thisq[index] == NULL) {
        thistail[index] = msg;
    } else {
        thisq[index]->prev = msg;
        msg->next = thisq[index];
    } 
    thisq[index] = msg;
}

static fattree_message_list* return_head(
        fattree_message_list ** thisq,
        fattree_message_list ** thistail,
        int index) {
    fattree_message_list *head = thisq[index];
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

static fattree_message_list* return_tail(
        fattree_message_list ** thisq,
        fattree_message_list ** thistail,
        int index) {
    fattree_message_list *tail = thistail[index];
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

//decl
void switch_credit_send(switch_state * s, tw_bf * bf, fattree_message * msg,
    tw_lp * lp, int sq);
int ft_get_output_port( switch_state * s, tw_bf * bf, fattree_message * msg,
    tw_lp * lp, int *out_off );
int get_base_port(switch_state *s, int from_term, int index);


/* returns the fattree switch lp type for lp registration */
static const tw_lptype* fattree_get_switch_lp_type(void);

/* returns the fattree message size */
static int fattree_get_msg_sz(void)
{
  return sizeof(fattree_message);
}

static void fattree_read_config(char * anno, fattree_param *p){
  int i;

  p->ft_type = 0;
  configuration_get_value_int(&config, "PARAMS", "ft_type", anno, 
      &p->ft_type);

  configuration_get_value_int(&config, "PARAMS", "num_levels", anno, 
      &p->num_levels);
  if(p->num_levels <= 0) {
    tw_error(TW_LOC, "Too few num_levels, Aborting\n");
  }
  if(p->num_levels > 3) {
    tw_error(TW_LOC, "Too many num_levels, only upto 3 supported Aborting\n");
  }

  p->num_switches = (int *) malloc (p->num_levels * sizeof(int));
  p->switch_radix = (int*) malloc (p->num_levels * sizeof(int));

  char switch_counts_str[MAX_NAME_LENGTH];
  int rc = configuration_get_value(&config, "PARAMS", "switch_count", anno,
      switch_counts_str, MAX_NAME_LENGTH);
  if (rc == 0){
    tw_error(TW_LOC, "couldn't read PARAMS:switch_count");
  }
  char* token;
  token = strtok(switch_counts_str, ",");
  i = 0;
  while(token != NULL)
  {
    sscanf(token, "%d", &p->num_switches[i]);
    if(p->num_switches[i] <= 0)
    {
      tw_error(TW_LOC, "Invalid switch count  specified "
          "(%d at pos %d), exiting... ", p->num_switches[i], i);
    }
    i++;
    token = strtok(NULL,",");
  }

  //if(i != p->num_levels) {
  //  tw_error(TW_LOC, "Not enough switch counts, Aborting\n");
  //}
  
  char switch_radix_str[MAX_NAME_LENGTH];
  rc = configuration_get_value(&config, "PARAMS", "switch_radix", anno,
      switch_radix_str, MAX_NAME_LENGTH);
  if (rc == 0){
    tw_error(TW_LOC, "couldn't read PARAMS:switch_radix");
  }
  token = strtok(switch_radix_str, ",");
  i = 0;
  while(token != NULL)
  {
    sscanf(token, "%d", &p->switch_radix[i]);
    if(p->switch_radix[i] <= 0)
    {
      tw_error(TW_LOC, "Invalid switch radix  specified "
          "(%d at pos %d), exiting... ", p->switch_radix[i], i);
    }
    i++;
    token = strtok(NULL,",");
  }

  if(p->num_levels == 2) {
    p->num_switches[1] = p->num_switches[0]/2;
    p->switch_radix[1] = p->switch_radix[0];
  } else {
    p->num_switches[1] = p->num_switches[0];
    p->num_switches[2] = p->num_switches[0]/2;
    p->switch_radix[1] = p->switch_radix[2] = p->switch_radix[0];
  }
  //if(i != p->num_levels) {
  //  tw_error(TW_LOC, "Not enough switch radix, Aborting\n");
  //}

  i = 1;
  for(i = 1; i < p->num_levels - 1; i++) {
    if(p->num_switches[i - 1] * p->switch_radix[i - 1] >
       p->num_switches[i] * p->switch_radix[i]) {
      tw_error(TW_LOC, "Not enough switches/radix at level %d for full "
          "bisection bandwidth\n", i);
    }
  }

  if(p->num_switches[i - 1] * p->switch_radix[i - 1] > 2 * p->num_switches[i] *
      p->switch_radix[i]) {
    tw_error(TW_LOC, "Not enough switches/radix at level %d (top) for full "
        "bisection bandwidth\n", i);
  }

  configuration_get_value_int(&config, "PARAMS", "packet_size", anno,
      &p->packet_size);
  if(!p->packet_size) {
    p->packet_size = 512;
    fprintf(stderr, "Packet size is not specified, setting to %d\n", 
        p->packet_size);
  }
    
  p->router_delay = 50;
  configuration_get_value_double(&config, "PARAMS", "router_delay", anno,
      &p->router_delay);
    
  p->soft_delay = 1000;
  configuration_get_value_double(&config, "PARAMS", "soft_delay", anno,
      &p->soft_delay);

  configuration_get_value_int(&config, "PARAMS", "vc_size", anno, &p->vc_size);
  if(!p->vc_size) {
    p->vc_size = 8*p->packet_size;
    fprintf(stderr, "Buffer size of global channels not specified, setting to "
        "%d\n", p->vc_size);
  }

  configuration_get_value_int(&config, "PARAMS", "cn_vc_size", anno, 
      &p->cn_vc_size);
  if(!p->cn_vc_size) {
    p->cn_vc_size = 8*p->packet_size;
    fprintf(stderr, "Buffer size of compute node channels not specified, " 
        "setting to %d\n", p->cn_vc_size);
  }

  rc = configuration_get_value_int(&config, "PARAMS", "chunk_size", anno, &p->chunk_size);
    if(rc) {
        p->chunk_size = 512;
        fprintf(stderr, "Chunk size for packets is specified, setting to %d\n", p->chunk_size);
    }

  configuration_get_value_double(&config, "PARAMS", "link_bandwidth", anno, 
      &p->link_bandwidth);
  if(!p->link_bandwidth) {
    p->link_bandwidth = 5;
    fprintf(stderr, "Bandwidth of links is specified, setting to %lf\n", 
        p->link_bandwidth);
  }

  configuration_get_value_double(&config, "PARAMS", "cn_bandwidth", anno, 
      &p->cn_bandwidth);
  if(!p->cn_bandwidth) {
    p->cn_bandwidth = 5;
    fprintf(stderr, "Bandwidth of compute node channels not specified, " 
        "setting to %lf\n", p->cn_bandwidth);
  }

  p->l1_set_size = p->switch_radix[0]/2;
 
  p->l1_term_size = (p->l1_set_size * (p->switch_radix[0] / 2));

  p->cn_delay = (1.0 / p->cn_bandwidth);
  p->head_delay = (1.0 / p->link_bandwidth);
  p->credit_delay = (1.0 / p->link_bandwidth) * 8; //assume 8 bytes packet
}

static void fattree_configure(){
  anno_map = codes_mapping_get_lp_anno_map(LP_CONFIG_NM);
  assert(anno_map);
  num_params = anno_map->num_annos + (anno_map->has_unanno_lp > 0);
  all_params = malloc(num_params * sizeof(*all_params));

  for (uint64_t i = 0; i < anno_map->num_annos; i++){
    char * anno = anno_map->annotations[i].ptr;
    fattree_read_config(anno, &all_params[i]);
  }
  if (anno_map->has_unanno_lp > 0){
    fattree_read_config(NULL, &all_params[anno_map->num_annos]);
  }
}

/* initialize a fattree compute node terminal */
void ft_terminal_init( ft_terminal_state * s, tw_lp * lp )
{
    s->packet_fin = 0;

    uint32_t h1 = 0, h2 = 0; 
    bj_hashlittle2(LP_METHOD_NM, strlen(LP_METHOD_NM), &h1, &h2);
    fattree_terminal_magic_num = h1 + h2;

    int i;
    char anno[MAX_NAME_LENGTH];

    if(def_gname_set == 0) {
      def_gname_set = 1;
      codes_mapping_get_lp_info(0, def_group_name, &mapping_grp_id, NULL,
          &mapping_type_id, anno, &mapping_rep_id, &mapping_offset);
    }

    // Assign the global switch ID
    codes_mapping_get_lp_info(lp->gid, lp_group_name, &mapping_grp_id, NULL,
            &mapping_type_id, anno, &mapping_rep_id, &mapping_offset);
    if (anno[0] == '\0'){
        s->anno = NULL;
        s->params = &all_params[num_params-1];
    } else {
        s->anno = strdup(anno);
        int id = configuration_get_annotation_index(anno, anno_map);
        s->params = &all_params[id];
    }

   int num_lps = codes_mapping_get_lp_count(lp_group_name, 1, LP_CONFIG_NM,
           s->anno, 0);

   if(num_lps != (s->params->switch_radix[0]/2)) {
     tw_error(TW_LOC, "Number of NICs per repetition has to be equal to "
         "half the radix of leaf level switches %d vs %d\n", num_lps,
          s->params->switch_radix[0]/2);
   }
   s->terminal_id = (mapping_rep_id * num_lps) + mapping_offset;  
   s->switch_id = s->terminal_id / (s->params->switch_radix[0] / 2);
   codes_mapping_get_lp_id(lp_group_name, "fattree_switch", NULL, 1,
           s->switch_id, 0, &s->switch_lp);
   s->terminal_available_time = 0.0;
   s->packet_counter = 0;
   s->terminal_msgs = 
     (fattree_message_list**)malloc(1*sizeof(fattree_message_list*));
   s->terminal_msgs_tail = 
     (fattree_message_list**)malloc(1*sizeof(fattree_message_list*));

#if FATTREE_HELLO
   printf("I am terminal %d (%ld), connected to switch %d\n", s->terminal_id,
       lp->gid, s->switch_id);
#endif

   rc_stack_create(&s->st);

   s->vc_occupancy = 0;
   s->terminal_msgs[0] = NULL;
   s->terminal_msgs_tail[0] = NULL;
   s->terminal_length = 0;
   s->in_send_loop = 0;
   s->issueIdle = 0;

   s->rank_tbl = qhash_init(fattree_rank_hash_compare, fattree_hash_func, FTREE_HASH_TABLE_SIZE);
   if(!s->rank_tbl)
     tw_error(TW_LOC, "\n Hash table not initialized! ");

   s->params->num_terminals = codes_mapping_get_lp_count(lp_group_name, 0, 
      LP_CONFIG_NM, s->anno, 0);
   return;
}

/* sets up the switch */
void switch_init(switch_state * r, tw_lp * lp)
{
  char anno[MAX_NAME_LENGTH];
  int num_terminals = -1, num_lps;

  if(def_gname_set == 0) {
    def_gname_set = 1;
    codes_mapping_get_lp_info(0, def_group_name, &mapping_grp_id, NULL,
        &mapping_type_id, anno, &mapping_rep_id, &mapping_offset);
    num_terminals = codes_mapping_get_lp_count(def_group_name, 0, 
      LP_CONFIG_NM, anno, 0);
    num_lps = codes_mapping_get_lp_count(def_group_name, 1, LP_CONFIG_NM,
           anno, 0);
  }

  codes_mapping_get_lp_info(lp->gid, lp_group_name, &mapping_grp_id, NULL,
      &mapping_type_id, anno, &mapping_rep_id, &mapping_offset);

  if (anno[0] == '\0'){
    r->anno = NULL;
    r->params = &all_params[num_params-1];
  } else {
    r->anno = strdup(anno);
    int id = configuration_get_annotation_index(anno, anno_map);
    r->params = &all_params[id];
  }

  // shorthand
  fattree_param *p = r->params;
  if(mapping_offset == p->num_levels - 1) {
    if(mapping_rep_id >= p->num_switches[mapping_offset]) {
      r->unused = 1;
      return;
    }
  }

  r->unused = 0;

  r->switch_id = mapping_rep_id + mapping_offset * p->num_switches[0];

  if(num_terminals != -1) {
    p->num_terminals = num_terminals;
  }

  r->switch_level = mapping_offset;

  r->radix = p->switch_radix[r->switch_level];

  r->next_output_available_time = (tw_stime*) malloc (r->radix * 
      sizeof(tw_stime));
  r->next_credit_available_time = (tw_stime*) malloc (r->radix * 
      sizeof(tw_stime));
  r->vc_occupancy = (int*) malloc (r->radix * sizeof(int));
  r->in_send_loop = (int*) malloc (r->radix * sizeof(int));
  r->link_traffic = (int64_t*) malloc (r->radix * sizeof(int64_t));
  r->port_connections = (tw_lpid*) malloc (r->radix * sizeof(tw_lpid));
  r->pending_msgs = 
    (fattree_message_list**)malloc(r->radix * sizeof(fattree_message_list*));
  r->pending_msgs_tail = 
    (fattree_message_list**)malloc(r->radix * sizeof(fattree_message_list*));
  r->queued_msgs = 
    (fattree_message_list**)malloc(r->radix * sizeof(fattree_message_list*));
  r->queued_msgs_tail = 
    (fattree_message_list**)malloc(r->radix * sizeof(fattree_message_list*));
  r->queued_length = (int*)malloc(r->radix * sizeof(int));

  rc_stack_create(&r->st);

  for(int i = 0; i < r->radix; i++)
  {
    // Set credit & switch occupancy
    r->next_output_available_time[i] = 0;
    r->next_credit_available_time[i] = 0;
    r->vc_occupancy[i] = 0;
    r->in_send_loop[i] = 0;
    r->link_traffic[i] = 0;
    r->pending_msgs[i] = NULL;
    r->pending_msgs_tail[i] = NULL;
    r->queued_msgs[i] = NULL;
    r->queued_msgs_tail[i] = NULL;
    r->queued_length[i] = 0;
  }

  //set lps connected to each port
  r->num_cons = 0;
  r->num_lcons = 0;
#if FATTREE_HELLO
  printf("I am switch %d (%d), level %d, radix %d\n", r->switch_id,
    lp->gid, r->switch_level, r->radix);
#endif
  //if at level 0, first half ports go to terminals
  if(r->switch_level == 0) {
    int start_terminal = r->switch_id * (p->switch_radix[0] / 2);
    int end_terminal = start_terminal + (p->switch_radix[0] / 2);
    for(int term = start_terminal; term < end_terminal; term++) {
      tw_lpid nextTerm;
      int rep = term / (p->switch_radix[0] / 2);
      int off = term % (p->switch_radix[0] / 2);
      codes_mapping_get_lp_id(def_group_name, LP_CONFIG_NM, NULL, 1,
          rep, off, &nextTerm);
      r->port_connections[r->num_cons++] = nextTerm;
      r->num_lcons++;
#if FATTREE_DEBUG
      printf("I am switch %d, connect to terminal %d (%d) at port %d\n",
          r->switch_id, term, nextTerm, r->num_cons - 1);
#endif
    }
    r->start_lneigh = start_terminal;
    r->end_lneigh = end_terminal;
    r->con_per_lneigh = 1;
    assert(r->num_lcons == (r->radix / 2));
    int l1_set;
    if(p->num_levels == 2) {
      l1_set = 0;
    } else {
      l1_set = r->switch_id / p->l1_set_size;
    }
    int l1_base = l1_set * p->l1_set_size;
    r->start_uneigh = p->num_switches[0] + l1_base;
    r->con_per_uneigh = 1;
    for(int l1 = 0; l1 < p->l1_set_size; l1++) {
      tw_lpid nextTerm;
      codes_mapping_get_lp_id(lp_group_name, "fattree_switch", NULL, 1,
          l1_base, 1, &nextTerm);
      for(int con = 0; con < r->con_per_uneigh; con++) {
        r->port_connections[r->num_cons++] = nextTerm;
#if FATTREE_DEBUG
      printf("I am switch %d, connect to upper switch %d L1 (%d) at port %d\n",
          r->switch_id, l1_base, nextTerm, r->num_cons - 1);
#endif
      }
      l1_base++;
    }
  } else if (r->switch_level == 1) {
    int l0_set_size, l0_base;
    if(p->num_levels == 2) {
      l0_set_size = p->num_switches[0];
      l0_base = 0;
      r->start_lneigh = 0;
      r->end_lneigh = p->num_switches[0];
    } else {
      l0_set_size = p->l1_set_size;
      l0_base = ((r->switch_id - p->num_switches[0]) / p->l1_set_size) *
        l0_set_size;
      r->start_lneigh = l0_base;
      r->end_lneigh = l0_base + l0_set_size;
    }
    r->con_per_lneigh = 1;
    for(int l0 = 0; l0 < l0_set_size; l0++) {
      tw_lpid nextTerm;
      codes_mapping_get_lp_id(def_group_name, "fattree_switch", NULL, 1,
          l0_base, 0, &nextTerm);
      for(int con = 0; con < r->con_per_lneigh; con++) {
        r->port_connections[r->num_cons++] = nextTerm;
        r->num_lcons++;
#if FATTREE_DEBUG
        printf("I am switch %d, connect to switch %d L0 (%d) at port %d\n",
            r->switch_id, l0_base, nextTerm, r->num_cons - 1);
#endif
      }
      l0_base++;
    }
    if(p->num_levels == 3) {
      int l2_base = 0;
      if(p->ft_type == 0) {
//        int l2_base = 0;
        /* not true anymore */
        r->start_uneigh = p->num_switches[0] + l2_base;
        r->con_per_uneigh = 1;
        if((r->switch_id - p->num_switches[0]) % p->l1_set_size >=
            p->l1_set_size/2) {
          l2_base += (p->num_switches[2]/2);
        }
        for(int l2 = 0; l2 < p->num_switches[2]/2; l2++) {
          tw_lpid nextTerm;
          codes_mapping_get_lp_id(lp_group_name, "fattree_switch", NULL, 1,
              l2_base, 2, &nextTerm);
          for(int con = 0; con < r->con_per_uneigh; con++) {
            r->port_connections[r->num_cons++] = nextTerm;
#if FATTREE_DEBUG
            printf("I am switch %d, connect to upper switch %d L2 (%d) at port %d\n",
                r->switch_id, l2_base, nextTerm, r->num_cons - 1);
#endif
          }
          l2_base++;
        }
      } else {
        int l2 = ((r->switch_id - p->num_switches[0]) % p->l1_set_size);
        /* not true anymore */
        r->start_uneigh = p->num_switches[0] + l2;
        r->con_per_uneigh = 2;
        for(; l2 < p->num_switches[2]; l2 += p->l1_set_size) {
          tw_lpid nextTerm;
          codes_mapping_get_lp_id(lp_group_name, "fattree_switch", NULL, 1,
              l2, 2, &nextTerm);
          for(int con = 0; con < r->con_per_uneigh; con++) {
            r->port_connections[r->num_cons++] = nextTerm;
#if FATTREE_DEBUG
            printf("I am switch %d, connect to upper switch %d L2 (%d) at port %d\n",
                r->switch_id, l2, nextTerm, r->num_cons - 1);
#endif
          }
        }
      }
    }
  } else {
    if(p->ft_type == 0) {
      r->con_per_lneigh = 1;
      /* not true anymore */
      r->start_lneigh = p->num_switches[0];
      r->end_lneigh = r->start_lneigh + p->num_switches[1];
      int l1 = 0;
      if(r->switch_id - p->num_switches[0] - p->num_switches[1] >=
          (p->num_switches[2]/2)) {
        l1 += (p->l1_set_size/2);
      }
      int count = 0;
      for(; l1 < p->num_switches[1]; l1++) {
        tw_lpid nextTerm;
        codes_mapping_get_lp_id(lp_group_name, "fattree_switch", NULL, 1,
            l1, 1, &nextTerm);
        for(int con = 0; con < r->con_per_lneigh; con++) {
          r->port_connections[r->num_cons++] = nextTerm;
          r->num_lcons++;
#if FATTREE_DEBUG
          printf("I am switch %d, connect to  switch %d L1 (%d) at port %d\n",
              r->switch_id, l1, nextTerm, r->num_cons - 1);
#endif
        }
        count++;
        if(count == (p->l1_set_size/2)) {
          l1 += (p->l1_set_size/2);
          count = 0;
        }
      }
    } else {
      r->con_per_lneigh = 2;
      /* not true anymore */
      r->start_lneigh = p->num_switches[0];
      r->end_lneigh = r->start_lneigh + p->num_switches[1];
      int l1 = (r->switch_id - p->num_switches[0] - p->num_switches[1]) % p->l1_set_size;
      for(; l1 < p->num_switches[1]; l1 += p->l1_set_size) {
        tw_lpid nextTerm;
        codes_mapping_get_lp_id(lp_group_name, "fattree_switch", NULL, 1,
            l1, 1, &nextTerm);
        for(int con = 0; con < r->con_per_lneigh; con++) {
          r->port_connections[r->num_cons++] = nextTerm;
          r->num_lcons++;
#if FATTREE_DEBUG
          printf("I am switch %d, connect to  switch %d L1 (%d) at port %d\n",
              r->switch_id, l1, nextTerm, r->num_cons - 1);
#endif
        }
      }
    }
  }
  return;
}	

/* empty for now.. */
static void fattree_report_stats() { }

/* fattree packet event */
static tw_stime fattree_packet_event(
        model_net_request const * req,
        uint64_t message_offset,
        uint64_t packet_size,
        tw_stime offset,
        mn_sched_params const * sched_params,
        void const * remote_event,
        void const * self_event,
        tw_lp *sender,
        int is_last_pckt)
/*	model_net_request* req, 
	char* category, 
    tw_lpid final_dest_lp, 
	uint64_t packet_size, 
	int is_pull, 
    uint64_t pull_size, 
	tw_stime offset, 
	const mn_sched_params *sched_params, 
    int remote_event_size, 
	const void* remote_event, 
	int self_event_size, 
    const void* self_event, 
	tw_lpid src_lp, 
	tw_lp *sender, 
	int is_last_pckt) 
*/{

  tw_event * e_new;
  tw_stime xfer_to_nic_time;
  fattree_message * msg;
  char* tmp_ptr;

  xfer_to_nic_time = codes_local_latency(sender);
  e_new = model_net_method_event_new(sender->gid, xfer_to_nic_time + offset,
      sender, FATTREE, (void**)&msg, (void**)&tmp_ptr);
  strcpy(msg->category, req->category);
  msg->final_dest_gid = req->final_dest_lp;
  msg->total_size = req->msg_size;
  msg->sender_lp = req->src_lp;
  msg->sender_mn_lp = sender->gid;
  msg->packet_size = packet_size;
  msg->travel_start_time = tw_now(sender);
  msg->remote_event_size_bytes = 0;
  msg->local_event_size_bytes = 0;
  msg->type = T_GENERATE;
  msg->dest_terminal_id = req->dest_mn_lp;
  msg->message_id = req->msg_id;
  msg->is_pull = req->is_pull;
  msg->pull_size = req->pull_size;
  msg->magic = fattree_terminal_magic_num; 
  msg->msg_start_time = req->msg_start_time;

  /* Its the last packet so pass in remote and local event information*/
  if(is_last_pckt) 
  {
    if(req->remote_event_size > 0)
    {
      msg->remote_event_size_bytes = req->remote_event_size;
      memcpy(tmp_ptr, remote_event, req->remote_event_size);
      tmp_ptr += req->remote_event_size;
    }
    if(req->self_event_size > 0)
    {
      msg->local_event_size_bytes = req->self_event_size;
      memcpy(tmp_ptr, self_event, req->self_event_size);
      tmp_ptr += req->self_event_size;
    }
  }
  //printf("[%d] Send to %d\n", sender->gid, sender->gid);
  tw_event_send(e_new);
  return xfer_to_nic_time;
}

/* fattree packet event reverse handler */
static void fattree_packet_event_rc(tw_lp *sender)
{
  codes_local_latency_reverse(sender);
  return;
}

/* generates packet at the current fattree compute node */
void ft_packet_generate(ft_terminal_state * s, tw_bf * bf, fattree_message * msg,
    tw_lp * lp) {
  fattree_packet_gen++;
  s->packet_gen++;
  
  fattree_param *p = s->params;
  tw_stime ts, nic_ts;

  int total_event_size;
  uint64_t num_chunks = msg->packet_size / p->chunk_size;
  if(msg->packet_size % s->params->chunk_size)
    num_chunks++;
  if(!num_chunks)
    num_chunks = 1;
 
  nic_ts = g_tw_lookahead + (num_chunks * s->params->cn_delay) + tw_rand_unif(lp->rng);

//  codes_mapping_get_lp_info(msg->final_dest_gid, lp_group_name, &mapping_grp_id,
//      NULL, &mapping_type_id, NULL, &mapping_rep_id, &mapping_offset);
//  msg->dest_terminal_id = (mapping_rep_id * (p->switch_radix[0] / 2)) +
//      (mapping_offset % (p->switch_radix[0]/2));
 
/*  //message for process on the same terminal
  if(msg->dest_terminal_id == s->terminal_id) {
    bf->c1 = 1;
    model_net_method_idle_event(nic_ts - s->params->cn_delay * msg->packet_size, 0, lp);
    // Trigger an event on receiving server
    if(msg->remote_event_size_bytes)
    {
      void *tmp_ptr = model_net_method_get_edata(FATTREE, msg);
      bf->c2 = 1;
      ts = codes_local_latency(lp);
      tw_event *e = tw_event_new(msg->final_dest_gid, ts, lp);
      fattree_message* m = tw_event_data(e);
      memcpy(m, tmp_ptr, msg->remote_event_size_bytes);
      //printf("[%d] pack gen Send to %d\n", lp->gid, msg->final_dest_gid);
      tw_event_send(e);
    }
    if(msg->local_event_size_bytes > 0)
    {
      tw_event* e_new;
      fattree_message* m_new;
      void* local_event;
      bf->c3 = 1;
      ts = codes_local_latency(lp); 
      e_new = tw_event_new(msg->sender_lp, ts, lp);
      m_new = tw_event_data(e_new);
      local_event = (char*)model_net_method_get_edata(FATTREE, msg) +
        msg->remote_event_size_bytes;
      memcpy(m_new, local_event, msg->local_event_size_bytes);
      tw_event_send(e_new);
    }
    return;
  }
*/

  msg->packet_ID = lp->gid + g_tw_nlp * s->packet_counter;
//  msg->dest_terminal_id = msg->final_dest_gid;
  if(msg->packet_ID == LLU(TRACK_PKT))
    printf("\n Packet %llu generated at terminal %d terminal_gid %llu dest_terminal_id %llu final_dest_gid %llu size %llu num chunks %llu \n",
       msg->packet_ID, s->terminal_id, lp->gid, LLU(msg->dest_terminal_id), LLU(msg->final_dest_gid),
       LLU(msg->packet_size), LLU(num_chunks));

  for(uint64_t i = 0; i < num_chunks; i++)
  {
    fattree_message_list * cur_chunk = (fattree_message_list *)malloc( 
      sizeof(fattree_message_list));
    msg->origin_switch_id = s->switch_id;
    init_fattree_message_list(cur_chunk, msg);
  
    if(msg->remote_event_size_bytes + msg->local_event_size_bytes > 0) {
      cur_chunk->event_data = (char*)malloc(
        msg->remote_event_size_bytes + msg->local_event_size_bytes);
    }

    void *m_data_src = model_net_method_get_edata(FATTREE, msg);
    if(msg->remote_event_size_bytes){
      memcpy(cur_chunk->event_data, m_data_src, msg->remote_event_size_bytes);
    }
    if(msg->local_event_size_bytes){
      m_data_src = (char*)m_data_src + msg->remote_event_size_bytes;
      memcpy((char*)cur_chunk->event_data + msg->remote_event_size_bytes, 
        m_data_src, msg->local_event_size_bytes);
    }

    cur_chunk->msg.chunk_id = i;
    cur_chunk->msg.origin_switch_id = s->switch_id;
    append_to_fattree_message_list(s->terminal_msgs, s->terminal_msgs_tail, 
      0, cur_chunk);
    s->terminal_length += s->params->chunk_size;
  }

  if(s->terminal_length < 2 * s->params->cn_vc_size) {
    model_net_method_idle_event(nic_ts, 0, lp);
  } else {
    bf->c11 = 1;
    s->issueIdle = 1;
    msg->saved_busy_time = s->last_buf_full;
    s->last_buf_full = tw_now(lp);
  }

  if(s->in_send_loop == 0) {
    fattree_message *m;
    bf->c5 = 1;
    ts = codes_local_latency(lp);
    tw_event* e = model_net_method_event_new(lp->gid, ts, lp, FATTREE, 
      (void**)&m, NULL);
    m->type = T_SEND;
    m->magic = fattree_terminal_magic_num;
    s->in_send_loop = 1;
    tw_event_send(e);
    //printf("[%d] send loop triggered with ts %lf band %lf\n", 
    // lp->gid, ts, s->params->cn_bandwidth);
  }

  total_event_size = model_net_get_msg_sz(FATTREE) +
    msg->remote_event_size_bytes+ msg->local_event_size_bytes;
  mn_stats* stat;
  stat = model_net_find_stats(msg->category, s->fattree_stats_array);
  stat->send_count++;
  stat->send_bytes += msg->packet_size;
  stat->send_time += (1/p->cn_bandwidth) * msg->packet_size;
  if(stat->max_event_size < total_event_size)
    stat->max_event_size = total_event_size;

  return;
}

/* sends the packet from the compute node to the attached switch */
void ft_packet_send(ft_terminal_state * s, tw_bf * bf, fattree_message * msg,
    tw_lp * lp) {

  tw_stime ts;
  tw_event *e;
  fattree_message *m;
 
  fattree_message_list* cur_entry = s->terminal_msgs[0];

  if(s->vc_occupancy + s->params->packet_size > s->params->cn_vc_size ||
    cur_entry == NULL) {
    bf->c1 = 1;
    s->in_send_loop = 0;
    return;
  }

  //  Each packet is broken into chunks and then sent over the channel
  msg->saved_available_time = s->terminal_available_time;
  ts = g_tw_lookahead + s->params->cn_delay * cur_entry->msg.packet_size 
    + g_tw_lookahead * tw_rand_unif(lp->rng);
  s->terminal_available_time = maxd(s->terminal_available_time, tw_now(lp));
  s->terminal_available_time += ts;

  // we are sending an event to the switch, so no method_event here
  ts = s->terminal_available_time - tw_now(lp);

  e = tw_event_new(s->switch_lp, ts, lp);
  m = tw_event_data(e);
  memcpy(m, &cur_entry->msg, sizeof(fattree_message));
  if (m->remote_event_size_bytes){
    memcpy(model_net_method_get_edata(FATTREE, m), cur_entry->event_data,
        m->remote_event_size_bytes);
  }

  m->type = S_ARRIVE;
  m->src_terminal_id = lp->gid;
  m->intm_id = s->terminal_id;
  m->vc_index = 0;
  m->vc_off = 0; //we only have one connection to the terminal NIC
  m->local_event_size_bytes = 0;
  m->last_hop = TERMINAL;
  //printf("[%d] pack send Send to %d\n", lp->gid, s->switch_lp);
  tw_event_send(e);
      
  /* local completion message */
  if(cur_entry->msg.local_event_size_bytes > 0)
  {
    bf->c2 = 1;
    double tsT = codes_local_latency(lp); 
    tw_event *e_new = tw_event_new(cur_entry->msg.sender_lp, tsT, lp);
    fattree_message *m_new = tw_event_data(e_new);
    void *local_event = (char*)cur_entry->event_data + 
                cur_entry->msg.remote_event_size_bytes;
    memcpy(m_new, local_event, cur_entry->msg.local_event_size_bytes);
    tw_event_send(e_new);
  }
   
  s->packet_counter++;
  s->vc_occupancy += s->params->packet_size;
  cur_entry = return_head(s->terminal_msgs, s->terminal_msgs_tail, 0); 
  //delete_fattree_message_list(cur_entry);
  s->terminal_length -= s->params->packet_size;

  cur_entry = s->terminal_msgs[0];

  if(cur_entry != NULL &&
    s->vc_occupancy + s->params->packet_size <= s->params->cn_vc_size) {
    bf->c3 = 1;
    fattree_message *m;
    ts = ts + g_tw_lookahead * tw_rand_unif(lp->rng);
    tw_event* e = model_net_method_event_new(lp->gid, ts, lp, FATTREE, 
      (void**)&m, NULL);
    m->type = T_SEND;
	m->magic = fattree_terminal_magic_num;
    tw_event_send(e);
  } else {
    bf->c4 = 1;
    s->in_send_loop = 0;
  }
  
  if(s->issueIdle) {
    bf->c5 = 1;
    s->issueIdle = 0;
    model_net_method_idle_event(codes_local_latency(lp), 0, lp);
  }

  return;
}

/* Packet arrives at the switch and a credit is sent back to the sending 
 * terminal/switch */
void switch_packet_receive( switch_state * s, tw_bf * bf, 
    fattree_message * msg, tw_lp * lp ) {

  tw_event *e;
  fattree_message *m;
  tw_stime ts;

  //printf("[%d] Switch %d recv packet %d\n", lp->gid, msg->vc_index);
  int output_port = -1, out_off = 0;

  output_port = ft_get_output_port(s, bf, msg, lp, &out_off);
  assert(output_port < s->radix);

  int max_vc_size = s->params->vc_size;
  int to_terminal = 0;

  //If going to terminal, use a different max
  if(s->switch_level == 0 && output_port < s->num_lcons) {
    max_vc_size = s->params->cn_vc_size;
    to_terminal = 1;
  }
 
  int dest_term_local_id = codes_mapping_get_lp_relative_id(msg->dest_terminal_id, 0, 0);

if(msg->packet_ID == LLU(TRACK_PKT))
    printf("\n Packet %llu arrived at switch %d switch_gid %llu dest_terminal_id %llu dest_term_local_id %llu final_dest_gid %llu output_port %d to_terminal %d\n",
       msg->packet_ID, s->switch_id, lp->gid, LLU(msg->dest_terminal_id), LLU(dest_term_local_id), LLU(msg->final_dest_gid),
       output_port, to_terminal);
 
  fattree_message_list * cur_chunk = (fattree_message_list *)malloc( 
      sizeof(fattree_message_list));
  init_fattree_message_list(cur_chunk, msg);
  if(msg->remote_event_size_bytes > 0) 
  {
       void *m_data_src = model_net_method_get_edata(FATTREE, msg);

       cur_chunk->event_data = (char*)malloc(msg->remote_event_size_bytes);
       memcpy(cur_chunk->event_data, m_data_src, 
        msg->remote_event_size_bytes);
  }

  cur_chunk->msg.vc_index = output_port;
  cur_chunk->msg.vc_off = out_off;

  if(s->vc_occupancy[output_port] + s->params->packet_size <= max_vc_size) {
    bf->c1 = 1;
    switch_credit_send(s, bf, msg, lp, -1);
    append_to_fattree_message_list( s->pending_msgs, s->pending_msgs_tail, 
      output_port, cur_chunk);
    s->vc_occupancy[output_port] += s->params->packet_size;
    if(s->in_send_loop[output_port] == 0) {
      bf->c2 = 1;
      fattree_message *m;
      ts = codes_local_latency(lp); 
      tw_event *e = tw_event_new(lp->gid, ts, lp);
      m = tw_event_data(e);
      m->type = S_SEND;
      m->vc_index = output_port;
      //printf("[%d] pack recv Send to %d\n", lp->gid, lp->gid);
      tw_event_send(e);
      s->in_send_loop[output_port] = 1;
    }
  } else {
    bf->c3 = 1;
    cur_chunk->msg.saved_vc = msg->vc_index;
    cur_chunk->msg.saved_off = msg->vc_off;
    append_to_fattree_message_list( s->queued_msgs, s->queued_msgs_tail, 
      output_port, cur_chunk);
    s->queued_length[output_port] += s->params->packet_size;
  }
  //for reverse
  msg->saved_vc = output_port;

  return;
}

/* routes the current packet to the next stop */
void switch_packet_send( switch_state * s, tw_bf * bf, fattree_message * msg,
    tw_lp * lp) {

  tw_stime ts;
  tw_event *e;
  fattree_message *m;

  int output_port = msg->vc_index;
  fattree_message_list *cur_entry = s->pending_msgs[output_port];

  if(cur_entry == NULL) {
    bf->c1 = 1;
    s->in_send_loop[output_port] = 0;
    return;
  }
  
  tw_lpid next_stop = s->port_connections[output_port];
  int to_terminal = 0;
  tw_stime delay = s->params->head_delay;

  // dest can be a switch or a terminal, so we must check
  if(s->switch_level == 0 && output_port < s->num_lcons) {
    to_terminal = 1;
    delay = s->params->cn_delay;
  }

  ts = s->params->router_delay;
  double bytetime = delay * cur_entry->msg.packet_size;
  ts = g_tw_lookahead + g_tw_lookahead * tw_rand_unif( lp->rng) + bytetime + ts;

  msg->saved_available_time = s->next_output_available_time[output_port];
  s->next_output_available_time[output_port] = 
    maxd(s->next_output_available_time[output_port], tw_now(lp));
  s->next_output_available_time[output_port] += ts;

  ts = s->next_output_available_time[output_port] - tw_now(lp);
  void * m_data;
  if (to_terminal) {
    e = model_net_method_event_new(next_stop, ts, lp,
        FATTREE, (void**)&m, &m_data);
  } else {
      e = tw_event_new(next_stop, ts, lp);
      m = tw_event_data(e);
      m_data = model_net_method_get_edata(FATTREE, m);
  }

  memcpy(m, &cur_entry->msg, sizeof(fattree_message));
  if (m->remote_event_size_bytes){
      memcpy(m_data, cur_entry->event_data, m->remote_event_size_bytes);
  }

  m->last_hop = LINK;
  m->intm_lp_id = lp->gid;
  m->intm_id = s->switch_id;
  s->link_traffic[output_port] += cur_entry->msg.packet_size;

  /* Determine the event type. If the packet has arrived at the final destination
     switch then it should arrive at the destination terminal next. */
  if(to_terminal) {
    m->type = T_ARRIVE;
  } else {
    /* The packet has to be sent to another switch */
    m->type = S_ARRIVE;
  }
  tw_event_send(e);
  
  cur_entry = return_head(s->pending_msgs, s->pending_msgs_tail, 
    output_port);
  //delete_fattree_message_list(cur_entry);
  rc_stack_push(lp, cur_entry, free, s->st);    

  if(bytetime > s->params->router_delay) {
    s->next_output_available_time[output_port] -= s->params->router_delay;
    ts -= s->params->router_delay;
  } else {
    s->next_output_available_time[output_port] -= bytetime;
    ts -= bytetime;
  }

  cur_entry = s->pending_msgs[output_port];
  if(cur_entry != NULL) {
    bf->c3 = 1;
    fattree_message *m;
    ts = ts + g_tw_lookahead * tw_rand_unif(lp->rng);
    tw_event *e = tw_event_new(lp->gid, ts, lp);
    m = tw_event_data(e);
    m->type = S_SEND;
    m->vc_index = output_port;
    //printf("[%d] switch send loop Send to %d\n", lp->gid, lp->gid);
    tw_event_send(e);
  } else {
    bf->c4 = 1;
    s->in_send_loop[output_port] = 0;
  }
  return;
}

/* When a packet is sent from the current switch and a buffer slot 
 * becomes available, a credit is sent back to schedule another packet 
 * event */
void switch_credit_send(switch_state * s, tw_bf * bf, fattree_message * msg,
    tw_lp * lp, int sq) {

  tw_event * buf_e;
  tw_stime ts;
  fattree_message * buf_msg;

  int dest = 0, type = S_BUFFER;
  int is_terminal = 0;

  fattree_param *p = s->params;
  // Notify sender terminal about available buffer space
  if(msg->last_hop == TERMINAL) {
    dest = msg->src_terminal_id;
    type = T_BUFFER;
    is_terminal = 1;
  } else if(msg->last_hop == LINK) {
    dest = msg->intm_lp_id;
  } 

  // Assume it takes 0.1 ns of serialization latency for processing the 
  // credits in the queue
  //int output_port = msg->vc_off; //src used this offset, so I have to
  //if(sq == 1) {
  //  output_port = msg->saved_off;
  //}
  //output_port += get_base_port(s, is_terminal, msg->intm_id);

  ts = g_tw_lookahead + s->params->credit_delay + g_tw_lookahead * tw_rand_unif(lp->rng);
	
  if (is_terminal) {
    buf_e = model_net_method_event_new(dest, ts, lp, FATTREE, 
      (void**)&buf_msg, NULL);
	buf_msg->magic = fattree_terminal_magic_num;
  } else {
    buf_e = tw_event_new(dest, ts , lp);
    buf_msg = tw_event_data(buf_e);
  }

  buf_msg->type = type;

  if(sq == 1) {
    buf_msg->vc_index = msg->saved_vc;
  } else {
    buf_msg->vc_index = msg->vc_index; //the port src used to send me this data
  }

  //printf("[%d] credit send Send to %d\n", lp->gid, dest);
  tw_event_send(buf_e);
  return;
}

/* update the compute node-switch channel buffer */
void ft_terminal_buf_update(ft_terminal_state * s, tw_bf * bf,
    fattree_message * msg, tw_lp * lp) {
  s->vc_occupancy -= s->params->packet_size;
  if(s->in_send_loop == 0 && s->terminal_msgs[0] != NULL) {
    fattree_message *m;
    bf->c1 = 1;
    tw_stime ts = codes_local_latency(lp);
    tw_event* e = model_net_method_event_new(lp->gid, ts, lp, FATTREE, 
        (void**)&m, NULL);
    m->type = T_SEND;
	m->type = fattree_terminal_magic_num;
    s->in_send_loop = 1;
    //printf("[%d] term buf Send to %d\n", lp->gid, lp->gid);
    tw_event_send(e);
  }
  return;
}

void switch_buf_update(switch_state * s, tw_bf * bf, fattree_message * msg, 
  tw_lp * lp) {
  int indx = msg->vc_index;
  s->vc_occupancy[indx] -= s->params->packet_size;

  if(s->queued_msgs[indx] != NULL) {
    bf->c1 = 1;
    fattree_message_list *head = return_head( s->queued_msgs,
        s->queued_msgs_tail, indx);
    s->queued_length[indx] -= s->params->packet_size;
    switch_credit_send( s, bf,  &head->msg, lp, 1); 
    append_to_fattree_message_list( s->pending_msgs, s->pending_msgs_tail, 
      indx, head);
    s->vc_occupancy[indx] += s->params->packet_size;
  }

  if(s->in_send_loop[indx] == 0 && s->pending_msgs[indx] != NULL) {
    bf->c2 = 1;
    fattree_message *m;
    tw_stime ts = codes_local_latency(lp);
    tw_event *e = tw_event_new(lp->gid, ts, lp);
    m = tw_event_data(e);
    m->type = S_SEND;
    m->vc_index = indx;
    s->in_send_loop[indx] = 1;
    //printf("[%d] switch buf Send to %d\n", lp->gid, lp->gid);
    tw_event_send(e);
  }
  return;
}

void ft_send_remote_event(ft_terminal_state * s, fattree_message * msg, tw_lp * lp, tw_bf * bf, char * event_data, int remote_event_size)
{
        void * tmp_ptr = model_net_method_get_edata(FATTREE, msg);

        tw_stime ts = g_tw_lookahead + bytes_to_ns(msg->remote_event_size_bytes, (1/s->params->cn_bandwidth));

        if (msg->is_pull){
            bf->c4 = 1;
            struct codes_mctx mc_dst =
                codes_mctx_set_global_direct(msg->sender_mn_lp);
            struct codes_mctx mc_src =
                codes_mctx_set_global_direct(lp->gid);
            int net_id = model_net_get_id(LP_METHOD_NM);

            model_net_set_msg_param(MN_MSG_PARAM_START_TIME, MN_MSG_PARAM_START_TIME_VAL, &(msg->msg_start_time));

            msg->event_rc = model_net_event_mctx(net_id, &mc_src, &mc_dst, msg->category,
                    msg->sender_lp, msg->pull_size, ts,
                    remote_event_size, tmp_ptr, 0, NULL, lp);
        }
        else{
            tw_event * e = tw_event_new(msg->final_dest_gid, ts, lp);
            if(msg->packet_ID == LLU(TRACK_PKT))
//                printf("\n Packet %llu arrived at lp %llu\n", msg->packet_ID, LLU(lp->gid));
                printf("lp:%d sending pscket %llu remote event to model-net final_dest_gid:%d\n",(int)lp->gid, msg->packet_ID, (int)msg->final_dest_gid);
            void * m_remote = tw_event_data(e);
            memcpy(m_remote, event_data, remote_event_size);
            tw_event_send(e);
        }
    return;
}


/* packet arrives at the destination terminal */
void ft_packet_arrive(ft_terminal_state * s, tw_bf * bf, fattree_message * msg,
    tw_lp * lp) {

  //Establish msg hash keys
  struct ftree_hash_key key;
  key.message_id = msg->message_id; 
  key.sender_id = msg->sender_lp;
  //Compute total number of chuncks to expect for the message
  uint64_t total_chunks = msg->total_size / s->params->chunk_size;
  //If total chunks doesn't divid evenly then add one extra for left over
  if(msg->total_size % s->params->chunk_size)
          total_chunks++;
  //If no chunks (sending msgs as whole) then set to 1
  if(!total_chunks)
          total_chunks = 1;
// printf("packet:%llu arrived at destination terminal lp->gid:%d msg->dest_terminal_id:%d\n",msg->packet_ID, (int)lp->gid, (int)msg->dest_terminal_id); 
  assert(lp->gid == msg->dest_terminal_id);

if(msg->packet_ID == LLU(TRACK_PKT))
        printf("\n Packet %llu arrived at lp %llu\n", msg->packet_ID, LLU(lp->gid));

//  if(msg->packet_ID == LLU(TRACK_PKT))
//        printf("\n Packet %llu arrived at lp %llu hops %d", msg->packet_ID, LLU(lp->gid), msg->my_N_hop);

  // Packet arrives and accumulate # queued
  // Find a queue with an empty buffer slot
  tw_event * e, * buf_e;
  fattree_message * m, * buf_msg;
  tw_stime ts;

  // NIC aggregation - should this be a separate function?
/*  uint64_t recvSize;
  int eventSize;
  char *data;
  void *tmp_ptr = model_net_method_get_edata(FATTREE, msg);
  int used = addMsgInfo(msg->src_nic, msg->uniq_id, msg->packet_size, 
      msg->remote_event_size_bytes, tmp_ptr);
  getMsgInfo(msg->src_nic, msg->uniq_id, &recvSize, &eventSize, &data);
  // Trigger an event on receiving server
  if(used) bf->c1 = 1;

  if(recvSize >= msg->msg_size && eventSize > 0) {
    bf->c2 = 1;
    void * tmp_ptr = model_net_method_get_edata(FATTREE, msg);
    ts = g_tw_lookahead + g_tw_lookahead * tw_rand_unif(lp->rng) +  
      s->params->cn_delay * eventSize;
    e = tw_event_new(msg->final_dest_gid, ts, lp);
    m = tw_event_data(e);
    memcpy(m, data, eventSize);
    tw_event_send(e); 
    if(!used) {
      msg->remote_event_size_bytes = eventSize;
      memcpy(tmp_ptr, data, eventSize);
    }
    msg->saved_size = recvSize;
    deleteMsgInfo(msg->src_nic, msg->uniq_id);
  }
*/
  ts = g_tw_lookahead + s->params->credit_delay + g_tw_lookahead * tw_rand_unif(lp->rng);
  
  // no method_event here - message going to switch
  buf_e = tw_event_new(s->switch_lp, ts, lp);
  buf_msg = tw_event_data(buf_e);
  buf_msg->magic = switch_magic_num;
  buf_msg->vc_index = msg->vc_index;
  buf_msg->type = S_BUFFER;
  //printf("[%d] pack arrive credit Send to %d\n", lp->gid, s->switch_lp);
  tw_event_send(buf_e);

  bf->c1 = 0;
  bf->c3 = 0;
  bf->c4 = 0;
  bf->c7 = 0;

  /* Total overall finished chunks in simulation */
  N_finished_chunks++;
  /* Finished chunks on a LP basis */
  s->finished_chunks++;
  /* Finished chunks per sample */
  s->fin_chunks_sample++;

   s->data_size_sample += msg->total_size;
  /* WE do not allow self messages through fattree */
  assert(lp->gid != msg->src_terminal_id);

  uint64_t num_chunks = msg->packet_size / s->params->chunk_size;
  if (msg->packet_size % s->params->chunk_size)
    num_chunks++;

  if(!num_chunks)
     num_chunks = 1;

  if(msg->chunk_id == num_chunks - 1)
  {
    bf->c31 = 1;
    s->packet_fin++;
    fattree_packet_fin++;
  }

  /* save the sample time */
    msg->saved_sample_time = s->fin_chunks_time;
    s->fin_chunks_time += (tw_now(lp) - msg->travel_start_time);
    
    /* save the total time per LP */
    msg->saved_avg_time = s->total_time;
    s->total_time += (tw_now(lp) - msg->travel_start_time); 

    msg->saved_total_time = fattree_total_time;
    fattree_total_time += tw_now( lp ) - msg->travel_start_time;
    total_hops += msg->my_N_hop;
    s->total_hops += msg->my_N_hop;
    s->fin_hops_sample += msg->my_N_hop;

    mn_stats* stat = model_net_find_stats(msg->category, s->fattree_stats_array);
    msg->saved_rcv_time = stat->recv_time;
    stat->recv_time += (tw_now(lp) - msg->travel_start_time);

#if DEBUG == 1
 if( msg->packet_ID == TRACK 
          && msg->chunk_id == num_chunks-1
          && msg->message_id == TRACK_MSG)
  {
    printf( "(%lf) [Terminal %d] packet %lld has arrived  \n",
        tw_now(lp), (int)lp->gid, msg->packet_ID);

    printf("travel start time is %f\n",
        msg->travel_start_time);

    printf("My hop now is %d\n",msg->my_N_hop);
  }
#endif

/* Now retreieve the number of chunks completed from the hash and update
    * them */
   void *m_data_src = model_net_method_get_edata(FATTREE, msg);
    
   struct qhash_head *hash_link = NULL;
   struct ftree_qhash_entry * tmp = NULL;
      
   hash_link = qhash_search(s->rank_tbl, &key);
    
   /* If an entry does not exist then create one */
   if(!hash_link)
   {
       bf->c5 = 1;
       struct ftree_qhash_entry * d_entry = malloc(sizeof (struct ftree_qhash_entry));
       d_entry->num_chunks = 0;
       d_entry->key = key;
       d_entry->remote_event_data = NULL;
       d_entry->remote_event_size = 0;
       qhash_add(s->rank_tbl, &key, &(d_entry->hash_link));
       s->rank_tbl_pop++;
      
       hash_link = &(d_entry->hash_link);
   }
    
    if(hash_link)
        tmp = qhash_entry(hash_link, struct ftree_qhash_entry, hash_link);

    assert(tmp);
    tmp->num_chunks++;

    // If it's the last chunk of the packet then collect statistics 
    if(msg->chunk_id == num_chunks - 1)
    {
        bf->c1 = 1;
        stat->recv_count++;
        stat->recv_bytes += msg->packet_size;

        N_finished_packets++;
        s->finished_packets++;
    }
    // If it's the main chunk of the packet then handle the remote event data
    if(msg->remote_event_size_bytes > 0 && !tmp->remote_event_data)
    {
        /* Retreive the remote event entry */
         tmp->remote_event_data = (void*)malloc(msg->remote_event_size_bytes);
         assert(tmp->remote_event_data);
         tmp->remote_event_size = msg->remote_event_size_bytes; 
         memcpy(tmp->remote_event_data, m_data_src, msg->remote_event_size_bytes);
    }
    if (fattree_max_latency < tw_now( lp ) - msg->travel_start_time) 
    {
         bf->c3 = 1;
         msg->saved_available_time = fattree_max_latency;
         fattree_max_latency = tw_now( lp ) - msg->travel_start_time;
    }
    /* If all chunks of a message have arrived then send a remote event to the
     * callee*/
    /*if(tmp->num_chunks >= total_chunks || tmp->num_chunks < 0)
        {
           //tw_output(lp, "\n invalid number of chunks %d for LP %ld ", tmp->num_chunks, lp->gid);
           tw_lp_suspend(lp, 0, 0);
           return;
        }*/

    if(tmp->num_chunks >= total_chunks)
    {
        bf->c7 = 1;

        N_finished_msgs++;
        total_msg_sz += msg->total_size;
        s->total_msg_size += msg->total_size;
        s->finished_msgs++;
       
        
	if(msg->packet_ID == LLU(TRACK_PKT))
            printf("\n Packet %llu has been sent from lp %llu\n", msg->packet_ID, LLU(lp->gid));
 
        //assert(tmp->remote_event_data && tmp->remote_event_size > 0);
        ft_send_remote_event(s, msg, lp, bf, tmp->remote_event_data, tmp->remote_event_size);
        /* Remove the hash entry */
        qhash_del(hash_link);
        rc_stack_push(lp, tmp, free_tmp, s->st);
        s->rank_tbl_pop--;
   }

  return;
}

/* gets the output port corresponding to the next stop of the message */
/* expects dest_terminal_id to be a local ID not global ID */
int ft_get_output_port( switch_state * s, tw_bf * bf, fattree_message * msg,
    tw_lp * lp, int *out_off) {

  int outport = -1;
  int start_port, end_port;
  fattree_param *p = s->params;

  int dest_term_local_id = codes_mapping_get_lp_relative_id(msg->dest_terminal_id, 0, 0);

  if(s->switch_level == 0) {
    //message for a terminal node
    if(dest_term_local_id >= s->start_lneigh && dest_term_local_id < s->end_lneigh) {
      outport = dest_term_local_id - s->start_lneigh;
      *out_off = 0;
      return outport;
    } else { //go up the least congested path
      start_port = s->num_lcons;
      end_port = s->num_cons;
    }
  } else if(s->switch_level == 1) {
    int dest_switch_id = dest_term_local_id / (p->switch_radix[0] / 2);
    //if only two level or packet going down, send to the right switch
    if(p->num_levels == 2 || (dest_switch_id >= s->start_lneigh && 
      dest_switch_id < s->end_lneigh)) {
      start_port = (dest_switch_id - s->start_lneigh) * s->con_per_lneigh;
      end_port = start_port + s->con_per_lneigh;
    } else {
      start_port = s->num_lcons;
      end_port = s->num_cons;
    }
  } else { //switch level 2
    int dest_l1_group = dest_term_local_id / p->l1_term_size;
    if(s->params->ft_type == 0) {
      start_port = dest_l1_group * (p->l1_set_size/2) * s->con_per_lneigh;
      end_port = start_port + ((p->l1_set_size/2) * s->con_per_lneigh);
    } else {
      start_port = dest_l1_group * s->con_per_lneigh;
      end_port = start_port + s->con_per_lneigh;
    }
  }

  assert(end_port > start_port);

  outport = start_port;
  int load = s->vc_occupancy[outport] + s->queued_length[outport];
  if(load != 0) {
    for(int port = start_port + 1; port < end_port; port++) {
      if(s->vc_occupancy[port] +  s->queued_length[port] < load) {
        load = s->vc_occupancy[port] +  s->queued_length[port];
        outport = port;
        if(load <= 0) break;
      }
    }
  }
  assert(outport != -1);
  if(outport < s->num_lcons) {
    *out_off = outport % s->con_per_lneigh;
  } else {
    *out_off = (outport - s->num_lcons) % s->con_per_uneigh;
  }
  return outport;
}

/* Currently incomplete. */
int get_base_port(switch_state *s, int from_term, int index) {
  int return_port;
  if(s->switch_level == 2) {
  } else if(from_term || index < s->switch_id) {
    return_port = ((index - s->start_lneigh) * s->con_per_lneigh);
  } else {
    return_port = s->num_lcons;
    return_port += ((index - s->start_uneigh) * s->con_per_uneigh);
  }
  return return_port;
}

void ft_terminal_event( ft_terminal_state * s, tw_bf * bf, fattree_message * msg,
		tw_lp * lp ) {

  assert(msg->magic == fattree_terminal_magic_num);
  *(int *)bf = (int)0;
  switch(msg->type) {

    case T_GENERATE:
      ft_packet_generate(s, bf, msg, lp);
      break;

    case T_ARRIVE:
      ft_packet_arrive(s, bf, msg, lp);
      break;

    case T_SEND:
      ft_packet_send(s, bf, msg, lp);
      break;

    case T_BUFFER:
      ft_terminal_buf_update(s, bf, msg, lp);
      break;

    default:
      printf("\n LP %d Terminal message type not supported %d ", 
        (int)lp->gid, msg->type);
      tw_error(TW_LOC, "Msg type not supported");
  }
}

void fattree_terminal_final( ft_terminal_state * s, tw_lp * lp ) 
{ 
    model_net_print_stats(lp->gid, s->fattree_stats_array);
   
    //if(s->packet_gen != s->packet_fin)
    //    printf("\n generated %d finished %d ", s->packet_gen, s->packet_fin);
    
    qhash_finalize(s->rank_tbl);
    rc_stack_destroy(s->st);
//    free(s->vc_occupancy);
    free(s->terminal_msgs);
    free(s->terminal_msgs_tail);
//    free(s->children); 
}

void fattree_switch_final(switch_state * s, tw_lp * lp) {
  if(s->unused) return;

  rc_stack_destroy(s->st);

  char *stats_file = getenv("TRACER_LINK_FILE");
  if(stats_file != NULL) {
    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    char file_name[512];
    sprintf(file_name, "%s.%d", stats_file, rank);
    FILE *fout = fopen(file_name, "a");
    fattree_param *p = s->params;
    //int result = flock(fileno(fout), LOCK_EX);
    fprintf(fout, "%d %d ", s->switch_id, s->switch_level);
    for(int d = 0; d < s->num_cons; d++) {
      fprintf(fout, "%lld ", s->link_traffic[d]);
    }
    fprintf(fout, "\n");
    //result = flock(fileno(fout), LOCK_UN);
    fclose(fout);
  }
}

/* Update the buffer space associated with this switch LP */
void switch_event(switch_state * s, tw_bf * bf, fattree_message * msg,
    tw_lp * lp) {

  *(int *)bf = (int)0;
  switch(msg->type) {

    case S_SEND: 
      switch_packet_send(s, bf, msg, lp);
      break;

    case S_ARRIVE: 
      switch_packet_receive(s, bf, msg, lp);
      break;

    case S_BUFFER:
      switch_buf_update(s, bf, msg, lp);
      break;

    default:
      printf("\n (%lf) [Switch %d] Switch Message type not supported %d " 
        "dest terminal id %d packet ID %d ", tw_now(lp), (int)lp->gid, 
        msg->type, (int)msg->dest_terminal_id, (int)msg->packet_ID);
      tw_error(TW_LOC, "Msg type not supported in switch");
      break;
  }	   
}

/* Reverse computation handler for a terminal event */
void ft_terminal_rc_event_handler(ft_terminal_state * s, tw_bf * bf,
    fattree_message * msg, tw_lp * lp) {

  switch(msg->type) {

    case T_GENERATE:
      {
        tw_rand_reverse_unif(lp->rng);
        if( bf->c1 == 1 ) {
          if(bf->c2) {
            codes_local_latency_reverse(lp);
          }
          if(bf->c3) {
            codes_local_latency_reverse(lp);
          }
          break;
        }
        delete_fattree_message_list(return_tail(s->terminal_msgs, 
          s->terminal_msgs_tail, 0));
        s->terminal_length -= s->params->packet_size;
        if(bf->c11) {
          s->issueIdle = 0;
        }
        if(bf->c5) {
          tw_rand_reverse_unif(lp->rng);
          s->in_send_loop = 0;
        }
      }
      break;

    case T_SEND:
      {
        if(bf->c1) {
          s->in_send_loop = 1;
          break;
        }
        s->terminal_available_time = msg->saved_available_time;
        tw_rand_reverse_unif(lp->rng);
        if(bf->c2) {
          codes_local_latency_reverse(lp);
        }
        s->packet_counter--;
        s->vc_occupancy -= s->params->packet_size;
        
	fattree_message_list* cur_entry = rc_stack_pop(s->st);	

        prepend_to_fattree_message_list(s->terminal_msgs,
            s->terminal_msgs_tail, 0, cur_entry);
        s->terminal_length += s->params->packet_size;
        if(bf->c3) {
          tw_rand_reverse_unif(lp->rng);
        }
        if(bf->c4) {
          s->in_send_loop = 1;
        }
        if(bf->c5) {
          codes_local_latency_reverse(lp);
          s->issueIdle = 1;
        }
      }
      break;

    case T_ARRIVE:
      {
        if(bf->c2) {
          tw_rand_reverse_unif(lp->rng);
          void *tmp_ptr = model_net_method_get_edata(FATTREE, msg);
          if(msg->saved_size -  msg->packet_size) {
//            addMsgInfo(msg->src_nic, msg->uniq_id, 
//                msg->saved_size -  msg->packet_size, 
//                msg->remote_event_size_bytes, tmp_ptr);
          }
        } else {
          int unset = 0;
          if(bf->c1) unset = 1;
//          decreaseMsgInfo(msg->src_nic, msg->uniq_id, 
//              msg->packet_size, unset);
        }


        struct qhash_head * hash_link = NULL;
        struct ftree_qhash_entry * tmp = NULL; 
      
        struct ftree_hash_key key;
        key.message_id = msg->message_id;
        key.sender_id = msg->sender_lp;
	
        hash_link = qhash_search(s->rank_tbl, &key);
        tmp = qhash_entry(hash_link, struct ftree_qhash_entry, hash_link);

        if(bf->c7)
        {
            N_finished_msgs--;
            s->finished_msgs--;
            total_msg_sz -= msg->total_size;
            s->total_msg_size -= msg->total_size;

            struct ftree_qhash_entry * d_entry_pop = rc_stack_pop(s->st);
            qhash_add(s->rank_tbl, &key, &(d_entry_pop->hash_link));
            s->rank_tbl_pop++; 

            hash_link = &(d_entry_pop->hash_link);
            tmp = d_entry_pop; 

            if(bf->c4)
                model_net_event_rc2(lp, &msg->event_rc);
        }
        tw_rand_reverse_unif(lp->rng);
      }
      break;

    case T_BUFFER:
      {
        s->vc_occupancy += s->params->packet_size;
        if(bf->c1) {
          codes_local_latency_reverse(lp);
          s->in_send_loop = 0;
        }
      }  
      break;
  }
}

/* Reverse computation handler for a switch event */
void switch_rc_event_handler(switch_state * s, tw_bf * bf,
    fattree_message * msg, tw_lp * lp) {

  switch(msg->type) {
    case S_SEND:
      {
        int output_port = msg->vc_index;
        if(bf->c1) {
          s->in_send_loop[output_port] = 1;
          break;
        }
        tw_rand_reverse_unif(lp->rng);
        s->next_output_available_time[output_port] = 
          msg->saved_available_time;
        s->link_traffic[output_port] -= msg->packet_size;

	fattree_message_list * cur_entry = rc_stack_pop(s->st);
	assert(cur_entry);

        prepend_to_fattree_message_list(s->pending_msgs,
            s->pending_msgs_tail, output_port, cur_entry);
        if(bf->c3) {
          tw_rand_reverse_unif(lp->rng);
        }
        if(bf->c4) {
          s->in_send_loop[output_port] = 1;
        }
      }
      break;

    case S_ARRIVE:
      {
        int output_port = msg->saved_vc;
        if(bf->c1) {
          tw_rand_reverse_unif(lp->rng);
          delete_fattree_message_list(return_tail(s->pending_msgs, 
                s->pending_msgs_tail, output_port));
          s->vc_occupancy[output_port] -= s->params->packet_size;
          if(bf->c2) {
            codes_local_latency_reverse(lp);
            s->in_send_loop[output_port] = 0;
          }
        }
        if(bf->c3) {
          delete_fattree_message_list(return_tail(s->queued_msgs, 
                s->queued_msgs_tail, output_port));
          s->queued_length[output_port] -= s->params->packet_size;
        }

      }
      break;

    case S_BUFFER:
      {
        int indx = msg->vc_index;
        s->vc_occupancy[indx] += s->params->packet_size;
        if(bf->c1) {
          fattree_message_list* head = return_tail(s->pending_msgs,
            s->pending_msgs_tail, indx);
          tw_rand_reverse_unif(lp->rng);
          prepend_to_fattree_message_list(s->queued_msgs, 
            s->queued_msgs_tail, indx, head);
          s->vc_occupancy[indx] -= s->params->packet_size;
          s->queued_length[indx] += s->params->packet_size;
        }
        if(bf->c2) {
          codes_local_latency_reverse(lp);
          s->in_send_loop[indx] = 0;
        }
      }
      break;

  }
}
/* fattree compute node and switch LP types */
tw_lptype fattree_lps[] =
{
  // Terminal handling functions
  {
    (init_f)ft_terminal_init,
    (pre_run_f) NULL,
    (event_f) ft_terminal_event,
    (revent_f) ft_terminal_rc_event_handler,
//    (commit_f) NULL,
    (final_f) fattree_terminal_final,
    (map_f) codes_mapping,
    sizeof(ft_terminal_state)
  },
  {
    (init_f) switch_init,
    (pre_run_f) NULL,
    (event_f) switch_event,
    (revent_f) switch_rc_event_handler,
    (final_f) fattree_switch_final,
    (map_f) codes_mapping,
    sizeof(switch_state),
  },
  {0},
};

/* returns the fattree lp type for lp registration */
static const tw_lptype* fattree_get_cn_lp_type(void)
{
  return(&fattree_lps[0]);
}
static const tw_lptype* fattree_get_switch_lp_type(void)
{
  return(&fattree_lps[1]);
}          

static void fattree_register(tw_lptype *base_type) {
    lp_type_register(LP_CONFIG_NM, base_type);
    lp_type_register("fattree_switch", &fattree_lps[1]);
}

struct model_net_method fattree_method =
{
  .mn_configure = fattree_configure,
  .mn_register = fattree_register,
  .model_net_method_packet_event = fattree_packet_event,
  .model_net_method_packet_event_rc = fattree_packet_event_rc,
  .model_net_method_recv_msg_event = NULL,
  .model_net_method_recv_msg_event_rc = NULL,
  .mn_get_lp_type = fattree_get_cn_lp_type,
  .mn_get_msg_sz = fattree_get_msg_sz,
  .mn_report_stats = fattree_report_stats,
//  .model_net_method_find_local_device = NULL,
  .mn_collective_call = NULL,
  .mn_collective_call_rc = NULL
};

