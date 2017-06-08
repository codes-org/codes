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
#include "codes/rc-stack.h"

#ifdef ENABLE_CORTEX
#include <cortex/cortex.h>
#include <cortex/topology.h>
#endif

#define DEBUG 1
#define MEAN_INTERVAL 100
// type casts to make compiler happy
#define TRACE ((unsigned long long)(-1))
#define TRACK ((tw_lpid)(-1))

#define STATICQ 0
/* collective specific parameters */
#define TREE_DEGREE 4
#define LEVEL_DELAY 1000
#define TORUS_COLLECTIVE_DEBUG 0
#define NUM_COLLECTIVES  1
#define COLLECTIVE_COMPUTATION_DELAY 5700
#define TORUS_FAN_OUT_DELAY 20.0

#define LP_CONFIG_NM (model_net_lp_config_names[TORUS])
#define LP_METHOD_NM (model_net_method_names[TORUS])

#ifdef ENABLE_CORTEX
/* This structure is defined at the end of the file */
extern cortex_topology torus_cortex_topology;
#endif

static double maxd(double a, double b) { return a < b ? b : a; }

/* Torus network model implementation of codes, implements the modelnet API */
typedef struct nodes_message_list nodes_message_list;
struct nodes_message_list {
    nodes_message msg;
    char* event_data;
    nodes_message_list *next;
    nodes_message_list *prev;
};

void init_nodes_message_list(nodes_message_list *this, nodes_message *inmsg) {
    this->msg = *inmsg;
    this->event_data = NULL;
    this->next = NULL;
    this->prev = NULL;
}

void delete_nodes_message_list(nodes_message_list *this) {
    if(this->event_data != NULL) free(this->event_data);
    free(this);
}

static void free_tmp(void * ptr)
{
    nodes_message_list * entry = ptr;
    if(entry->event_data != NULL)
        free(entry->event_data);

    free(entry);
}
typedef struct torus_param torus_param;
struct torus_param
{
    int n_dims; /*Dimension of the torus network, 5-D, 7-D or any other*/
    int* dim_length; /*Length of each torus dimension*/
    double link_bandwidth;/* bandwidth for each torus link */
    double cn_bandwidth; /* injection bandwidth */
    int buffer_size; /* number of buffer slots for each vc in flits*/
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
    double cn_delay;

    double router_delay;
    int routing;
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
static int                       num_params = 0;
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
  /* Head and tail of the terminal messages list */
  nodes_message_list **terminal_msgs;
  nodes_message_list **terminal_msgs_tail;
  int all_term_length;
  int *terminal_length, *queued_length;
  /* pending packets to be sent out */
  nodes_message_list ***pending_msgs;
  nodes_message_list ***pending_msgs_tail;
  nodes_message_list ***queued_msgs;
  nodes_message_list ***queued_msgs_tail;
  nodes_message_list **other_msgs;
  nodes_message_list **other_msgs_tail;
  int *in_send_loop;
  /* traffic through each torus link */
  int64_t *link_traffic;
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

   /* create the RC stack */
   struct rc_stack * st;

   /* finished chunks */
   long finished_chunks;

   /* finished packets */
   long finished_packets;

   /* total hops */
   double total_hops;

   /* total time */
   double total_time;

   /* total data */
   long total_data_sz;
   /* output buf */
   char output_buf[2048];
   char output_busy_buf[2048];

   /* busy time */
   tw_stime * busy_time;
   tw_stime * last_buf_full;
};

static void append_to_node_message_list(
        nodes_message_list ** thisq,
        nodes_message_list ** thistail,
        int index,
        nodes_message_list *msg) {
    if(thisq[index] == NULL) {
        thisq[index] = msg;
    } else {
        thistail[index]->next = msg;
        msg->prev = thistail[index];
    }
    thistail[index] = msg;
}
static void prepend_to_node_message_list(
        nodes_message_list ** thisq,
        nodes_message_list ** thistail,
        int index,
        nodes_message_list *msg) {
    if(thisq[index] == NULL) {
        thistail[index] = msg;
    } else {
        thisq[index]->prev = msg;
        msg->next = thisq[index];
    }
    thisq[index] = msg;
}

