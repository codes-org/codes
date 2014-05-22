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
#include "model-net.h"
#include "model-net-sched.h"
#include "net/dragonfly.h"
#include "net/loggp.h"
#include "net/simplenet-upd.h"
#include "net/simplewan.h"
#include "net/torus.h"

extern int model_net_base_magic;

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

// Get a ptr to past the message struct area, where the self/remote events
// are located, given the type of network.
// NOTE: this should ONLY be called on model-net implementations, nowhere else
void * model_net_method_get_edata(int net_id, void * msg);

/// The following functions/data structures should not need to be used by
/// model developers - they are just provided so other internal components can
/// use them

// init method
void model_net_base_init();

enum model_net_base_event_type {
    MN_BASE_NEW_MSG,
    // schedule next packet
    MN_BASE_SCHED_NEXT,
    // message goes directly down to topology-specific event handler
    MN_BASE_PASS
};

typedef struct model_net_base_msg {
    // no need for event type - in wrap message
    union {
        model_net_request req;
        struct {} sched; // needs nothing at the moment
    } u;
    model_net_sched_rc rc; // rc for scheduling events
} model_net_base_msg;

typedef struct model_net_wrap_msg {
    enum model_net_base_event_type event_type;
    int magic;
    union {
        model_net_base_msg m_base;  // base lp
        terminal_message   m_dfly;  // dragonfly
        loggp_message      m_loggp; // loggp
        sn_message         m_snet;  // simplenet
        sw_message         m_swan;  // simplewan
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
