/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

// Local router ID: 0 --- total_router-1
// Router LP ID 
// Terminal LP ID

#include <ross.h>

#include "codes/codes_mapping.h"
#include "codes/codes.h"
#include "codes/model-net.h"
#include "codes/model-net-method.h"
#include "codes/model-net-lp.h"
#include "codes/net/dragonfly.h"

#define CREDIT_SIZE 8
#define MEAN_PROCESS 1.0

/* collective specific parameters */
#define TREE_DEGREE 4
#define LEVEL_DELAY 1000
#define DRAGONFLY_COLLECTIVE_DEBUG 0
#define NUM_COLLECTIVES  1
#define COLLECTIVE_COMPUTATION_DELAY 5700
#define DRAGONFLY_FAN_OUT_DELAY 20.0

// debugging parameters
#define TRACK 235221
#define PRINT_ROUTER_TABLE 1
#define DEBUG 1

#define LP_CONFIG_NM (model_net_lp_config_names[DRAGONFLY])
#define LP_METHOD_NM (model_net_method_names[DRAGONFLY])

static double maxd(double a, double b) { return a < b ? b : a; }

// arrival rate
static double MEAN_INTERVAL=200.0;
// threshold for adaptive routing
static int adaptive_threshold = 10;

/* minimal and non-minimal packet counts for adaptive routing*/
int minimal_count=0, nonmin_count=0;

typedef struct dragonfly_param dragonfly_param;
/* annotation-specific parameters (unannotated entry occurs at the 
 * last index) */
static uint64_t                  num_params = 0;
static dragonfly_param         * all_params = NULL;
static const config_anno_map_t * anno_map   = NULL;

/* global variables for codes mapping */
static char lp_group_name[MAX_NAME_LENGTH];
static int mapping_grp_id, mapping_type_id, mapping_rep_id, mapping_offset;

struct dragonfly_param
{
    // configuration parameters
    int num_routers; /*Number of routers in a group*/
    double local_bandwidth;/* bandwidth of the router-router channels within a group */
    double global_bandwidth;/* bandwidth of the inter-group router connections */
    double cn_bandwidth;/* bandwidth of the compute node channels connected to routers */
    int num_vcs; /* number of virtual channels */
    int local_vc_size; /* buffer size of the router-router channels */
    int global_vc_size; /* buffer size of the global channels */
    int cn_vc_size; /* buffer size of the compute node channels */
    int routing; /* minimal or non-minimal routing */
    int chunk_size; /* full-sized packets are broken into smaller chunks.*/

    // derived parameters
    int num_cn;
    int num_groups;
    int radix;
    int total_routers;
    int num_global_channels;
};

/* handles terminal and router events like packet generate/send/receive/buffer */
typedef enum event_t event_t;

typedef struct terminal_state terminal_state;
typedef struct router_state router_state;

/* dragonfly compute node data structure */
struct terminal_state
{
   unsigned long long packet_counter;

   // Dragonfly specific parameters
   unsigned int router_id;
   unsigned int terminal_id;

   // Each terminal will have an input and output channel with the router
   int* vc_occupancy; // NUM_VC
   int* output_vc_state;
   tw_stime terminal_available_time;
   tw_stime next_credit_available_time;
// Terminal generate, sends and arrival T_SEND, T_ARRIVAL, T_GENERATE
// Router-Router Intra-group sends and receives RR_LSEND, RR_LARRIVE
// Router-Router Inter-group sends and receives RR_GSEND, RR_GARRIVE
   struct mn_stats dragonfly_stats_array[CATEGORY_MAX];
  /* collective init time */
  tw_stime collective_init_time;

  /* node ID in the tree */ 
   tw_lpid node_id;

   /* messages sent & received in collectives may get interchanged several times so we have to save the 
     origin server information in the node's state */
   tw_lpid origin_svr; 
  
  /* parent node ID of the current node */
   tw_lpid parent_node_id;
   /* array of children to be allocated in terminal_init*/
   tw_lpid* children;

   /* children of a node can be less than or equal to the tree degree */
   int num_children;

   short is_root;
   short is_leaf;

   /* to maintain a count of child nodes that have fanned in at the parent during the collective
      fan-in phase*/
   int num_fan_nodes;

   const char * anno;
   const dragonfly_param *params;
};

/* terminal event type (1-4) */
enum event_t
{
  T_GENERATE=1,
  T_ARRIVE,
  T_SEND,
  T_BUFFER,
  R_SEND,
  R_ARRIVE,
  R_BUFFER,
  D_COLLECTIVE_INIT,
  D_COLLECTIVE_FAN_IN,
  D_COLLECTIVE_FAN_OUT
};
/* status of a virtual channel can be idle, active, allocated or wait for credit */
enum vc_status
{
   VC_IDLE,
   VC_ACTIVE,
   VC_ALLOC,
   VC_CREDIT
};

/* whether the last hop of a packet was global, local or a terminal */
enum last_hop
{
   GLOBAL,
   LOCAL,
   TERMINAL
};

/* three forms of routing algorithms available, adaptive routing is not
 * accurate and fully functional in the current version as the formulas
 * for detecting load on global channels are not very accurate */
enum ROUTING_ALGO
{
    MINIMAL = 0,
    NON_MINIMAL,
    ADAPTIVE
};

struct router_state
{
   unsigned int router_id;
   unsigned int group_id;
  
   int* global_channel; 
   tw_stime* next_output_available_time;
   tw_stime* next_credit_available_time;
   int* vc_occupancy;
   int* output_vc_state;

   const char * anno;
   const dragonfly_param *params;
};

static short routing = MINIMAL;

static tw_stime         dragonfly_total_time = 0;
static tw_stime         dragonfly_max_latency = 0;
static tw_stime         max_collective = 0;


static long long       total_hops = 0;
static long long       N_finished_packets = 0;

/* returns the dragonfly router lp type for lp registration */
static const tw_lptype* dragonfly_get_router_lp_type(void);

/* returns the dragonfly message size */
static int dragonfly_get_msg_sz(void)
{
	   return sizeof(terminal_message);
}

static void dragonfly_read_config(const char * anno, dragonfly_param *params){
    // shorthand
    dragonfly_param *p = params;

    configuration_get_value_int(&config, "PARAMS", "num_routers", anno,
            &p->num_routers);
    if(p->num_routers <= 0) {
        p->num_routers = 4;
        fprintf(stderr, "Number of dimensions not specified, setting to %d\n",
                p->num_routers);
    }

    configuration_get_value_int(&config, "PARAMS", "num_vcs", anno,
            &p->num_vcs);
    if(p->num_vcs <= 0) {
        p->num_vcs = 1;
        fprintf(stderr, "Number of virtual channels not specified, setting to %d\n", p->num_vcs);
    }

    configuration_get_value_int(&config, "PARAMS", "local_vc_size", anno, &p->local_vc_size);
    if(!p->local_vc_size) {
        p->local_vc_size = 1024;
        fprintf(stderr, "Buffer size of local channels not specified, setting to %d\n", p->local_vc_size);
    }

    configuration_get_value_int(&config, "PARAMS", "global_vc_size", anno, &p->global_vc_size);
    if(!p->global_vc_size) {
        p->global_vc_size = 2048;
        fprintf(stderr, "Buffer size of global channels not specified, setting to %d\n", p->global_vc_size);
    }

    configuration_get_value_int(&config, "PARAMS", "cn_vc_size", anno, &p->cn_vc_size);
    if(!p->cn_vc_size) {
        p->cn_vc_size = 1024;
        fprintf(stderr, "Buffer size of compute node channels not specified, setting to %d\n", p->cn_vc_size);
    }

    configuration_get_value_int(&config, "PARAMS", "chunk_size", anno, &p->chunk_size);
    if(!p->chunk_size) {
        p->chunk_size = 64;
        fprintf(stderr, "Chunk size for packets is specified, setting to %d\n", p->chunk_size);
    }

    configuration_get_value_double(&config, "PARAMS", "local_bandwidth", anno, &p->local_bandwidth);
    if(!p->local_bandwidth) {
        p->local_bandwidth = 5.25;
        fprintf(stderr, "Bandwidth of local channels not specified, setting to %lf\n", p->local_bandwidth);
    }

    configuration_get_value_double(&config, "PARAMS", "global_bandwidth", anno, &p->global_bandwidth);
    if(!p->global_bandwidth) {
        p->global_bandwidth = 4.7;
        fprintf(stderr, "Bandwidth of global channels not specified, setting to %lf\n", p->global_bandwidth);
    }

    configuration_get_value_double(&config, "PARAMS", "cn_bandwidth", anno, &p->cn_bandwidth);
    if(!p->cn_bandwidth) {
        p->cn_bandwidth = 5.25;
        fprintf(stderr, "Bandwidth of compute node channels not specified, setting to %lf\n", p->cn_bandwidth);
    }


    char routing[MAX_NAME_LENGTH];
    configuration_get_value(&config, "PARAMS", "routing", anno, routing,
            MAX_NAME_LENGTH);
    if(strcmp(routing, "minimal") == 0)
        p->routing = 0;
    else if(strcmp(routing, "nonminimal")==0 || strcmp(routing,"non-minimal")==0)
        p->routing = 1;
    else if (strcmp(routing, "adaptive") == 0)
        p->routing = 2;
    else
    {
        fprintf(stderr, 
                "No routing protocol specified, setting to minimal routing\n");
        p->routing = 0;
    }

    // set the derived parameters
    p->num_cn = p->num_routers/2;
    p->num_global_channels = p->num_routers/2;
    p->num_groups = p->num_routers * p->num_cn + 1;
    p->radix = p->num_vcs *
        (p->num_cn + p->num_global_channels + p->num_routers);
    p->total_routers = p->num_groups * p->num_routers;
}

