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
    char* method_name;  /* example: "dragonfly" */
    int packet_size; /* packet size */
    void (*mn_setup)(const void* net_params); /* For initializing the network */
    void (*model_net_method_packet_event)(
        char* category, 
        tw_lpid final_dest_lp, 
        int packet_size, 
        int remote_event_size,  /* 0 means don't deliver remote event */
        const void* remote_event,
        int self_event_size,    /* 0 means don't deliver self event */
        const void* self_event,
        tw_lp *sender,
	int is_last_pckt);
    void (*model_net_method_packet_event_rc)(tw_lp *sender);
    const tw_lptype* (*mn_get_lp_type)();
    int (*mn_get_msg_sz)();
    void (*mn_report_stats)();
};

#endif /* MODELNET_METHOD_H */

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
