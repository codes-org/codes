/*
 * Copyright (C) 2014 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

/* This is the base model-net LP that all events pass through before
 * performing any topology-specific work. Packet scheduling, dealing with
 * packet loss (potentially), etc. happens here. 
 * Additionally includes wrapper event "send" function that all 
 * events for underlying models must go through */

#ifndef MODEL_NET_LP_H  
#define MODEL_NET_LP_H

#include <ross.h>
#include "codes/lp-msg.h"
#include "model-net.h"
#include "model-net-sched.h"
#include "net/dragonfly.h"
#include "net/loggp.h"
#include "net/simplenet-upd.h"
#include "net/simplep2p.h"
#include "net/torus.h"

extern int model_net_base_magic;

// register the networks with ROSS, given the array of flags, one for each
// network type
void model_net_base_register(int *do_config_nets);
// configure the base LP type, setting up general parameters
void model_net_base_configure();

/// The remaining functions/data structures are only of interest to model-net
/// model developers

// Construct a model-net-specific event, analagous to a tw_event_new and
// codes_event_new. The difference here is that we return pointers to 
// both the message data (to be cast into the appropriate type) and the
// pointer to the end of the event struct.
// 
// This function is expected to be called within each specific model-net
// method - strange and disturbing things will happen otherwise
tw_event * model_net_method_event_new(
        tw_lpid dest_gid,
        tw_stime offset_ts,
        tw_lp *sender,
        int net_id,
        void **msg_data,
        void **extra_data);

// Construct a model-net-specific event, similar to model_net_method_event_new.
// The primary differences are:
// - the event gets sent to final_dest_lp and put on it's receiver queue
// - no message initialization is needed - that's the job of the
//   model_net_method_recv_msg_event functions
//
// NOTE: this is largely a constructor of a model_net_request
void model_net_method_send_msg_recv_event(
        tw_lpid final_dest_lp,
        tw_lpid dest_mn_lp, // which model-net lp is going to handle message
        tw_lpid src_lp, // the "actual" source (as opposed to the model net lp)
        uint64_t msg_size, // the size of this message
        int is_pull,
        uint64_t pull_size, // the size of the message to pull if is_pull==1
        int remote_event_size,
        const mn_sched_params *sched_params,
        const char * category,
        int net_id,
        void * msg,
        tw_stime offset,
        tw_lp *sender);
// just need to reverse an RNG for the time being
void model_net_method_send_msg_recv_event_rc(tw_lp *sender);

// Issue an event from the underlying model (e.g., simplenet, loggp) to tell the
// scheduler when next to issue a packet event. As different models update their
// notion of "idleness" separately, this is necessary. DANGER: Failure to call
// this function appropriately will cause the scheduler to hang or cause other
// weird behavior.
//
// This function is expected to be called within each specific model-net
// method - strange and disturbing things will happen otherwise
void model_net_method_idle_event(tw_stime offset_ts, int is_recv_queue,
        tw_lp * lp);

// Get a ptr to past the message struct area, where the self/remote events
// are located, given the type of network.
// NOTE: this should ONLY be called on model-net implementations, nowhere else
void * model_net_method_get_edata(int net_id, void * msg);

/// The following functions/data structures should not need to be used by
/// model developers - they are just provided so other internal components can
/// use them

enum model_net_base_event_type {
    MN_BASE_NEW_MSG,
    // schedule next packet
    MN_BASE_SCHED_NEXT,
    // message goes directly down to topology-specific event handler
    MN_BASE_PASS
};

typedef struct model_net_base_msg {
    // no need for event type - in wrap message
    model_net_request req;
    int is_from_remote;
    // parameters to pass to new messages (via model_net_set_msg_params)
    // TODO: make this a union for multiple types of parameters
    mn_sched_params sched_params;
    model_net_sched_rc rc; // rc for scheduling events
} model_net_base_msg;

typedef struct model_net_wrap_msg {
    msg_header h;
    union {
        model_net_base_msg m_base;  // base lp
        terminal_message   m_dfly;  // dragonfly
        loggp_message      m_loggp; // loggp
        sn_message         m_snet;  // simplenet
        sp_message         m_sp2p;  // simplep2p
        nodes_message      m_torus; // torus
        // add new ones here
    } msg;
} model_net_wrap_msg;

#endif /* end of include guard: MODEL_NET_LP_H */

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