static void dragonfly_configure(){
    anno_map = codes_mapping_get_lp_anno_map(LP_CONFIG_NM);
    assert(anno_map);
    num_params = anno_map->num_annos + (anno_map->has_unanno_lp > 0);
    all_params = malloc(num_params * sizeof(*all_params));

    for (uint64_t i = 0; i < anno_map->num_annos; i++){
        const char * anno = anno_map->annotations[i];
        dragonfly_read_config(anno, &all_params[i]);
    }
    if (anno_map->has_unanno_lp > 0){
        dragonfly_read_config(NULL, &all_params[anno_map->num_annos]);
    }
}

/* report dragonfly statistics like average and maximum packet latency, average number of hops traversed */
static void dragonfly_report_stats()
{
/* TODO: Add dragonfly packet average, maximum latency and average number of hops traversed */
   long long avg_hops, total_finished_packets;
   tw_stime avg_time, max_time;
   int total_minimal_packets, total_nonmin_packets;

   MPI_Reduce( &total_hops, &avg_hops, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
   MPI_Reduce( &N_finished_packets, &total_finished_packets, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
   MPI_Reduce( &dragonfly_total_time, &avg_time, 1,MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
   MPI_Reduce( &dragonfly_max_latency, &max_time, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);
   if(routing == ADAPTIVE)
    {
	MPI_Reduce(&minimal_count, &total_minimal_packets, 1, MPI_INT, MPI_SUM, 0, MPI_COMM_WORLD);
 	MPI_Reduce(&nonmin_count, &total_nonmin_packets, 1, MPI_INT, MPI_SUM, 0, MPI_COMM_WORLD);
    }

   /* print statistics */
   if(!g_tw_mynode)
   {
      printf(" Average number of hops traversed %f average message latency %lf us maximum message latency %lf us \n", (float)avg_hops/total_finished_packets, avg_time/(total_finished_packets*1000), max_time/1000);
     if(routing == ADAPTIVE)
              printf("\n ADAPTIVE ROUTING STATS: %d packets routed minimally %d packets routed non-minimally ", total_minimal_packets, total_nonmin_packets);
 
  }
   return;
}

void dragonfly_collective_init(terminal_state * s,
           		   tw_lp * lp)
{
    // TODO: be annotation-aware
    codes_mapping_get_lp_info(lp->gid, lp_group_name, &mapping_grp_id, NULL,
            &mapping_type_id, NULL, &mapping_rep_id, &mapping_offset);
    int num_lps = codes_mapping_get_lp_count(lp_group_name, 1, LP_CONFIG_NM,
            NULL, 1);
    int num_reps = codes_mapping_get_group_reps(lp_group_name);
    s->node_id = (mapping_rep_id * num_lps) + mapping_offset;

    int i;
   /* handle collective operations by forming a tree of all the LPs */
   /* special condition for root of the tree */
   if( s->node_id == 0)
    {
        s->parent_node_id = -1;
        s->is_root = 1;
   }
   else
   {
       s->parent_node_id = (s->node_id - ((s->node_id - 1) % TREE_DEGREE)) / TREE_DEGREE;
       s->is_root = 0;
   }
   s->children = (tw_lpid*)malloc(TREE_DEGREE * sizeof(tw_lpid));

   /* set the isleaf to zero by default */
   s->is_leaf = 1;
   s->num_children = 0;

   /* calculate the children of the current node. If its a leaf, no need to set children,
      only set isleaf and break the loop*/

   for( i = 0; i < TREE_DEGREE; i++ )
    {
        tw_lpid next_child = (TREE_DEGREE * s->node_id) + i + 1;
        if(next_child < (num_lps * num_reps))
        {
            s->num_children++;
            s->is_leaf = 0;
            s->children[i] = next_child;
        }
        else
           s->children[i] = -1;
    }

#if DRAGONFLY_COLLECTIVE_DEBUG == 1
   printf("\n LP %ld parent node id ", s->node_id);

   for( i = 0; i < TREE_DEGREE; i++ )
        printf(" child node ID %ld ", s->children[i]);
   printf("\n");

   if(s->is_leaf)
        printf("\n LP %ld is leaf ", s->node_id);
#endif
}

/* dragonfly packet event , generates a dragonfly packet on the compute node */
static tw_stime dragonfly_packet_event(char* category, tw_lpid final_dest_lp, uint64_t packet_size, int is_pull, uint64_t pull_size, tw_stime offset, const mn_sched_params *sched_params, int remote_event_size, const void* remote_event, int self_event_size, const void* self_event, tw_lpid src_lp, tw_lp *sender, int is_last_pckt)
{
    tw_event * e_new;
    tw_stime xfer_to_nic_time;
    terminal_message * msg;
    char* tmp_ptr;

    xfer_to_nic_time = codes_local_latency(sender); /* Throws an error of found last KP time > current event time otherwise when LPs of one type are placed together*/
    //printf("\n transfer in time %f %f ", xfer_to_nic_time+offset, tw_now(sender));
    //e_new = tw_event_new(sender->gid, xfer_to_nic_time+offset, sender);
    //msg = tw_event_data(e_new);
    e_new = model_net_method_event_new(sender->gid, xfer_to_nic_time+offset,
            sender, DRAGONFLY, (void**)&msg, (void**)&tmp_ptr);
    strcpy(msg->category, category);
    msg->final_dest_gid = final_dest_lp;
    msg->sender_lp=src_lp;
    msg->packet_size = packet_size;
    msg->remote_event_size_bytes = 0;
    msg->local_event_size_bytes = 0;
    msg->type = T_GENERATE;
    msg->is_pull = is_pull;
    msg->pull_size = pull_size;

    if(is_last_pckt) /* Its the last packet so pass in remote and local event information*/
      {
	if(remote_event_size > 0)
	 {
		msg->remote_event_size_bytes = remote_event_size;
		memcpy(tmp_ptr, remote_event, remote_event_size);
		tmp_ptr += remote_event_size;
	}
	if(self_event_size > 0)
	{
		msg->local_event_size_bytes = self_event_size;
		memcpy(tmp_ptr, self_event, self_event_size);
		tmp_ptr += self_event_size;
	}
     }
	   //printf("\n dragonfly remote event %d local event %d last packet %d %lf ", msg->remote_event_size_bytes, msg->local_event_size_bytes, is_last_pckt, xfer_to_nic_time);
    tw_event_send(e_new);
    return xfer_to_nic_time;
}

/* dragonfly packet event reverse handler */
static void dragonfly_packet_event_rc(tw_lp *sender)
{
	  codes_local_latency_reverse(sender);
	    return;
}

/* given a group ID gid, find the router in the current group that is attached
 * to a router in the group gid */
tw_lpid getRouterFromGroupID(int gid, 
		    router_state * r)
{
    const dragonfly_param *p = r->params;
  int group_begin = r->group_id * p->num_routers;
  int group_end = (r->group_id * p->num_routers) + p->num_routers-1;
  int offset = (gid * p->num_routers - group_begin) / p->num_routers;
  
  if((gid * p->num_routers) < group_begin)
    offset = (group_begin - gid * p->num_routers) / p->num_routers; // take absolute value
  
  int half_channel = p->num_global_channels / 2;
  int index = (offset - 1)/(half_channel * p->num_routers);
  
  offset=(offset - 1) % (half_channel * p->num_routers);

  // If the destination router is in the same group
  tw_lpid router_id;

  if(index % 2 != 0)
    router_id = group_end - (offset / half_channel); // start from the end
  else
    router_id = group_begin + (offset / half_channel);

  return router_id;
}	

/*When a packet is sent from the current router and a buffer slot becomes available, a credit is sent back to schedule another packet event*/
void router_credit_send(router_state * s, tw_bf * bf, terminal_message * msg, tw_lp * lp)
{
  tw_event * buf_e;
  tw_stime ts;
  terminal_message * buf_msg;

  int dest=0, credit_delay=0, type = R_BUFFER;
  int is_terminal = 0;

  const dragonfly_param *p = s->params;
  int sender_radix;
 // Notify sender terminal about available buffer space
  if(msg->last_hop == TERMINAL)
  {
   dest = msg->src_terminal_id;
   sender_radix = msg->local_id % p->num_cn;  
   //determine the time in ns to transfer the credit
   credit_delay = (1 / p->cn_bandwidth) * CREDIT_SIZE;
   type = T_BUFFER;
   is_terminal = 1;
  }
   else if(msg->last_hop == GLOBAL)
   {
     dest = msg->intm_lp_id;
     sender_radix = p->num_cn + (msg->local_id % p->num_routers);
     credit_delay = (1 / p->global_bandwidth) * CREDIT_SIZE;
   }
    else if(msg->last_hop == LOCAL)
     {
        dest = msg->intm_lp_id;
        sender_radix = p->num_cn + p->num_routers + (msg->local_id % p->num_routers);
     	credit_delay = (1/p->local_bandwidth) * CREDIT_SIZE;
     }
    else
      printf("\n Invalid message type");

   // Assume it takes 0.1 ns of serialization latency for processing the credits in the queue
    int output_port = msg->saved_vc / p->num_vcs;

    msg->saved_available_time = s->next_credit_available_time[sender_radix];
    s->next_credit_available_time[sender_radix] = maxd(tw_now(lp), s->next_credit_available_time[output_port]);
    ts = credit_delay + 0.1 + tw_rand_exponential(lp->rng, (double)credit_delay/1000);
	
    s->next_credit_available_time[sender_radix]+=ts;
    if (is_terminal){
        buf_e = model_net_method_event_new(dest, 
                s->next_credit_available_time[sender_radix] - tw_now(lp), lp,
                DRAGONFLY, (void**)&buf_msg, NULL);
    }
    else{
        buf_e = tw_event_new(dest, s->next_credit_available_time[sender_radix] - tw_now(lp) , lp);
        buf_msg = tw_event_data(buf_e);
    }
    buf_msg->vc_index = msg->saved_vc;
    buf_msg->type=type;
    buf_msg->last_hop = msg->last_hop;
    buf_msg->packet_ID=msg->packet_ID;

    tw_event_send(buf_e);

    return;
}

/* generates packet at the current dragonfly compute node */
void packet_generate(terminal_state * s, tw_bf * bf, terminal_message * msg, tw_lp * lp)
{
    tw_lpid dest_terminal_id;
    dest_terminal_id = model_net_find_local_device(DRAGONFLY, s->anno, 0,
            msg->final_dest_gid);
    msg->dest_terminal_id = dest_terminal_id;

    const dragonfly_param *p = s->params;

  tw_stime ts;
  tw_event *e;
  terminal_message *m;
  int i, total_event_size;
  uint64_t num_chunks = msg->packet_size / p->chunk_size;
  if (msg->packet_size % s->params->chunk_size)
      num_chunks++;
  msg->packet_ID = lp->gid + g_tw_nlp * s->packet_counter + tw_rand_integer(lp->rng, 0, lp->gid + g_tw_nlp * s->packet_counter);
  msg->travel_start_time = tw_now(lp);
  msg->my_N_hop = 0;
  for(i = 0; i < num_chunks; i++)
  {
	  // Before
	  // msg->my_N_hop = 0; generating a packet, check if the input queue is available
        ts = g_tw_lookahead + 0.1 + tw_rand_exponential(lp->rng, MEAN_INTERVAL/200);
	int chan = -1, j;
	for(j = 0; j < p->num_vcs; j++)
	 {
	     if(s->vc_occupancy[j] < p->cn_vc_size * num_chunks)
	      {
	       chan=j;
	       break;
	      }
         }

        // this is a terminal event, so use the method-event version
       //e = tw_event_new(lp->gid, i + ts, lp);
       //m = tw_event_data(e);
       //memcpy(m, msg, sizeof(terminal_message) + msg->remote_event_size_bytes + msg->local_event_size_bytes);
       void * m_data;
       e = model_net_method_event_new(lp->gid, i+ts, lp, DRAGONFLY,
               (void**)&m, &m_data);
       memcpy(m, msg, sizeof(terminal_message));
       m->dest_terminal_id = dest_terminal_id;
       void * m_data_src = model_net_method_get_edata(DRAGONFLY, msg);
       if (msg->remote_event_size_bytes){
            memcpy(m_data, m_data_src, msg->remote_event_size_bytes);
       }
       if (msg->local_event_size_bytes){ 
            memcpy((char*)m_data + msg->remote_event_size_bytes,
                    (char*)m_data_src + msg->remote_event_size_bytes,
                    msg->local_event_size_bytes);
       }
       m->intm_group_id = -1;
       m->saved_vc=0;
       m->chunk_id = i;
       
       if(msg->packet_ID == TRACK && msg->chunk_id == num_chunks-1)
         printf("\n packet generated %lld at terminal %d chunk id %d ", msg->packet_ID, (int)lp->gid, i);
       
       m->output_chan = -1;
       if(chan != -1) // If the input queue is available
   	{
	    // Send the packet out
	     m->type = T_SEND;
 	     tw_event_send(e);
        }
      else
         {
	  printf("\n Exceeded queue size, exitting %d", s->vc_occupancy[0]);
	  MPI_Finalize();
	  exit(-1);
        } //else
  } // for
  total_event_size = model_net_get_msg_sz(DRAGONFLY) + 
      msg->remote_event_size_bytes + msg->local_event_size_bytes;
  mn_stats* stat;
  stat = model_net_find_stats(msg->category, s->dragonfly_stats_array);
  stat->send_count++;
  stat->send_bytes += msg->packet_size;
  stat->send_time += (1/p->cn_bandwidth) * msg->packet_size;
  if(stat->max_event_size < total_event_size)
	  stat->max_event_size = total_event_size;

  return;
}

/* sends the packet from the current dragonfly compute node to the attached router */
void packet_send(terminal_state * s, tw_bf * bf, terminal_message * msg, tw_lp * lp)
{
  tw_stime ts;
  tw_event *e;
  terminal_message *m;
  tw_lpid router_id;
  /* Route the packet to its source router */ 
   int vc=msg->saved_vc;

   //  Each packet is broken into chunks and then sent over the channel
   msg->saved_available_time = s->terminal_available_time;
   double head_delay = (1/s->params->cn_bandwidth) * s->params->chunk_size;
   ts = head_delay + tw_rand_exponential(lp->rng, (double)head_delay/200);
   s->terminal_available_time = maxd(s->terminal_available_time, tw_now(lp));
   s->terminal_available_time += ts;

   //TODO: be annotation-aware
   codes_mapping_get_lp_info(lp->gid, lp_group_name, &mapping_grp_id, NULL,
           &mapping_type_id, NULL, &mapping_rep_id, &mapping_offset);
   codes_mapping_get_lp_id(lp_group_name, "dragonfly_router", NULL, 1,
           s->router_id, 0, &router_id);
   // we are sending an event to the router, so no method_event here
   e = tw_event_new(router_id, s->terminal_available_time - tw_now(lp), lp);

   uint64_t num_chunks = msg->packet_size/s->params->chunk_size;
   if(msg->packet_size % s->params->chunk_size)
       num_chunks++;

   if(msg->packet_ID == TRACK && msg->chunk_id == num_chunks-1)
     printf("\n terminal %d packet %lld chunk %d being sent to router %d router id %d ", (int)lp->gid, (long long)msg->packet_ID, msg->chunk_id, (int)router_id, s->router_id);
   m = tw_event_data(e);
   memcpy(m, msg, sizeof(terminal_message));
   if (msg->remote_event_size_bytes){
        memcpy(m+1, model_net_method_get_edata(DRAGONFLY, msg),
                msg->remote_event_size_bytes);
   }
   m->type = R_ARRIVE;
   m->src_terminal_id = lp->gid;
   m->saved_vc = vc;
   m->last_hop = TERMINAL;
   m->intm_group_id = -1;
   m->local_event_size_bytes = 0;
   m->local_id = s->terminal_id;
   tw_event_send(e);
//  Each chunk is 32B and the VC occupancy is in chunks to enable efficient flow control

   if(msg->chunk_id == num_chunks - 1) 
    {
      // now that message is sent, issue an "idle" event to tell the scheduler
      // when I'm next available
      model_net_method_idle_event(codes_local_latency(lp) +
              s->terminal_available_time - tw_now(lp), 0, lp);

      /* local completion message */
      if(msg->local_event_size_bytes > 0)
	 {
           tw_event* e_new;
	   terminal_message* m_new;
	   void* local_event = 
               (char*)model_net_method_get_edata(DRAGONFLY, msg) + 
               msg->remote_event_size_bytes;
	   ts = g_tw_lookahead + (1/s->params->cn_bandwidth) * msg->local_event_size_bytes;
	   e_new = tw_event_new(msg->sender_lp, ts, lp);
	   m_new = tw_event_data(e_new);
	   memcpy(m_new, local_event, msg->local_event_size_bytes);
	   tw_event_send(e_new);
	}
    }
   
   s->packet_counter++;
   s->vc_occupancy[vc]++;

   if(s->vc_occupancy[vc] >= (s->params->cn_vc_size * num_chunks))
      s->output_vc_state[vc] = VC_CREDIT;
   return;
}

/* packet arrives at the destination terminal */
void packet_arrive(terminal_state * s, tw_bf * bf, terminal_message * msg, tw_lp * lp)
{
    uint64_t num_chunks = msg->packet_size / s->params->chunk_size;
    if (msg->packet_size % s->params->chunk_size)
        num_chunks++;
#if DEBUG
if( msg->packet_ID == TRACK && msg->chunk_id == num_chunks-1)
    {
	printf( "(%lf) [Terminal %d] packet %lld has arrived  \n",
              tw_now(lp), (int)lp->gid, msg->packet_ID);

	printf("travel start time is %f\n",
                msg->travel_start_time);

	printf("My hop now is %d\n",msg->my_N_hop);
    }
#endif

  // Packet arrives and accumulate # queued
  // Find a queue with an empty buffer slot
   tw_event * e, * buf_e;
   terminal_message * m, * buf_msg;
   tw_stime ts;
   bf->c3 = 0;
   bf->c2 = 0;

   msg->my_N_hop++;
  if(msg->chunk_id == num_chunks-1)
  {
	 bf->c2 = 1;
	 mn_stats* stat = model_net_find_stats(msg->category, s->dragonfly_stats_array);
	 stat->recv_count++;
	 stat->recv_bytes += msg->packet_size;
	 stat->recv_time += tw_now(lp) - msg->travel_start_time;

	 N_finished_packets++;
	 dragonfly_total_time += tw_now( lp ) - msg->travel_start_time;
	 total_hops += msg->my_N_hop;

	 if (dragonfly_max_latency < tw_now( lp ) - msg->travel_start_time) 
	 {
		bf->c3 = 1;
		msg->saved_available_time = dragonfly_max_latency;
		dragonfly_max_latency=tw_now( lp ) - msg->travel_start_time;
	 }
	// Trigger an event on receiving server
	if(msg->remote_event_size_bytes)
	{
            void * tmp_ptr = model_net_method_get_edata(DRAGONFLY, msg);
            ts = g_tw_lookahead + 0.1 + (1/s->params->cn_bandwidth) * msg->remote_event_size_bytes;
            if (msg->is_pull){
                int net_id = model_net_get_id(LP_METHOD_NM);
                model_net_event(net_id, msg->category, msg->sender_lp,
                        msg->pull_size, ts, msg->remote_event_size_bytes,
                        tmp_ptr, 0, NULL, lp);
            }
            else{
                e = tw_event_new(msg->final_dest_gid, ts, lp);
                m = tw_event_data(e);
                memcpy(m, tmp_ptr, msg->remote_event_size_bytes);
                tw_event_send(e); 
            }
	}
  }

  int credit_delay = (1 / s->params->cn_bandwidth) * CREDIT_SIZE;
  ts = credit_delay + 0.1 + tw_rand_exponential(lp->rng, credit_delay/1000);
  
  msg->saved_credit_time = s->next_credit_available_time;
  s->next_credit_available_time = maxd(s->next_credit_available_time, tw_now(lp));
  s->next_credit_available_time += ts;

  tw_lpid router_dest_id;
  //TODO: be annotation-aware
  codes_mapping_get_lp_info(lp->gid, lp_group_name, &mapping_grp_id, NULL,
          &mapping_type_id, NULL, &mapping_rep_id, &mapping_offset);
  codes_mapping_get_lp_id(lp_group_name, "dragonfly_router", s->anno, 0,
          s->router_id, 0, &router_dest_id);
  // no method_event here - message going to router
  buf_e = tw_event_new(router_dest_id, s->next_credit_available_time - tw_now(lp), lp);
  buf_msg = tw_event_data(buf_e);
  buf_msg->vc_index = msg->saved_vc;
  buf_msg->type=R_BUFFER;
  buf_msg->packet_ID=msg->packet_ID;
  buf_msg->last_hop = TERMINAL;
  tw_event_send(buf_e);

  return;
}

/* initialize a dragonfly compute node terminal */
void 
terminal_init( terminal_state * s, 
	       tw_lp * lp )
{
    int i;
    char anno[MAX_NAME_LENGTH];

    // Assign the global router ID
    // TODO: be annotation-aware
    codes_mapping_get_lp_info(lp->gid, lp_group_name, &mapping_grp_id, NULL,
            &mapping_type_id, anno, &mapping_rep_id, &mapping_offset);
    if (anno[0] == '\0'){
        s->anno = NULL;
        s->params = &all_params[num_params-1];
    }
    else{
        s->anno = strdup(anno);
        int id = configuration_get_annotation_index(anno, anno_map);
        s->params = &all_params[id];
    }

   int num_lps = codes_mapping_get_lp_count(lp_group_name, 1, LP_CONFIG_NM,
           s->anno, 0);

   s->terminal_id = (mapping_rep_id * num_lps) + mapping_offset;  
   s->router_id=(int)s->terminal_id / (s->params->num_routers/2);
   s->terminal_available_time = 0.0;
   s->packet_counter = 0;

   s->vc_occupancy = (int*)malloc(s->params->num_vcs * sizeof(int));
   s->output_vc_state = (int*)malloc(s->params->num_vcs * sizeof(int));

   for( i = 0; i < s->params->num_vcs; i++ )
    {
      s->vc_occupancy[i]=0;
      s->output_vc_state[i]=VC_IDLE;
    }
   dragonfly_collective_init(s, lp);
   return;
}

/* collective operation for the torus network */
void dragonfly_collective(char* category, int message_size, int remote_event_size, const void* remote_event, tw_lp* sender)
{
    tw_event * e_new;
    tw_stime xfer_to_nic_time;
    terminal_message * msg;
    tw_lpid local_nic_id;
    char* tmp_ptr;

    codes_mapping_get_lp_info(sender->gid, lp_group_name, &mapping_grp_id,
            NULL, &mapping_type_id, NULL, &mapping_rep_id, &mapping_offset);
    codes_mapping_get_lp_id(lp_group_name, LP_CONFIG_NM, NULL, 1,
            mapping_rep_id, mapping_offset, &local_nic_id);

    xfer_to_nic_time = codes_local_latency(sender);
    e_new = model_net_method_event_new(local_nic_id, xfer_to_nic_time,
            sender, DRAGONFLY, (void**)&msg, (void**)&tmp_ptr);

    msg->remote_event_size_bytes = message_size;
    strcpy(msg->category, category);
    msg->sender_svr=sender->gid;
    msg->type = D_COLLECTIVE_INIT;

    tmp_ptr = (char*)msg;
    tmp_ptr += dragonfly_get_msg_sz();
    if(remote_event_size > 0)
     {
            msg->remote_event_size_bytes = remote_event_size;
            memcpy(tmp_ptr, remote_event, remote_event_size);
            tmp_ptr += remote_event_size;
     }

    tw_event_send(e_new);
    return;
}

/* reverse for collective operation of the dragonfly network */
void dragonfly_collective_rc(int message_size, tw_lp* sender)
{
     codes_local_latency_reverse(sender);
     return;
}

static void send_remote_event(terminal_state * s,
                        tw_bf * bf,
                        terminal_message * msg,
                        tw_lp * lp)
{
    // Trigger an event on receiving server
    if(msg->remote_event_size_bytes)
     {
            tw_event* e;
            tw_stime ts;
            terminal_message * m;
            ts = (1/s->params->cn_bandwidth) * msg->remote_event_size_bytes;
            e = codes_event_new(s->origin_svr, ts, lp);
            m = tw_event_data(e);
            char* tmp_ptr = (char*)msg;
            tmp_ptr += dragonfly_get_msg_sz();
            memcpy(m, tmp_ptr, msg->remote_event_size_bytes);
            tw_event_send(e);
     }
}

static void node_collective_init(terminal_state * s,
                        tw_bf * bf,
                        terminal_message * msg,
                        tw_lp * lp)
{
        tw_event * e_new;
        tw_lpid parent_nic_id;
        tw_stime xfer_to_nic_time;
        terminal_message * msg_new;
        int num_lps;

        msg->saved_collective_init_time = s->collective_init_time;
        s->collective_init_time = tw_now(lp);
	s->origin_svr = msg->sender_svr;
	
        if(s->is_leaf)
        {
            //printf("\n LP %ld sending message to parent %ld ", s->node_id, s->parent_node_id);
            /* get the global LP ID of the parent node */
            // TODO: be annotation-aware
            codes_mapping_get_lp_info(lp->gid, lp_group_name, &mapping_grp_id,
                    NULL, &mapping_type_id, NULL, &mapping_rep_id,
                    &mapping_offset);
            num_lps = codes_mapping_get_lp_count(lp_group_name, 1, LP_CONFIG_NM,
                    s->anno, 0);
            codes_mapping_get_lp_id(lp_group_name, LP_CONFIG_NM, s->anno, 0,
                    s->parent_node_id/num_lps, (s->parent_node_id % num_lps),
                    &parent_nic_id);

           /* send a message to the parent that the LP has entered the collective operation */
            xfer_to_nic_time = g_tw_lookahead + LEVEL_DELAY;
            //e_new = codes_event_new(parent_nic_id, xfer_to_nic_time, lp);
	    void* m_data;
	    e_new = model_net_method_event_new(parent_nic_id, xfer_to_nic_time,
            	lp, DRAGONFLY, (void**)&msg_new, (void**)&m_data);
	    	
            memcpy(msg_new, msg, sizeof(terminal_message));
	    if (msg->remote_event_size_bytes){
        	memcpy(m_data, model_net_method_get_edata(DRAGONFLY, msg),
                	msg->remote_event_size_bytes);
      	    }
	    
            msg_new->type = D_COLLECTIVE_FAN_IN;
            msg_new->sender_node = s->node_id;

            tw_event_send(e_new);
        }
        return;
}

static void node_collective_fan_in(terminal_state * s,
                        tw_bf * bf,
                        terminal_message * msg,
                        tw_lp * lp)
{
        int i;
        s->num_fan_nodes++;

        codes_mapping_get_lp_info(lp->gid, lp_group_name, &mapping_grp_id,
                NULL, &mapping_type_id, NULL, &mapping_rep_id, &mapping_offset);
        int num_lps = codes_mapping_get_lp_count(lp_group_name, 1, LP_CONFIG_NM,
                s->anno, 0);

        tw_event* e_new;
        terminal_message * msg_new;
        tw_stime xfer_to_nic_time;

        bf->c1 = 0;
        bf->c2 = 0;

        /* if the number of fanned in nodes have completed at the current node then signal the parent */
        if((s->num_fan_nodes == s->num_children) && !s->is_root)
        {
            bf->c1 = 1;
            msg->saved_fan_nodes = s->num_fan_nodes-1;
            s->num_fan_nodes = 0;
            tw_lpid parent_nic_id;
            xfer_to_nic_time = g_tw_lookahead + LEVEL_DELAY;

            /* get the global LP ID of the parent node */
            codes_mapping_get_lp_id(lp_group_name, LP_CONFIG_NM, s->anno, 0,
                    s->parent_node_id/num_lps, (s->parent_node_id % num_lps),
                    &parent_nic_id);

           /* send a message to the parent that the LP has entered the collective operation */
            //e_new = codes_event_new(parent_nic_id, xfer_to_nic_time, lp);
            //msg_new = tw_event_data(e_new);
	    void * m_data;
      	    e_new = model_net_method_event_new(parent_nic_id,
              xfer_to_nic_time,
              lp, DRAGONFLY, (void**)&msg_new, &m_data);
	    
            memcpy(msg_new, msg, sizeof(terminal_message));
            msg_new->type = D_COLLECTIVE_FAN_IN;
            msg_new->sender_node = s->node_id;

            if (msg->remote_event_size_bytes){
	        memcpy(m_data, model_net_method_get_edata(DRAGONFLY, msg),
        	        msg->remote_event_size_bytes);
      	   }
	    
            tw_event_send(e_new);
      }

      /* root node starts off with the fan-out phase */
      if(s->is_root && (s->num_fan_nodes == s->num_children))
      {
           bf->c2 = 1;
           msg->saved_fan_nodes = s->num_fan_nodes-1;
           s->num_fan_nodes = 0;
           send_remote_event(s, bf, msg, lp);

           for( i = 0; i < s->num_children; i++ )
           {
                tw_lpid child_nic_id;
                /* Do some computation and fan out immediate child nodes from the collective */
                xfer_to_nic_time = g_tw_lookahead + COLLECTIVE_COMPUTATION_DELAY + LEVEL_DELAY + tw_rand_exponential(lp->rng, (double)LEVEL_DELAY/50);

                /* get global LP ID of the child node */
                codes_mapping_get_lp_id(lp_group_name, LP_CONFIG_NM, NULL, 1,
                        s->children[i]/num_lps, (s->children[i] % num_lps),
                        &child_nic_id);
                //e_new = codes_event_new(child_nic_id, xfer_to_nic_time, lp);

                //msg_new = tw_event_data(e_new);
                void * m_data;
	        e_new = model_net_method_event_new(child_nic_id,
                xfer_to_nic_time,
		lp, DRAGONFLY, (void**)&msg_new, &m_data);

		memcpy(msg_new, msg, sizeof(terminal_message));
	        if (msg->remote_event_size_bytes){
	                memcpy(m_data, model_net_method_get_edata(DRAGONFLY, msg),
        	               msg->remote_event_size_bytes);
      		}
		
                msg_new->type = D_COLLECTIVE_FAN_OUT;
                msg_new->sender_node = s->node_id;

                tw_event_send(e_new);
           }
      }
}

static void node_collective_fan_out(terminal_state * s,
                        tw_bf * bf,
                        terminal_message * msg,
                        tw_lp * lp)
{
        int i;
        int num_lps = codes_mapping_get_lp_count(lp_group_name, 1, LP_CONFIG_NM,
                NULL, 1);
        bf->c1 = 0;
        bf->c2 = 0;

        send_remote_event(s, bf, msg, lp);

        if(!s->is_leaf)
        {
            bf->c1 = 1;
            tw_event* e_new;
            nodes_message * msg_new;
            tw_stime xfer_to_nic_time;

           for( i = 0; i < s->num_children; i++ )
           {
                xfer_to_nic_time = g_tw_lookahead + DRAGONFLY_FAN_OUT_DELAY + tw_rand_exponential(lp->rng, (double)DRAGONFLY_FAN_OUT_DELAY/10);

                if(s->children[i] > 0)
                {
                        tw_lpid child_nic_id;

                        /* get global LP ID of the child node */
                        codes_mapping_get_lp_id(lp_group_name, LP_CONFIG_NM,
                                s->anno, 0, s->children[i]/num_lps,
                                (s->children[i] % num_lps), &child_nic_id);
                        //e_new = codes_event_new(child_nic_id, xfer_to_nic_time, lp);
                        //msg_new = tw_event_data(e_new);
                        //memcpy(msg_new, msg, sizeof(nodes_message) + msg->remote_event_size_bytes);
			void* m_data;
			e_new = model_net_method_event_new(child_nic_id,
							xfer_to_nic_time,
					                lp, DRAGONFLY, (void**)&msg_new, &m_data);
		        memcpy(msg_new, msg, sizeof(nodes_message));
		        if (msg->remote_event_size_bytes){
			        memcpy(m_data, model_net_method_get_edata(DRAGONFLY, msg),
			                msg->remote_event_size_bytes);
      			}


                        msg_new->type = D_COLLECTIVE_FAN_OUT;
                        msg_new->sender_node = s->node_id;
                        tw_event_send(e_new);
                }
           }
         }
	//printf("\n Fan out phase completed %ld ", lp->gid);
        if(max_collective < tw_now(lp) - s->collective_init_time )
          {
              bf->c2 = 1;
              max_collective = tw_now(lp) - s->collective_init_time;
          }
}
/* update the compute node-router channel buffer */
void 
terminal_buf_update(terminal_state * s, 
		    tw_bf * bf, 
		    terminal_message * msg, 
		    tw_lp * lp)
{
  // Update the buffer space associated with this router LP 
    int msg_indx = msg->vc_index;
    
    s->vc_occupancy[msg_indx]--;
    s->output_vc_state[msg_indx] = VC_IDLE;

    return;
}

void 
terminal_event( terminal_state * s, 
		tw_bf * bf, 
		terminal_message * msg, 
		tw_lp * lp )
{
  *(int *)bf = (int)0;
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
    
    case D_COLLECTIVE_INIT:
      node_collective_init(s, bf, msg, lp);
    break;

    case D_COLLECTIVE_FAN_IN:
      node_collective_fan_in(s, bf, msg, lp);
    break;

    case D_COLLECTIVE_FAN_OUT:
      node_collective_fan_out(s, bf, msg, lp);
    break;
    
    default:
       printf("\n LP %d Terminal message type not supported %d ", (int)lp->gid, msg->type);
    }
}

void 
dragonfly_terminal_final( terminal_state * s, 
      tw_lp * lp )
{
	model_net_print_stats(lp->gid, s->dragonfly_stats_array);
}

void dragonfly_router_final(router_state * s,
		tw_lp * lp)
{
   free(s->global_channel);
}
/* get the next stop for the current packet
 * determines if it is a router within a group, a router in another group
 * or the destination terminal */
tw_lpid 
get_next_stop(router_state * s, 
		      tw_bf * bf, 
		      terminal_message * msg, 
		      tw_lp * lp, 
		      int path)
{
   int dest_lp;
   tw_lpid router_dest_id = -1;
   int i;
   int dest_group_id;

   //TODO: be annotation-aware
   codes_mapping_get_lp_info(msg->dest_terminal_id, lp_group_name,
           &mapping_grp_id, NULL, &mapping_type_id, NULL, &mapping_rep_id,
           &mapping_offset); 
   int num_lps = codes_mapping_get_lp_count(lp_group_name, 1, LP_CONFIG_NM,
           s->anno, 0);
   int dest_router_id = (mapping_offset + (mapping_rep_id * num_lps)) / s->params->num_routers;
   
   codes_mapping_get_lp_info(lp->gid, lp_group_name, &mapping_grp_id, NULL,
           &mapping_type_id, NULL, &mapping_rep_id, &mapping_offset);
   int local_router_id = (mapping_offset + mapping_rep_id);

   bf->c2 = 0;

  /* If the packet has arrived at the destination router */
   if(dest_router_id == local_router_id)
    {
        dest_lp = msg->dest_terminal_id;

        return dest_lp;
    }
   /* Generate inter-mediate destination for non-minimal routing (selecting a random group) */
   if(msg->last_hop == TERMINAL && msg->path_type == NON_MINIMAL)
    {
      if(dest_router_id / s->params->num_routers != s->group_id)
         {
            bf->c2 = 1;
            int intm_grp_id = tw_rand_integer(lp->rng, 0, s->params->num_groups-1);
            //int intm_grp_id = (s->group_id + s->group_id/2) % num_groups;
	    msg->intm_group_id = intm_grp_id;
          }    
    }
  /* It means that the packet has arrived at the inter-mediate group for non-minimal routing. Reset the group now. */
   if(msg->intm_group_id == s->group_id)
   {  
           msg->intm_group_id = -1;//no inter-mediate group
   } 
  /* Intermediate group ID is set. Divert the packet to an intermediate group. */
  if(msg->intm_group_id >= 0)
   {
      dest_group_id = msg->intm_group_id;
   }
  else /* direct the packet to the destination group */
   {
     dest_group_id = dest_router_id / s->params->num_routers;
   }
  
  /* It means the packet has arrived at the destination group. Now divert it to the destination router. */
  if(s->group_id == dest_group_id)
   {
     dest_lp = dest_router_id;
   }
   else
   {
      /* Packet is at the source or intermediate group. Find a router that has a path to the destination group. */
      dest_lp=getRouterFromGroupID(dest_group_id,s);
  
      if(dest_lp == local_router_id)
      {
        for(i=0; i < s->params->num_global_channels; i++)
           {
            if(s->global_channel[i] / s->params->num_routers == dest_group_id)
                dest_lp=s->global_channel[i];
          }
      }
   }
  codes_mapping_get_lp_id(lp_group_name, "dragonfly_router", s->anno, 0, dest_lp,
          0, &router_dest_id);
  return router_dest_id;
}

/* gets the output port corresponding to the next stop of the message */
int 
get_output_port( router_state * s, 
		tw_bf * bf, 
		terminal_message * msg, 
		tw_lp * lp, 
		int next_stop )
{
  int output_port = -1, i, terminal_id;
  codes_mapping_get_lp_info(msg->dest_terminal_id, lp_group_name,
          &mapping_grp_id, NULL, &mapping_type_id, NULL, &mapping_rep_id,
          &mapping_offset);
  int num_lps = codes_mapping_get_lp_count(lp_group_name,1,LP_CONFIG_NM,s->anno,0);
  terminal_id = (mapping_rep_id * num_lps) + mapping_offset;

  if(next_stop == msg->dest_terminal_id)
   {
      output_port = s->params->num_routers + s->params->num_global_channels +
          ( terminal_id % s->params->num_cn);
      //if(output_port > 6)
	//      printf("\n incorrect output port %d terminal id %d ", output_port, terminal_id);
    }
    else
    {
     codes_mapping_get_lp_info(next_stop, lp_group_name, &mapping_grp_id,
             NULL, &mapping_type_id, NULL, &mapping_rep_id, &mapping_offset);
     int local_router_id = mapping_rep_id + mapping_offset;
     int intm_grp_id = local_router_id / s->params->num_routers;

     if(intm_grp_id != s->group_id)
      {
        for(i=0; i < s->params->num_global_channels; i++)
         {
           if(s->global_channel[i] == local_router_id)
             output_port = s->params->num_routers + i;
          }
      }
      else
       {
        output_port = local_router_id % s->params->num_routers;
       }
//	      printf("\n output port not found %d next stop %d local router id %d group id %d intm grp id %d %d", output_port, next_stop, local_router_id, s->group_id, intm_grp_id, local_router_id%num_routers);
    }
    return output_port;
}

/* routes the current packet to the next stop */
void 
router_packet_send( router_state * s, 
		    tw_bf * bf, 
		     terminal_message * msg, tw_lp * lp)
{
//   *(int *)bf = (int)0;
   tw_stime ts;
   tw_event *e;
   terminal_message *m;

   int next_stop = -1, output_port = -1, output_chan = -1;
   float bandwidth = s->params->local_bandwidth;
   int path = s->params->routing;
   int minimal_out_port = -1, nonmin_out_port = -1;
   bf->c3 = 0;

   uint64_t num_chunks = msg->packet_size/s->params->chunk_size;
   if(msg->packet_size % s->params->chunk_size)
       num_chunks++;
    

   if(msg->last_hop == TERMINAL && s->params->routing == ADAPTIVE)
   {
  // decide which routing to take
    int minimal_next_stop=get_next_stop(s, bf, msg, lp, MINIMAL);
    minimal_out_port = get_output_port(s, bf, msg, lp, minimal_next_stop);
    int nonmin_next_stop = get_next_stop(s, bf, msg, lp, NON_MINIMAL);
    nonmin_out_port = get_output_port(s, bf, msg, lp, nonmin_next_stop);
    int nonmin_port_count = s->vc_occupancy[nonmin_out_port];
    int min_port_count = s->vc_occupancy[minimal_out_port];
    int nonmin_vc = s->vc_occupancy[nonmin_out_port * s->params->num_vcs + 2];
    int min_vc = s->vc_occupancy[minimal_out_port * s->params->num_vcs + 1];

    // Adaptive routing condition from the dragonfly paper Page 83
   // modified according to booksim adaptive routing condition
   if((min_vc <= (nonmin_vc * 2 + adaptive_threshold) && minimal_out_port == nonmin_out_port)
               || (min_port_count <= (nonmin_port_count * 2 + adaptive_threshold) && minimal_out_port != nonmin_out_port))
        {
	   msg->path_type = MINIMAL;
           next_stop = minimal_next_stop;
           output_port = minimal_out_port;
           minimal_count++;
           msg->intm_group_id = -1;

           if(msg->packet_ID == TRACK)
              printf("\n (%lf) [Router %d] Packet %d routing minimally ", tw_now(lp), (int)lp->gid, (int)msg->packet_ID);
        }
       else
         {
	   msg->path_type = NON_MINIMAL;
           next_stop = nonmin_next_stop;
           output_port = nonmin_out_port;
           nonmin_count++;
           if(msg->packet_ID == TRACK)
                printf("\n (%lf) [Router %d] Packet %d routing non-minimally ", tw_now(lp), (int)lp->gid, (int)msg->packet_ID);

         }
  }
  else
   {
	msg->path_type = routing; /*defaults to the routing algorithm if we don't have adaptive routing here*/
   	next_stop = get_next_stop(s, bf, msg, lp, path);
   	output_port = get_output_port(s, bf, msg, lp, next_stop); 
   }
   output_chan = output_port * s->params->num_vcs;
    // Even numbered channels for minimal routing
   // Odd numbered channels for nonminimal routing
   // Separate the queue occupancy into minimal and non minimal virtual channels if the min & non min
   // paths start at the same output port
   /*if((routing == ADAPTIVE) && (minimal_out_port == nonmin_out_port))
   {
        if(path == MINIMAL)
          output_chan = output_chan + 1;
        else
          if(path == NON_MINIMAL)
            output_chan = output_chan + 2;
   }*/

   int global=0;
   int buf_size = s->params->local_vc_size;

   assert(output_port != -1);
   assert(output_chan != -1);
   // Allocate output Virtual Channel
  if(output_port >= s->params->num_routers && 
          output_port < s->params->num_routers + s->params->num_global_channels)
  {
	 bandwidth = s->params->global_bandwidth;
	 global = 1;
	 buf_size = s->params->global_vc_size;
  }

  if(output_port >= s->params->num_routers + s->params->num_global_channels)
	buf_size = s->params->cn_vc_size;

   if(s->vc_occupancy[output_chan] >= buf_size)
    {
	    printf("\n %lf Router %ld buffers overflowed from incoming terminals channel %d occupancy %d radix %d next_stop %d ", tw_now(lp),(long int) lp->gid, output_chan, s->vc_occupancy[output_chan], s->params->radix, next_stop);
	    bf->c3 = 1;
	    return;
	    //MPI_Finalize();
	    //exit(-1);
    }

#if DEBUG
if( msg->packet_ID == TRACK && next_stop != msg->dest_terminal_id && msg->chunk_id == num_chunks-1)
  {
   printf("\n (%lf) [Router %d] Packet %lld being sent to intermediate group router %d Final destination terminal %d Output Channel Index %d Saved vc %d msg_intm_id %d \n", 
              tw_now(lp), (int)lp->gid, msg->packet_ID, next_stop, 
	      msg->dest_terminal_id, output_chan, msg->saved_vc, msg->intm_group_id);
  }
#endif
 // If source router doesn't have global channel and buffer space is available, then assign to appropriate intra-group virtual channel 
  msg->saved_available_time = s->next_output_available_time[output_port];
  ts = g_tw_lookahead + 0.1 + ((1/bandwidth) * s->params->chunk_size) + tw_rand_exponential(lp->rng, (double)s->params->chunk_size/200);

  s->next_output_available_time[output_port] = maxd(s->next_output_available_time[output_port], tw_now(lp));
  s->next_output_available_time[output_port] += ts;
  // dest can be a router or a terminal, so we must check
  void * m_data;
  if (next_stop == msg->dest_terminal_id){
      e = model_net_method_event_new(next_stop, 
              s->next_output_available_time[output_port] - tw_now(lp), lp,
              DRAGONFLY, (void**)&m, &m_data);
  }
  else{
      e = tw_event_new(next_stop, s->next_output_available_time[output_port] - tw_now(lp), lp);
      m = tw_event_data(e);
      m_data = m+1;
  }
  memcpy(m, msg, sizeof(terminal_message));
  if (msg->remote_event_size_bytes){
      memcpy(m_data, msg+1, msg->remote_event_size_bytes);
  }

  if(global)
    m->last_hop=GLOBAL;
  else
    m->last_hop = LOCAL;

  m->saved_vc = output_chan;
  m->local_id = s->router_id;
  msg->old_vc = output_chan;
  m->intm_lp_id = lp->gid;
  s->vc_occupancy[output_chan]++;

  /* Determine the event type. If the packet has arrived at the final destination
     router then it should arrive at the destination terminal next. */
  if(next_stop == msg->dest_terminal_id)
  {
    m->type = T_ARRIVE;

    if(s->vc_occupancy[output_chan] >= s->params->cn_vc_size * num_chunks)
      s->output_vc_state[output_chan] = VC_CREDIT;
  }
  else
  {
    /* The packet has to be sent to another router */
    m->type = R_ARRIVE;

   /* If this is a global channel then the buffer space is different */
   if( global )
   {
     if(s->vc_occupancy[output_chan] >= s->params->global_vc_size * num_chunks )
       s->output_vc_state[output_chan] = VC_CREDIT;
   }
  else
    {
     /* buffer space is less for local channels */
     if( s->vc_occupancy[output_chan] >= s->params->local_vc_size * num_chunks )
	s->output_vc_state[output_chan] = VC_CREDIT;
    }
  }
  tw_event_send(e);
  return;
}

/* Packet arrives at the router and a credit is sent back to the sending terminal/router */
void 
router_packet_receive( router_state * s, 
			tw_bf * bf, 
			terminal_message * msg, 
			tw_lp * lp )
{
    tw_event *e;
    terminal_message *m;
    tw_stime ts;

    msg->my_N_hop++;
    ts = g_tw_lookahead + 0.1 + tw_rand_exponential(lp->rng, (double)MEAN_INTERVAL/200);
    uint64_t num_chunks = msg->packet_size/s->params->chunk_size;
    if(msg->packet_size % s->params->chunk_size)
        num_chunks++;

    if(msg->packet_ID == TRACK && msg->chunk_id == num_chunks-1)
       printf("\n packet %lld chunk %d received at router %d ", msg->packet_ID, msg->chunk_id, (int)lp->gid);
   
    // router self message - no need for method_event
    e = tw_event_new(lp->gid, ts, lp);
    m = tw_event_data(e);
    memcpy(m, msg, sizeof(terminal_message) + msg->remote_event_size_bytes);
    m->type = R_SEND;
    router_credit_send(s, bf, msg, lp);
    tw_event_send(e);  
    return;
}

/* sets up the router virtual channels, global channels, local channels, compute node channels */
void router_setup(router_state * r, tw_lp * lp)
{
    char anno[MAX_NAME_LENGTH];
    codes_mapping_get_lp_info(lp->gid, lp_group_name, &mapping_grp_id, NULL,
            &mapping_type_id, anno, &mapping_rep_id, &mapping_offset);

    if (anno[0] == '\0'){
        r->anno = NULL;
        r->params = &all_params[num_params-1];
    }
    else{
        r->anno = strdup(anno);
        int id = configuration_get_annotation_index(anno, anno_map);
        r->params = &all_params[id];
    }

    // shorthand
    const dragonfly_param *p = r->params;

   r->router_id=mapping_rep_id + mapping_offset;
   r->group_id=r->router_id/p->num_routers;

   int i;
   int router_offset=(r->router_id % p->num_routers) * (p->num_global_channels / 2) + 1;

   r->global_channel = (int*)malloc(p->num_global_channels * sizeof(int));
   r->next_output_available_time = (tw_stime*)malloc(p->radix * sizeof(tw_stime));
   r->next_credit_available_time = (tw_stime*)malloc(p->radix * sizeof(tw_stime));
   r->vc_occupancy = (int*)malloc(p->radix * sizeof(int));
   r->output_vc_state = (int*)malloc(p->radix * sizeof(int));
  
   for(i=0; i < p->radix; i++)
    {
       // Set credit & router occupancy
	r->next_output_available_time[i]=0;
        r->next_credit_available_time[i]=0;
        r->vc_occupancy[i]=0;
        r->output_vc_state[i]= VC_IDLE;
    }

   //round the number of global channels to the nearest even number
   for(i=0; i < p->num_global_channels; i++)
    {
      if(i % 2 != 0)
          {
             r->global_channel[i]=(r->router_id + (router_offset * p->num_routers))%p->total_routers;
             router_offset++;
          }
          else
           {
             r->global_channel[i]=r->router_id - ((router_offset) * p->num_routers);
           }
        if(r->global_channel[i]<0)
         {
           r->global_channel[i]=p->total_routers+r->global_channel[i]; 
	 }
    }
   return;
}	

/* Update the buffer space associated with this router LP */
void router_buf_update(router_state * s, tw_bf * bf, terminal_message * msg, tw_lp * lp)
{
    int msg_indx = msg->vc_index;
    s->vc_occupancy[msg_indx]--;
    s->output_vc_state[msg_indx] = VC_IDLE;
    return;
}

void router_event(router_state * s, tw_bf * bf, terminal_message * msg, tw_lp * lp)
{
  *(int *)bf = (int)0;
  switch(msg->type)
   {
	   case R_SEND: // Router has sent a packet to an intra-group router (local channel)
 		 router_packet_send(s, bf, msg, lp);
           break;

	   case R_ARRIVE: // Router has received a packet from an intra-group router (local channel)
	        router_packet_receive(s, bf, msg, lp);
	   break;
	
	   case R_BUFFER:
	        router_buf_update(s, bf, msg, lp);
	   break;

	   default:
		  printf("\n (%lf) [Router %d] Router Message type not supported %d dest terminal id %d packet ID %d ", tw_now(lp), (int)lp->gid, msg->type, (int)msg->dest_terminal_id, (int)msg->packet_ID);
	   break;
   }	   
}

/* Reverse computation handler for a terminal event */
void terminal_rc_event_handler(terminal_state * s, tw_bf * bf, terminal_message * msg, tw_lp * lp)
{
    uint64_t num_chunks = msg->packet_size/s->params->chunk_size;
    if(msg->packet_size % s->params->chunk_size)
        num_chunks++;
   switch(msg->type)
   {
	   case T_GENERATE:
		 {
		 int i;
		 tw_rand_reverse_unif(lp->rng);

		 for(i = 0; i < num_chunks; i++)
                     tw_rand_reverse_unif(lp->rng);
		 mn_stats* stat;
		 stat = model_net_find_stats(msg->category, s->dragonfly_stats_array);
		 stat->send_count--;
		 stat->send_bytes -= msg->packet_size;
		 stat->send_time -= (1/s->params->cn_bandwidth) * msg->packet_size;
		 }
	   break;
	   
	   case T_SEND:
	         {
	           s->terminal_available_time = msg->saved_available_time;
		   tw_rand_reverse_unif(lp->rng);
		   int vc = msg->saved_vc;
		   s->vc_occupancy[vc]--;
		   s->packet_counter--;
		   s->output_vc_state[vc] = VC_IDLE;

                   uint64_t num_chunks = msg->packet_size / s->params->chunk_size;
                   if (msg->chunk_id == num_chunks-1){
                     codes_local_latency_reverse(lp);
                   }
		 }
	   break;

	   case T_ARRIVE:
	   	 {
		   tw_rand_reverse_unif(lp->rng);
		   s->next_credit_available_time = msg->saved_credit_time;
		   if(bf->c2)
		   {
		    mn_stats* stat;
		    stat = model_net_find_stats(msg->category, s->dragonfly_stats_array);
		    stat->recv_count--;
		    stat->recv_bytes -= msg->packet_size;
		    stat->recv_time -= tw_now(lp) - msg->travel_start_time;
		    N_finished_packets--;
		    dragonfly_total_time -= (tw_now(lp) - msg->travel_start_time);
		    total_hops -= msg->my_N_hop;
		   if(bf->c3)
		         dragonfly_max_latency = msg->saved_available_time;
		   }
		    
		   msg->my_N_hop--;
		 }
           break;

	   case T_BUFFER:
	        {
		   int msg_indx = msg->vc_index;
		   s->vc_occupancy[msg_indx]++;
		   if(s->vc_occupancy[msg_indx] == s->params->cn_vc_size)
			s->output_vc_state[msg_indx] = VC_CREDIT;
	     }  
	   break;
	
          case D_COLLECTIVE_INIT:
                {
                    s->collective_init_time = msg->saved_collective_init_time;
                }
          break;

          case D_COLLECTIVE_FAN_IN:
                {
                   int i;
                   s->num_fan_nodes--;
                   if(bf->c1)
                    {
                        s->num_fan_nodes = msg->saved_fan_nodes;
                    }
                   if(bf->c2)
                     {
                        s->num_fan_nodes = msg->saved_fan_nodes;
                        for( i = 0; i < s->num_children; i++ )
                            tw_rand_reverse_unif(lp->rng);
                     }
                }
        break;

        case D_COLLECTIVE_FAN_OUT:
                {
                 int i;
                 if(bf->c1)
                    {
                        for( i = 0; i < s->num_children; i++ )
                            tw_rand_reverse_unif(lp->rng);
                    }
                }	 
   }
}

/* Reverse computation handler for a router event */
void router_rc_event_handler(router_state * s, tw_bf * bf, terminal_message * msg, tw_lp * lp)
{
    uint64_t num_chunks = msg->packet_size/s->params->chunk_size;
    if(msg->packet_size % s->params->chunk_size)
        num_chunks++;
  switch(msg->type)
    {
            case R_SEND:
		    {
			if(msg->path_type == NON_MINIMAL && bf->c2)
			   tw_rand_reverse_unif(lp->rng);

			if(routing == ADAPTIVE && msg->path_type == MINIMAL)
                                minimal_count--;
                        if(routing == ADAPTIVE && msg->path_type == NON_MINIMAL)
                                nonmin_count--;

			if(bf->c3)
			   return;
			    
		        tw_rand_reverse_unif(lp->rng);
			int output_chan = msg->old_vc;
			int output_port = output_chan / s->params->num_vcs;

			s->next_output_available_time[output_port] = msg->saved_available_time;
			s->vc_occupancy[output_chan]--;
			s->output_vc_state[output_chan]=VC_IDLE;

		    }
	    break;

	    case R_ARRIVE:
	    	    {
			msg->my_N_hop--;
			tw_rand_reverse_unif(lp->rng);
			tw_rand_reverse_unif(lp->rng);
			int output_port = msg->saved_vc/s->params->num_vcs;
			s->next_credit_available_time[output_port] = msg->saved_available_time;
                        if (msg->chunk_id == num_chunks-1 && 
                                msg->remote_event_size_bytes && 
                                msg->is_pull){
                            int net_id = model_net_get_id(LP_METHOD_NM);
                            model_net_event_rc(net_id, lp, msg->pull_size);
                        }
		    }
	    break;

	    case R_BUFFER:
	    	   {
		      int msg_indx = msg->vc_index;
                      s->vc_occupancy[msg_indx]++;

                      int buf = s->params->local_vc_size;

		      if(msg->last_hop == GLOBAL)
			 buf = s->params->global_vc_size;
		       else if(msg->last_hop == TERMINAL)
			 buf = s->params->cn_vc_size;
	 
		      if(s->vc_occupancy[msg_indx] >= buf * num_chunks)
                          s->output_vc_state[msg_indx] = VC_CREDIT;

		   }
	    break;
	  
    }
}
/* dragonfly compute node and router LP types */
tw_lptype dragonfly_lps[] =
{
   // Terminal handling functions
   {
    (init_f)terminal_init,
    (pre_run_f) NULL,
    (event_f) terminal_event,
    (revent_f) terminal_rc_event_handler,
    (final_f) dragonfly_terminal_final,
    (map_f) codes_mapping,
    sizeof(terminal_state)
    },
   {
     (init_f) router_setup,
     (pre_run_f) NULL,
     (event_f) router_event,
     (revent_f) router_rc_event_handler,
     (final_f) dragonfly_router_final,
     (map_f) codes_mapping,
     sizeof(router_state),
   },
   {0},
};

/* returns the dragonfly lp type for lp registration */
static const tw_lptype* dragonfly_get_cn_lp_type(void)
{
	   return(&dragonfly_lps[0]);
}
static const tw_lptype* dragonfly_get_router_lp_type(void)
{
	           return(&dragonfly_lps[1]);
}          

static tw_lpid dragonfly_find_local_device(
        const char * annotation,
        int          ignore_annotations,
        tw_lp      * sender)
{
     int mapping_grp_id, mapping_rep_id, mapping_type_id, mapping_offset;
     tw_lpid dest_id;

     codes_mapping_get_lp_info(sender->gid, lp_group_name, &mapping_grp_id,
             NULL, &mapping_type_id, NULL, &mapping_rep_id, &mapping_offset);
     codes_mapping_get_lp_id(lp_group_name, LP_CONFIG_NM, annotation,
             ignore_annotations, mapping_rep_id, mapping_offset, &dest_id);

    return(dest_id);
}

static void dragonfly_register(tw_lptype *base_type) {
    lp_type_register(LP_CONFIG_NM, base_type);
    lp_type_register("dragonfly_router", &dragonfly_lps[1]);
}

/* data structure for dragonfly statistics */
struct model_net_method dragonfly_method =
{
    .mn_configure = dragonfly_configure,
    .mn_register = dragonfly_register,
    .model_net_method_packet_event = dragonfly_packet_event,
    .model_net_method_packet_event_rc = dragonfly_packet_event_rc,
    .model_net_method_recv_msg_event = NULL,
    .model_net_method_recv_msg_event_rc = NULL,
    .mn_get_lp_type = dragonfly_get_cn_lp_type,
    .mn_get_msg_sz = dragonfly_get_msg_sz,
    .mn_report_stats = dragonfly_report_stats,
    .model_net_method_find_local_device = NULL,
    .mn_collective_call = dragonfly_collective,
    .mn_collective_call_rc = dragonfly_collective_rc   
};


/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
