/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#ifndef MODELNET_METHOD_H
#define MODELNET_METHOD_H

#ifdef __cplusplus
extern "C" {
#endif

#include <ross.h>

// forward decl of model_net_method since we currently have a circular include
// (method needs sched def, sched needs method def)
struct model_net_method;

#include "codes/model-net-sched.h"

// interface that each model-net model implements

struct model_net_method
{
    uint64_t packet_size; /* packet size */
    void (*mn_configure)(); /* For initializing the network */
    /* Register lp types with ROSS. This may be left as NULL, in which case the
     * type corresponding to name "modelnet_<type>" will be registered
     * automatically. Most networks don't need this (currently, only dragonfly
     * uses it) */
    void (*mn_register)(tw_lptype *base_type);
    tw_stime (*model_net_method_packet_event)(
            model_net_request const * req,
            uint64_t message_offset, // offset in the context of the whole message
            uint64_t packet_size, // needed in case message < packet
            tw_stime offset,
            mn_sched_params const * sched_params,
            void const * remote_event,
            void const * self_event,
            tw_lp *sender,
            int is_last_pckt);
    void (*model_net_method_packet_event_rc)(tw_lp *sender);
    tw_stime (*model_net_method_recv_msg_event)(
            const char * category,
            tw_lpid final_dest_lp,
            tw_lpid src_mn_lp, // the modelnet LP this message came from
            uint64_t msg_size,
            int is_pull,
            uint64_t pull_size,
            tw_stime offset,
            int remote_event_size,
            const void* remote_event,
            tw_lpid src_lp, // original caller of model_net_(pull_)event
            tw_lp *sender); // lp message is being called from (base LP)
    void (*model_net_method_recv_msg_event_rc)(tw_lp *lp);
    const tw_lptype* (*mn_get_lp_type)();
    int (*mn_get_msg_sz)();
    void (*mn_report_stats)();
    void (*mn_collective_call)(char const * category, int message_size, int remote_event_size, const void* remote_event, tw_lp* sender);
    void (*mn_collective_call_rc)(int message_size, tw_lp* sender);
    event_f mn_sample_fn;
    revent_f mn_sample_rc_fn;
    init_f mn_sample_init_fn;
    final_f mn_sample_fini_fn;
    void (*mn_model_stat_register)(st_model_types *base_type);
    const st_model_types* (*mn_get_model_stat_types)();
};

extern struct model_net_method * method_array[];

#ifdef __cplusplus
}
#endif

#endif /* MODELNET_METHOD_H */

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
