#include <ross.h>

#include "codes/jenkins-hash.h"
#include "codes/codes_mapping.h"
#include "codes/codes.h"
#include "codes/model-net.h"
#include "codes/model-net-method.h"
#include "codes/model-net-lp.h"

//CHANGE: use the network file created
#include "codes/net/express-mesh.h"

#include "codes/net/common-net.h"
#include "sys/file.h"
#include "codes/quickhash.h"
#include "codes/rc-stack.h"
#include <vector>

#define CREDIT_SZ 8
#define HASH_TABLE_SIZE 262144
#define MULT_FACTOR 2

#define DEBUG 0
#define MAX_STATS 65536

//CHANGE: define them for the local network
#define LOCAL_NETWORK_NAME EXPRESS_MESH
#define LOCAL_NETWORK_ROUTER_NAME EXPRESS_MESH_ROUTER
#define LOCAL_MSG_STRUCT em_message
#define LOCAL_MSG_NAME_FROM_UNION em_msg

#define LP_CONFIG_NM_TERM (model_net_lp_config_names[LOCAL_NETWORK_NAME])
#define LP_METHOD_NM_TERM (model_net_method_names[LOCAL_NETWORK_NAME])
#define LP_CONFIG_NM_ROUT (model_net_lp_config_names[LOCAL_NETWORK_ROUTER_NAME])
#define LP_METHOD_NM_ROUT (model_net_method_names[LOCAL_NETWORK_ROUTER_NAME])

static long packet_gen = 0, packet_fin = 0;

static double maxd(double a, double b) { return a < b ? b : a; }

typedef struct local_param local_param;
static uint64_t                  num_params = 0;
static local_param         * all_params = NULL;
static const config_anno_map_t * anno_map   = NULL;

/* global variables for codes mapping */
static char lp_group_name[MAX_NAME_LENGTH];
static int mapping_grp_id, mapping_type_id, mapping_rep_id, mapping_offset;

/* router magic number */
static int router_magic_num = 0;

/* terminal magic number */
static int terminal_magic_num = 0;

static int sample_bytes_written = 0;
static int sample_rtr_bytes_written = 0;

static char local_cn_sample_file[MAX_NAME_LENGTH];
static char local_rtr_sample_file[MAX_NAME_LENGTH];

static void init_message_list(message_list *thism,
    LOCAL_MSG_STRUCT *inmsg) {
  thism->LOCAL_MSG_NAME_FROM_UNION = *inmsg;
  thism->event_data = NULL;
  thism->next = NULL;
  thism->prev = NULL;
  thism->in_alt_q = 0;
  thism->altq_next = NULL;
  thism->altq_prev = NULL;
}

struct local_param
{
  double link_bandwidth;/* bandwidth of each link */
  double cn_bandwidth;/* injection bandwidth */
  int num_cn; // number of nodes per router
  int num_vcs; /* number of virtual channels */
  int vc_size; /* buffer size of the router-router channels */
  int cn_vc_size; /* buffer size of the compute node channels */
  int chunk_size; /* full-sized packets are broken into smaller chunks.*/
  int router_delay; /* delay at each router */
  int routing; /* type of routing */

  //derived param
  int radix; /* radix of the routers */
  int total_routers; /* how many routers in the system */
  int total_terminals; /* how many terminals in the system */
  double cn_delay; /* bandwidth based time for 1 byte */
  double link_delay; /* bandwidth based time for 1 byte */
  double credit_delay; /* how long for credit to arrive - all bytes */

  //CHANGE: add network specific data here
  int n_dims; // Dimensions in the base torus layout
  int *dim_length;
  int gap; // Gap at which nodes are connected (0 for log)
  int * factor; /* used in torus coordinate calculation */
  int *cons_per_dim, *offset_per_dim;
};

struct local_router_sample
{
  tw_lpid router_id;
  tw_stime* busy_time;
  int64_t* link_traffic_sample;
  tw_stime end_time;
  long fwd_events;
  long rev_events;
};

struct local_cn_sample
{
  tw_lpid terminal_id;
  long fin_chunks_sample;
  long data_size_sample;
  double fin_hops_sample;
  tw_stime fin_chunks_time;
  tw_stime busy_time_sample;
  tw_stime end_time;
  long fwd_events;
  long rev_events;
};

/* handles terminal and router events like packet generate/send/receive/buffer */
typedef struct terminal_state terminal_state;
typedef struct router_state router_state;

/* compute node data (think NIC) structure */
struct terminal_state
{
  unsigned int terminal_id; //what is my local id
  const char * anno;
  const local_param *params;

  //which router I am connected to
  unsigned int router_id;
  tw_lpid router_gid;

  // Each terminal will have input/output channel(s) with the router
  int** vc_occupancy; // NUM_VC
  tw_stime terminal_available_time;

  //available messages to be sent
  message_list ***terminal_msgs;
  message_list ***terminal_msgs_tail;
  int terminal_length;
  int in_send_loop;
  int issueIdle;

  //packet aggregation
  struct qhash_table *rank_tbl;
  //transient storage for reverse computation
  struct rc_stack * st;

  //stats collection
  uint64_t packet_counter;
  int packet_gen;
  int packet_fin;
  struct mn_stats local_stats_array[CATEGORY_MAX];
  tw_stime   total_time;
  uint64_t total_msg_size;
  double total_hops;
  long finished_msgs;
  long finished_chunks;
  long finished_packets;

  //sampling
  tw_stime last_buf_full;
  tw_stime busy_time;
  char output_buf[4096];
  long fin_chunks_sample;
  long data_size_sample;
  double fin_hops_sample;
  tw_stime fin_chunks_time;
  tw_stime busy_time_sample;
  char sample_buf[4096];
  struct local_cn_sample * sample_stat;
  int op_arr_size;
  int max_arr_size;
  /* for logging forward and reverse events */
  long fwd_events;
  long rev_events;
};

//CHANGE: may need to change if more functionality is desired
/* event types */
enum event_t
{
  T_GENERATE=1,
  T_ARRIVE,
  T_SEND,
  T_BUFFER,
  R_SEND,
  R_ARRIVE,
  R_BUFFER,
};
typedef enum event_t event_t;

//CHANGE: may need to change if more functionality is desired
/* whether the last hop of a packet was global, local or a terminal */
enum last_hop
{
  ROUTER=1,
  TERMINAL
};

//CHANGE: may need to change if more functionality is desired
enum ROUTING_ALGO
{
  STATIC = 0,
  ADAPTIVE,
};

struct router_state
{
  //who am I
  unsigned int router_id;
  const char * anno;
  const local_param *params;

  //CHANGE: may need to be changed if linear storage is not desired
  //array/linked list based storage of info about ports/vcs
  tw_lpid* link_connections;
  tw_stime* next_output_available_time;
  message_list ***pending_msgs;
  message_list ***pending_msgs_tail;
  message_list ***queued_msgs;
  message_list ***queued_msgs_tail;
  int** vc_occupancy;
  int *in_send_loop;
  int *queued_count;

  //for reverse computation
  struct rc_stack * st;

  //sampling and stats
  int64_t* link_traffic;
  tw_stime* last_buf_full;
  tw_stime* busy_time;
  tw_stime* busy_time_sample;
  struct local_router_sample * rsamples;
  int op_arr_size;
  int max_arr_size;
  long fwd_events, rev_events;
  int64_t * link_traffic_sample;
  char output_buf[4096];
  char output_buf2[4096];

  //CHANGE: add network specific data here
  int* dim_position;
};

struct VC_Entry {
  int vc;
  message_list* entry;
};

//global stats
static tw_stime         local_total_time = 0;
static tw_stime         local_max_latency = 0;

static long long       total_hops = 0;
static long long       N_finished_packets = 0;
static long long       total_msg_sz = 0;
static long long       N_finished_msgs = 0;
static long long       N_finished_chunks = 0;

/* returns the message size */
static int local_get_msg_sz(void)
{
  return sizeof(LOCAL_MSG_STRUCT);
}

/* helper functions - convert between flat ids and torus n-dimensional ids */
static void to_dim_id(
    int flat_id,
    int ndims,
    const int *dim_lens,
    int *out_dim_ids)
{
  for (int i = 0; i < ndims; i++) {
    out_dim_ids[i] = flat_id % dim_lens[i];
    flat_id /= dim_lens[i];
  }
}

static int to_flat_id(
    int ndims,
    const int *dim_lens,
    const int *dim_ids)
{
  int flat_id = dim_ids[0];
  int mult = dim_lens[0];
  for (int i = 1; i < ndims; i++) {
    flat_id += dim_ids[i] * mult;
    mult *= dim_lens[i];
  }
  return flat_id;
}

//CHANGE: network specific params have to be read here
static void local_read_config(const char * anno, local_param *params){
  local_param *p = params;

  // general params - do not change unless you intent to modify them
  int rc = configuration_get_value_double(&config, "PARAMS", "link_bandwidth",
      anno, &p->link_bandwidth);
  if(rc) {
    p->link_bandwidth = 5.25;
    fprintf(stderr, "Bandwidth of links  not specified, setting to %lf\n",
        p->link_bandwidth);
  }

  rc = configuration_get_value_double(&config, "PARAMS", "cn_bandwidth",
      anno, &p->cn_bandwidth);
  if(rc) {
    p->cn_bandwidth = 5.25;
    fprintf(stderr, "Bandwidth of compute node channels not specified, setting "
        "to %lf\n", p->cn_bandwidth);
  }

  rc = configuration_get_value_int(&config, "PARAMS", "num_cn", anno,
      &p->num_cn);
  if(rc) {
    tw_error(TW_LOC, "Nodes per router (num_cn) not specified\n");
  }

  rc = configuration_get_value_int(&config, "PARAMS", "num_vcs", anno,
      &p->num_vcs);
  if(rc) {
    p->num_vcs = 1;
  }

  rc = configuration_get_value_int(&config, "PARAMS", "chunk_size", anno,
      &p->chunk_size);
  if(rc) {
    p->chunk_size = 512;
    fprintf(stderr, "Chunk size for packets is not specified, setting to %d\n",
      p->chunk_size);
  }

  rc = configuration_get_value_int(&config, "PARAMS", "vc_size", anno,
      &p->vc_size);
  if(rc) {
    p->vc_size = 32768;
    fprintf(stderr, "Buffer size of link channels not specified, setting to %d\n",
      p->vc_size);
  }

  rc = configuration_get_value_int(&config, "PARAMS", "cn_vc_size", anno,
      &p->cn_vc_size);
  if(rc) {
    p->cn_vc_size = 65536;
    fprintf(stderr, "Buffer size of compute node channels not specified, "
        "setting to %d\n", p->cn_vc_size);
  }

  p->router_delay = 50;
  configuration_get_value_int(&config, "PARAMS", "router_delay", anno,
      &p->router_delay);

  configuration_get_value(&config, "PARAMS", "cn_sample_file", anno,
      local_cn_sample_file, MAX_NAME_LENGTH);
  configuration_get_value(&config, "PARAMS", "rt_sample_file", anno,
      local_rtr_sample_file, MAX_NAME_LENGTH);

  //CHANGE: add network specific parameters here
  rc = configuration_get_value_int(&config, "PARAMS", "n_dims", anno,
      &p->n_dims);
  if(rc) {
    tw_error(TW_LOC, "Number of dimensions not specified\n");
  }

  rc = configuration_get_value_int(&config, "PARAMS", "gap", anno, &p->gap);
  if(rc) {
    tw_error(TW_LOC, "Gap not specified\n");
  }

  char dim_length_str[MAX_NAME_LENGTH];
  rc = configuration_get_value(&config, "PARAMS", "dim_length", anno,
      dim_length_str, MAX_NAME_LENGTH);
  if (rc == 0){
    tw_error(TW_LOC, "couldn't read PARAMS:dim_length");
  }
  char* token;
  p->dim_length= (int*)malloc(p->n_dims * sizeof(*p->dim_length));
  token = strtok(dim_length_str, ",");
  int i = 0;
  while(token != NULL)
  {
    sscanf(token, "%d", &p->dim_length[i]);
    if(p->dim_length[i] <= 0)
    {
      tw_error(TW_LOC, "Invalid torus dimension specified "
          "(%d at pos %d), exiting... ", p->dim_length[i], i);
    }
    i++;
    token = strtok(NULL,",");
  }

  char routing_str[MAX_NAME_LENGTH];
  configuration_get_value(&config, "PARAMS", "routing", anno, routing_str,
      MAX_NAME_LENGTH);
  if(strcmp(routing_str, "static") == 0)
    p->routing = STATIC;
  else if (strcmp(routing_str, "adaptive") == 0) {
    p->routing = ADAPTIVE;
    if(p->num_vcs < 2) {
      p->num_vcs = 2;
    }
  }
  else
  {
    p->routing = STATIC;
    fprintf(stderr,
        "No routing protocol specified, setting to static routing\n");
  }

  //CHANGE: derived parameters often are computed based on network specifics
  p->radix = 0;
  p->total_routers = 1;
  p->cons_per_dim = (int *)malloc(p->n_dims * sizeof(int));
  p->offset_per_dim = (int *)malloc(p->n_dims * sizeof(int));
  for(int i = 0; i < p->n_dims; i++) {
    p->cons_per_dim[i] = 2 + (p->dim_length[i] - 3)/p->gap;
    p->radix += p->cons_per_dim[i];
    p->total_routers *= p->dim_length[i];
    if(i == 0) {
      p->offset_per_dim[i] = p->num_cn;
    } else {
      p->offset_per_dim[i] = p->offset_per_dim[i - 1] + p->cons_per_dim[i - 1];
    }
  }
  p->radix += p->num_cn;
  p->total_terminals = p->total_routers * p->num_cn;

  int rank;
  MPI_Comm_rank(MPI_COMM_CODES, &rank);
  if(!rank) {
    printf("\n Total nodes %d routers %d radix %d \n",
        p->total_terminals, p->total_routers, p->radix);
  }

  //general derived parameters
  p->cn_delay = bytes_to_ns(1, p->cn_bandwidth);
  p->link_delay = bytes_to_ns(1, p->link_bandwidth);
  p->credit_delay = bytes_to_ns(CREDIT_SZ, p->link_bandwidth);

  uint32_t h1 = 0, h2 = 0;
  bj_hashlittle2(LP_METHOD_NM_TERM, strlen(LP_METHOD_NM_TERM), &h1, &h2);
  terminal_magic_num = h1 + h2;

  bj_hashlittle2(LP_METHOD_NM_ROUT, strlen(LP_METHOD_NM_ROUT), &h1, &h2);
  router_magic_num = h1 + h2;

}

