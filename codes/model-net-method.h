/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#ifndef MODELNET_METHOD_H
#define MODELNET_METHOD_H

#include <ross.h>

struct model_net_method
{
    uint64_t packet_size; /* packet size */
    void (*mn_setup)(const void* net_params); /* For initializing the network */
    tw_stime (*model_net_method_packet_event)(
        char* category, 
        tw_lpid final_dest_lp, 
        uint64_t packet_size, 
        int is_pull,
        uint64_t pull_size, /* only used when is_pull==1 */
        tw_stime offset,
        int remote_event_size,  /* 0 means don't deliver remote event */
        const void* remote_event,
        int self_event_size,    /* 0 means don't deliver self event */
        const void* self_event,
        tw_lpid src_lp, // original caller of model_net_(pull_)event
        tw_lp *sender, // lp message is being called from (base LP)
	int is_last_pckt);
    void (*model_net_method_packet_event_rc)(tw_lp *sender);
    const tw_lptype* (*mn_get_lp_type)();
    int (*mn_get_msg_sz)();
    void (*mn_report_stats)();
    tw_lpid (*model_net_method_find_local_device)(tw_lp *sender);
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
