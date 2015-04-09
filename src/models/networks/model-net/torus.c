/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#include <ross.h>
#include <assert.h>
#include <string.h>

#include "codes/lp-io.h"
#include "codes/codes_mapping.h"
#include "codes/codes.h"
#include "codes/model-net.h"
#include "codes/model-net-method.h"
#include "codes/model-net-lp.h"
#include "codes/net/torus.h"

#define CHUNK_SIZE 32
#define DEBUG 1
#define MEAN_INTERVAL 100
#define TRACE -1

/* collective specific parameters */
#define TREE_DEGREE 4
#define LEVEL_DELAY 1000
#define TORUS_COLLECTIVE_DEBUG 0
#define NUM_COLLECTIVES  1
#define COLLECTIVE_COMPUTATION_DELAY 5700
#define TORUS_FAN_OUT_DELAY 20.0

#define LP_CONFIG_NM (model_net_lp_config_names[TORUS])
#define LP_METHOD_NM (model_net_method_names[TORUS])

static double maxd(double a, double b) { return a < b ? b : a; }

/* Torus network model implementation of codes, implements the modelnet API */

typedef struct torus_param torus_param;
struct torus_param
{
    int n_dims; /*Dimension of the torus network, 5-D, 7-D or any other*/
    int* dim_length; /*Length of each torus dimension*/
    double link_bandwidth;/* bandwidth for each torus link */
    int buffer_size; /* number of buffer slots for each vc in flits*/
    //int num_net_traces; /* number of network traces to be mapped on torus */
    int num_vc; /* number of virtual channels for each torus link */
    float mean_process;/* mean process time for each flit  */
    int chunk_size; /* chunk is the smallest unit--default set to 32 */

    /* "derived" torus parameters */

    /* factor, used in torus coordinate calculation */
    int * factor;
    /* half length of each dimension, used in torus coordinates calculation */
    int * half_length;

    double head_delay;
    double credit_delay;
};

/* codes mapping group name, lp type name */
static char grp_name[MAX_NAME_LENGTH];
/* codes mapping group id, lp type id, repetition id and offset */
int mapping_grp_id, mapping_rep_id, mapping_type_id, mapping_offset;

/* for calculating torus model statistics, average and maximum travel time of a packet */
static tw_stime         total_time = 0;
static tw_stime         max_latency = 0;
static tw_stime         max_collective = 0;

/* number of finished packets on each PE */
static long long       N_finished_packets = 0;
/* total number of hops traversed by a message on each PE */
static long long       total_hops = 0;

/* annotation-specific parameters (unannotated entry occurs at the 
 * last index) */
static uint64_t                  num_params = 0;
static torus_param             * all_params = NULL;
static const config_anno_map_t * anno_map   = NULL;

typedef struct nodes_state nodes_state;

/* state of a torus node */
struct nodes_state
{
  /* counts the number of packets sent from this compute node */
  unsigned long long packet_counter;            
  /* availability time of each torus link */
  tw_stime** next_link_available_time; 
  /* availability of each torus credit link */
  tw_stime** next_credit_available_time;
  /* next flit generate time */
  tw_stime** next_flit_generate_time;
  /* buffer size for each torus virtual channel */
  int** buffer;
  /* coordinates of the current torus node */
  int* dim_position;
  /* neighbor LP ids for this torus node */
  int* neighbour_minus_lpID;
  int* neighbour_plus_lpID;

  /* records torus statistics for this LP having different communication categories */
  struct mn_stats torus_stats_array[CATEGORY_MAX];
   /* for collective operations */

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

   /* LPs annotation */
   const char * anno;
   /* LPs configuration */
   const torus_param * params;
};

static void torus_read_config(
        const char         * anno,
        torus_param        * params){
    char dim_length_str[MAX_NAME_LENGTH];
    int i;

    // shorthand
    torus_param *p = params;

    configuration_get_value_int(&config, "PARAMS", "n_dims", anno, &p->n_dims);
    if(!p->n_dims) {
        p->n_dims = 4; /* a 4-D torus */
        fprintf(stderr, 
                "Warning: Number of dimensions not specified, setting to %d\n",
                p->n_dims);
    }

    configuration_get_value_double(&config, "PARAMS", "link_bandwidth", anno,
            &p->link_bandwidth);
    if(!p->link_bandwidth) {
        p->link_bandwidth = 2.0; /*default bg/q configuration */
        fprintf(stderr, "Link bandwidth not specified, setting to %lf\n",
                p->link_bandwidth);
    }

    configuration_get_value_int(&config, "PARAMS", "buffer_size", anno, &p->buffer_size);
    if(!p->buffer_size) {
        p->buffer_size = 2048;
        fprintf(stderr, "Buffer size not specified, setting to %d",
                p->buffer_size);
    }


    configuration_get_value_int(&config, "PARAMS", "chunk_size", anno, &p->chunk_size);
    if(!p->chunk_size) {
        p->chunk_size = 32;
        fprintf(stderr, "Warning: Chunk size not specified, setting to %d\n",
                p->chunk_size);
    }
    configuration_get_value_int(&config, "PARAMS", "num_vc", anno, &p->num_vc);
    if(!p->num_vc) {
        /* by default, we have one for taking packets,
         * another for taking credit*/
        p->num_vc = 1;
        fprintf(stderr, "Warning: num_vc not specified, setting to %d\n",
                p->num_vc);
    }

    int rc = configuration_get_value(&config, "PARAMS", "dim_length", anno,
            dim_length_str, MAX_NAME_LENGTH);
    if (rc == 0){
        tw_error(TW_LOC, "couldn't read PARAMS:dim_length");
    }
    char* token;
    p->dim_length=malloc(p->n_dims*sizeof(*p->dim_length));
    token = strtok(dim_length_str, ",");
    i = 0;
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
    /*int num_nodes = 1;

    for( i = 0; i < p->n_dims; i++)
	   num_nodes *= p->dim_length[i];
    
    configuration_get_value_int(&config, "PARAMS", "num_net_traces", anno, &p->num_net_traces);
    if(!p->num_net_traces) {

	p->num_net_traces = num_nodes;
        fprintf(stderr, "Number of network traces not specified, setting to %d",
                p->num_net_traces);
    }
   // Number of network traces should be <= number of torus network nodes `
   assert(p->num_net_traces <= num_nodes);*/
    // create derived parameters
   
    // factor is an exclusive prefix product
    p->factor = malloc(p->n_dims * sizeof(int));
    p->factor[0] = 1;
    for(i = 1; i < p->n_dims; i++)
        p->factor[i] = p->factor[i-1] * p->dim_length[i-1];

    p->half_length = malloc(p->n_dims * sizeof(int));
    for (i = 0; i < p->n_dims; i++)
        p->half_length[i] = p->dim_length[i] / 2;

    // some latency numbers
    p->head_delay = (1.0 / p->link_bandwidth) * p->chunk_size;
    p->credit_delay = (1.0 / p->link_bandwidth) * p->chunk_size;
}