static void local_configure(){
  anno_map = codes_mapping_get_lp_anno_map(LP_CONFIG_NM_TERM);
  assert(anno_map);
  num_params = anno_map->num_annos + (anno_map->has_unanno_lp > 0);
  all_params = (local_param *)malloc(num_params * sizeof(*all_params));

  for (int i = 0; i < anno_map->num_annos; i++){
    const char * anno = anno_map->annotations[i].ptr;
    local_read_config(anno, &all_params[i]);
  }
  if (anno_map->has_unanno_lp > 0){
    local_read_config(NULL, &all_params[anno_map->num_annos]);
  }
}

/* report statistics like average and maximum packet latency, average number of hops traversed */
static void local_report_stats()
{
  long long avg_hops, total_finished_packets, total_finished_chunks;
  long long total_finished_msgs, final_msg_sz;
  tw_stime avg_time, max_time;
  int total_minimal_packets, total_nonmin_packets;
  long total_gen, total_fin;

  MPI_Reduce( &total_hops, &avg_hops, 1, MPI_LONG_LONG, MPI_SUM, 0,
      MPI_COMM_CODES);
  MPI_Reduce( &N_finished_packets, &total_finished_packets, 1, MPI_LONG_LONG,
      MPI_SUM, 0, MPI_COMM_CODES);
  MPI_Reduce( &N_finished_msgs, &total_finished_msgs, 1, MPI_LONG_LONG, MPI_SUM,
      0, MPI_COMM_CODES);
  MPI_Reduce( &N_finished_chunks, &total_finished_chunks, 1, MPI_LONG_LONG,
      MPI_SUM, 0, MPI_COMM_CODES);
  MPI_Reduce( &total_msg_sz, &final_msg_sz, 1, MPI_LONG_LONG, MPI_SUM, 0,
      MPI_COMM_CODES);
  MPI_Reduce( &local_total_time, &avg_time, 1,MPI_DOUBLE, MPI_SUM, 0,
      MPI_COMM_CODES);
  MPI_Reduce( &local_max_latency, &max_time, 1, MPI_DOUBLE, MPI_MAX, 0,
      MPI_COMM_CODES);

  MPI_Reduce( &packet_gen, &total_gen, 1, MPI_LONG, MPI_SUM, 0, MPI_COMM_CODES);
  MPI_Reduce( &packet_fin, &total_fin, 1, MPI_LONG, MPI_SUM, 0, MPI_COMM_CODES);

  /* print statistics */
  if(!g_tw_mynode)
  {
    printf(" Average number of hops traversed %f average chunk latency %lf us "
      "maximum chunk latency %lf us avg message size %lf bytes finished "
      "messages %lld finished chunks %lld \n",
      (float)avg_hops/total_finished_chunks,
      avg_time/(total_finished_chunks*1000), max_time/1000,
      (float)final_msg_sz/total_finished_msgs, total_finished_msgs,
      total_finished_chunks);
    printf("\n Total packets generated %ld finished %ld \n", total_gen, total_fin);
  }
  return;
}

/* initialize a compute node terminal */
static void terminal_init( terminal_state * s, tw_lp * lp )
{
  int i;
  char anno[MAX_NAME_LENGTH];

  s->packet_gen = 0;
  s->packet_fin = 0;

  codes_mapping_get_lp_info(lp->gid, lp_group_name, &mapping_grp_id, NULL,
      &mapping_type_id, anno, &mapping_rep_id, &mapping_offset);

  if (anno[0] == '\0') {
    s->anno = NULL;
    s->params = &all_params[num_params-1];
  } else {
    s->anno = strdup(anno);
    int id = configuration_get_annotation_index(anno, anno_map);
    s->params = &all_params[id];
  }

  int num_lps = codes_mapping_get_lp_count(lp_group_name, 0, LP_CONFIG_NM_TERM,
      s->anno, 0);

  if(num_lps != s->params->total_terminals) {
    tw_error(TW_LOC, "Number of terminals LP does not match number of nodes\n");
  }

  s->terminal_id = codes_mapping_get_lp_relative_id(lp->gid, 0, 0);

  s->router_id = (int)s->terminal_id / s->params->num_cn;
  s->router_gid = codes_mapping_get_lpid_from_relative(s->router_id, NULL,
      LP_CONFIG_NM_ROUT, s->anno, 1);

  s->terminal_available_time = 0.0;
  s->packet_counter = 0;
  s->finished_msgs = 0;
  s->finished_chunks = 0;
  s->finished_packets = 0;
  s->total_time = 0.0;
  s->total_msg_size = 0;

  s->last_buf_full = 0.0;
  s->busy_time = 0.0;

  s->fwd_events = 0;
  s->rev_events = 0;

  rc_stack_create(&s->st);
  s->vc_occupancy = (int **)malloc(sizeof(int*));

  s->vc_occupancy[0] = (int*)malloc(s->params->num_vcs * sizeof(int));
  for(i = 0; i < s->params->num_vcs; i++ ) {
    s->vc_occupancy[0][i] = 0;
  }

  s->rank_tbl = qhash_init(mn_rank_hash_compare, mn_hash_func, HASH_TABLE_SIZE);

  if(!s->rank_tbl)
    tw_error(TW_LOC, "\n Hash table not initialized! ");

  s->terminal_msgs = (message_list ***)malloc(sizeof(message_list**));
  s->terminal_msgs_tail = (message_list ***)malloc(sizeof(message_list**));
  s->terminal_msgs[0] =
    (message_list **)malloc(s->params->num_vcs * sizeof(message_list*));
  s->terminal_msgs_tail[0] =
    (message_list **)malloc(s->params->num_vcs * sizeof(message_list*));
  for(i = 0; i < s->params->num_vcs; i++ ) {
    s->terminal_msgs[0][i] = NULL;
    s->terminal_msgs_tail[0][i] = NULL;
  }
  s->terminal_length = 0;
  s->in_send_loop = 0;
  s->issueIdle = 0;

  return;
}

//CHANGE: fill this function to set up router's connectivity and network
//specific data structures
static void create_router_connections(router_state * r, tw_lp * lp) {
  local_param *p = (local_param *)r->params;

  r->dim_position = (int *)malloc(p->n_dims * sizeof(int));

  to_dim_id(r->router_id, r->params->n_dims, r->params->dim_length,
      r->dim_position);

  //set up connections
  int curr_con = 0;
  int first = r->router_id * p->num_cn;
  for(; curr_con < p->num_cn; curr_con++) {
    r->link_connections[curr_con] = codes_mapping_get_lpid_from_relative(
      first, NULL, LP_CONFIG_NM_TERM, r->anno, 1);
    first++;
  }

  int temp_dim_pos[p->n_dims];
  for(int i = 0; i < p->n_dims; i++)
    temp_dim_pos[i] = r->dim_position[i];

  for(int curr_dim = 0; curr_dim < p->n_dims; curr_dim++) {
    curr_con = p->offset_per_dim[curr_dim];
    for(int loc = 0; loc < p->dim_length[curr_dim]; loc++) {
      if(loc != r->dim_position[curr_dim]) {
        if(loc == r->dim_position[curr_dim] - 1 ||
           loc == r->dim_position[curr_dim] + 1 ||
           (loc < r->dim_position[curr_dim] &&
           ((r->dim_position[curr_dim] - 1 - loc) % p->gap == 0)) ||
           (loc > r->dim_position[curr_dim] &&
           ((loc - r->dim_position[curr_dim] - 1) % p->gap == 0))) {
          temp_dim_pos[curr_dim] = loc;
          int neigh_id = to_flat_id(p->n_dims, p->dim_length, temp_dim_pos);
          temp_dim_pos[curr_dim] = r->dim_position[curr_dim];
          r->link_connections[curr_con] = codes_mapping_get_lpid_from_relative(
              neigh_id, NULL, LP_CONFIG_NM_ROUT, r->anno, 1);
          curr_con++;
        }
      }
    }
  }
}

