#ifndef INC_torus_h
#define INC_torus_h

#include <ross.h>
#include <assert.h>

#include "codes/codes_mapping.h"
#include "codes/codes.h"
#include "codes/model-net.h"
#include "codes/model-net-method.h"

#define CHUNK_SIZE 32
#define DEBUG 1
#define MEAN_INTERVAL 100
#define CATEGORY_NAME_MAX 16
#define MAX_NAME_LENGTH 256
#define TRACE -1 

// Total number of nodes in torus, calculate in main
int N_nodes = 1;
double link_bandwidth;
int buffer_size;
int num_vc;
int n_dims;
int * dim_length;
int * factor;
int * half_length;

char grp_name[MAX_NAME_LENGTH], type_name[MAX_NAME_LENGTH];
int grp_id, lp_type_id, rep_id, offset;

typedef enum nodes_event_t nodes_event_t;
typedef struct nodes_state nodes_state;
typedef struct nodes_message nodes_message;

/* Issues a torus packet event call */
static void torus_packet_event(
		       char* category,
		       tw_lpid final_dest_lp,
		       int packet_size,
		       int remote_event_size,
		       const void* remote_event,
		       int self_event_size,
		       const void* self_event,
		       tw_lp *sender,
		       int is_last_pckt);

static void torus_packet_event_rc(tw_lp *sender);
static void torus_setup(const void* net_params);
static int torus_get_msg_sz(void);
static const tw_lptype* torus_get_lp_type(void);
static void torus_report_stats(void);

/* data structure for torus statistics */
struct model_net_method torus_method =
{
   .method_name = "torus",
   .mn_setup = torus_setup,
   .model_net_method_packet_event = torus_packet_event,
   .model_net_method_packet_event_rc = torus_packet_event_rc,
   .mn_get_lp_type = torus_get_lp_type,
   .mn_get_msg_sz = torus_get_msg_sz,
   .mn_report_stats = torus_report_stats,
};

enum nodes_event_t
{
  GENERATE = 1,
  ARRIVAL, 
  SEND,
  CREDIT,
};

struct nodes_state
{
  unsigned long long packet_counter;            
  tw_stime** next_link_available_time; 
  tw_stime** next_credit_available_time;
  tw_stime** next_flit_generate_time;
  int** buffer;
  int* dim_position;
  int* neighbour_minus_lpID;
  int* neighbour_plus_lpID;
  int source_dim;
  int direction;
};

struct nodes_message
{
  char category[CATEGORY_NAME_MAX];
  tw_stime travel_start_time;
  tw_stime saved_available_time;

  unsigned long long packet_ID;
  nodes_event_t	 type;

  int saved_src_dim;
  int saved_src_dir;

  int* dest;

  tw_lpid final_dest_gid;
  tw_lpid dest_lp;
  tw_lpid sender_lp;

  int my_N_hop;
  int source_dim;
  int source_direction;
  int next_stop;
  int packet_size;
  short chunk_id;

  // for codes local and remote events
  int local_event_size_bytes;
  int remote_event_size_bytes;
};

tw_stime         average_travel_time = 0;
tw_stime         total_time = 0;
tw_stime         max_latency = 0;

float head_delay=0.0;
float credit_delay = 0.0;

static unsigned long long       N_finished_packets = 0;
static unsigned long long       total_hops = 0;
// run time arguments
int num_packets;
int num_chunks;
int packet_offset = 0;

#endif