static void torus_configure(){
    anno_map = codes_mapping_get_lp_anno_map(LP_CONFIG_NM);
    assert(anno_map);
    num_params = anno_map->num_annos + (anno_map->has_unanno_lp > 0);
    all_params = malloc(num_params * sizeof(*all_params));

    for (uint64_t i = 0; i < anno_map->num_annos; i++){
        const char * anno = anno_map->annotations[i];
        torus_read_config(anno, &all_params[i]);
    }
    if (anno_map->has_unanno_lp > 0){
        torus_read_config(NULL, &all_params[anno_map->num_annos]);
    }
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

void torus_collective_init(nodes_state * s,
           		   tw_lp * lp)
{
    // TODO: be annotation-aware somehow 
    codes_mapping_get_lp_info(lp->gid, grp_name, &mapping_grp_id, NULL, &mapping_type_id, NULL, &mapping_rep_id, &mapping_offset);
    int num_lps = codes_mapping_get_lp_count(grp_name, 1, LP_CONFIG_NM, s->anno, 0);
    int num_reps = codes_mapping_get_group_reps(grp_name);
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

#if TORUS_COLLECTIVE_DEBUG == 1
   printf("\n LP %ld parent node id ", s->node_id);

   for( i = 0; i < TREE_DEGREE; i++ )
        printf(" child node ID %ld ", s->children[i]);
   printf("\n");

   if(s->is_leaf)
        printf("\n LP %ld is leaf ", s->node_id);
#endif
}
/* torus packet reverse event */
static void torus_packet_event_rc(tw_lp *sender)
{
  codes_local_latency_reverse(sender);
  return;
}

/* returns the torus message size */
static int torus_get_msg_sz(void)
{
   return sizeof(nodes_message);
}

/* torus packet event , generates a torus packet on the compute node */
static tw_stime torus_packet_event(char* category, tw_lpid final_dest_lp, uint64_t packet_size, int is_pull, uint64_t pull_size, tw_stime offset, const mn_sched_params *sched_params, int remote_event_size, const void* remote_event, int self_event_size, const void* self_event, tw_lpid src_lp, tw_lp *sender, int is_last_pckt)
{
    tw_event * e_new;
    tw_stime xfer_to_nic_time;
    nodes_message * msg;
    char* tmp_ptr;
   
    xfer_to_nic_time = g_tw_lookahead + codes_local_latency(sender); /* Throws an error of found last KP time > current event time otherwise */
    //e_new = tw_event_new(local_nic_id, xfer_to_nic_time+offset, sender);
    //msg = tw_event_data(e_new);
    e_new = model_net_method_event_new(sender->gid, xfer_to_nic_time+offset,
            sender, TORUS, (void**)&msg, (void**)&tmp_ptr);
    strcpy(msg->category, category);
    msg->final_dest_gid = final_dest_lp;
    msg->sender_svr= src_lp;
    msg->packet_size = packet_size;
    msg->remote_event_size_bytes = 0;
    msg->local_event_size_bytes = 0;
    msg->type = GENERATE;
    msg->is_pull = is_pull;
    msg->pull_size = pull_size;
    
    if(is_last_pckt) /* Its the last packet so pass in remote event information*/
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
      // printf("\n torus remote event %d local event %d last packet %d %lf ", msg->remote_event_size_bytes, msg->local_event_size_bytes, is_last_pckt, xfer_to_nic_time);
     }
    tw_event_send(e_new);
    return xfer_to_nic_time;
}