static void router_setup(router_state * r, tw_lp * lp)
{
  char anno[MAX_NAME_LENGTH];
  codes_mapping_get_lp_info(lp->gid, lp_group_name, &mapping_grp_id, NULL,
      &mapping_type_id, anno, &mapping_rep_id, &mapping_offset);

  if (anno[0] == '\0'){
    r->anno = NULL;
    r->params = &all_params[num_params-1];
  } else{
    r->anno = strdup(anno);
    int id = configuration_get_annotation_index(anno, anno_map);
    r->params = &all_params[id];
  }

  local_param *p = (local_param *)r->params;

  r->link_connections = (tw_lpid *)malloc(p->radix * sizeof(tw_lpid));

  r->router_id = codes_mapping_get_lp_relative_id(lp->gid, 0, 0);

  r->fwd_events = 0;
  r->rev_events = 0;

  r->next_output_available_time = (tw_stime*)malloc(p->radix * sizeof(tw_stime));
  r->link_traffic = (int64_t*)malloc(p->radix * sizeof(int64_t));
  r->link_traffic_sample = (int64_t*)malloc(p->radix * sizeof(int64_t));

  r->vc_occupancy = (int**)malloc(p->radix * sizeof(int*));
  r->in_send_loop = (int*)malloc(p->radix * sizeof(int));
  r->pending_msgs =
    (message_list ***)malloc(p->radix * sizeof(message_list**));
  r->pending_msgs_tail =
    (message_list ***)malloc(p->radix * sizeof(message_list**));
  r->queued_msgs =
    (message_list ***)malloc(p->radix * sizeof(message_list**));
  r->queued_msgs_tail =
    (message_list ***)malloc(p->radix * sizeof(message_list**));
  r->queued_count = (int*)malloc(p->radix * sizeof(int));
  r->last_buf_full = (tw_stime*)malloc(p->radix * sizeof(tw_stime));
  r->busy_time = (tw_stime*)malloc(p->radix * sizeof(tw_stime));
  r->busy_time_sample = (tw_stime*)malloc(p->radix * sizeof(tw_stime));

  rc_stack_create(&r->st);
  for(int i = 0; i < p->radix; i++)
  {
    // Set credit & router occupancy
    r->last_buf_full[i] = 0.0;
    r->busy_time[i] = 0.0;
    r->busy_time_sample[i] = 0.0;
    r->next_output_available_time[i] = 0;
    r->link_traffic[i] = 0;
    r->link_traffic_sample[i] = 0;
    r->queued_count[i] = 0;
    r->in_send_loop[i] = 0;
    r->vc_occupancy[i] = (int *)malloc(p->num_vcs * sizeof(int));
    r->pending_msgs[i] = (message_list **)malloc(p->num_vcs *
        sizeof(message_list*));
    r->pending_msgs_tail[i] = (message_list **)malloc(p->num_vcs *
        sizeof(message_list*));
    r->queued_msgs[i] = (message_list **)malloc(p->num_vcs *
        sizeof(message_list*));
    r->queued_msgs_tail[i] = (message_list **)malloc(p->num_vcs *
        sizeof(message_list*));
    for(int j = 0; j < p->num_vcs; j++) {
      r->vc_occupancy[i][j] = 0;
      r->pending_msgs[i][j] = NULL;
      r->pending_msgs_tail[i][j] = NULL;
      r->queued_msgs[i][j] = NULL;
      r->queued_msgs_tail[i][j] = NULL;
    }
  }

  create_router_connections(r, lp);
  return;
}

/* packet event , generates a packet on the compute node */
static tw_stime local_packet_event(
    model_net_request const * req,
    uint64_t message_offset,
    uint64_t packet_size,
    tw_stime offset,
    mn_sched_params const * sched_params,
    void const * remote_event,
    void const * self_event,
    tw_lp *sender,
    int is_last_pckt)
{
  (void)message_offset;
  (void)sched_params;
  tw_event * e_new;
  tw_stime xfer_to_nic_time;
  LOCAL_MSG_STRUCT * msg;
  char* tmp_ptr;

  xfer_to_nic_time = codes_local_latency(sender);
  e_new = model_net_method_event_new(sender->gid, xfer_to_nic_time+offset,
      sender, LOCAL_NETWORK_NAME, (void**)&msg, (void**)&tmp_ptr);
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
  msg->dest_terminal = codes_mapping_get_lp_relative_id(msg->dest_terminal_id, 0, 0);
  msg->message_id = req->msg_id;
  msg->is_pull = req->is_pull;
  msg->pull_size = req->pull_size;
  msg->magic = terminal_magic_num;
  msg->msg_start_time = req->msg_start_time;

  if(is_last_pckt) /* Its the last packet so pass in remote and local event information*/
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
  tw_event_send(e_new);
  return xfer_to_nic_time;
}

/* packet event reverse handler */
static void local_packet_event_rc(tw_lp *sender)
{
  codes_local_latency_reverse(sender);
  return;
}

/* generates packet at the current compute node */
static void packet_generate(terminal_state * s, tw_bf * bf, LOCAL_MSG_STRUCT * msg,
    tw_lp * lp) {
  packet_gen++;
  s->packet_gen++;

  tw_stime ts, nic_ts;

  assert(lp->gid != msg->dest_terminal_id);
  const local_param *p = s->params;

  int total_event_size;
  uint64_t num_chunks = msg->packet_size / p->chunk_size;
  if (msg->packet_size % s->params->chunk_size)
    num_chunks++;

  if(!num_chunks)
    num_chunks = 1;

  nic_ts = g_tw_lookahead + (msg->packet_size * s->params->cn_delay) +
    tw_rand_unif(lp->rng);

  msg->packet_ID = lp->gid + g_tw_nlp * s->packet_counter;
  for(int i = 0; i < p->n_dims; i++) {
    msg->hops[i] = 0;
  }
  msg->my_N_hop = 0;

  /* TODO: how do we get a better vc selection mechanism */
  //CHANGE: use only 1 VC for NIC to router transfer; VC selection mechanism on the NIC can be added here
  int use_vc = 0;
  msg->saved_channel = use_vc;

  for(uint64_t i = 0; i < num_chunks; i++)
  {
    message_list *cur_chunk = (message_list*)malloc(
        sizeof(message_list));
    init_message_list(cur_chunk, msg);

    if(msg->remote_event_size_bytes + msg->local_event_size_bytes > 0) {
      cur_chunk->event_data = (char*)malloc(
          msg->remote_event_size_bytes + msg->local_event_size_bytes);
    }

    void * m_data_src = model_net_method_get_edata(LOCAL_NETWORK_NAME, msg);
    if (msg->remote_event_size_bytes){
      memcpy(cur_chunk->event_data, m_data_src, msg->remote_event_size_bytes);
    }
    if (msg->local_event_size_bytes){
      m_data_src = (char*)m_data_src + msg->remote_event_size_bytes;
      memcpy((char*)cur_chunk->event_data + msg->remote_event_size_bytes,
          m_data_src, msg->local_event_size_bytes);
    }

    cur_chunk->LOCAL_MSG_NAME_FROM_UNION.chunk_id = i;
    cur_chunk->port = 0; cur_chunk->index = use_vc;
    append_to_message_list(s->terminal_msgs[0], s->terminal_msgs_tail[0],
        use_vc, cur_chunk);
    s->terminal_length += s->params->chunk_size;
  }

  if(s->terminal_length < s->params->num_vcs * s->params->cn_vc_size) {
    model_net_method_idle_event(nic_ts, 0, lp);
  } else {
    bf->c11 = 1;
    s->issueIdle = 1;
    msg->saved_busy_time = s->last_buf_full;
    s->last_buf_full = tw_now(lp);
  }

  if(s->in_send_loop == 0) {
    bf->c5 = 1;
    ts = codes_local_latency(lp);
    LOCAL_MSG_STRUCT *m;
    tw_event* e = model_net_method_event_new(lp->gid, ts, lp, LOCAL_NETWORK_NAME,
        (void**)&m, NULL);
    m->type = T_SEND;
    m->magic = terminal_magic_num;
    s->in_send_loop = 1;
    tw_event_send(e);
  }

  total_event_size = model_net_get_msg_sz(LOCAL_NETWORK_NAME) +
    msg->remote_event_size_bytes + msg->local_event_size_bytes;
  mn_stats* stat;
  stat = model_net_find_stats(msg->category, s->local_stats_array);
  stat->send_count++;
  stat->send_bytes += msg->packet_size;
  stat->send_time += p->cn_delay * msg->packet_size;
  if(stat->max_event_size < total_event_size)
    stat->max_event_size = total_event_size;

  return;
}

static void packet_generate_rc(terminal_state * s, tw_bf * bf, LOCAL_MSG_STRUCT * msg, tw_lp * lp)
{
  s->packet_gen--;
  packet_gen--;

  tw_rand_reverse_unif(lp->rng);

  int num_chunks = msg->packet_size/s->params->chunk_size;
  if(msg->packet_size % s->params->chunk_size)
    num_chunks++;

  if(!num_chunks)
    num_chunks = 1;

  int i;
  for(i = 0; i < num_chunks; i++) {
    delete_message_list(return_tail(s->terminal_msgs[0],
          s->terminal_msgs_tail[0], msg->saved_channel));
    s->terminal_length -= s->params->chunk_size;
  }
  if(bf->c11) {
    s->issueIdle = 0;
    s->last_buf_full = msg->saved_busy_time;
  }
  if(bf->c5) {
    codes_local_latency_reverse(lp);
    s->in_send_loop = 0;
  }
  struct mn_stats* stat;
  stat = model_net_find_stats(msg->category, s->local_stats_array);
  stat->send_count--;
  stat->send_bytes -= msg->packet_size;
  stat->send_time -= s->params->cn_delay * msg->packet_size;
}


/* sends the packet from the current compute node to the attached router */
static void packet_send(terminal_state * s, tw_bf * bf, LOCAL_MSG_STRUCT * msg,
    tw_lp * lp) {

  tw_stime ts;
  tw_event *e;
  LOCAL_MSG_STRUCT *m;
  tw_lpid router_id;

  std::vector<VC_Entry> entries;

  for(int i = 0; i < s->params->num_vcs; i++) {
    if(s->terminal_msgs[0][i] != NULL &&
      s->vc_occupancy[0][i] + s->params->chunk_size <= s->params->cn_vc_size) {
      VC_Entry tmp;
      tmp.vc = i; tmp.entry = s->terminal_msgs[0][i];
      entries.push_back(tmp);
    }
  }

  if(entries.size() == 0) {
    bf->c1 = 1;
    s->in_send_loop = 0;

    msg->saved_busy_time = s->last_buf_full;
    s->last_buf_full = tw_now(lp);
    return;
  }

  //CHANGE: randomly pick the VC to send on (by default only VC 0 is filed); can be changed.
  int pick = tw_rand_integer(lp->rng, 0, entries.size() - 1);
  message_list* cur_entry = entries[pick].entry;
  int use_vc = entries[pick].vc;
  msg->saved_channel = use_vc;

  uint64_t num_chunks = cur_entry->LOCAL_MSG_NAME_FROM_UNION.packet_size/s->params->chunk_size;
  if(cur_entry->LOCAL_MSG_NAME_FROM_UNION.packet_size % s->params->chunk_size)
    num_chunks++;

  if(!num_chunks)
    num_chunks = 1;

  tw_stime delay;
  if((cur_entry->LOCAL_MSG_NAME_FROM_UNION.packet_size % s->params->chunk_size)
      && (cur_entry->LOCAL_MSG_NAME_FROM_UNION.chunk_id == num_chunks - 1))
    delay = (cur_entry->LOCAL_MSG_NAME_FROM_UNION.packet_size % s->params->chunk_size) *
      s->params->cn_delay;
  else
    delay = s->params->chunk_size * s->params->cn_delay;

  msg->saved_available_time = s->terminal_available_time;
  ts = g_tw_lookahead + delay + tw_rand_unif(lp->rng);
  s->terminal_available_time = maxd(s->terminal_available_time, tw_now(lp));
  s->terminal_available_time += ts;

  ts = s->terminal_available_time - tw_now(lp);
  void * remote_event;
  e = model_net_method_event_new(s->router_gid, ts, lp, LOCAL_NETWORK_ROUTER_NAME,
      (void**)&m, &remote_event);
  memcpy(m, &cur_entry->LOCAL_MSG_NAME_FROM_UNION, sizeof(LOCAL_MSG_STRUCT));
  if (m->remote_event_size_bytes){
    memcpy(remote_event, cur_entry->event_data, m->remote_event_size_bytes);
  }

  m->type = R_ARRIVE;
  m->src_terminal_id = lp->gid;
  m->vc_index = 0;
  m->output_chan = use_vc;
  m->last_hop = TERMINAL;
  m->magic = router_magic_num;
  m->local_event_size_bytes = 0;
  tw_event_send(e);

  if(cur_entry->LOCAL_MSG_NAME_FROM_UNION.chunk_id == num_chunks - 1 &&
      (cur_entry->LOCAL_MSG_NAME_FROM_UNION.local_event_size_bytes > 0)) {
    bf->c2 = 1;
    tw_stime local_ts = codes_local_latency(lp);
    tw_event *e_new = tw_event_new(cur_entry->LOCAL_MSG_NAME_FROM_UNION.sender_lp, local_ts, lp);
    void * m_new = tw_event_data(e_new);
    void *local_event = (char*)cur_entry->event_data +
      cur_entry->LOCAL_MSG_NAME_FROM_UNION.remote_event_size_bytes;
    memcpy(m_new, local_event, cur_entry->LOCAL_MSG_NAME_FROM_UNION.local_event_size_bytes);
    tw_event_send(e_new);
  }
  s->packet_counter++;
  s->vc_occupancy[0][use_vc] += s->params->chunk_size;
  cur_entry = return_head(s->terminal_msgs[0], s->terminal_msgs_tail[0], use_vc);
  rc_stack_push(lp, cur_entry, delete_message_list, s->st);
  s->terminal_length -= s->params->chunk_size;

  LOCAL_MSG_STRUCT *m_new;
  ts += tw_rand_unif(lp->rng);
  e = model_net_method_event_new(lp->gid, ts, lp, LOCAL_NETWORK_NAME,
      (void**)&m_new, NULL);
  m_new->type = T_SEND;
  m_new->magic = terminal_magic_num;
  tw_event_send(e);

  if(s->issueIdle) {
    bf->c5 = 1;
    s->issueIdle = 0;
    ts += tw_rand_unif(lp->rng);
    model_net_method_idle_event(ts, 0, lp);

    if(s->last_buf_full > 0.0)
    {
      bf->c6 = 1;
      msg->saved_total_time = s->busy_time;
      msg->saved_busy_time = s->last_buf_full;
      msg->saved_sample_time = s->busy_time_sample;

      s->busy_time += (tw_now(lp) - s->last_buf_full);
      s->busy_time_sample += (tw_now(lp) - s->last_buf_full);
      s->last_buf_full = 0.0;
    }
  }
  return;
}

