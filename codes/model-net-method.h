/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#ifndef MODELNET_METHOD_H
#define MODELNET_METHOD_H

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
        char* category, 
        tw_lpid final_dest_lp, 
        uint64_t packet_size, 
        int is_pull,
        uint64_t pull_size, /* only used when is_pull==1 */
        tw_stime offset,
        // this parameter is used to propagate message specific parameters
        // to modelnet models that need it. Required by routing-related
        // functions (currently just model_net_method_send_msg_recv_event)
        //
        // TODO: make this param more general
        const mn_sched_params *sched_params,
        int remote_event_size,  /* 0 means don't deliver remote event */
        const void* remote_event,
        int self_event_size,    /* 0 means don't deliver self event */
        const void* self_event,
        tw_lpid src_lp, // original caller of model_net_(pull_)event
        tw_lp *sender, // lp message is being called from (base LP)
	int is_last_pckt);
    void (*model_net_method_packet_event_rc)(tw_lp *sender);
    tw_stime (*model_net_method_recv_msg_event)(
            const char * category,
            tw_lpid final_dest_lp,
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
    tw_lpid (*model_net_method_find_local_device)(
        const char * annotation,
        int          ignore_annotations,
        tw_lpid      sender_gid);
    void (*mn_collective_call)(char* category, int message_size, int remote_event_size, const void* remote_event, tw_lp* sender);
    void (*mn_collective_call_rc)(int message_size, tw_lp* sender);    
};

extern struct model_net_method * method_array[];

#endif /* MODELNET_METHOD_H */

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
