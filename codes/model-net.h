/*
 * Copyright (C) 2013, University of Chicago
 *
 * See COPYRIGHT notice in top-level directory.
 */

#ifndef MODELNET_H
#define MODELNET_H

#include "ross.h"
#include "codes/lp-type-lookup.h"

typedef struct simplenet_param simplenet_param;
typedef struct dragonfly_param dragonfly_param;
typedef struct torus_param torus_param;

enum NETWORKS
{
  SIMPLENET,
  DRAGONFLY,
  TORUS
};

/* structs for initializing a network/ specifying network parameters */
struct simplenet_param
{
  double net_startup_ns; /*simplenet startup cost*/
  double net_bw_mbps; /*Link bandwidth per byte*/
  int num_nics;
};

struct dragonfly_param
{
  char* name;
  int num_routers; /*Number of routers in a group*/
  int num_nodes; /*Number of compute nodes connected to a router*/
  int num_chans; /*Number of global channels connected to each router*/
};

struct torus_param
{
  char* name;
  int n_dim; /*Dimension of the torus network, 5-D, 7-D or any other*/
  int* dim_length; /*Length of each torus dimension*/
};
/* NOTE: the following auxilliary functions are probably wrong; just leaving
 * these examples from simplenet for reference purposes.
 *
 * In general we need to figure out how to pass configuration information to
 * the methods and we need to be able to calculate ross event message size.
 */

#if 0
/* returns a pointer to the lptype struct to use for simplenet LPs */
const tw_lptype* sn_get_lp_type(void);

/* set model parameters:
 *
 * - net_startup_ns: network startup cost in ns.
 * - net_bs_mbs: network bandwidth in MiB/s (megabytes per second).
 */
void sn_set_params(double net_startup_ns, double net_bw_mbs);

/* retrieve the size of the portion of the event struct that is consumed by
 * the simplenet module.  The caller should add this value to the size of
 * its own event structure to get the maximum total size of a message.
 */
int sn_get_msg_sz(void);

/* retrieve the minimum timestamp offset that simplenet will use */
tw_stime sn_get_min_ts(void);

#endif
/*Initialize the network by specifying the network parameters. The 
 * underlying model-net.c function call will set the network parameters 
 * according to the network name specified*/
// return an integer being the identifier for the type of network
// call modelnet setup 1 time for a torus and retur value is 0 for e.g.
// call modelnet setup second time for a simplenet
int model_net_setup(char* net_name, int packet_size, const void* net_params);
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
    int message_size, 
    int remote_event_size,
    const void* remote_event,
    int self_event_size,
    const void* self_event,
    tw_lp *sender);

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
    int message_size);

const int model_net_get_msg_sz(int net_id);

/* returns pointer to LP information for simplenet module */
const tw_lptype* model_net_get_lp_type(int net_id);

const int model_net_get_packet_size(int net_id);

void model_net_add_lp_type(int net_id);
#endif /* MODELNET_H */

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