static void packet_send_rc(terminal_state * s, tw_bf * bf, LOCAL_MSG_STRUCT * msg,
    tw_lp * lp)
{
  if(bf->c1) {
    s->in_send_loop = 1;
    s->last_buf_full = msg->saved_busy_time;
    return;
  }

  tw_rand_reverse_unif(lp->rng);
  tw_rand_reverse_unif(lp->rng);
  s->terminal_available_time = msg->saved_available_time;
  if(bf->c2) {
    codes_local_latency_reverse(lp);
  }

  int use_vc = msg->saved_channel;

  s->packet_counter--;
  s->vc_occupancy[0][use_vc] -= s->params->chunk_size;

  message_list* cur_entry = (message_list *)rc_stack_pop(s->st);
  cur_entry->port = 0; cur_entry->index = use_vc;
  prepend_to_message_list(s->terminal_msgs[0], s->terminal_msgs_tail[0],
      use_vc, cur_entry);
  s->terminal_length += s->params->chunk_size;

  tw_rand_reverse_unif(lp->rng);
  if(bf->c5)
  {
    tw_rand_reverse_unif(lp->rng);
    s->issueIdle = 1;
    if(bf->c6)
    {
      s->busy_time = msg->saved_total_time;
      s->last_buf_full = msg->saved_busy_time;
      s->busy_time_sample = msg->saved_sample_time;
    }
  }
  return;
}

static void send_remote_event(terminal_state * s, LOCAL_MSG_STRUCT * msg,
  tw_lp * lp, tw_bf * bf, char * event_data, int remote_event_size)
{
  void * tmp_ptr = model_net_method_get_edata(LOCAL_NETWORK_NAME, msg);
  //tw_stime ts = g_tw_lookahead + s->params->cn_delay *
  //    msg->remote_event_size_bytes + tw_rand_unif(lp->rng);;
  tw_stime ts = g_tw_lookahead + tw_rand_unif(lp->rng);
  if (msg->is_pull){
    bf->c4 = 1;
    struct codes_mctx mc_dst =
      codes_mctx_set_global_direct(msg->sender_mn_lp);
    struct codes_mctx mc_src =
      codes_mctx_set_global_direct(lp->gid);
    int net_id = model_net_get_id(LP_METHOD_NM_TERM);

    model_net_set_msg_param(MN_MSG_PARAM_START_TIME,
        MN_MSG_PARAM_START_TIME_VAL, &(msg->msg_start_time));

    msg->event_rc = model_net_event_mctx(net_id, &mc_src, &mc_dst, msg->category,
        msg->sender_lp, msg->pull_size, ts,
        remote_event_size, tmp_ptr, 0, NULL, lp);
  } else {
    tw_event * e = tw_event_new(msg->final_dest_gid, ts, lp);
    void * m_remote = tw_event_data(e);
    memcpy(m_remote, event_data, remote_event_size);
    tw_event_send(e);
  }
  return;
}

/* packet arrives at the destination terminal */
static void packet_arrive(terminal_state * s, tw_bf * bf, LOCAL_MSG_STRUCT * msg,
    tw_lp * lp) {

  assert(lp->gid == msg->dest_terminal_id);

  //total chunks expected in this message
  uint64_t total_chunks = msg->total_size / s->params->chunk_size;
  if(msg->total_size % s->params->chunk_size)
    total_chunks++;
  if(!total_chunks)
    total_chunks = 1;

  /* send credit back to router */
  tw_stime ts = g_tw_lookahead + s->params->credit_delay + tw_rand_unif(lp->rng);
  tw_event * buf_e;
  LOCAL_MSG_STRUCT * buf_msg;
  buf_e = model_net_method_event_new(msg->intm_lp_id, ts, lp,
      LOCAL_NETWORK_ROUTER_NAME, (void**)&buf_msg, NULL);
  buf_msg->magic = router_magic_num;
  buf_msg->vc_index = msg->vc_index;
  buf_msg->output_chan = msg->output_chan;
  buf_msg->type = R_BUFFER;
  tw_event_send(buf_e);

  //save stats
  /* Total overall finished chunks in simulation */
  N_finished_chunks++;
  /* Finished chunks on a LP basis */
  s->finished_chunks++;
  /* Finished chunks per sample */
  s->fin_chunks_sample++;

  assert(lp->gid != msg->src_terminal_id);

  // chunks part of this packet
  uint64_t num_chunks = msg->packet_size / s->params->chunk_size;
  if (msg->packet_size % s->params->chunk_size)
    num_chunks++;
  if(!num_chunks)
    num_chunks = 1;

  if(msg->chunk_id == num_chunks - 1)
  {
    bf->c31 = 1;
    s->packet_fin++;
    packet_fin++;
  }

  /* save the sample time */
  msg->saved_sample_time = s->fin_chunks_time;
  s->fin_chunks_time += (tw_now(lp) - msg->travel_start_time);
  /* save the total time per LP */
  msg->saved_avg_time = s->total_time;
  s->total_time += (tw_now(lp) - msg->travel_start_time);
  msg->saved_total_time = local_total_time;
  local_total_time += tw_now( lp ) - msg->travel_start_time;
  total_hops += msg->my_N_hop;
  s->total_hops += msg->my_N_hop;
  s->fin_hops_sample += msg->my_N_hop;

  mn_stats* stat = model_net_find_stats(msg->category, s->local_stats_array);
  msg->saved_rcv_time = stat->recv_time;
  stat->recv_time += (tw_now(lp) - msg->travel_start_time);

  /* Now retreieve the number of chunks completed from the hash and update
   * them */

  struct mn_hash_key key;
  key.message_id = msg->message_id;
  key.sender_id = msg->sender_lp;
  struct qhash_head *hash_link = NULL;
  struct mn_qhash_entry * tmp = NULL;
  hash_link = qhash_search(s->rank_tbl, &key);
  if(hash_link)
    tmp = qhash_entry(hash_link, struct mn_qhash_entry, hash_link);

  /* If an entry does not exist then create one */
  if(!tmp)
  {
    bf->c5 = 1;
    struct mn_qhash_entry * d_entry = (struct mn_qhash_entry *)
        malloc(sizeof(struct mn_qhash_entry));
    d_entry->num_chunks = 0;
    d_entry->key = key;
    d_entry->remote_event_data = NULL;
    d_entry->remote_event_size = 0;
    qhash_add(s->rank_tbl, &key, &(d_entry->hash_link));

    hash_link = &(d_entry->hash_link);
    tmp = d_entry;
  }

  assert(tmp);
  tmp->num_chunks++;

  if(msg->chunk_id == num_chunks - 1)
  {
    bf->c1 = 1;
    stat->recv_count++;
    stat->recv_bytes += msg->packet_size;
    N_finished_packets++;
    s->finished_packets++;
  }

  /* if its the last chunk of the packet then handle the remote event data */
  if(msg->remote_event_size_bytes > 0 && !tmp->remote_event_data)
  {
    /* Retreive the remote event entry */
    void *m_data_src = model_net_method_get_edata(LOCAL_NETWORK_NAME, msg);
    tmp->remote_event_data = (char*)malloc(msg->remote_event_size_bytes);
    assert(tmp->remote_event_data);
    tmp->remote_event_size = msg->remote_event_size_bytes;
    memcpy(tmp->remote_event_data, m_data_src, msg->remote_event_size_bytes);
  }

  if (local_max_latency < tw_now( lp ) - msg->travel_start_time) {
    bf->c3 = 1;
    msg->saved_available_time = local_max_latency;
    local_max_latency = tw_now( lp ) - msg->travel_start_time;
  }

  if(tmp->num_chunks >= total_chunks)
  {
    bf->c7 = 1;

    N_finished_msgs++;
    total_msg_sz += msg->total_size;
    s->total_msg_size += msg->total_size;
    s->data_size_sample += msg->total_size;
    s->finished_msgs++;

    if(tmp->remote_event_data && tmp->remote_event_size > 0) {
      bf->c8 = 1;
      send_remote_event(s, msg, lp, bf, tmp->remote_event_data,
          tmp->remote_event_size);
    }

    /* Remove the hash entry */
    qhash_del(hash_link);
    rc_stack_push(lp, tmp, free_tmp, s->st);
  }
  return;
}

static void packet_arrive_rc(terminal_state * s, tw_bf * bf, LOCAL_MSG_STRUCT * msg, tw_lp * lp)
{
  tw_rand_reverse_unif(lp->rng);
  N_finished_chunks--;
  s->finished_chunks--;
  s->fin_chunks_sample--;
  if(bf->c31)
  {
    s->packet_fin--;
    packet_fin--;
  }

  s->fin_chunks_time = msg->saved_sample_time;
  s->total_time = msg->saved_avg_time;
  local_total_time  = msg->saved_total_time;
  total_hops -= msg->my_N_hop;
  s->total_hops -= msg->my_N_hop;
  s->fin_hops_sample -= msg->my_N_hop;

  mn_stats* stat;
  stat = model_net_find_stats(msg->category, s->local_stats_array);
  stat->recv_time = msg->saved_rcv_time;

  struct qhash_head * hash_link = NULL;
  struct mn_qhash_entry * tmp = NULL;

  struct mn_hash_key key;
  key.message_id = msg->message_id;
  key.sender_id = msg->sender_lp;

  hash_link = qhash_search(s->rank_tbl, &key);
  if(hash_link)
    tmp = qhash_entry(hash_link, struct mn_qhash_entry, hash_link);

  if(bf->c1)
  {
    stat->recv_count--;
    stat->recv_bytes -= msg->packet_size;
    N_finished_packets--;
    s->finished_packets--;
  }

  if(bf->c3)
    local_max_latency = msg->saved_available_time;

  if(bf->c7)
  {
    N_finished_msgs--;
    total_msg_sz -= msg->total_size;
    s->total_msg_size -= msg->total_size;
    s->data_size_sample -= msg->total_size;
    s->finished_msgs--;

    if(bf->c8)
      tw_rand_reverse_unif(lp->rng);

    struct mn_qhash_entry * d_entry_pop = (struct mn_qhash_entry * )
        rc_stack_pop(s->st);
    qhash_add(s->rank_tbl, &key, &(d_entry_pop->hash_link));

    hash_link = &(d_entry_pop->hash_link);
    tmp = d_entry_pop;

    if(bf->c4)
      model_net_event_rc2(lp, &msg->event_rc);
  }

  assert(tmp);
  tmp->num_chunks--;

  if(bf->c5)
  {
    qhash_del(hash_link);
    free_tmp(tmp);
  }
  return;
}

