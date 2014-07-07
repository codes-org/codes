/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#ifndef MODELNET_H
#define MODELNET_H

#include "ross.h"
#include "codes/lp-type-lookup.h"
#include "codes/configuration.h"
#include "codes/lp-io.h"
#include <stdint.h>

#define PULL_MSG_SIZE 128

#define MAX_NAME_LENGTH 256
#define CATEGORY_NAME_MAX 16
#define CATEGORY_MAX 12

/* HACK: there is currently no scheduling fidelity across multiple
 * model_net_event calls. Hence, problems arise when some LP sends multiple
 * messages as part of an event and expects FCFS ordering. A proper fix which
 * involves model-net LP-level scheduling of requests is ideal, but not 
 * feasible for now (would basically have to redesign model-net), so expose
 * explicit start-sequence and stop-sequence markers as a workaround */
extern int in_sequence;
extern tw_stime mn_msg_offset;
#define MN_START_SEQ() do {\
    in_sequence = 1; \
    mn_msg_offset = 0.0; \
} while (0)
#define MN_END_SEQ() do {\
    in_sequence = 0;\
} while (0)


typedef struct simplenet_param simplenet_param;
typedef struct simplewan_param simplewan_param;
typedef struct dragonfly_param dragonfly_param;
typedef struct torus_param torus_param;
typedef struct loggp_param loggp_param;
typedef struct mn_stats mn_stats;

// use the X-macro to get types and names rolled up into one structure
// format: { enum vals, config name, internal lp name, lp method struct}
// last value is sentinel
#define NETWORK_DEF \
    X(SIMPLENET, "modelnet_simplenet", "simplenet", &simplenet_method)\
    X(SIMPLEWAN, "modelnet_simplewan", "simplewan", &simplewan_method)\
    X(TORUS,     "modelnet_torus",     "torus",     &torus_method)\
    X(DRAGONFLY, "modelnet_dragonfly", "dragonfly", &dragonfly_method)\
    X(LOGGP,     "modelnet_loggp",     "loggp",     &loggp_method)\
    X(MAX_NETS,  NULL,                 NULL,        NULL)

#define X(a,b,c,d) a,
enum NETWORKS
{
    NETWORK_DEF
};
#undef X

// network identifiers (both the config lp names and the model-net internal
// names)
extern char * model_net_lp_config_names[];
extern char * model_net_method_names[];

// request structure that gets passed around (by the model-net implementation,
// not the user)
typedef struct model_net_request {
    tw_lpid  final_dest_lp;
    tw_lpid  src_lp;
    uint64_t msg_size;
    uint64_t packet_size;
    int      net_id;
    int      is_pull;
    int      remote_event_size;
    int      self_event_size;
    char     category[CATEGORY_NAME_MAX];
} model_net_request;

/* data structure for tracking network statistics */
struct mn_stats
{
    char category[CATEGORY_NAME_MAX];
    long send_count;
    long send_bytes;
    tw_stime send_time;
    long recv_count;
    long recv_bytes;
    tw_stime recv_time;
    long max_event_size;
};

/* structs for initializing a network/ specifying network parameters */
struct loggp_param
{
  char* net_config_file; /* file with loggp parameter table */
};

/* structs for initializing a network/ specifying network parameters */
struct simplenet_param
{
  double net_startup_ns; /*simplenet startup cost*/
  double net_bw_mbps; /*Link bandwidth per byte*/
  int num_nics;
};

struct simplewan_param
{
    char bw_filename[MAX_NAME_LENGTH];
    char startup_filename[MAX_NAME_LENGTH];
};

struct dragonfly_param
{
  int num_routers; /*Number of routers in a group*/
  double local_bandwidth;/* bandwidth of the router-router channels within a group */
  double global_bandwidth;/* bandwidth of the inter-group router connections */
  double cn_bandwidth;/* bandwidth of the compute node channels connected to routers */
  int num_vcs; /* number of virtual channels */
  int local_vc_size; /* buffer size of the router-router channels */
  int global_vc_size; /* buffer size of the global channels */
  int cn_vc_size; /* buffer size of the compute node channels */
  short routing; /* minimal or non-minimal routing */
  short chunk_size; /* full-sized packets are broken into smaller chunks.*/
};

struct torus_param
{
  int n_dims; /*Dimension of the torus network, 5-D, 7-D or any other*/
  int* dim_length; /*Length of each torus dimension*/
  double link_bandwidth;/* bandwidth for each torus link */
  int buffer_size; /* number of buffer slots for each vc in flits*/
  int num_vc; /* number of virtual channels for each torus link */
  float mean_process;/* mean process time for each flit  */
  int chunk_size; /* chunk is the smallest unit--default set to 32 */
};
 /* In general we need to figure out how to pass configuration information to
 * the methods and we need to be able to calculate ross event message size.
 */
