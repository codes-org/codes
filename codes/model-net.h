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
 * explicit start-sequence and stop-sequence markers as a workaround
 */
extern int mn_in_seqence;
extern tw_stime mn_msg_offset;
#define MN_START_SEQ() do {\
    mn_in_seqence = 1; \
    mn_msg_offset = 0.0; \
} while (0)
#define MN_END_SEQ() do {\
    mn_in_seqence = 0;\
} while (0)


typedef struct mn_stats mn_stats;

// use the X-macro to get types and names rolled up into one structure
// format: { enum vals, config name, internal lp name, lp method struct}
// last value is sentinel
#define NETWORK_DEF \
    X(SIMPLENET, "modelnet_simplenet", "simplenet", &simplenet_method)\
    X(SIMPLEP2P, "modelnet_simplep2p", "simplep2p", &simplep2p_method)\
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

// message parameter types
enum msg_param_type {
    // currently, scheduler parameters are the only type
    MN_MSG_PARAM_SCHED,
    MAX_MN_MSG_PARAM_TYPES
};

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

/* Registers all model-net LPs in ROSS. Should be called after 
 * configuration_load, but before codes_mapping_setup */
void model_net_register();

/* Configures all model-net LPs based on the CODES configuration, and returns
 * ids to address the different types by.
 *
 * id_count - the output number of networks
 *
 * return - the set of network IDs, indexed in the order given by the
 * modelnet_order configuration parameter */
int* model_net_configure(int *id_count);

/* Initialize/configure the network(s) based on the CODES configuration.
 * returns an array of the network ids, indexed in the order given by the 
 * modelnet_order configuration parameter 
 * OUTPUT id_count - the output number of networks */
int* model_net_set_params(int *id_count);

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
 * - net_id: the type of network to send this message through. The set of
 *   net_id's is given by model_net_configure.
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
 * - remote_event: pointer to data to be used as the remote event message. When
 *   the message payload (of size message_size) has been fully delivered to the
 *   destination (given by final_dest_lp), this event will be issued.
 * - self_event_size: this is the size of the ROSS event structure that will
 *     be delivered to the calling LP once local completion has occurred for
 *     the network transmission.
 *     - NOTE: "local completion" in this sense means that model_net has
 *       transmitted the data off of the local node, but it does not mean that
 *       the data has been (or even will be) delivered.  Once this event is
 *       delivered the caller is free to re-use its buffer.
 * - self_event: pointer to data to be used as the self event message. When the
 *   message payload (of size message_size) has been fully sent from the
 *   sender's point of view (e.g. the corresponding NIC has sent out all
 *   packets for this message), the event will be issued to the sender.
 * - sender: pointer to the tw_lp structure of the API caller.  This is
 *     identical to the sender argument to tw_event_new().
 *
 * The modelnet LP used for communication is the LP in the same group, same
 * repetition, using net_id to differentiate different model types. If
 * more than one modelnet model of the same type but different annotation exist,
 * then the first one listed will be used.
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
/*
 * See model_net_event for a general description.
 *
 * Unlike model_net_event, this function uses the annotation to differentiate
 * multiple modelnet LPs with the same type but different annotation. The caller
 * annotation is not consulted here.
 */
void model_net_event_annotated(
        int net_id,
        const char * annotation,
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
tw_lpid model_net_find_local_device(
        int          net_id,
        const char * annotation,
        int          ignore_annotations,
        tw_lpid      sender_gid);

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
void model_net_pull_event_annotated(
        int net_id,
        const char * annotation,
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

/*
 * Set message-specific parameters
 * type     - overall type (see msg_param_type)
 * sub_type - type of parameter specific to type. This is intended to be
 *            an enum for each of msg_param_type's values
 * params   - the parameter payload
 *
 * This function works by setting up a temporary parameter context within the
 * model-net implementation (currently implemented as a set of translation-unit
 * globals). Upon a subsequent model_net_*event* call, the context is consumed
 * and reset to an unused state.
 * 
 * NOTE: this call MUST be placed in the same calling context as the subsequent
 * model_net_*event* call. Otherwise, the parameters are not guaranteed to work
 * on the intended event, and may possibly be consumed by another, unrelated
 * event.
 */
void model_net_set_msg_param(
        enum msg_param_type type,
        int sub_type,
        const void * params);

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