/*Initialize the torus model, this initialization part is borrowed from Ning's torus model */
static void torus_init( nodes_state * s, 
	   tw_lp * lp )
{
    int i, j;
    char anno[MAX_NAME_LENGTH];

    codes_mapping_get_lp_info(lp->gid, grp_name, &mapping_grp_id, NULL, &mapping_type_id, anno, &mapping_rep_id, &mapping_offset);
    if (anno[0] == '\0'){
        s->anno = NULL;
        s->params = &all_params[num_params-1];
    }
    else{
        s->anno = strdup(anno);
        int id = configuration_get_annotation_index(anno, anno_map);
        s->params = &all_params[id];
    }

    // shorthand
    const torus_param *p = s->params;

    s->neighbour_minus_lpID = (int*)malloc(p->n_dims * sizeof(int));
    s->neighbour_plus_lpID = (int*)malloc(p->n_dims * sizeof(int));
    s->dim_position = (int*)malloc(p->n_dims * sizeof(int));
    s->buffer = (int**)malloc(2*p->n_dims * sizeof(int*));
    s->next_link_available_time = 
        (tw_stime**)malloc(2*p->n_dims * sizeof(tw_stime*));
    s->next_credit_available_time = 
        (tw_stime**)malloc(2*p->n_dims * sizeof(tw_stime*));
    s->next_flit_generate_time = 
        (tw_stime**)malloc(2*p->n_dims*sizeof(tw_stime*));

    for(i=0; i < 2*p->n_dims; i++)
    {
	s->buffer[i] = (int*)malloc(p->num_vc * sizeof(int));
	s->next_link_available_time[i] =
            (tw_stime*)malloc(p->num_vc * sizeof(tw_stime));
	s->next_credit_available_time[i] = 
            (tw_stime*)malloc(p->num_vc * sizeof(tw_stime));
	s->next_flit_generate_time[i] = 
            (tw_stime*)malloc(p->num_vc * sizeof(tw_stime));
    }

    // calculate my torus coords
    to_dim_id(codes_mapping_get_lp_relative_id(lp->gid, 0, 1),
            s->params->n_dims, s->params->dim_length, s->dim_position);
    /* DEBUG
    printf("%lu: my coords:", lp->gid);
    for (i = 0; i < p->n_dims; i++)
        printf(" %d", s->dim_position[i]);
    printf("\n");
    */

  int temp_dim_pos[ p->n_dims ];
  for ( i = 0; i < p->n_dims; i++ )
    temp_dim_pos[ i ] = s->dim_position[ i ];

  // calculate minus neighbour's lpID
  for ( j = 0; j < p->n_dims; j++ )
    {
      temp_dim_pos[ j ] = (s->dim_position[ j ] -1 + p->dim_length[ j ]) %
          p->dim_length[ j ];

      s->neighbour_minus_lpID[j] =
          to_flat_id(p->n_dims, p->dim_length, temp_dim_pos);

      /* DEBUG
      printf(" minus neighbor: flat:%d lpid:%lu\n",
              s->neighbour_minus_lpID[j],
              codes_mapping_get_lpid_from_relative(s->neighbour_minus_lpID[j],
                      NULL, LP_CONFIG_NM, s->anno, 1));
      */

      temp_dim_pos[ j ] = s->dim_position[ j ];
    }
  // calculate plus neighbour's lpID
  for ( j = 0; j < p->n_dims; j++ )
    {
      temp_dim_pos[ j ] = ( s->dim_position[ j ] + 1 + p->dim_length[ j ]) %
          p->dim_length[ j ];

      s->neighbour_plus_lpID[j] =
          to_flat_id(p->n_dims, p->dim_length, temp_dim_pos);

      /* DEBUG
      printf(" plus neighbor: flat:%d lpid:%lu\n",
              s->neighbour_plus_lpID[j],
              codes_mapping_get_lpid_from_relative(s->neighbour_plus_lpID[j],
                      NULL, LP_CONFIG_NM, s->anno, 1));
      */

      temp_dim_pos[ j ] = s->dim_position[ j ];
    }

  //printf("\n");
  for( j=0; j < 2 * p->n_dims; j++ )
   {
    for( i = 0; i < p->num_vc; i++ )
     {
       s->buffer[ j ][ i ] = 0; 
       s->next_link_available_time[ j ][ i ] = 0.0;
       s->next_credit_available_time[j][i] = 0.0; 
     }
   }
  // record LP time
    s->packet_counter = 0;
    torus_collective_init(s, lp);
}