/* update the compute node-router channel buffer */
static void
terminal_buf_update(terminal_state * s,
    tw_bf * bf,
    LOCAL_MSG_STRUCT * msg,
    tw_lp * lp)
{
  s->vc_occupancy[0][msg->output_chan] -= s->params->chunk_size;

  if(s->in_send_loop == 0) {
    int do_send = 0;
    for(int i = 0; i < s->params->num_vcs; i++) {
      if(s->terminal_msgs[0][i] != NULL) {
        do_send = 1;
        break;
      }
    }
    if(do_send) {
      LOCAL_MSG_STRUCT *m;
      bf->c1 = 1;
      tw_stime ts = codes_local_latency(lp);
      tw_event* e = model_net_method_event_new(lp->gid, ts, lp, LOCAL_NETWORK_NAME,
          (void**)&m, NULL);
      m->type = T_SEND;
      m->magic = terminal_magic_num;
      s->in_send_loop = 1;
      tw_event_send(e);
    }
  }
  return;
}

static void terminal_buf_update_rc(terminal_state * s,
    tw_bf * bf,
    LOCAL_MSG_STRUCT * msg,
    tw_lp * lp)
{
  s->vc_occupancy[0][msg->output_chan] += s->params->chunk_size;
  if(bf->c1) {
    codes_local_latency_reverse(lp);
    s->in_send_loop = 0;
  }
  return;
}

static void terminal_event( terminal_state * s,
    tw_bf * bf,
    LOCAL_MSG_STRUCT * msg,
    tw_lp * lp )
{
  s->fwd_events++;
  assert(msg->magic == terminal_magic_num);
  rc_stack_gc(lp, s->st);
  switch(msg->type)
  {
    case T_GENERATE:
      packet_generate(s,bf,msg,lp);
      break;

    case T_ARRIVE:
      packet_arrive(s,bf,msg,lp);
      break;

    case T_SEND:
      packet_send(s,bf,msg,lp);
      break;

    case T_BUFFER:
      terminal_buf_update(s, bf, msg, lp);
      break;

    default:
      printf("\n LP %d Terminal message type not supported %d ", (int)lp->gid, msg->type);
      tw_error(TW_LOC, "Msg type not supported");
  }
}

/* Reverse computation handler for a terminal event */
static void terminal_event_rc (terminal_state * s,
    tw_bf * bf,
    LOCAL_MSG_STRUCT * msg,
    tw_lp * lp)
{
  s->rev_events++;
  switch(msg->type)
  {
    case T_GENERATE:
      packet_generate_rc(s, bf, msg, lp);
      break;

    case T_SEND:
      packet_send_rc(s, bf, msg, lp);
      break;

    case T_ARRIVE:
      packet_arrive_rc(s, bf, msg, lp);
      break;

    case T_BUFFER:
      terminal_buf_update_rc(s, bf, msg, lp);
      break;

  }
}

static void
terminal_final( terminal_state * s, tw_lp * lp )
{
  model_net_print_stats(lp->gid, s->local_stats_array);

  int written = 0;
  if(!s->terminal_id)
    written = sprintf(s->output_buf, "# Format <LP id> <Terminal ID> <Total Data Size> <Avg packet latency> <# Flits/Packets finished> <Avg hops> <Busy Time>");

  written += sprintf(s->output_buf + written, "\n %llu %u %llu %lf %ld %lf %lf",
      LLU(lp->gid), s->terminal_id, s->total_msg_size, s->total_time,
      s->finished_packets, (double)s->total_hops/s->finished_chunks,
      s->busy_time);

  lp_io_write(lp->gid, (char*)"msg-stats", written, s->output_buf);

  for(int i = 0; i < s->params->num_vcs; i++) {
    if(s->terminal_msgs[0][i] != NULL)
      printf("[%llu] leftover terminal messages \n", LLU(lp->gid));
  }

  qhash_finalize(s->rank_tbl);
  rc_stack_destroy(s->st);
  free(s->vc_occupancy[0]);
  free(s->vc_occupancy);
  free(s->terminal_msgs[0]);
  free(s->terminal_msgs_tail[0]);
  free(s->terminal_msgs);
  free(s->terminal_msgs_tail);
}

struct NextHop {
  int dim, port;
  NextHop(int _dim, int _port) {
    dim = _dim;
    port = _port;
  }
};

//CHANGE: implement the get next stop function for the network - some
//implementations may choose to change the function prototype
/* get the next stop for the current packet */
static void
get_next_stop(router_state * s,
    LOCAL_MSG_STRUCT * msg,
    tw_bf *bf,
    int *port,
    int *vc,
    int *src_dim,
    int *dst_dim,
    int *static_port)
{
  *static_port = -1;
  *src_dim = -2;
  if(msg->last_hop == TERMINAL) {
    *src_dim = -1;
  } else {
    for(int i = s->params->n_dims - 1; i >= 0; i--) {
      if(msg->vc_index >= s->params->offset_per_dim[i]) {
        *src_dim = i;
        break;
      }
    }
  }
  assert(*src_dim > -2);

  int dest[s->params->n_dims];
  to_dim_id(msg->dest_terminal/s->params->num_cn, s->params->n_dims,
    s->params->dim_length, dest);

  std::vector<NextHop> port_options;
  bool at_dest = true;
  int first_dim = -1;
  for(int i = 0; i < s->params->n_dims; i++)
  {
    if(s->dim_position[i] != dest[i]) {
      at_dest = false;
      *port = s->params->offset_per_dim[i];
      if(first_dim  == -1) {
        first_dim = i;
      }

      int first_dim_con;
      if(s->dim_position[i] == 0) {
        first_dim_con = -1;
      } else {
        first_dim_con = (s->dim_position[i] - 1) % s->params->gap;
      }

      if(dest[i] == s->dim_position[i] - 1) {
        *port += (s->dim_position[i] - 1 - first_dim_con) / s->params->gap;
        port_options.push_back(NextHop(i, *port));
      } else if(dest[i] == s->dim_position[i] + 1) {
        *port += 1 + (s->dim_position[i] - 1 - first_dim_con) / s->params->gap;
        *port -= (s->dim_position[i] == 0 ? 1 : 0);
        port_options.push_back(NextHop(i, *port));
      } else {
        if(dest[i] < s->dim_position[i]) {
          if((s->dim_position[i] - 1 - dest[i]) % s->params->gap == 0) {
            *port += (dest[i] - first_dim_con) / s->params->gap;
            port_options.push_back(NextHop(i, *port));
          } else {
            int long_hop;
            if(dest[i] < first_dim_con) {
              long_hop = *port;
            } else {
              long_hop = *port + (dest[i] - first_dim_con) / s->params->gap + 1;
            }
            //int long_hop = *port + (dest[i] - first_dim_con) / s->params->gap + 1;
            int short_hop = *port + (s->dim_position[i] - 1 - first_dim_con) / s->params->gap;
            port_options.push_back(NextHop(i, long_hop));
            port_options.push_back(NextHop(i, short_hop));
          }
        } else if(dest[i] > s->dim_position[i]) {
          if((dest[i] - s->dim_position[i] - 1) % s->params->gap == 0) {
            *port += (s->dim_position[i] - 1 - first_dim_con) / s->params->gap
              + 1 + (dest[i] - s->dim_position[i] - 1) / s->params->gap;
            *port -= (s->dim_position[i] == 0 ? 1 : 0);
            port_options.push_back(NextHop(i, *port));
          } else {
            int long_hop = *port + (s->dim_position[i] - 1 - first_dim_con) / s->params->gap
              + 1 + (dest[i] - s->dim_position[i] - 1) / s->params->gap;
            int short_hop = *port + 1 + (s->dim_position[i] - 1 - first_dim_con) / s->params->gap;
            short_hop -= (s->dim_position[i] == 0 ? 1 : 0);
            long_hop -= (s->dim_position[i] == 0 ? 1 : 0);
            port_options.push_back(NextHop(i, long_hop));
            port_options.push_back(NextHop(i, short_hop));
          }
        } else {
          tw_error(TW_LOC, "Impossible condition in get_next_stop");
        }
      }
      if(s->params->routing == STATIC) {
        break;
      }
    }
  }

  int try_vcs = s->params->num_vcs;
  if(at_dest) {
    *port = (msg->dest_terminal % s->params->num_cn);
    port_options.push_back(NextHop(-1, *port));
    try_vcs = 1;
    assert(port_options.size() == 1);
  }
  if(s->params->routing == STATIC || at_dest) {
    *vc = 0;
    *port = port_options[0].port;
    *dst_dim = port_options[0].dim;
    int vc_s = s->vc_occupancy[*port][*vc] + s->queued_count[*port];
    for(int i = 0; i < port_options.size(); i++) {
      for(int j = 0; j < try_vcs; j++) {
        if(s->vc_occupancy[port_options[i].port][j] +
           s->queued_count[port_options[i].port] < vc_s) {
          vc_s = s->vc_occupancy[port_options[i].port][j] +
                 s->queued_count[port_options[i].port];
          *port = port_options[i].port;
          *dst_dim = port_options[i].dim;
          *vc = j;
        }
      }
    }
  } else {
    *vc = 1; //dynamic VC
    *port = port_options[0].port;
    *dst_dim = port_options[0].dim;
    int vc_s = s->vc_occupancy[*port][*vc] + s->queued_count[*port];
    for(int i = 0; i < port_options.size(); i++) {
      for(int j = 1; j < try_vcs; j++) {
        if(s->vc_occupancy[port_options[i].port][j] +
           s->queued_count[port_options[i].port] < vc_s) {
          vc_s = s->vc_occupancy[port_options[i].port][j] +
                 s->queued_count[port_options[i].port];
          *port = port_options[i].port;
          *dst_dim = port_options[i].dim;
          *vc = j;
        }
      }
    }
    //if going to queued list, find the first dim port also
    if((s->vc_occupancy[*port][*vc] + s->params->chunk_size > s->params->vc_size)) {
      int start_vc = 1;
      int end_vc = s->params->num_vcs;
      *static_port = port_options[0].port;
      int vc_s = s->vc_occupancy[*static_port][start_vc] + s->queued_count[*static_port];
      for(int i = 0; (i < port_options.size()) &&
                     (port_options[i].dim == first_dim); i++) {
        for(int j = start_vc; j < end_vc; j++) {
          if(s->vc_occupancy[port_options[i].port][j] +
              s->queued_count[port_options[i].port] < vc_s) {
            vc_s = s->vc_occupancy[port_options[i].port][j] +
              s->queued_count[port_options[i].port];
            *static_port = port_options[i].port;
          }
        }
      }
    }
  }
#if DEBUG
  printf("[%lld-%d] Me: %d %d %d: Dest %d %d %d Found %d %d\n", msg->packet_ID, s->params->n_dims,
      s->dim_position[0], s->dim_position[1], s->dim_position[2],
      dest[0], dest[1], dest[2], *dst_dim, *port);
#endif
}

//CHANGE: reverse computation for get_next_stop
/* get the next stop for the current packet */
static void
get_next_stop_rc(router_state * s,
    LOCAL_MSG_STRUCT * msg,
    tw_bf *bf)
{
}