/*Initialize the network by specifying the network parameters. The 
 * underlying model-net.c function call will set the network parameters 
 * according to the network name specified*/
// return an integer being the identifier for the type of network
// call modelnet setup 1 time for a torus and retur value is 0 for e.g.

/* call set params for configuring the network parameters from the config file*/
int model_net_set_params();

// setup the modelnet parameters
int model_net_setup(char* net_name, uint64_t packet_size, const void* net_params);

/* utility function to get the modelnet ID post-setup */
int model_net_get_id(char *net_name);

/* This event does a collective operation call for model-net */
void model_net_event_collective(
    int net_id,
    char* category,
    int message_size,
    int remote_event_size,
    const void* remote_event,
    tw_lp *sender);

/* reverse event of the collective operation call */
void model_net_event_collective_rc(
        int net_id,
        int message_size,
        tw_lp *sender);

/* allocate and transmit a new event that will pass through model_net to 
 * arrive at its destination:
 *
 * - category: category name to associate with this communication
 *   - OPTIONAL: callers can set this to NULL if they don't want to use it,
 *     and model_net methods can ignore it if they don't support it
 * - final_dest_lp: the LP that the message should be delivered to.
 *   - NOTE: this is _not_ the LP of an underlying network method (for
 *     example, it is not a torus or dragonfly LP), but rather the LP of an
 *     MPI process or storage server that you are transmitting to.
 * - message_size: this is the size of the message (in bytes) that modelnet
 *     will simulate transmitting to the final_dest_lp.  It can be any size
 *     (i.e. it is not constrained by transport packet size).
 * - remote_event_size: this is the size of the ROSS event structure that
 *     will be delivered to the final_dest_lp.
 * - remote_event: pointer ot data to be used as the remove event message
 * - self_event_size: this is the size of the ROSS event structure that will
 *     be delivered to the calling LP once local completion has occurred for
 *     the network transmission.
 *     - NOTE: "local completion" in this sense means that model_net has
 *       transmitted the data off of the local node, but it does not mean that
 *       the data has been (or even will be) delivered.  Once this event is
 *       delivered the caller is free to re-use its buffer.
 * - self_event: pionter to data to be used as the self event message
 * - sender: pointer to the tw_lp structure of the API caller.  This is
 *     identical to the sender argument to tw_event_new().
 */
// first argument becomes the network ID
void model_net_event(
    int net_id,
    char* category, 
    tw_lpid final_dest_lp, 
    uint64_t message_size, 
    tw_stime offset,
    int remote_event_size,
    const void* remote_event,
    int self_event_size,
    const void* self_event,
    tw_lp *sender);

/* model_net_find_local_device()
 *
 * returns the LP id of the network card attached to the calling LP
 */
tw_lpid model_net_find_local_device(int net_id, tw_lp *sender);

int model_net_get_msg_sz(int net_id);

/* model_net_event_rc()
 *
 * This function does reverse computation for the model_net_event_new()
 * function.
 * - sender: pointer to the tw_lp structure of the API caller.  This is
 *   identical to the sender argument to tw_event_new().
 */
/* NOTE: we may end up needing additoinal arguments here to track state for
 * reverse computation; add as needed 
 */
void model_net_event_rc(
    int net_id,
    tw_lp *sender,
    uint64_t message_size);


/* Issue a 'pull' from the memory of the destination LP, without
 * requiring the destination LP to do event processing. This is meant as a
 * simulation-based abstraction of RDMA. A control packet will be sent to the
 * destination LP, the payload will be sent back to the requesting LP, and the
 * requesting LP will be issued it's given completion event.
 *
 * Parameters are largely the same as model_net_event, with the following
 * exceptions:
 * - final_dest_lp is the lp to pull data from
 * - self_event_size, self_event are applied at the requester upon receipt of 
 *   the payload from the dest
 */
void model_net_pull_event(
        int net_id,
        char *category,
        tw_lpid final_dest_lp,
        uint64_t message_size,
        tw_stime offset,
        int self_event_size,
        const void *self_event,
        tw_lp *sender);
void model_net_pull_event_rc(
        int net_id,
        tw_lp *sender);

/* returns pointer to LP information for simplenet module */
const tw_lptype* model_net_get_lp_type(int net_id);

uint64_t model_net_get_packet_size(int net_id);

/* used for reporting overall network statistics for e.g. average latency ,
 * maximum latency, total number of packets finished during the entire
 * simulation etc. */
void model_net_report_stats(int net_id);

/* writing model-net statistics on a per LP basis */
void model_net_write_stats(tw_lpid lpid, mn_stats* stat);

/* printing model-net statistics on a per LP basis */
void model_net_print_stats(tw_lpid lpid, mn_stats mn_stats_array[]);

/* find model-net statistics */
mn_stats* model_net_find_stats(const char* category, mn_stats mn_stats_array[]);
#endif /* MODELNET_H */

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