/* collective operation for the torus network */
void torus_collective(char* category, int message_size, int remote_event_size, const void* remote_event, tw_lp* sender)
{
    tw_event * e_new;
    tw_stime xfer_to_nic_time;
    nodes_message * msg;
    tw_lpid local_nic_id;
    char* tmp_ptr;

    // TODO: be annotation-aware
    codes_mapping_get_lp_info(sender->gid, grp_name, &mapping_grp_id, NULL,
            &mapping_type_id, NULL, &mapping_rep_id, &mapping_offset);
    codes_mapping_get_lp_id(grp_name, LP_CONFIG_NM, NULL, 1,
            mapping_rep_id, mapping_offset, &local_nic_id);

    xfer_to_nic_time = g_tw_lookahead + codes_local_latency(sender);
    e_new = model_net_method_event_new(local_nic_id, xfer_to_nic_time,
            sender, TORUS, (void**)&msg, (void**)&tmp_ptr);

    msg->remote_event_size_bytes = message_size;
    strcpy(msg->category, category);
    msg->sender_svr=sender->gid;
    msg->type = T_COLLECTIVE_INIT;

    tmp_ptr = (char*)msg;
    tmp_ptr += torus_get_msg_sz();
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
void torus_collective_rc(int message_size, tw_lp* sender)
{
     codes_local_latency_reverse(sender);
     return;
}

static void send_remote_event(nodes_state * s,
                        tw_bf * bf,
                        nodes_message * msg,
                        tw_lp * lp)
{
    // Trigger an event on receiving server
    if(msg->remote_event_size_bytes)
     {
            tw_event* e;
            tw_stime ts;
            nodes_message * m;
            ts = (1/s->params->link_bandwidth) * msg->remote_event_size_bytes;
            e = codes_event_new(s->origin_svr, ts, lp);
            m = tw_event_data(e);
            char* tmp_ptr = (char*)msg;
            tmp_ptr += torus_get_msg_sz();
            memcpy(m, tmp_ptr, msg->remote_event_size_bytes);
            tw_event_send(e);
     }
}

static void node_collective_init(nodes_state * s,
                        tw_bf * bf,
                        nodes_message * msg,
                        tw_lp * lp)
{
        tw_event * e_new;
        tw_lpid parent_nic_id;
        tw_stime xfer_to_nic_time;
        nodes_message * msg_new;
        int num_lps;

        msg->saved_collective_init_time = s->collective_init_time;
        s->collective_init_time = tw_now(lp);
	s->origin_svr = msg->sender_svr;
	
        if(s->is_leaf)
        {
            //printf("\n LP %ld sending message to parent %ld ", s->node_id, s->parent_node_id);
            /* get the global LP ID of the parent node */
            // TODO: be annotation-aware
            codes_mapping_get_lp_info(lp->gid, grp_name, &mapping_grp_id, NULL,
                    &mapping_type_id, NULL, &mapping_rep_id, &mapping_offset);
            num_lps = codes_mapping_get_lp_count(grp_name, 1, LP_CONFIG_NM,
                    NULL, 1);
            codes_mapping_get_lp_id(grp_name, LP_CONFIG_NM, NULL, 1,
                    s->parent_node_id/num_lps, (s->parent_node_id % num_lps),
                    &parent_nic_id);

           /* send a message to the parent that the LP has entered the collective operation */
            xfer_to_nic_time = g_tw_lookahead + LEVEL_DELAY;
            //e_new = codes_event_new(parent_nic_id, xfer_to_nic_time, lp);
	    void* m_data;
	    e_new = model_net_method_event_new(parent_nic_id, xfer_to_nic_time,
            	lp, TORUS, (void**)&msg_new, (void**)&m_data);
	    	
            memcpy(msg_new, msg, sizeof(nodes_message));
	    if (msg->remote_event_size_bytes){
        	memcpy(m_data, model_net_method_get_edata(TORUS, msg),
                	msg->remote_event_size_bytes);
      	    }
	    
            msg_new->type = T_COLLECTIVE_FAN_IN;
            msg_new->sender_node = s->node_id;

            tw_event_send(e_new);
        }
        return;
}

static void node_collective_fan_in(nodes_state * s,
                        tw_bf * bf,
                        nodes_message * msg,
                        tw_lp * lp)
{
        int i;
        s->num_fan_nodes++;

        // TODO: be annotation-aware
        codes_mapping_get_lp_info(lp->gid, grp_name, &mapping_grp_id, NULL,
                &mapping_type_id, NULL, &mapping_rep_id, &mapping_offset);
        int num_lps = codes_mapping_get_lp_count(grp_name, 1, LP_CONFIG_NM,
                NULL, 1);

        tw_event* e_new;
        nodes_message * msg_new;
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
            codes_mapping_get_lp_id(grp_name, LP_CONFIG_NM, NULL, 1,
                    s->parent_node_id/num_lps, (s->parent_node_id % num_lps),
                    &parent_nic_id);

           /* send a message to the parent that the LP has entered the collective operation */
            //e_new = codes_event_new(parent_nic_id, xfer_to_nic_time, lp);
            //msg_new = tw_event_data(e_new);
	    void * m_data;
      	    e_new = model_net_method_event_new(parent_nic_id,
              xfer_to_nic_time,
              lp, TORUS, (void**)&msg_new, &m_data);
	    
            memcpy(msg_new, msg, sizeof(nodes_message));
            msg_new->type = T_COLLECTIVE_FAN_IN;
            msg_new->sender_node = s->node_id;

            if (msg->remote_event_size_bytes){
	        memcpy(m_data, model_net_method_get_edata(TORUS, msg),
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
                codes_mapping_get_lp_id(grp_name, LP_CONFIG_NM, NULL, 1,
                        s->children[i]/num_lps, (s->children[i] % num_lps),
                        &child_nic_id);
                //e_new = codes_event_new(child_nic_id, xfer_to_nic_time, lp);

                //msg_new = tw_event_data(e_new);
                void * m_data;
	        e_new = model_net_method_event_new(child_nic_id,
                xfer_to_nic_time,
		lp, TORUS, (void**)&msg_new, &m_data);

		memcpy(msg_new, msg, sizeof(nodes_message));
	        if (msg->remote_event_size_bytes){
	                memcpy(m_data, model_net_method_get_edata(TORUS, msg),
        	               msg->remote_event_size_bytes);
      		}
		
                msg_new->type = T_COLLECTIVE_FAN_OUT;
                msg_new->sender_node = s->node_id;

                tw_event_send(e_new);
           }
      }
}
	     
static void node_collective_fan_out(nodes_state * s,
                        tw_bf * bf,
                        nodes_message * msg,
                        tw_lp * lp)
{
        int i;
        //TODO: be annotation-aware
        int num_lps = codes_mapping_get_lp_count(grp_name, 1, LP_CONFIG_NM,
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
                xfer_to_nic_time = g_tw_lookahead + TORUS_FAN_OUT_DELAY + tw_rand_exponential(lp->rng, (double)TORUS_FAN_OUT_DELAY/10);

                if(s->children[i] > 0)
                {
                        tw_lpid child_nic_id;

                        /* get global LP ID of the child node */
                        codes_mapping_get_lp_id(grp_name, LP_CONFIG_NM, NULL, 1,
                                s->children[i]/num_lps,
                                (s->children[i] % num_lps), &child_nic_id);
                        //e_new = codes_event_new(child_nic_id, xfer_to_nic_time, lp);
                        //msg_new = tw_event_data(e_new);
                        //memcpy(msg_new, msg, sizeof(nodes_message) + msg->remote_event_size_bytes);
			void* m_data;
			e_new = model_net_method_event_new(child_nic_id,
							xfer_to_nic_time,
					                lp, TORUS, (void**)&msg_new, &m_data);
		        memcpy(msg_new, msg, sizeof(nodes_message));
		        if (msg->remote_event_size_bytes){
			        memcpy(m_data, model_net_method_get_edata(TORUS, msg),
			                msg->remote_event_size_bytes);
      			}


                        msg_new->type = T_COLLECTIVE_FAN_OUT;
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
    
/*Returns the next neighbor to which the packet should be routed by using DOR (Taken from Ning's code of the torus model)*/
static void dimension_order_routing( nodes_state * s,
			     tw_lpid * dst_lp, 
			     int * dim, 
			     int * dir )
{
     int dest[s->params->n_dims];
     int dest_id;

  /* dummys - check later */
  *dim = -1;
  *dir = -1;

  to_dim_id(codes_mapping_get_lp_relative_id(*dst_lp, 0, 1),
          s->params->n_dims, s->params->dim_length, dest);

  for(int i = 0; i < s->params->n_dims; i++ )
    {
      if ( s->dim_position[ i ] - dest[ i ] > s->params->half_length[ i ] )
	{
	  dest_id = s->neighbour_plus_lpID[ i ];
	  *dim = i;
	  *dir = 1;
	  break;
	}
      if ( s->dim_position[ i ] - dest[ i ] < -s->params->half_length[ i ] )
	{
	  dest_id = s->neighbour_minus_lpID[ i ];
	  *dim = i;
	  *dir = 0;
	  break;
	}
      if ( ( s->dim_position[i] - dest[i] <= s->params->half_length[i] ) &&
              ( s->dim_position[ i ] - dest[ i ] > 0 ) )
	{
	  dest_id = s->neighbour_minus_lpID[ i ];
	  *dim = i;
	  *dir = 0;
	  break;
	}
      if (( s->dim_position[i] - dest[i] >= -s->params->half_length[i] ) &&
              ( s->dim_position[ i ] - dest[ i ] < 0) )
	{
	  dest_id = s->neighbour_plus_lpID[ i ];
	  *dim = i;
	  *dir = 1;
	  break;
	}
    }

  assert(*dim != -1 && *dir != -1);
  *dst_lp = codes_mapping_get_lpid_from_relative(dest_id, NULL, LP_CONFIG_NM,
          s->anno, 1);
}

/*Generates a packet. If there is a buffer slot available, then the packet is 
injected in the network. Else, a buffer overflow exception is thrown.
TODO: We might want to modify this so that if the buffer is full, the packet
injection is delayed in turn slowing down the injection rate. The average achieved
injection rate can be reported at the end of the simulation. */
static void packet_generate( nodes_state * s, 
		tw_bf * bf, 
		nodes_message * msg, 
		tw_lp * lp )
{
//    printf("\n msg local event size %d remote event size %d ", msg->local_event_size_bytes, msg->remote_event_size_bytes);
    int j, tmp_dir=-1, tmp_dim=-1, total_event_size;
    tw_stime ts;

//    event triggered when packet head is sent
    tw_event * e_h;
    nodes_message *m;

    tw_lpid dst_lp;
    // TODO: be annotation-aware
    dst_lp = model_net_find_local_device(TORUS, s->anno, 0,
            msg->final_dest_gid);
            //mapping_offset, &dst_lp);
    // dest_lp gets included to other required msgs through memcpys, so just
    // set here
    msg->dest_lp = dst_lp;

    dimension_order_routing( s, &dst_lp, &tmp_dim, &tmp_dir );

    msg->saved_src_dim = tmp_dim;
    msg->saved_src_dir = tmp_dir;

    //msg->saved_available_time = s->next_flit_generate_time[(2*tmp_dim) + tmp_dir][0];
    msg->travel_start_time = tw_now(lp);
    msg->packet_ID = lp->gid + g_tw_nlp * s->packet_counter;
    msg->my_N_hop = 0;

    uint64_t num_chunks = msg->packet_size/s->params->chunk_size;
    if(msg->packet_size % s->params->chunk_size)
        num_chunks++;


    s->packet_counter++;

    if(msg->packet_ID == TRACE)
	    printf("\n packet generated %lld at lp %d dest %d final dest %d", msg->packet_ID, (int)lp->gid, (int)dst_lp, (int)msg->dest_lp);
    for(j = 0; j < num_chunks; j++)
    { 
     if(s->buffer[ tmp_dir + ( tmp_dim * 2 ) ][ 0 ] < s->params->buffer_size)
      {
       ts = j + tw_rand_exponential(lp->rng, MEAN_INTERVAL/200);
       //s->next_flit_generate_time[(2*tmp_dim) + tmp_dir][0] = max(s->next_flit_generate_time[(2*tmp_dim) + tmp_dir][0], tw_now(lp));
       //s->next_flit_generate_time[(2*tmp_dim) + tmp_dir][0] += ts;
       //e_h = tw_event_new( lp->gid, s->next_flit_generate_time[(2*tmp_dim) + tmp_dir][0] - tw_now(lp), lp);
       //e_h = tw_event_new(lp->gid, ts, lp);
       msg->source_direction = tmp_dir;
       msg->source_dim = tmp_dim;

       void *m_data;
       e_h = model_net_method_event_new(lp->gid, ts, lp, TORUS, (void**)&m,
               (void**)&m_data);

       //m = tw_event_data( e_h );
       //memcpy(m, msg, torus_get_msg_sz() + msg->local_event_size_bytes + msg->remote_event_size_bytes);
       void *m_data_src = model_net_method_get_edata(TORUS, msg);
       memcpy(m, msg, sizeof(nodes_message));
       if (msg->remote_event_size_bytes){
           memcpy(m_data, m_data_src,
                   msg->remote_event_size_bytes);
           m_data = (char*)m_data + msg->remote_event_size_bytes;
           m_data_src = (char*)m_data_src + msg->remote_event_size_bytes;
       }
       if (msg->local_event_size_bytes){
           memcpy(m_data, m_data_src, msg->local_event_size_bytes);
       }
       m->next_stop = dst_lp;
       m->chunk_id = j;

      // find destination dimensions using destination LP ID 
       m->type = SEND;
       m->source_direction = tmp_dir;
       m->source_dim = tmp_dim;
       tw_event_send(e_h);
      }
      else 
       {
   printf("\n %d Packet queued in line increase buffer space, dir %d dim %d buffer space %d dest LP %d ", (int)lp->gid, tmp_dir, tmp_dim, s->buffer[ tmp_dir + ( tmp_dim * 2 ) ][ 0 ], (int)msg->dest_lp);
       MPI_Finalize();
       exit(-1); 
       }
   }

   total_event_size = model_net_get_msg_sz(TORUS) + msg->remote_event_size_bytes + msg->local_event_size_bytes;   
   /* record the statistics of the generated packets */
   mn_stats* stat;
   stat = model_net_find_stats(msg->category, s->torus_stats_array);
   stat->send_count++;  
   stat->send_bytes += msg->packet_size;
   stat->send_time += (1/s->params->link_bandwidth) * msg->packet_size;
   /* record the maximum ROSS event size */
   if(stat->max_event_size < total_event_size)
	   stat->max_event_size = total_event_size;
}
/*Sends a 8-byte credit back to the torus node LP that sent the message */
static void credit_send( nodes_state * s, 
	    tw_bf * bf, 
	    tw_lp * lp, 
	    nodes_message * msg)
{
#if DEBUG
    //printf("\n (%lf) sending credit tmp_dir %d tmp_dim %d %lf ", tw_now(lp), msg->source_direction, msg->source_dim, s->params->credit_delay );
#endif
    bf->c1 = 0;
    tw_event * buf_e;
    nodes_message *m;
    tw_stime ts;
    int src_dir = msg->source_direction;
    int src_dim = msg->source_dim;

    msg->saved_available_time = s->next_credit_available_time[(2 * src_dim) + src_dir][0];
    s->next_credit_available_time[(2 * src_dim) + src_dir][0] = maxd(s->next_credit_available_time[(2 * src_dim) + src_dir][0], tw_now(lp));
    ts =  s->params->credit_delay + 
        tw_rand_exponential(lp->rng, s->params->credit_delay/1000);
    s->next_credit_available_time[(2 * src_dim) + src_dir][0] += ts;

    //buf_e = tw_event_new( msg->sender_lp, s->next_credit_available_time[(2 * src_dim) + src_dir][0] - tw_now(lp), lp);
    //m = tw_event_data(buf_e);
    buf_e = model_net_method_event_new(msg->sender_node,
            s->next_credit_available_time[(2*src_dim) + src_dir][0] - tw_now(lp),
            lp, TORUS, (void**)&m, NULL);
    m->source_direction = msg->source_direction;
    m->source_dim = msg->source_dim;

    m->type = CREDIT;
    tw_event_send( buf_e );
}
/* send a packet from one torus node to another torus node
 A packet can be up to 256 bytes on BG/L and BG/P and up to 512 bytes on BG/Q */
static void packet_send( nodes_state * s, 
	         tw_bf * bf, 
		 nodes_message * msg, 
		 tw_lp * lp )
{ 
    bf->c2 = 0;
    bf->c1 = 0;
    int tmp_dir, tmp_dim;
    tw_stime ts;
    tw_event *e;
    nodes_message *m;
    tw_lpid dst_lp = msg->dest_lp;
    dimension_order_routing( s, &dst_lp, &tmp_dim, &tmp_dir );     

    if(s->buffer[ tmp_dir + ( tmp_dim * 2 ) ][ 0 ] < s->params->buffer_size)
    {
       bf->c2 = 1;
       msg->saved_src_dir = tmp_dir;
       msg->saved_src_dim = tmp_dim;
       ts = tw_rand_exponential( lp->rng, s->params->head_delay/200.0 ) + 
           s->params->head_delay;

//    For reverse computation 
      msg->saved_available_time = s->next_link_available_time[tmp_dir + ( tmp_dim * 2 )][0];

      s->next_link_available_time[tmp_dir + ( tmp_dim * 2 )][0] = maxd( s->next_link_available_time[ tmp_dir + ( tmp_dim * 2 )][0], tw_now(lp) );
      s->next_link_available_time[tmp_dir + ( tmp_dim * 2 )][0] += ts;
    
      //e = tw_event_new( dst_lp, s->next_link_available_time[tmp_dir + ( tmp_dim * 2 )][0] - tw_now(lp), lp );
      //m = tw_event_data( e );
      //memcpy(m, msg, torus_get_msg_sz() + msg->remote_event_size_bytes);
      void * m_data;
      e = model_net_method_event_new(dst_lp, 
              s->next_link_available_time[tmp_dir+(tmp_dim*2)][0] - tw_now(lp),
              lp, TORUS, (void**)&m, &m_data);
      memcpy(m, msg, sizeof(nodes_message));
      if (msg->remote_event_size_bytes){
        memcpy(m_data, model_net_method_get_edata(TORUS, msg),
                msg->remote_event_size_bytes);
      }
      m->type = ARRIVAL;

      if(msg->packet_ID == TRACE)
        printf("\n lp %d packet %lld flit id %d being sent to %d after time %lf ", (int) lp->gid, msg->packet_ID, msg->chunk_id, (int)dst_lp, s->next_link_available_time[tmp_dir + ( tmp_dim * 2 )][0] - tw_now(lp)); 
      //Carry on the message info
      m->source_dim = tmp_dim;
      m->source_direction = tmp_dir;
      m->next_stop = dst_lp;
      m->sender_node = lp->gid;
      m->local_event_size_bytes = 0; /* We just deliver the local event here */

      tw_event_send( e );

      s->buffer[ tmp_dir + ( tmp_dim * 2 ) ][ 0 ]++;
    
      uint64_t num_chunks = msg->packet_size/s->params->chunk_size;

      if(msg->packet_size % s->params->chunk_size)
          num_chunks++;

      if(msg->chunk_id == num_chunks - 1)
      {
        bf->c1 = 1;
	/* Invoke an event on the sending server */
	if(msg->local_event_size_bytes > 0)
	{
          tw_event* e_new;
	  nodes_message* m_new;
	  void* local_event;
	  ts = (1/s->params->link_bandwidth) * msg->local_event_size_bytes;
	  e_new = tw_event_new(msg->sender_svr, ts, lp);
	  m_new = tw_event_data(e_new);
	  //local_event = (char*)msg;
	  //local_event += torus_get_msg_sz() + msg->remote_event_size_bytes;
          local_event = (char*)model_net_method_get_edata(TORUS, msg) +
              msg->remote_event_size_bytes;
	  memcpy(m_new, local_event, msg->local_event_size_bytes);
	  tw_event_send(e_new);
	}
     }
  } // end if
    else
    {
	    printf("\n buffer overflown ");
	    MPI_Finalize();
	    exit(-1);
    }
}

/*Processes the packet after it arrives from the neighboring torus node 
 * routes it to the next compute node if this is not the destination
 * OR if this is the destination then a remote event at the server is issued. */
static void packet_arrive( nodes_state * s, 
		    tw_bf * bf, 
		    nodes_message * msg, 
		    tw_lp * lp )
{
  bf->c2 = 0;
  tw_event *e;
  tw_stime ts;
  nodes_message *m;
  mn_stats* stat;

  credit_send( s, bf, lp, msg); 
  
  msg->my_N_hop++;
  ts = 0.1 + tw_rand_exponential(lp->rng, MEAN_INTERVAL/200);
  if(msg->packet_ID == TRACE)
	  printf("\n packet arrived at lp %d final dest %d ", (int)lp->gid, (int)msg->dest_lp);
  if( lp->gid == msg->dest_lp )
    {   
        uint64_t num_chunks = msg->packet_size/s->params->chunk_size;
        if(msg->packet_size % s->params->chunk_size)
            num_chunks++;

        if( msg->chunk_id == num_chunks - 1 )    
        {
	    bf->c2 = 1;
	    stat = model_net_find_stats(msg->category, s->torus_stats_array);
	    stat->recv_count++;
	    stat->recv_bytes += msg->packet_size;
	    stat->recv_time += tw_now( lp ) - msg->travel_start_time;

	    /*count the number of packets completed overall*/
	    N_finished_packets++;
	    total_time += tw_now( lp ) - msg->travel_start_time;
	    total_hops += msg->my_N_hop;

	    if (max_latency < tw_now( lp ) - msg->travel_start_time) {
		  bf->c3 = 1;
		  msg->saved_available_time = max_latency;
	          max_latency=tw_now( lp ) - msg->travel_start_time;
     		}
	    // Trigger an event on receiving server
	    if(msg->remote_event_size_bytes)
	    {
               void *tmp_ptr = model_net_method_get_edata(TORUS, msg);
               if (msg->is_pull){
                   int net_id = model_net_get_id(LP_METHOD_NM);
                   model_net_event(net_id, msg->category, msg->sender_svr,
                           msg->pull_size, 0.0, msg->remote_event_size_bytes,
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
    }
  else
    {
      //e = tw_event_new(lp->gid, ts , lp);
      //m = tw_event_data( e );
      //memcpy(m, msg, torus_get_msg_sz() + msg->remote_event_size_bytes);
      void *m_data;
      e = model_net_method_event_new(lp->gid, ts, lp, TORUS, (void**)&m,
              &m_data);
      memcpy(m, msg, sizeof(nodes_message));
      if (msg->remote_event_size_bytes){
        memcpy(m_data, model_net_method_get_edata(TORUS, msg),
                msg->remote_event_size_bytes);
      }
      m->type = SEND;
      m->next_stop = -1;
      tw_event_send(e);
   }
}

/* reports torus statistics like average packet latency, maximum packet latency and average
 * number of torus hops traversed by the packet */
static void torus_report_stats()
{
    long long avg_hops, total_finished_packets;
    tw_stime avg_time, max_time;

    MPI_Reduce( &total_hops, &avg_hops, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce( &N_finished_packets, &total_finished_packets, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce( &total_time, &avg_time, 1,MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce( &max_latency, &max_time, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);

    if(!g_tw_mynode)
     {
       printf(" Average number of hops traversed %f average message latency %lf us maximum message latency %lf us \n", (float)avg_hops/total_finished_packets, avg_time/(total_finished_packets*1000), max_time/1000);
     }
}
/* finalize the torus node and free all event buffers available */
void
final( nodes_state * s, tw_lp * lp )
{
  model_net_print_stats(lp->gid, &s->torus_stats_array[0]); 
  free(s->next_link_available_time);
  free(s->next_credit_available_time);
  free(s->next_flit_generate_time);
  // since all LPs are sharing params, just let them leak for now
  // TODO: add a post-sim "cleanup" function?
  //free(s->buffer); 
  //free(s->params->dim_length);
  //free(s->params->factor);
  //free(s->params->half_length);
}

/* increments the buffer count after a credit arrives from the remote compute node */
static void packet_buffer_process( nodes_state * s, tw_bf * bf, nodes_message * msg, tw_lp * lp )
{
   s->buffer[ msg->source_direction + ( msg->source_dim * 2 ) ][  0 ]--;
}

/* reverse handler for torus node */
static void node_rc_handler(nodes_state * s, tw_bf * bf, nodes_message * msg, tw_lp * lp)
{
  switch(msg->type)
    {
       case GENERATE:
		   {
		     s->packet_counter--;
		     int i;//, saved_dim, saved_dir;
	 	     //saved_dim = msg->saved_src_dim;
		     //saved_dir = msg->saved_src_dir;

                     uint64_t num_chunks = msg->packet_size/s->params->chunk_size;
                     if(msg->packet_size % s->params->chunk_size)
                         num_chunks++;

		     //s->next_flit_generate_time[(saved_dim * 2) + saved_dir][0] = msg->saved_available_time;
		     for(i=0; i < num_chunks; i++)
  		        tw_rand_reverse_unif(lp->rng);
	     	     mn_stats* stat;
		     stat = model_net_find_stats(msg->category, s->torus_stats_array);
		     stat->send_count--; 
		     stat->send_bytes -= msg->packet_size;
		     stat->send_time -= (1/s->params->link_bandwidth) * msg->packet_size;
		   }
	break;
	
	case ARRIVAL:
		   {
  		    tw_rand_reverse_unif(lp->rng);
		    tw_rand_reverse_unif(lp->rng);
		    int next_dim = msg->source_dim;
		    int next_dir = msg->source_direction;
                    uint64_t num_chunks = msg->packet_size/s->params->chunk_size;
                    if(msg->packet_size % s->params->chunk_size)
                        num_chunks++;

		    s->next_credit_available_time[next_dir + ( next_dim * 2 )][0] = msg->saved_available_time;
		    if(bf->c2)
		    {
		       struct mn_stats* stat;
		       stat = model_net_find_stats(msg->category, s->torus_stats_array);
		       stat->recv_count--;
		       stat->recv_bytes -= msg->packet_size;
		        stat->recv_time -= tw_now(lp) - msg->travel_start_time;	    
		       N_finished_packets--;
		       total_time -= tw_now( lp ) - msg->travel_start_time;
		       total_hops -= msg->my_N_hop;
		    }
 		    msg->my_N_hop--;
                    if (lp->gid == msg->dest_lp && 
                            msg->chunk_id == num_chunks-1 &&
                            msg->remote_event_size_bytes && msg->is_pull){
                        int net_id = model_net_get_id(LP_METHOD_NM);
                        model_net_event_rc(net_id, lp, msg->pull_size);
                    }
		   }
	break;	

	case SEND:
		 {
		    if(bf->c2)
		     {
                        int next_dim = msg->saved_src_dim;
			int next_dir = msg->saved_src_dir;
			s->next_link_available_time[next_dir + ( next_dim * 2 )][0] = msg->saved_available_time;
			s->buffer[ next_dir + ( next_dim * 2 ) ][ 0 ] --;
	                tw_rand_reverse_unif(lp->rng);
		    }
		 }
	break;

       case CREDIT:
		{
		  s->buffer[ msg->source_direction + ( msg->source_dim * 2 ) ][  0 ]++;
              }
       break;
	
       case T_COLLECTIVE_INIT:
                {
                    s->collective_init_time = msg->saved_collective_init_time;
                }
        break;

        case T_COLLECTIVE_FAN_IN:
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

        case T_COLLECTIVE_FAN_OUT:
                {
                 int i;
                 if(bf->c1)
                    {
                        for( i = 0; i < s->num_children; i++ )
                            tw_rand_reverse_unif(lp->rng);
                    }
                }
        break;      
     }
}

/* forward event handler for torus node event */
static void event_handler(nodes_state * s, tw_bf * bf, nodes_message * msg, tw_lp * lp)
{
 *(int *) bf = (int) 0;
 switch(msg->type)
 {
  case GENERATE:
    packet_generate(s,bf,msg,lp);
  break;

  case ARRIVAL:
    packet_arrive(s,bf,msg,lp);
  break;

  case SEND:
   packet_send(s,bf,msg,lp);
  break;

  case CREDIT:
    packet_buffer_process(s,bf,msg,lp);
   break;

  case T_COLLECTIVE_INIT:
    node_collective_init(s, bf, msg, lp);
  break;

  case T_COLLECTIVE_FAN_IN:
    node_collective_fan_in(s, bf, msg, lp);
  break;

  case T_COLLECTIVE_FAN_OUT:
    node_collective_fan_out(s, bf, msg, lp);
  break;
 
  default:
	printf("\n Being sent to wrong LP %d", msg->type);
  break;
 }
}
/* event types */
tw_lptype torus_lp =
{
    (init_f) torus_init,
    (pre_run_f) NULL,
    (event_f) event_handler,
    (revent_f) node_rc_handler,
    (final_f) final,
    (map_f) codes_mapping,
    sizeof(nodes_state),
};

/* returns the torus lp type for lp registration */
static const tw_lptype* torus_get_lp_type(void)
{
   return(&torus_lp); 
}

static tw_lpid torus_find_local_device(
        const char * annotation,
        int          ignore_annotations,
        tw_lp *sender)
{
     tw_lpid dest_id;

     codes_mapping_get_lp_info(sender->gid, grp_name, &mapping_grp_id, NULL,
             &mapping_type_id, NULL, &mapping_rep_id, &mapping_offset);
     codes_mapping_get_lp_id(grp_name, LP_CONFIG_NM, annotation,
             ignore_annotations, mapping_rep_id, mapping_offset, &dest_id);

    return(dest_id);
}

/* data structure for torus statistics */
struct model_net_method torus_method =
{
   .mn_configure = torus_configure,
   .mn_register = NULL,
   .model_net_method_packet_event = torus_packet_event,
   .model_net_method_packet_event_rc = torus_packet_event_rc,
   .model_net_method_recv_msg_event = NULL,
   .model_net_method_recv_msg_event_rc = NULL,
   .mn_get_lp_type = torus_get_lp_type,
   .mn_get_msg_sz = torus_get_msg_sz,
   .mn_report_stats = torus_report_stats,
   .model_net_method_find_local_device = NULL,
   .mn_collective_call = torus_collective,
   .mn_collective_call_rc = torus_collective_rc
};

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