/*When a packet is sent from the current router and a buffer slot becomes
 * available, a credit is sent back to schedule another packet event*/
static void router_credit_send(router_state * s, LOCAL_MSG_STRUCT * msg,
    tw_lp * lp) {
  tw_event * buf_e;
  tw_stime ts;
  LOCAL_MSG_STRUCT * buf_msg;

  int dest = 0,  type = R_BUFFER;
  int is_terminal = 0;

  const local_param *p = s->params;

  // Notify sender terminal about available buffer space
  if(msg->last_hop == TERMINAL) {
    dest = msg->src_terminal_id;
    type = T_BUFFER;
    is_terminal = 1;
  } else {
    dest = msg->intm_lp_id;
  }

  ts = g_tw_lookahead + p->credit_delay +  tw_rand_unif(lp->rng);

  if (is_terminal) {
    buf_e = model_net_method_event_new(dest, ts, lp, LOCAL_NETWORK_NAME,
        (void**)&buf_msg, NULL);
    buf_msg->magic = terminal_magic_num;
  } else {
    buf_e = model_net_method_event_new(dest, ts, lp, LOCAL_NETWORK_ROUTER_NAME,
        (void**)&buf_msg, NULL);
    buf_msg->magic = router_magic_num;
  }

  buf_msg->vc_index = msg->vc_index;
  buf_msg->output_chan = msg->output_chan;
  buf_msg->type = type;
  tw_event_send(buf_e);
  return;
}

//CHANGE: may need to modify to add network-specific handling of packets
/* Packet arrives at the router and a credit is sent back to the sending terminal/router */
static void
router_packet_receive( router_state * s,
    tw_bf * bf,
    LOCAL_MSG_STRUCT * msg,
    tw_lp * lp )
{
  tw_stime ts;
  int next_port, next_vc, src_dim, next_dim, static_port;

  get_next_stop(s, msg, bf, &next_port, &next_vc, &src_dim, &next_dim, &static_port);

  if(s->params->routing != STATIC && next_dim != -1) {
    assert(next_vc != 0);
  }

  message_list * cur_chunk = (message_list*)malloc(sizeof(message_list));
  init_message_list(cur_chunk, msg);

  if(msg->remote_event_size_bytes > 0) {
    void *m_data_src = model_net_method_get_edata(LOCAL_NETWORK_ROUTER_NAME, msg);
    cur_chunk->event_data = (char*)malloc(msg->remote_event_size_bytes);
    memcpy(cur_chunk->event_data, m_data_src, msg->remote_event_size_bytes);
  }

  int max_vc_size = s->params->cn_vc_size;
  if(next_port >= s->params->num_cn) {
    max_vc_size = s->params->vc_size;
  }

  cur_chunk->LOCAL_MSG_NAME_FROM_UNION.my_N_hop++;
  assert(cur_chunk->LOCAL_MSG_NAME_FROM_UNION.my_N_hop < s->params->n_dims * s->params->gap + 2);

  //additional condition for Express Mesh when changing dimension
  int multfactor = 1;
  if(s->params->routing == STATIC && next_dim != -1 && src_dim != next_dim) {
    multfactor = MULT_FACTOR;
  }

  if(s->vc_occupancy[next_port][next_vc] + multfactor * s->params->chunk_size
      <= max_vc_size) {
    bf->c2 = 1;
    router_credit_send(s, msg, lp);
    cur_chunk->port = next_port; cur_chunk->index = next_vc;
    append_to_message_list(s->pending_msgs[next_port],
        s->pending_msgs_tail[next_port], next_vc, cur_chunk);
    s->vc_occupancy[next_port][next_vc] += s->params->chunk_size;
    if(s->in_send_loop[next_port] == 0) {
      bf->c3 = 1;
      LOCAL_MSG_STRUCT *m;
      ts = codes_local_latency(lp);
      tw_event *e = model_net_method_event_new(lp->gid, ts, lp,
          LOCAL_NETWORK_ROUTER_NAME, (void**)&m, NULL);
      m->type = R_SEND;
      m->magic = router_magic_num;
      m->vc_index = next_port;
      tw_event_send(e);
      s->in_send_loop[next_port] = 1;
    }
  } else {
    bf->c4 = 1;
    if(multfactor == 1) {
      cur_chunk->LOCAL_MSG_NAME_FROM_UNION.dim_change = 0;
    } else {
      cur_chunk->LOCAL_MSG_NAME_FROM_UNION.dim_change = 1;
    }
    cur_chunk->port = next_port; cur_chunk->index = next_vc;
    append_to_message_list(s->queued_msgs[next_port],
        s->queued_msgs_tail[next_port], next_vc, cur_chunk);
    s->queued_count[next_port] += s->params->chunk_size;
    msg->saved_busy_time = s->last_buf_full[next_port];
    s->last_buf_full[next_port] = tw_now(lp);
    //additional routing optimization for Express Mesh : if both static outport
    //and dynamic output is not currently available, get queued to both
    if(s->params->routing != STATIC && next_dim != -1) {
      assert(static_port >= s->params->num_cn);
      assert(next_vc != 0);
      bf->c6 = 1;
      cur_chunk->in_alt_q = 1;
      cur_chunk->altq_port = static_port;
      altq_append_to_message_list(s->queued_msgs[static_port],
          s->queued_msgs_tail[static_port], 0, cur_chunk);
      bool triggerSend = (s->vc_occupancy[static_port][0] + MULT_FACTOR * s->params->chunk_size <= s->params->vc_size);
      if(triggerSend && s->in_send_loop[static_port] == 0) {
        bf->c5 = 1;
        LOCAL_MSG_STRUCT *m;
        ts = codes_local_latency(lp);
        tw_event *e = model_net_method_event_new(lp->gid, ts, lp,
            LOCAL_NETWORK_ROUTER_NAME, (void**)&m, NULL);
        m->type = R_SEND;
        m->magic = router_magic_num;
        m->vc_index = static_port;
        tw_event_send(e);
        s->in_send_loop[static_port] = 1;
      }
    }
  }

  msg->saved_vc = next_port;
  msg->saved_channel = next_vc;
  return;
}

//CHANGE: need to modify if router_packet_receive is modified
static void router_packet_receive_rc(router_state * s,
    tw_bf * bf,
    LOCAL_MSG_STRUCT * msg,
    tw_lp * lp)
{
  get_next_stop_rc(s, msg, bf);
  int next_port = msg->saved_vc;
  int next_vc = msg->saved_channel;

  if(bf->c2) {
    tw_rand_reverse_unif(lp->rng);
    message_list * tail = return_tail(s->pending_msgs[next_port],
      s->pending_msgs_tail[next_port], next_vc);
    delete_message_list(tail);
    s->vc_occupancy[next_port][next_vc] -= s->params->chunk_size;
    if(bf->c3) {
      codes_local_latency_reverse(lp);
      s->in_send_loop[next_port] = 0;
    }
  }
  if(bf->c4) {
    message_list *tail = return_tail(s->queued_msgs[next_port],
          s->queued_msgs_tail[next_port], next_vc);
    s->queued_count[next_port] -= s->params->chunk_size;
    s->last_buf_full[next_port] = msg->saved_busy_time;
    if(bf->c6) {
      assert(tail->in_alt_q == 1);
      int static_port = tail->altq_port;
      message_list *stail = altq_return_tail(s->queued_msgs[static_port],
        s->queued_msgs_tail[static_port], 0);
      assert(stail == tail);
      if(bf->c5) {
        codes_local_latency_reverse(lp);
        s->in_send_loop[static_port] = 0;
      }
    }
    delete_message_list(tail);
  }
}

/* routes the current packet to the next stop */
static void
router_packet_send( router_state * s,
    tw_bf * bf,
    LOCAL_MSG_STRUCT * msg, tw_lp * lp)
{
  tw_stime ts;
  tw_event *e;
  LOCAL_MSG_STRUCT *m;
  int output_port = msg->vc_index;

  std::vector<VC_Entry> entries;

  for(int i = 0; i < s->params->num_vcs; i++) {
    if(s->pending_msgs[output_port][i] != NULL) {
      VC_Entry tmp;
      tmp.vc = i; tmp.entry = s->pending_msgs[output_port][i];
      entries.push_back(tmp);
    }
  }

  //possible move from dynamics VCs to static VC
  if(entries.size() == 0) {
    if((output_port >= s->params->num_cn) && (s->params->routing != STATIC) &&
       (s->vc_occupancy[output_port][0] + MULT_FACTOR * s->params->chunk_size <= s->params->vc_size)) {
      if(s->queued_msgs[output_port][0] != NULL) {
        bf->c21 = 1;
        message_list *head = altq_return_head(s->queued_msgs[output_port],
            s->queued_msgs_tail[output_port], 0);
        head->in_alt_q = 0;
        router_credit_send(s, &head->LOCAL_MSG_NAME_FROM_UNION, lp);
        delete_from_message_list(s->queued_msgs, s->queued_msgs_tail, head);
        s->queued_count[head->port] -= s->params->chunk_size;
        msg->saved_vc = head->port;
        msg->saved_channel = head->index;
        s->vc_occupancy[output_port][0] += s->params->chunk_size;
        VC_Entry tmp;
        tmp.vc = 0; tmp.entry = head;
        entries.push_back(tmp);
      }
    }
    if(entries.size() == 0) {
      bf->c1 = 1;
      s->in_send_loop[output_port] = 0;
      return;
    }
  }

  //CHANGE: no priorities among VCs; can be changed.
  int pick = tw_rand_integer(lp->rng, 0, entries.size() - 1);
  message_list* cur_entry = entries[pick].entry;
  int use_vc = entries[pick].vc;
  msg->output_chan = use_vc;

  int to_terminal = 1;
  double delay = s->params->cn_delay;

  if(output_port >= s->params->num_cn) {
    to_terminal = 0;
    delay = s->params->link_delay;
  }

  uint64_t num_chunks = cur_entry->LOCAL_MSG_NAME_FROM_UNION.packet_size / s->params->chunk_size;
  if(msg->packet_size % s->params->chunk_size)
    num_chunks++;
  if(!num_chunks)
    num_chunks = 1;

  double bytetime;
  if((cur_entry->LOCAL_MSG_NAME_FROM_UNION.packet_size % s->params->chunk_size)
      && (cur_entry->LOCAL_MSG_NAME_FROM_UNION.chunk_id == num_chunks - 1))
    bytetime = delay * (cur_entry->LOCAL_MSG_NAME_FROM_UNION.packet_size % s->params->chunk_size);
  else
    bytetime = delay * s->params->chunk_size;

  ts = g_tw_lookahead + tw_rand_unif(lp->rng) + bytetime + s->params->router_delay;

  msg->saved_available_time = s->next_output_available_time[output_port];
  s->next_output_available_time[output_port] =
    maxd(s->next_output_available_time[output_port], tw_now(lp));
  s->next_output_available_time[output_port] += ts;

  ts = s->next_output_available_time[output_port] - tw_now(lp);

  // dest can be a router or a terminal, so we must check
  void * m_data;
  if (to_terminal) {
    assert(s->link_connections[output_port] == cur_entry->LOCAL_MSG_NAME_FROM_UNION.dest_terminal_id);
    e = model_net_method_event_new(s->link_connections[output_port],
        ts, lp, LOCAL_NETWORK_NAME, (void**)&m, &m_data);
  } else {
    e = model_net_method_event_new(s->link_connections[output_port],
        ts, lp, LOCAL_NETWORK_ROUTER_NAME, (void**)&m, &m_data);
  }
  memcpy(m, &cur_entry->LOCAL_MSG_NAME_FROM_UNION, sizeof(LOCAL_MSG_STRUCT));
  if (m->remote_event_size_bytes){
    memcpy(m_data, cur_entry->event_data, m->remote_event_size_bytes);
  }

  m->last_hop = ROUTER;
  m->intm_lp_id = lp->gid;
  m->vc_index = output_port;
  m->output_chan = use_vc;

  if((cur_entry->LOCAL_MSG_NAME_FROM_UNION.packet_size % s->params->chunk_size)
      && (cur_entry->LOCAL_MSG_NAME_FROM_UNION.chunk_id == num_chunks - 1)) {
    bf->c11 = 1;
    s->link_traffic[output_port] +=  (cur_entry->LOCAL_MSG_NAME_FROM_UNION.packet_size %
        s->params->chunk_size);
    s->link_traffic_sample[output_port] += (cur_entry->LOCAL_MSG_NAME_FROM_UNION.packet_size %
        s->params->chunk_size);
  } else {
    bf->c12 = 1;
    s->link_traffic[output_port] += s->params->chunk_size;
    s->link_traffic_sample[output_port] += s->params->chunk_size;
  }

  /* Determine the event type. If the packet has arrived at the final
   * destination router then it should arrive at the destination terminal
   * next.*/
  if(to_terminal) {
    m->type = T_ARRIVE;
    m->magic = terminal_magic_num;
  } else {
    /* The packet has to be sent to another router */
    m->type = R_ARRIVE;
    m->magic = router_magic_num;
  }
  tw_event_send(e);

  if(!bf->c21) {
    cur_entry = return_head(s->pending_msgs[output_port],
        s->pending_msgs_tail[output_port], use_vc);
  }
  rc_stack_push(lp, cur_entry, delete_message_list, s->st);

  s->next_output_available_time[output_port] -= s->params->router_delay;
  ts -= s->params->router_delay;

  LOCAL_MSG_STRUCT *m_new;
  ts = ts +  g_tw_lookahead * tw_rand_unif(lp->rng);
  e = model_net_method_event_new(lp->gid, ts, lp, LOCAL_NETWORK_ROUTER_NAME,
      (void**)&m_new, NULL);
  m_new->type = R_SEND;
  m_new->magic = router_magic_num;
  m_new->vc_index = output_port;
  tw_event_send(e);
  return;
}