static nodes_message_list* return_head(
        nodes_message_list ** thisq,
        nodes_message_list ** thistail,
        int index) {
    nodes_message_list *head = thisq[index];
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

static nodes_message_list* return_tail(
        nodes_message_list ** thisq,
        nodes_message_list ** thistail,
        int index) {
    nodes_message_list *tail = thistail[index];
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

static void torus_read_config(
        const char         * anno,
        torus_param        * params){
    char dim_length_str[MAX_NAME_LENGTH];
    int i;

    // shorthand
    torus_param *p = params;

    int rc = configuration_get_value_int(&config, "PARAMS", "n_dims", anno, &p->n_dims);
    if(rc) {
        p->n_dims = 4; /* a 4-D torus */
        fprintf(stderr,
                "Warning: Number of dimensions not specified, setting to %d\n",
                p->n_dims);
    }

   rc = configuration_get_value_double(&config, "PARAMS", "link_bandwidth", anno,
            &p->link_bandwidth);
    if(rc) {
        p->link_bandwidth = 2.0; /*default bg/q configuration */
        fprintf(stderr, "Link bandwidth not specified, setting to %lf\n",
                p->link_bandwidth);
    }

    rc = configuration_get_value_int(&config, "PARAMS", "buffer_size", anno, &p->buffer_size);
    if(rc) {
        p->buffer_size = 2048;
        fprintf(stderr, "Buffer size not specified, setting to %d",
                p->buffer_size);
    }


    rc = configuration_get_value_int(&config, "PARAMS", "chunk_size", anno, &p->chunk_size);
    if(rc) {
        p->chunk_size = 128;
        fprintf(stderr, "Warning: Chunk size not specified, setting to %d\n",
                p->chunk_size);
    }
        /* by default, we have one for taking packets,
         * another for taking credit*/
        p->num_vc = 1;

    rc = configuration_get_value(&config, "PARAMS", "dim_length", anno,
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
    // create derived parameters

    // factor is an exclusive prefix product
    p->router_delay = 50;
    p->cn_delay = 10;
    p->factor = malloc(p->n_dims * sizeof(int));
    p->factor[0] = 1;
    for(i = 1; i < p->n_dims; i++)
        p->factor[i] = p->factor[i-1] * p->dim_length[i-1];

    p->half_length = malloc(p->n_dims * sizeof(int));
    for (i = 0; i < p->n_dims; i++)
        p->half_length[i] = p->dim_length[i] / 2;

    // some latency numbers
    p->head_delay = bytes_to_ns(p->chunk_size, p->link_bandwidth);
    p->credit_delay = bytes_to_ns(8, p->link_bandwidth);
}

static void torus_configure(){
    anno_map = codes_mapping_get_lp_anno_map(LP_CONFIG_NM);
    assert(anno_map);
    num_params = anno_map->num_annos + (anno_map->has_unanno_lp > 0);
    all_params = malloc(num_params * sizeof(*all_params));

    for (int i = 0; i < anno_map->num_annos; i++){
        const char * anno = anno_map->annotations[i].ptr;
        torus_read_config(anno, &all_params[i]);
    }
    if (anno_map->has_unanno_lp > 0){
        torus_read_config(NULL, &all_params[anno_map->num_annos]);
    }
#ifdef ENABLE_CORTEX
	model_net_topology = torus_cortex_topology;
#endif
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
    tw_lpid num_lps = codes_mapping_get_lp_count(grp_name, 0, LP_CONFIG_NM, s->anno, 0);

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
        if(next_child < num_lps)
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
static tw_stime torus_packet_event(
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
    (void)message_offset; // not using atm...
    (void)sched_params; // not using atm...
    tw_event * e_new;
    tw_stime xfer_to_nic_time;
    nodes_message * msg;
    char* tmp_ptr;

    xfer_to_nic_time = codes_local_latency(sender); /* Throws an error of found last KP time > current event time otherwise */
    //e_new = tw_event_new(local_nic_id, xfer_to_nic_time+offset, sender);
    //msg = tw_event_data(e_new);
    e_new = model_net_method_event_new(sender->gid, xfer_to_nic_time+offset,
            sender, TORUS, (void**)&msg, (void**)&tmp_ptr);
    strcpy(msg->category, req->category);
    msg->final_dest_gid = req->final_dest_lp;
    msg->dest_lp = req->dest_mn_lp;
    msg->sender_svr= req->src_lp;
    msg->sender_node = sender->gid;
    msg->packet_size = packet_size;
    msg->travel_start_time = tw_now(sender);
    msg->remote_event_size_bytes = 0;
    msg->local_event_size_bytes = 0;
    msg->chunk_id = 0;
    msg->type = GENERATE;
    msg->is_pull = req->is_pull;
    msg->pull_size = req->pull_size;

    if(is_last_pckt) /* Its the last packet so pass in remote event information*/
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
        // printf("\n torus remote event %d local event %d last packet %d %lf ", msg->remote_event_size_bytes, msg->local_event_size_bytes, is_last_pckt, xfer_to_nic_time);
    }
    tw_event_send(e_new);
    return xfer_to_nic_time;
}

/*Sends a 8-byte credit back to the torus node LP that sent the message */
static void credit_send( nodes_state * s,
	    tw_lp * lp,
	    nodes_message * msg,
            int sq)
{
    tw_event * e;
    nodes_message *m;
    tw_stime ts;

    ts = (1.1 * g_tw_lookahead) + s->params->credit_delay + tw_rand_unif(lp->rng);
    e = model_net_method_event_new(msg->sender_node, ts, lp, TORUS,
        (void**)&m, NULL);
    if(sq == -1) {
        m->source_direction = msg->source_direction;
        m->source_dim = msg->source_dim;
    } else {
        m->source_direction = msg->saved_queue % 2;
        m->source_dim = msg->saved_queue / 2;
    }
    m->type = CREDIT;
    tw_event_send(e);
}
/*Initialize the torus model, this initialization part is borrowed from Ning's torus model */
static void torus_init( nodes_state * s,
	   tw_lp * lp )
{
    int i, j;
    char anno[MAX_NAME_LENGTH];

    rc_stack_create(&s->st);

    codes_mapping_get_lp_info(lp->gid, grp_name, &mapping_grp_id, NULL, &mapping_type_id, anno, &mapping_rep_id, &mapping_offset);

    s->node_id = codes_mapping_get_lp_relative_id(lp->gid, 0, 0);

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

    s->finished_chunks = 0;
    s->finished_packets = 0;

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
    s->terminal_length = (int*)malloc(2*p->n_dims*sizeof(int));
    s->queued_length = (int*)malloc(2*p->n_dims*sizeof(int));
    s->terminal_msgs =
        (nodes_message_list**)malloc(2*p->n_dims*sizeof(nodes_message_list*));
    s->terminal_msgs_tail =
        (nodes_message_list**)malloc(2*p->n_dims*sizeof(nodes_message_list*));
    s->pending_msgs =
        (nodes_message_list***)malloc(2*p->n_dims*sizeof(nodes_message_list**));
    s->pending_msgs_tail =
        (nodes_message_list***)malloc(2*p->n_dims*sizeof(nodes_message_list**));
    s->queued_msgs =
        (nodes_message_list***)malloc(2*p->n_dims*sizeof(nodes_message_list**));
    s->queued_msgs_tail =
        (nodes_message_list***)malloc(2*p->n_dims*sizeof(nodes_message_list**));

    s->busy_time =
        (tw_stime*)malloc(2*p->n_dims*sizeof(tw_stime));
    s->last_buf_full =
        (tw_stime*)malloc(2*p->n_dims*sizeof(tw_stime));

    for(i = 0; i < 2*p->n_dims; i++) {
        s->pending_msgs[i] =
            (nodes_message_list**)malloc(p->num_vc*sizeof(nodes_message_list*));
        s->pending_msgs_tail[i] =
            (nodes_message_list**)malloc(p->num_vc*sizeof(nodes_message_list*));
        s->queued_msgs[i] =
            (nodes_message_list**)malloc(p->num_vc*sizeof(nodes_message_list*));
        s->queued_msgs_tail[i] =
            (nodes_message_list**)malloc(p->num_vc*sizeof(nodes_message_list*));

    }
    s->other_msgs =
        (nodes_message_list**)malloc(2*p->n_dims*sizeof(nodes_message_list*));
    s->other_msgs_tail =
        (nodes_message_list**)malloc(2*p->n_dims*sizeof(nodes_message_list*));
    s->in_send_loop =
        (int *)malloc(2*p->n_dims*sizeof(int));

    s->link_traffic = (int64_t *)malloc(2*p->n_dims*sizeof(int64_t));
    s->total_data_sz = 0;

    for(i=0; i < 2*p->n_dims; i++)
    {
	s->buffer[i] = (int*)malloc(p->num_vc * sizeof(int));
	s->next_link_available_time[i] =
            (tw_stime*)malloc(p->num_vc * sizeof(tw_stime));
	s->next_credit_available_time[i] =
            (tw_stime*)malloc(p->num_vc * sizeof(tw_stime));
	s->next_flit_generate_time[i] =
            (tw_stime*)malloc(p->num_vc * sizeof(tw_stime));
        s->terminal_msgs[i] = NULL;
        s->terminal_msgs_tail[i] = NULL;
        s->other_msgs[i] = NULL;
        s->other_msgs_tail[i] = NULL;
        s->in_send_loop[i] = 0;
        s->terminal_length[i] = 0;
        s->queued_length[i] = 0;
        s->all_term_length = 0;
        s->busy_time[i] = 0;
        s->last_buf_full[i] = 0;
    }

    // calculate my torus coords
    to_dim_id(codes_mapping_get_lp_relative_id(lp->gid, 0, 1),
            s->params->n_dims, s->params->dim_length, s->dim_position);
    /* DEBUG */
    /*printf("%lu: my coords:", lp->gid);
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
       s->pending_msgs[j][i] = NULL;
       s->pending_msgs_tail[j][i] = NULL;
       s->queued_msgs[j][i] = NULL;
       s->queued_msgs_tail[j][i] = NULL;
     }
   }
  // record LP time
    s->packet_counter = 0;
    torus_collective_init(s, lp);
}


/* collective operation for the torus network */
void torus_collective(char const * category, int message_size, int remote_event_size, const void* remote_event, tw_lp* sender)
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
    (void)message_size; // unneeded
     codes_local_latency_reverse(sender);
     return;
}

static void send_remote_event(nodes_state * s,
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
            e = tw_event_new(s->origin_svr, ts, lp);
            m = tw_event_data(e);
            char* tmp_ptr = (char*)msg;
            tmp_ptr += torus_get_msg_sz();
            memcpy(m, tmp_ptr, msg->remote_event_size_bytes);
            tw_event_send(e);
     }
}

static void node_collective_init(nodes_state * s,
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
           send_remote_event(s, msg, lp);

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

        send_remote_event(s, msg, lp);

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
static void packet_generate( nodes_state * ns,
        tw_bf * bf,
        nodes_message * msg,
        tw_lp * lp)
{
    int tmp_dir=-1, tmp_dim=-1, queue, total_event_size;
    tw_stime ts;
    tw_event * e;
    nodes_message *m;

    tw_lpid intm_dst = msg->dest_lp;
    dimension_order_routing(ns, &intm_dst, &tmp_dim, &tmp_dir);
    queue = tmp_dir + ( tmp_dim * 2 );

    msg->packet_ID = ns->packet_counter;
    msg->my_N_hop = 0;

    if(lp->gid == TRACK && msg->packet_ID == TRACE)
        tw_output(lp, "\n packet generated %lld at lp %d dest %d final dest %d",
        msg->packet_ID, (int)lp->gid, (int)intm_dst, (int)msg->dest_lp);

    uint64_t num_chunks = msg->packet_size/ns->params->chunk_size;
    if(msg->packet_size % ns->params->chunk_size)
        num_chunks++;
    if(!num_chunks)
        num_chunks = 1;

    ns->packet_counter++;

    msg->source_direction = tmp_dir;
    msg->source_dim = tmp_dim;
    msg->next_stop = intm_dst;
    msg->saved_queue = -1;

    for(uint64_t j = 0; j < num_chunks; j++) {
        nodes_message_list * cur_chunk = (nodes_message_list *)malloc(
                sizeof(nodes_message_list));

        init_nodes_message_list(cur_chunk, msg);

        if(msg->remote_event_size_bytes + msg->local_event_size_bytes > 0) {
            cur_chunk->event_data = (char*)malloc(
                msg->remote_event_size_bytes + msg->local_event_size_bytes);
        }

        void *m_data_src = model_net_method_get_edata(TORUS, msg);
        if (msg->remote_event_size_bytes){
            memcpy(cur_chunk->event_data, m_data_src, msg->remote_event_size_bytes);
        }
        if (msg->local_event_size_bytes){
            m_data_src = (char*)m_data_src + msg->remote_event_size_bytes;
            memcpy((char*)cur_chunk->event_data + msg->remote_event_size_bytes,
                m_data_src, msg->local_event_size_bytes);
        }
        cur_chunk->msg.chunk_id = j;

        append_to_node_message_list(ns->terminal_msgs,
            ns->terminal_msgs_tail, queue, cur_chunk);
        ns->terminal_length[queue] += ns->params->chunk_size;
        ns->all_term_length += ns->params->chunk_size;
    }

   if(ns->in_send_loop[queue] == 0) {
       bf->c8 = 1;
       ts = codes_local_latency(lp) + ns->params->cn_delay * msg->packet_size;
       e = model_net_method_event_new(lp->gid, ts, lp, TORUS, (void**)&m, NULL);
       m->type = SEND;
       m->source_direction = tmp_dir;
       m->source_dim = tmp_dim;
       ns->in_send_loop[queue] = 1;
       tw_event_send(e);
   }

   /* record the statistics of the generated packets */
   total_event_size = model_net_get_msg_sz(TORUS) + msg->remote_event_size_bytes + msg->local_event_size_bytes;
   mn_stats* stat;
   stat = model_net_find_stats(msg->category, ns->torus_stats_array);
   stat->send_count++;
   stat->send_bytes += msg->packet_size;
   stat->send_time += (1/ns->params->link_bandwidth) * msg->packet_size;
   /* record the maximum ROSS event size */
   if(stat->max_event_size < total_event_size)
	   stat->max_event_size = total_event_size;
}
   /* record the maximum ROSS event size */

static void packet_generate_rc( nodes_state * s,
		tw_bf * bf,
		nodes_message * msg,
		tw_lp * lp )
{
    s->packet_counter--;

    int queue = msg->source_direction + (msg->source_dim * 2);

    uint64_t num_chunks = msg->packet_size/s->params->chunk_size;
    if(msg->packet_size % s->params->chunk_size)
       num_chunks++;

     if(!num_chunks)
        num_chunks = 1;

     for(uint64_t j = 0; j < num_chunks; j++)
     {
       nodes_message_list* cur_entry = return_tail(
              s->terminal_msgs, s->terminal_msgs_tail, queue);
       s->terminal_length[queue] -= s->params->chunk_size;
       s->all_term_length -= s->params->chunk_size;
       delete_nodes_message_list(cur_entry);
     }

     if(bf->c8) {
        codes_local_latency_reverse(lp);
        s->in_send_loop[queue] = 0;
     }
     mn_stats* stat;
     stat = model_net_find_stats(msg->category, s->torus_stats_array);
     stat->send_count--;
     stat->send_bytes -= msg->packet_size;
     stat->send_time -= (1/s->params->link_bandwidth) * msg->packet_size;
}

static void packet_send_rc(nodes_state * s,
        tw_bf * bf,
        nodes_message * msg,
        tw_lp * lp)
{
    int queue = msg->source_direction + (msg->source_dim * 2);

    if(bf->c1 || bf->c4)
    {
        if(bf->c24)
        {
            s->last_buf_full[queue] = msg->saved_busy_time;
        }
        s->in_send_loop[queue] = 1;
        return;
    }
     if(bf->c3) {
         s->buffer[queue][STATICQ] -= s->params->chunk_size;
     }

     codes_local_latency_reverse(lp);
     s->next_link_available_time[queue][0] = msg->saved_available_time;

     nodes_message_list * cur_entry = rc_stack_pop(s->st);
     assert(cur_entry);

     if(bf->c20)
     {
        s->link_traffic[queue] -= cur_entry->msg.packet_size % s->params->chunk_size;
     }

     if(bf->c21)
     {
        s->link_traffic[queue] -= s->params->chunk_size;
     }

     if(bf->c6)
     {
        codes_local_latency_reverse(lp);
     }


     if(bf->c31)
     {
         prepend_to_node_message_list(s->terminal_msgs,
                  s->terminal_msgs_tail, queue, cur_entry);
        s->terminal_length[queue] += s->params->chunk_size;
        s->all_term_length += s->params->chunk_size;
     }

     if(bf->c8)
     {
        prepend_to_node_message_list(s->pending_msgs[queue],
                s->pending_msgs_tail[queue], STATICQ, cur_entry);
     }

     if(bf->c9)
     {
         codes_local_latency_reverse(lp);
     }

     if(bf->c10)
     {
        s->in_send_loop[queue] = 1;
     }
}
/* send a packet from one torus node to another torus node
 A packet can be up to 256 bytes on BG/L and BG/P and up to 512 bytes on BG/Q */
static void packet_send( nodes_state * s,
         tw_bf * bf,
		 nodes_message * msg,
		 tw_lp * lp )
{
    tw_stime ts;
    tw_event *e;
    nodes_message *m;
    int isT = 0;

    int queue = msg->source_direction + (msg->source_dim * 2);

    if(s->pending_msgs[queue][STATICQ] == NULL
        && s->terminal_msgs[queue] == NULL) {
        bf->c1 = 1;
        s->in_send_loop[queue] = 0;
        return;
    }

    nodes_message_list *cur_entry = s->pending_msgs[queue][STATICQ];

    if(cur_entry == NULL) {
        /* Bubble flow control method here, checking if there are 2 empty
         * buffer slots only then forward newly injected packets */
                if((s->buffer[queue][STATICQ] + (2 * s->params->chunk_size) <= s->params->buffer_size)) {
                    bf->c3 = 1;
                    s->buffer[queue][STATICQ] += s->params->chunk_size;
                    cur_entry = s->terminal_msgs[queue];
                    isT = 1;
                }
              if(cur_entry == NULL)
              {
                bf->c4 = 1;
                if(!s->last_buf_full[queue])
                {
                    bf->c24 = 1;
                    msg->saved_busy_time = s->last_buf_full[queue];
                    s->last_buf_full[queue] = tw_now(lp);
                }

                s->in_send_loop[queue] = 0;
                return;
            }
        }

    uint64_t num_chunks = cur_entry->msg.packet_size/s->params->chunk_size;
    if(cur_entry->msg.packet_size % s->params->chunk_size)
        num_chunks++;

    if(!num_chunks)
        num_chunks = 1;

    double bytetime;
/*    if((cur_entry->msg.packet_size % s->params->chunk_size) && (cur_entry->msg.chunk_id == num_chunks - 1))
    {
        bytetime = s->params->head_delay * (cur_entry->msg.packet_size % s->params->chunk_size);
    }
    else
        bytetime = s->params->head_delay * s->params->chunk_size;
*/
    bytetime = s->params->head_delay;

    ts = codes_local_latency(lp) + bytetime + s->params->router_delay;

    //For reverse computation
    msg->saved_available_time = s->next_link_available_time[queue][0];
    s->next_link_available_time[queue][0] = maxd( s->next_link_available_time[queue][0], tw_now(lp) );
    s->next_link_available_time[queue][0] += ts;

    void * m_data;
    ts = s->next_link_available_time[queue][0] - tw_now(lp);
    e = model_net_method_event_new(cur_entry->msg.next_stop, ts,
            lp, TORUS, (void**)&m, &m_data);
    memcpy(m, &cur_entry->msg, sizeof(nodes_message));
    if (m->remote_event_size_bytes){
        memcpy(m_data, cur_entry->event_data, m->remote_event_size_bytes);
    }

//    if(cur_entry->msg.packet_ID == TRACE && lp->gid == TRACK)
//        printf("\n packet sent %lld at lp %d dest %d final dest %d",
//        msg->packet_ID, (int)lp->gid, (int)intm_dst, (int)msg->dest_lp);
    m->type = ARRIVAL;
    m->sender_node = lp->gid;
    m->local_event_size_bytes = 0; /* We just deliver the local event here */

    /*if(lp->gid == TRACK && msg->packet_ID == TRACE)
       {
        tw_output(lp, "[%d] Packet sent: next stop %d dest %d chunk %d hops %d \n",
                lp->gid, m->next_stop, m->final_dest_gid, m->chunk_id, m->my_N_hop);
       }*/
    tw_event_send( e );
    if((cur_entry->msg.packet_size % s->params->chunk_size) && (cur_entry->msg.chunk_id == num_chunks - 1)) {
        bf->c20 = 1;
        s->link_traffic[queue] += cur_entry->msg.packet_size %
            s->params->chunk_size;
    } else {
        bf->c21 = 1;
        s->link_traffic[queue] += s->params->chunk_size;
    }

    if(cur_entry->msg.chunk_id == num_chunks - 1)
    {
        /* Invoke an event on the sending server */
        if(cur_entry->msg.local_event_size_bytes > 0)
        {
            bf->c6 = 1;
            tw_event* e_new;
            nodes_message* m_new;
            void* local_event;
            e_new = tw_event_new(cur_entry->msg.sender_svr, codes_local_latency(lp), lp);
            m_new = tw_event_data(e_new);
            local_event = (char*)cur_entry->event_data +
                cur_entry->msg.remote_event_size_bytes;
            memcpy(m_new, local_event, cur_entry->msg.local_event_size_bytes);
            tw_event_send(e_new);
        }
    }

    /* isT=1 means that we can send the newly injected packets */
    if(isT) {
        bf->c31 = 1;
        cur_entry = return_head(s->terminal_msgs, s->terminal_msgs_tail,
            queue);
        s->terminal_length[queue] -= s->params->chunk_size;
        s->all_term_length -= s->params->chunk_size;
    } else {
        bf->c8 = 1;
        cur_entry = return_head(s->pending_msgs[queue],
            s->pending_msgs_tail[queue], STATICQ);
    }

    rc_stack_push(lp, cur_entry, free_tmp, s->st);

    if(isT) {
        cur_entry = s->terminal_msgs[queue];
    } else {
        cur_entry = s->pending_msgs[queue][STATICQ];
            if(cur_entry == NULL) {
                cur_entry = s->terminal_msgs[queue];
            }
    }

    if(cur_entry != NULL) {
        bf->c9 = 1;
        ts = ts + codes_local_latency(lp);
        e = model_net_method_event_new(lp->gid, ts, lp, TORUS, (void**)&m, NULL);
        m->type = SEND;
        m->source_direction = msg->source_direction;
        m->source_dim = msg->source_dim;
        tw_event_send(e);
    } else {
        bf->c10 = 1;
        s->in_send_loop[queue] = 0;
        return;
    }
}

static void packet_arrive_rc(nodes_state * s,
        tw_bf * bf,
        nodes_message * msg,
        tw_lp * lp)
{
    codes_local_latency_reverse(lp);
    s->finished_chunks--;

    if(bf->c1)
    {
        tw_rand_reverse_unif(lp->rng);
        s->total_data_sz -= s->params->chunk_size;
        if(bf->c2)
        {
            struct mn_stats* stat;
            stat = model_net_find_stats(msg->category, s->torus_stats_array);
            stat->recv_count--;
            stat->recv_bytes -= msg->packet_size;
            stat->recv_time = msg->saved_recv_time;
            s->total_time = msg->saved_recv_time;
            N_finished_packets--;
            s->finished_packets--;

            //total_time = msg->saved_total_time;
            total_time -= (tw_now(lp) - msg->travel_start_time);
            total_hops -= msg->my_N_hop;
            s->total_hops -= msg->my_N_hop;

            if(bf->c3)
            {
                max_latency = msg->saved_available_time;
            }

            if(bf->c31)
            {
                model_net_event_rc2(lp, &msg->event_rc);
            }
        }
    }

    if(bf->c6)
    {
        int queue = msg->source_channel;
        nodes_message_list * cur_entry = NULL;

       if(bf->c30)
       {
        cur_entry = return_tail(s->queued_msgs[queue],
                s->queued_msgs_tail[queue], STATICQ);
        s->queued_length[queue] -= s->params->chunk_size;
        if(bf->c24)
        {
            s->last_buf_full[queue] = msg->saved_busy_time;
        }
       }

       if(bf->c9 || bf->c11)
       {
        cur_entry = return_tail(s->pending_msgs[queue],
                s->pending_msgs_tail[queue], STATICQ);
        s->buffer[queue][STATICQ] -= s->params->chunk_size;
        tw_rand_reverse_unif(lp->rng);
       }

       if(bf->c8)
       {
        cur_entry = return_tail(s->other_msgs,
                s->other_msgs_tail, queue);
        if(bf->c24)
            s->last_buf_full[queue] = msg->saved_busy_time;
       }
       assert(cur_entry);

       delete_nodes_message_list(cur_entry);

        if(bf->c13)
        {
           codes_local_latency_reverse(lp);
           s->in_send_loop[queue] = 0;
        }
    }
    msg->my_N_hop--;
}
/*Processes the packet after it arrives from the neighboring torus node
 * routes it to the next compute node if this is not the destination
 * OR if this is the destination then a remote event at the server is issued. */
static void packet_arrive( nodes_state * s,
		    tw_bf * bf,
		    nodes_message * msg,
		    tw_lp * lp )
{
  tw_event *e;
  tw_stime ts;
  nodes_message *m;
  mn_stats* stat;

  ts = codes_local_latency(lp);

  msg->my_N_hop++;
  s->finished_chunks++;

  if( lp->gid == msg->dest_lp )
    {
          if(lp->gid == TRACK && msg->packet_ID == TRACE)
          {
              tw_output(lp, "\n packet %ld arrived at lp %d from %ld final dest %d hops %d",
                      msg->packet_ID, (int)lp->gid, msg->sender_node, (int)msg->dest_lp, msg->my_N_hop);
          }
        bf->c1 = 1;
        s->total_data_sz += s->params->chunk_size;

        credit_send( s, lp, msg, -1);

        uint64_t num_chunks = msg->packet_size/s->params->chunk_size;
        if(msg->packet_size % s->params->chunk_size)
            num_chunks++;

        if(!num_chunks)
            num_chunks = 1;

        if( msg->chunk_id == num_chunks - 1 )
        {
	    bf->c2 = 1;
	    stat = model_net_find_stats(msg->category, s->torus_stats_array);
	    stat->recv_count++;
	    stat->recv_bytes += msg->packet_size;
	    msg->saved_recv_time = s->total_time;
        s->total_time += tw_now( lp ) - msg->travel_start_time;
        stat->recv_time += tw_now( lp ) - msg->travel_start_time;

	    /*count the number of packets completed overall*/
	    N_finished_packets++;
        s->finished_packets++;

	    msg->saved_total_time = total_time;
        total_time += tw_now( lp ) - msg->travel_start_time;
	    total_hops += msg->my_N_hop;
        s->total_hops += msg->my_N_hop;

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
                   bf->c31 = 1;
                   int net_id = model_net_get_id(LP_METHOD_NM);
                   struct codes_mctx mc_dst =
                       codes_mctx_set_global_direct(msg->sender_node);
                   struct codes_mctx mc_src =
                       codes_mctx_set_global_direct(lp->gid);
                   msg->event_rc = model_net_event_mctx(net_id, &mc_src, &mc_dst,
                           msg->category, msg->sender_svr, msg->pull_size,
                           0.0, msg->remote_event_size_bytes, tmp_ptr, 0,
                           NULL, lp);
               }
               else
               {
                   e = tw_event_new(msg->final_dest_gid, ts, lp);
                   void * m_remote = tw_event_data(e);
                   memcpy(m_remote, tmp_ptr, msg->remote_event_size_bytes);
                   tw_event_send(e);
               }
	    }
       }
    }
  else
    {
        bf->c6 = 1;
        int tmp_dir = -1, tmp_dim = -1, queue;
        tw_lpid dst_lp = msg->dest_lp;

        dimension_order_routing(s, &dst_lp, &tmp_dim, &tmp_dir);
        queue = tmp_dir + (tmp_dim * 2);

        nodes_message_list * cur_chunk = (nodes_message_list *)malloc(
                sizeof(nodes_message_list));
        init_nodes_message_list(cur_chunk, msg);

        msg->source_channel = queue;

        if(msg->remote_event_size_bytes > 0) {
            void *m_data_src = model_net_method_get_edata(TORUS, msg);
            cur_chunk->event_data = (char*)malloc(msg->remote_event_size_bytes);
            memcpy(cur_chunk->event_data, m_data_src,
                msg->remote_event_size_bytes);
        }
        int multfactor = 1;
        if(msg->source_dim != tmp_dim) {
            multfactor = 2;
        }
        cur_chunk->msg.next_stop = dst_lp;
        cur_chunk->msg.source_dim = tmp_dim;
        cur_chunk->msg.source_direction = tmp_dir;
        /* Message is traveling in the same dimension*/
        if(multfactor == 1) {
            if(s->buffer[queue][STATICQ] + s->params->chunk_size
                > s->params->buffer_size) {
                /* No buffer space available, add it in the queued messages for
                 * now */
                bf->c30 = 1;
                cur_chunk->msg.saved_queue =
                    msg->source_direction + ( msg->source_dim * 2 );
                append_to_node_message_list(s->queued_msgs[queue],
                        s->queued_msgs_tail[queue], STATICQ, cur_chunk);
                s->queued_length[queue] += s->params->chunk_size;

                if(!s->last_buf_full[queue])
                {
                    bf->c24 = 1;
                    msg->saved_busy_time = s->last_buf_full[queue];
                    s->last_buf_full[queue] = tw_now(lp);
                }

            } else {
                /* buffer space available, add it on the pending messages for
                 * this queue, send a credit back and increment the buffer
                 * space. */
                bf->c9 = 1;
                s->buffer[queue][STATICQ] += s->params->chunk_size;
                credit_send( s, lp, msg, -1 );
                append_to_node_message_list(s->pending_msgs[queue],
                    s->pending_msgs_tail[queue], STATICQ, cur_chunk);
            }
        }
        else
        {
            /* Message is travelling in different dimension so two buffer
             * spaces are required. */
                if(s->buffer[queue][STATICQ] + 2 * s->params->chunk_size
                    <= s->params->buffer_size) {
                    bf->c11 = 1;
                    s->buffer[queue][STATICQ] += s->params->chunk_size;
                    credit_send( s, lp, msg, -1 );
                    append_to_node_message_list(s->pending_msgs[queue],
                        s->pending_msgs_tail[queue], STATICQ, cur_chunk);
                }
                else
                {
                    bf->c8 = 1;
                    cur_chunk->msg.saved_queue =
                        msg->source_direction + ( msg->source_dim * 2 );
                    append_to_node_message_list(s->other_msgs,
                            s->other_msgs_tail, queue, cur_chunk);
                if(!s->last_buf_full[queue])
                {
                    bf->c24 = 1;
                    msg->saved_busy_time = s->last_buf_full[queue];
                    s->last_buf_full[queue] = tw_now(lp);
                }

                }
        }

        if(s->in_send_loop[queue] == 0) {
            bf->c13 = 1;
            ts = codes_local_latency(lp);
            e = model_net_method_event_new(lp->gid, ts, lp, TORUS, (void**)&m,
                NULL);
            m->type = SEND;
            m->source_direction = tmp_dir;
            m->source_dim = tmp_dim;
            s->in_send_loop[queue] = 1;
            tw_event_send(e);
        }
    }
}

/* reports torus statistics like average packet latency, maximum packet latency and average
 * number of torus hops traversed by the packet */
static void torus_report_stats()
{
    long long avg_hops, total_finished_packets;
    tw_stime avg_time, max_time;

    MPI_Reduce( &total_hops, &avg_hops, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_CODES);
    MPI_Reduce( &N_finished_packets, &total_finished_packets, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_CODES);
    MPI_Reduce( &total_time, &avg_time, 1,MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_CODES);
    MPI_Reduce( &max_latency, &max_time, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_CODES);

    if(!g_tw_mynode)
     {
       printf(" Average number of hops traversed %f average packet latency %lf us maximum packet latency %lf us finished packets %lld finished hops %lld \n",
               (float)avg_hops/total_finished_packets, avg_time/(total_finished_packets*1000), max_time/1000, total_finished_packets, avg_hops);
     }
}
/* finalize the torus node and free all event buffers available */
void
final( nodes_state * s, tw_lp * lp )
{
  int j;
  const torus_param *p = s->params;

  for( j = 0; j < 2 * p->n_dims; j++)
  {
  if(s->pending_msgs[j][STATICQ] != NULL)
      printf("\n LP %llu leftover pending messages ", LLU(lp->gid));

  if(s->other_msgs[j] != NULL)
      printf("\n LP %llu leftover other messages ", LLU(lp->gid));

  if(s->queued_msgs[j][STATICQ] != NULL)
      printf("\n LP %llu leftover queued messages ", LLU(lp->gid));

  if(s->terminal_msgs[j] != NULL)
      printf("\n LP %llu leftover terminal messages ", LLU(lp->gid));
  }
  rc_stack_destroy(s->st);

  model_net_print_stats(lp->gid, s->torus_stats_array);
  free(s->next_link_available_time);
  free(s->next_credit_available_time);
  free(s->next_flit_generate_time);
  free(s->link_traffic);
  free(s->terminal_msgs);
  free(s->terminal_msgs_tail);
  free(s->pending_msgs);
  free(s->pending_msgs_tail);
  free(s->queued_msgs);
  free(s->queued_msgs_tail);
  free(s->other_msgs);


  int written = 0;
  if(!s->node_id)
  {
      written = sprintf(s->output_buf,
              "# Format <LP id> <Node ID> <Total Data Size> <Total Time Spent> <# Packets finished> <Avg hops> \n");
  }
     written += sprintf(s->output_buf + written, "%llu %llu %ld %lf %ld %lf\n",
                          LLU(lp->gid), LLU(s->node_id), s->total_data_sz, s->total_time,
                          s->finished_packets, (double)s->total_hops/s->finished_packets);

     lp_io_write(lp->gid, "torus-msg-stats", written, s->output_buf);

     written = 0;
      if(!s->node_id)
      {
          written = sprintf(s->output_busy_buf,
                  "# Format <LP id> <Node ID> <Busy time(s) per torus link> \n");
      }
      written += sprintf(s->output_busy_buf + written, "%llu %llu", LLU(lp->gid), LLU(s->node_id));
     for(int i = 0; i < 2 * p->n_dims; i++)
     {
       written += sprintf(s->output_busy_buf + written, " %lf ", s->busy_time[i]);
     }
     written += sprintf(s->output_busy_buf + written, "\n");

     lp_io_write(lp->gid, "torus-link-stats", written, s->output_busy_buf);

  // since all LPs are sharing params, just let them leak for now
  // TODO: add a post-sim "cleanup" function?
  //free(s->buffer);
  //free(s->params->dim_length);
  //free(s->params->factor);
  //free(s->params->half_length);
}

static void packet_buffer_process_rc(nodes_state * s,
        tw_bf * bf,
        nodes_message * msg,
        tw_lp * lp)
{
    int queue = msg->source_direction + ( msg->source_dim * 2 );
    s->buffer[queue][STATICQ] += s->params->chunk_size;

    if(bf->c24)
    {
        s->busy_time[queue] = msg->saved_busy_time;
        s->last_buf_full[queue] = msg->saved_recv_time;
    }
    if(bf->c2)
    {
        nodes_message_list *tail = return_tail(
                s->pending_msgs[queue], s->pending_msgs_tail[queue],
                STATICQ);
        prepend_to_node_message_list(s->queued_msgs[queue],
                s->queued_msgs_tail[queue], STATICQ, tail);
        s->queued_length[queue] += s->params->chunk_size;
        tw_rand_reverse_unif(lp->rng);
        s->buffer[queue][STATICQ] -= s->params->chunk_size;
    }

    if(bf->c3)
    {
        nodes_message_list *tail = return_tail(
                s->pending_msgs[queue], s->pending_msgs_tail[queue],
                STATICQ);
        prepend_to_node_message_list(s->other_msgs,
                s->other_msgs_tail, queue, tail);
        tw_rand_reverse_unif(lp->rng);
        s->buffer[queue][STATICQ] -= s->params->chunk_size;
    }

    if(bf->c5)
    {
        codes_local_latency_reverse(lp);
        s->in_send_loop[queue] = 0;
    }
    return;
}
/* increments the buffer count after a credit arrives from the remote compute node */
static void packet_buffer_process( nodes_state * ns, tw_bf * bf, nodes_message * msg, tw_lp * lp )
{
    int queue = msg->source_direction + ( msg->source_dim * 2 );
    ns->buffer[queue][STATICQ] -= ns->params->chunk_size;
    if(ns->last_buf_full[queue])
    {
        bf->c24 = 1;
        msg->saved_busy_time =  ns->busy_time[queue];
        msg->saved_recv_time = ns->last_buf_full[queue];
        ns->busy_time[queue] += (tw_now(lp) - ns->last_buf_full[queue]);
        ns->last_buf_full[queue] = 0;
    }
    /* Priority of the queues:
     * queued_msgs store the messages that have not yet entered the buffer
     * space
     * pending_msgs are messages that have already occupied a buffer slot, they
     * are highest in priority
     * other_msgs are messages that want to travel in a different dimension but
     * the buffer space is not available right now (2 buffer spaces must be
     * available to go to a different dimension according to bubble flow
     * control */
    if(ns->queued_msgs[queue][STATICQ] != NULL) {
            bf->c2 = 1;
            nodes_message_list *head = return_head(ns->queued_msgs[queue],
                ns->queued_msgs_tail[queue], STATICQ);
            ns->queued_length[queue] -= ns->params->chunk_size;
            credit_send( ns, lp, &head->msg, 1);
            append_to_node_message_list(ns->pending_msgs[queue],
                ns->pending_msgs_tail[queue], STATICQ, head);
            ns->buffer[queue][STATICQ] += ns->params->chunk_size;
        } else if(ns->buffer[queue][STATICQ] + 2 * ns->params->chunk_size
            <= ns->params->buffer_size) {
            if(ns->other_msgs[queue] != NULL) {
                bf->c3 = 1;
                nodes_message_list *head = return_head(ns->other_msgs,
                        ns->other_msgs_tail, queue);
                credit_send( ns, lp, &head->msg, 1);
                append_to_node_message_list(ns->pending_msgs[queue],
                        ns->pending_msgs_tail[queue], STATICQ, head);
                ns->buffer[queue][STATICQ] += ns->params->chunk_size;
            }
           }
    if(ns->in_send_loop[queue] == 0) {
        bf->c5 = 1;
        tw_stime ts = codes_local_latency(lp);
        nodes_message *m;
        tw_event *e = model_net_method_event_new(lp->gid, ts, lp, TORUS,
            (void**)&m, NULL);
        m->type = SEND;
        m->source_direction = msg->source_direction;
        m->source_dim = msg->source_dim;
        ns->in_send_loop[queue] = 1;
        tw_event_send(e);
    }
}

/* reverse handler for torus node */
static void node_rc_handler(nodes_state * s, tw_bf * bf, nodes_message * msg, tw_lp * lp)
{
  switch(msg->type)
    {
    case GENERATE:
        packet_generate_rc(s, bf, msg, lp);
        break;

	case ARRIVAL:
        packet_arrive_rc(s, bf, msg, lp);
	break;

	case SEND:
         packet_send_rc(s, bf, msg, lp);
	break;

       case CREDIT:
          packet_buffer_process_rc(s, bf, msg, lp);
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
    default:
        printf("\n Being sent to wrong event handler %d ", msg->type);
     }
}

/* forward event handler for torus node event */
static void event_handler(nodes_state * s, tw_bf * bf, nodes_message * msg, tw_lp * lp)
{
 *(int *) bf = (int) 0;
 rc_stack_gc(lp, s->st);
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
    node_collective_init(s, msg, lp);
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
    (commit_f) NULL,
    (final_f) final,
    (map_f) codes_mapping,
    sizeof(nodes_state),
};

/* returns the torus lp type for lp registration */
static const tw_lptype* torus_get_lp_type(void)
{
   return(&torus_lp);
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
   .mn_collective_call = torus_collective,
   .mn_collective_call_rc = torus_collective_rc,
   .mn_sample_fn = NULL,
   .mn_sample_rc_fn = NULL,
   .mn_sample_init_fn = NULL,
   .mn_sample_fini_fn = NULL
};

/* user-facing modelnet functions */
void model_net_torus_get_dims(
        char const        * anno,
        int                 ignore_annotations,
        int               * n,
        int const        ** dims)
{
    torus_param const * p = NULL;

    if (ignore_annotations)
        p = &all_params[0];
    else if (anno_map->has_unanno_lp > 0 && anno == NULL)
        p = &all_params[anno_map->num_annos];
    else {
        for (int i = 0; i < num_params; i++) {
            if (strcmp(anno, anno_map->annotations[i].ptr) == 0) {
                p = &all_params[i];
                break;
            }
        }
    }

    if (p == NULL)
        tw_error(TW_LOC, "unable to find configuration for annotation %s", anno);

    *n = p->n_dims;
    *dims = p->dim_length;
}

void model_net_torus_get_dim_id(
        int         flat_id,
        int         ndims,
        const int * dim_lens,
        int       * out_dim_ids)
{
    to_dim_id(flat_id, ndims, dim_lens, out_dim_ids);
}

int model_net_torus_get_flat_id(
        int         ndims,
        const int * dim_lens,
        const int * dim_ids)
{
    return to_flat_id(ndims, dim_lens, dim_ids);
}

#ifdef ENABLE_CORTEX

static int torus_get_number_of_compute_nodes(void* topo) {
	// TODO
    (void)topo;
	return -1;
}

static int torus_get_number_of_routers(void* topo) {
	// TODO
    (void)topo;
	return -1;
}

static double torus_get_router_link_bandwidth(void* topo, router_id_t r1, router_id_t r2) {
	// TODO
    (void)topo;
    (void)r1;
    (void)r2;
	return -1.0;
}

static double torus_get_compute_node_bandwidth(void* topo, cn_id_t node) {
	// TODO
    (void)topo;
    (void)node;
	return -1.0;
}

static int torus_get_router_neighbor_count(void* topo, router_id_t r) {
	// TODO
    (void)topo;
    (void)r;
	return 0;
}

static void torus_get_router_neighbor_list(void* topo, router_id_t r, router_id_t* neighbors) {
	// TODO
    (void)topo;
    (void)r;
    (void)neighbors;
}

static int torus_get_router_location(void* topo, router_id_t r, int32_t* location, int size) {
	// TODO
    (void)topo;
    (void)r;
    (void)location;
    (void)size;
	return 0;
}

static int torus_get_compute_node_location(void* topo, cn_id_t node, int32_t* location, int size) {
	// TODO
    (void)topo;
    (void)node;
    (void)location;
    (void)size;
	return 0;
}

static router_id_t torus_get_router_from_compute_node(void* topo, cn_id_t node) {
	// TODO
    (void)topo;
    (void)node;
	return -1;
}

static int torus_get_router_compute_node_count(void* topo, router_id_t r) {
	// TODO
    (void)topo;
    (void)r;
	return 0;
}

static void torus_get_router_compute_node_list(void* topo, router_id_t r, cn_id_t* nodes) {
	// TODO
    (void)topo;
    (void)r;
    (void)nodes;
}

cortex_topology torus_cortex_topology = {
	.internal = NULL,
	.get_number_of_compute_nodes = torus_get_number_of_compute_nodes,
	.get_number_of_routers	= torus_get_number_of_routers,
	.get_router_link_bandwidth 	= torus_get_router_link_bandwidth,
	.get_compute_node_bandwidth 	= torus_get_compute_node_bandwidth,
	.get_router_neighbor_count 	= torus_get_router_neighbor_count,
	.get_router_neighbor_list	= torus_get_router_neighbor_list,
	.get_router_location		= torus_get_router_location,
	.get_compute_node_location	= torus_get_compute_node_location,
	.get_router_from_compute_node	= torus_get_router_from_compute_node,
	.get_router_compute_node_count	= torus_get_router_compute_node_count,
	.get_router_compute_node_list	= torus_get_router_compute_node_list,
};

#endif


/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