static void router_packet_send_rc(router_state * s,
    tw_bf * bf,
    LOCAL_MSG_STRUCT * msg,
    tw_lp * lp)
{
  int output_port = msg->vc_index;
  int use_vc = msg->output_chan;

  if(bf->c1) {
    s->in_send_loop[output_port] = 1;
    return;
  }

  tw_rand_reverse_unif(lp->rng);
  tw_rand_reverse_unif(lp->rng);

  s->next_output_available_time[output_port] = msg->saved_available_time;
  message_list * cur_entry = (message_list *)rc_stack_pop(s->st);
  assert(cur_entry);

  if(bf->c11)
  {
    s->link_traffic[output_port] -= cur_entry->LOCAL_MSG_NAME_FROM_UNION.packet_size % s->params->chunk_size;
    s->link_traffic_sample[output_port] -= cur_entry->LOCAL_MSG_NAME_FROM_UNION.packet_size % s->params->chunk_size;
  }
  if(bf->c12)
  {
    s->link_traffic[output_port] -= s->params->chunk_size;
    s->link_traffic_sample[output_port] -= s->params->chunk_size;
  }

  if(bf->c21) {
    tw_rand_reverse_unif(lp->rng);
    cur_entry->port = msg->saved_vc; cur_entry->index = msg->saved_channel;
    cur_entry->in_alt_q = 1;
    altq_prepend_to_message_list(s->queued_msgs[output_port],
        s->queued_msgs_tail[output_port], 0, cur_entry);
    add_to_message_list(s->queued_msgs, s->queued_msgs_tail, cur_entry);
    s->vc_occupancy[output_port][0] -= s->params->chunk_size;
    s->queued_count[msg->saved_vc] += s->params->chunk_size;
  } else {
    cur_entry->port = output_port; cur_entry->index = use_vc;
    prepend_to_message_list(s->pending_msgs[output_port],
        s->pending_msgs_tail[output_port], use_vc, cur_entry);
  }

  tw_rand_reverse_unif(lp->rng);
}

/* Update the buffer space associated with this router LP */
static void router_buf_update(router_state * s, tw_bf * bf, LOCAL_MSG_STRUCT * msg, tw_lp * lp)
{
  int indx = msg->vc_index;
  int output_chan = msg->output_chan;
  s->vc_occupancy[indx][output_chan] -= s->params->chunk_size;

  if(s->last_buf_full[indx])
  {
    bf->c3 = 1;
    msg->saved_rcv_time = s->busy_time[indx];
    msg->saved_busy_time = s->last_buf_full[indx];
    msg->saved_sample_time = s->busy_time_sample[indx];
    s->busy_time[indx] += (tw_now(lp) - s->last_buf_full[indx]);
    s->busy_time_sample[indx] += (tw_now(lp) - s->last_buf_full[indx]);
    s->last_buf_full[indx] = 0.0;
  }

  int max_vc_size = s->params->vc_size;
  if(indx < s->params->num_cn) {
    max_vc_size = s->params->cn_vc_size;
  }

  if((s->params->routing == STATIC || output_chan != 0 || indx < s->params->num_cn)
      && s->queued_msgs[indx][output_chan] != NULL) {
    if(!s->queued_msgs[indx][output_chan]->LOCAL_MSG_NAME_FROM_UNION.dim_change ||
      (s->vc_occupancy[indx][output_chan] + MULT_FACTOR * s->params->chunk_size
      <= max_vc_size)) {
      bf->c1 = 1;
      message_list *head = return_head(s->queued_msgs[indx],
          s->queued_msgs_tail[indx], output_chan);
      router_credit_send(s, &head->LOCAL_MSG_NAME_FROM_UNION, lp);
      if(head->in_alt_q) {
        bf->c21 = 1;
        altq_delete_from_message_list(s->queued_msgs, s->queued_msgs_tail, head);
        head->in_alt_q = 0;
      }
      head->port = indx; head->index = output_chan;
      append_to_message_list(s->pending_msgs[indx],
          s->pending_msgs_tail[indx], output_chan, head);
      s->vc_occupancy[indx][output_chan] += s->params->chunk_size;
      s->queued_count[indx] -= s->params->chunk_size;
    }
  }

  if(s->in_send_loop[indx] == 0 && ((s->pending_msgs[indx][output_chan] != NULL) ||
      ((s->vc_occupancy[indx][0] + MULT_FACTOR * s->params->chunk_size <= s->params->vc_size)
        && (s->queued_msgs[indx][0] != NULL)))) {
    bf->c2 = 1;
    LOCAL_MSG_STRUCT *m;
    tw_stime ts = codes_local_latency(lp);
    tw_event *e = model_net_method_event_new(lp->gid, ts, lp, LOCAL_NETWORK_ROUTER_NAME,
        (void**)&m, NULL);
    m->type = R_SEND;
    m->vc_index = indx;
    m->magic = router_magic_num;
    s->in_send_loop[indx] = 1;
    tw_event_send(e);
  }

  return;
}

static void router_buf_update_rc(router_state * s,
    tw_bf * bf,
    LOCAL_MSG_STRUCT * msg,
    tw_lp * lp)
{
  int indx = msg->vc_index;
  int output_chan = msg->output_chan;
  s->vc_occupancy[indx][output_chan] += s->params->chunk_size;
  if(bf->c3)
  {
    s->busy_time[indx] = msg->saved_rcv_time;
    s->busy_time_sample[indx] = msg->saved_sample_time;
    s->last_buf_full[indx] = msg->saved_busy_time;
  }
  if(bf->c1) {
    message_list* head = return_tail(s->pending_msgs[indx],
        s->pending_msgs_tail[indx], output_chan);
    tw_rand_reverse_unif(lp->rng);
    if(bf->c21) {
      head->in_alt_q = 1;
      altq_add_to_message_list(s->queued_msgs, s->queued_msgs_tail, head);
    }
    head->port = indx; head->index = output_chan;
    prepend_to_message_list(s->queued_msgs[indx],
        s->queued_msgs_tail[indx], output_chan, head);
    s->vc_occupancy[indx][output_chan] -= s->params->chunk_size;
    s->queued_count[indx] += s->params->chunk_size;
  }
  if(bf->c2) {
    codes_local_latency_reverse(lp);
    s->in_send_loop[indx] = 0;
  }
}

static void router_event(router_state * s, tw_bf * bf, LOCAL_MSG_STRUCT * msg,
    tw_lp * lp) {
  s->fwd_events++;
  rc_stack_gc(lp, s->st);
  assert(msg->magic == router_magic_num);
  switch(msg->type)
  {
    case R_SEND:
      router_packet_send(s, bf, msg, lp);
      break;

    case R_ARRIVE:
      router_packet_receive(s, bf, msg, lp);
      break;

    case R_BUFFER:
      router_buf_update(s, bf, msg, lp);
      break;

    default:
      printf("\n (%lf) [Router %d] Router Message type not supported %d dest "
          "terminal id %d packet ID %d ", tw_now(lp), (int)lp->gid, msg->type,
          (int)msg->dest_terminal_id, (int)msg->packet_ID);
      tw_error(TW_LOC, "Msg type not supported");
      break;
  }
}

/* Reverse computation handler for a router event */
static void router_rc_event_handler(router_state * s, tw_bf * bf,
    LOCAL_MSG_STRUCT * msg, tw_lp * lp) {
  s->rev_events++;

  switch(msg->type) {
    case R_SEND:
      router_packet_send_rc(s, bf, msg, lp);
      break;
    case R_ARRIVE:
      router_packet_receive_rc(s, bf, msg, lp);
      break;

    case R_BUFFER:
      router_buf_update_rc(s, bf, msg, lp);
      break;
  }
}

static void router_final(router_state * s,
    tw_lp * lp)
{
  int i, j;
  for(i = 0; i < s->params->radix; i++) {
    for(j = 0; j < s->params->num_vcs; j++) {
      if(s->queued_msgs[i][j] != NULL) {
        printf("[%llu] leftover queued messages %d %d %d\n", LLU(lp->gid), i, j,
            s->vc_occupancy[i][j]);
      }
      if(s->pending_msgs[i][j] != NULL) {
        printf("[%llu] lefover pending messages %d %d\n", LLU(lp->gid), i, j);
      }
    }
  }

  rc_stack_destroy(s->st);

  const local_param *p = s->params;
  int written = 0;
  if(!s->router_id)
  {
    written = sprintf(s->output_buf, "# Format <LP ID> <Router ID> <Busy time per router port(s)>");
  }
  written += sprintf(s->output_buf + written, "\n %llu %d ",
      LLU(lp->gid),
      s->router_id);
  for(int d = 0; d < p->radix; d++)
    written += sprintf(s->output_buf + written, " %lf", s->busy_time[d]);

  lp_io_write(lp->gid, (char*)"router-stats", written, s->output_buf);

  written = 0;
  if(!s->router_id)
  {
    written = sprintf(s->output_buf2, "# Format <LP ID> <Router ID> <Link traffic per router port(s)>");
  }
  written += sprintf(s->output_buf2 + written, "\n %llu %d ",
      LLU(lp->gid),
      s->router_id);

  for(int d = 0; d < p->radix; d++)
    written += sprintf(s->output_buf2 + written, " %lld", LLD(s->link_traffic[d]));

  assert(written < 4096);
  lp_io_write(lp->gid, (char*)"router-traffic", written, s->output_buf2);
}

static void local_rsample_init(router_state * s,
    tw_lp * lp)
{
  (void)lp;
  int i = 0;
  const local_param * p = s->params;

  assert(p->radix);

  s->max_arr_size = MAX_STATS;
  s->rsamples = (struct local_router_sample *)malloc(MAX_STATS * sizeof(struct local_router_sample));
  for(; i < s->max_arr_size; i++)
  {
    s->rsamples[i].busy_time = (tw_stime *)malloc(sizeof(tw_stime) * p->radix);
    s->rsamples[i].link_traffic_sample = (int64_t *)malloc(sizeof(int64_t) * p->radix);
  }
}

void local_rsample_rc_fn(router_state * s,
    tw_bf * bf,
    LOCAL_MSG_STRUCT * msg,
    tw_lp * lp)
{
  (void)bf;
  (void)lp;
  (void)msg;

  s->op_arr_size--;
  int cur_indx = s->op_arr_size;
  struct local_router_sample stat = s->rsamples[cur_indx];

  const local_param * p = s->params;
  int i =0;

  for(; i < p->radix; i++)
  {
    s->busy_time_sample[i] = stat.busy_time[i];
    s->link_traffic_sample[i] = stat.link_traffic_sample[i];
  }

  for( i = 0; i < p->radix; i++)
  {
    stat.busy_time[i] = 0;
    stat.link_traffic_sample[i] = 0;
  }
  s->fwd_events = stat.fwd_events;
  s->rev_events = stat.rev_events;
}

static void local_rsample_fn(router_state * s,
    tw_bf * bf,
    LOCAL_MSG_STRUCT * msg,
    tw_lp * lp)
{
  (void)bf;
  (void)lp;
  (void)msg;

  const local_param * p = s->params;

  if(s->op_arr_size >= s->max_arr_size)
  {
    struct local_router_sample * tmp = (struct local_router_sample *)malloc((MAX_STATS + s->max_arr_size) * sizeof(struct local_router_sample));
    memcpy(tmp, s->rsamples, s->op_arr_size * sizeof(struct local_router_sample));
    free(s->rsamples);
    s->rsamples = tmp;
    s->max_arr_size += MAX_STATS;
  }

  int i = 0;
  int cur_indx = s->op_arr_size;

  s->rsamples[cur_indx].router_id = s->router_id;
  s->rsamples[cur_indx].end_time = tw_now(lp);
  s->rsamples[cur_indx].fwd_events = s->fwd_events;
  s->rsamples[cur_indx].rev_events = s->rev_events;

  for(; i < p->radix; i++)
  {
    s->rsamples[cur_indx].busy_time[i] = s->busy_time_sample[i];
    s->rsamples[cur_indx].link_traffic_sample[i] = s->link_traffic_sample[i];
  }

  s->op_arr_size++;

  /* clear up the current router stats */
  s->fwd_events = 0;
  s->rev_events = 0;

  for( i = 0; i < p->radix; i++)
  {
    s->busy_time_sample[i] = 0;
    s->link_traffic_sample[i] = 0;
  }
}

static void local_rsample_fin(router_state * s,
    tw_lp * lp)
{
  (void)lp;
  const local_param * p = s->params;

  if(!g_tw_mynode)
  {

    /* write metadata file */
    char meta_fname[64];
    sprintf(meta_fname, "router-sampling.meta");

    FILE * fp = fopen(meta_fname, "w");
    fprintf(fp, "Router sample struct format: \nrouter_id (tw_lpid) \nbusy time for each of the %d links (double) \n"
        "link traffic for each of the %d links (int64_t) \nsample end time (double) forward events per sample \nreverse events per sample ",
        p->radix, p->radix);
    fprintf(fp, "\n\nOrdering of links \n%d local (router-router same group) channels \n%d global (router-router remote group)"
        " channels \n%d terminal channels", p->radix/2, p->radix/4, p->radix/4);
    fclose(fp);
  }
  char rt_fn[MAX_NAME_LENGTH];
  if(strcmp(local_rtr_sample_file, "") == 0)
    sprintf(rt_fn, "router-sampling-%ld.bin", g_tw_mynode);
  else
    sprintf(rt_fn, "%s-%ld.bin", local_rtr_sample_file, g_tw_mynode);

  int i = 0;

  int size_sample = sizeof(tw_lpid) + p->radix * (sizeof(int64_t) + sizeof(tw_stime)) + sizeof(tw_stime) + 2 * sizeof(long);
  FILE * fp = fopen(rt_fn, "a");
  fseek(fp, sample_rtr_bytes_written, SEEK_SET);

  for(; i < s->op_arr_size; i++)
  {
    fwrite((void*)&(s->rsamples[i].router_id), sizeof(tw_lpid), 1, fp);
    fwrite(s->rsamples[i].busy_time, sizeof(tw_stime), p->radix, fp);
    fwrite(s->rsamples[i].link_traffic_sample, sizeof(int64_t), p->radix, fp);
    fwrite((void*)&(s->rsamples[i].end_time), sizeof(tw_stime), 1, fp);
    fwrite((void*)&(s->rsamples[i].fwd_events), sizeof(long), 1, fp);
    fwrite((void*)&(s->rsamples[i].rev_events), sizeof(long), 1, fp);
  }
  sample_rtr_bytes_written += (s->op_arr_size * size_sample);
  fclose(fp);
}

static void local_sample_init(terminal_state * s,
    tw_lp * lp)
{
  (void)lp;
  s->fin_chunks_sample = 0;
  s->data_size_sample = 0;
  s->fin_hops_sample = 0;
  s->fin_chunks_time = 0;
  s->busy_time_sample = 0;

  s->op_arr_size = 0;
  s->max_arr_size = MAX_STATS;

  s->sample_stat = (struct local_cn_sample *)malloc(MAX_STATS * sizeof(struct local_cn_sample));
}

void local_sample_rc_fn(terminal_state * s,
    tw_bf * bf,
    LOCAL_MSG_STRUCT * msg,
    tw_lp * lp)
{
  (void)lp;
  (void)bf;
  (void)msg;

  s->op_arr_size--;
  int cur_indx = s->op_arr_size;
  struct local_cn_sample stat = s->sample_stat[cur_indx];
  s->busy_time_sample = stat.busy_time_sample;
  s->fin_chunks_time = stat.fin_chunks_time;
  s->fin_hops_sample = stat.fin_hops_sample;
  s->data_size_sample = stat.data_size_sample;
  s->fin_chunks_sample = stat.fin_chunks_sample;
  s->fwd_events = stat.fwd_events;
  s->rev_events = stat.rev_events;

  stat.busy_time_sample = 0;
  stat.fin_chunks_time = 0;
  stat.fin_hops_sample = 0;
  stat.data_size_sample = 0;
  stat.fin_chunks_sample = 0;
  stat.end_time = 0;
  stat.terminal_id = 0;
  stat.fwd_events = 0;
  stat.rev_events = 0;
}

static void local_sample_fn(terminal_state * s,
    tw_bf * bf,
    LOCAL_MSG_STRUCT * msg,
    tw_lp * lp)
{
  (void)lp;
  (void)msg;
  (void)bf;

  if(s->op_arr_size >= s->max_arr_size)
  {
    /* In the worst case, copy array to a new memory location, its very
     * expensive operation though */
    struct local_cn_sample * tmp = (struct local_cn_sample *)malloc((MAX_STATS + s->max_arr_size) * sizeof(struct local_cn_sample));
    memcpy(tmp, s->sample_stat, s->op_arr_size * sizeof(struct local_cn_sample));
    free(s->sample_stat);
    s->sample_stat = tmp;
    s->max_arr_size += MAX_STATS;
  }

  int cur_indx = s->op_arr_size;

  s->sample_stat[cur_indx].terminal_id = s->terminal_id;
  s->sample_stat[cur_indx].fin_chunks_sample = s->fin_chunks_sample;
  s->sample_stat[cur_indx].data_size_sample = s->data_size_sample;
  s->sample_stat[cur_indx].fin_hops_sample = s->fin_hops_sample;
  s->sample_stat[cur_indx].fin_chunks_time = s->fin_chunks_time;
  s->sample_stat[cur_indx].busy_time_sample = s->busy_time_sample;
  s->sample_stat[cur_indx].end_time = tw_now(lp);
  s->sample_stat[cur_indx].fwd_events = s->fwd_events;
  s->sample_stat[cur_indx].rev_events = s->rev_events;

  s->op_arr_size++;
  s->fin_chunks_sample = 0;
  s->data_size_sample = 0;
  s->fin_hops_sample = 0;
  s->fwd_events = 0;
  s->rev_events = 0;
  s->fin_chunks_time = 0;
  s->busy_time_sample = 0;
}

static void local_sample_fin(terminal_state * s,
    tw_lp * lp)
{
  (void)lp;

  if(!g_tw_mynode)
  {

    /* write metadata file */
    char meta_fname[64];
    sprintf(meta_fname, "cn-sampling.meta");

    FILE * fp = fopen(meta_fname, "w");
    fprintf(fp, "Compute node sample format\nterminal_id (tw_lpid) \nfinished chunks (long)"
        "\ndata size per sample (long) \nfinished hops (double) \ntime to finish chunks (double)"
        "\nbusy time (double)\nsample end time(double) \nforward events (long) \nreverse events (long)");
    fclose(fp);
  }
  char rt_fn[MAX_NAME_LENGTH];
  if(strncmp(local_cn_sample_file, "", 10) == 0)
    sprintf(rt_fn, "cn-sampling-%ld.bin", g_tw_mynode);
  else
    sprintf(rt_fn, "%s-%ld.bin", local_cn_sample_file, g_tw_mynode);

  FILE * fp = fopen(rt_fn, "a");
  fseek(fp, sample_bytes_written, SEEK_SET);
  fwrite(s->sample_stat, sizeof(struct local_cn_sample), s->op_arr_size, fp);
  fclose(fp);

  sample_bytes_written += (s->op_arr_size * sizeof(struct local_cn_sample));
}


/* compute node and router LP types */
tw_lptype local_lps[] =
{
  // Terminal handling functions
  {
    (init_f)terminal_init,
    (pre_run_f) NULL,
    (event_f) terminal_event,
    (revent_f) terminal_event_rc,
    (commit_f) NULL,
    (final_f) terminal_final,
    (map_f) codes_mapping,
    sizeof(terminal_state)
  },
  {
    (init_f) router_setup,
    (pre_run_f) NULL,
    (event_f) router_event,
    (revent_f) router_rc_event_handler,
    (commit_f) NULL,
    (final_f) router_final,
    (map_f) codes_mapping,
    sizeof(router_state),
  },
  {NULL, NULL, NULL, NULL, NULL, NULL, NULL, 0},
};

/* returns the lp type for lp registration */
static const tw_lptype* local_get_cn_lp_type(void)
{
  return(&local_lps[0]);
}
static const tw_lptype* router_get_lp_type(void)
{
  return (&local_lps[1]);
}

static void local_register(tw_lptype *base_type) {
  lp_type_register(LP_CONFIG_NM_TERM, base_type);
}

static void router_register(tw_lptype *base_type) {
  lp_type_register(LP_CONFIG_NM_ROUT, base_type);
}

extern "C" {
//CHANGE: use network specific struct names
struct model_net_method express_mesh_method  =
{
  0,
  local_configure,
  local_register,
  local_packet_event,
  local_packet_event_rc,
  NULL,
  NULL,
  local_get_cn_lp_type,
  local_get_msg_sz,
  local_report_stats,
  NULL,
  NULL,
  NULL,//(event_f)local_sample_fn,
  NULL,//(revent_f)local_sample_rc_fn,
  (init_f)local_sample_init,
  NULL,//(final_f)local_sample_fin,
  NULL, // for ROSS instrumentation
  NULL  // for ROSS instrumentation
};

struct model_net_method express_mesh_router_method =
{
  0,
  NULL,
  router_register,
  NULL,
  NULL,
  NULL,
  NULL,
  router_get_lp_type,
  local_get_msg_sz,
  NULL,
  NULL,
  NULL,
  NULL,//(event_f)local_rsample_fn,
  NULL,//(revent_f)local_rsample_rc_fn,
  (init_f)local_rsample_init,
  NULL,//(final_f)local_rsample_fin,
  NULL, // for ROSS instrumentation
  NULL  // for ROSS instrumentation
};

}
