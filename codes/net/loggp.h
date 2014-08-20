/*
 * Copyright (C) 2014 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#ifndef LOGGP_H
#define LOGGP_H

#include "../model-net-sched.h"

/* types of events that will constitute triton requests */
enum loggp_event_type 
{
    LG_MSG_READY = 1,  /* sender has transmitted msg to receiver */
    LG_MSG_START,      /* initiate a transmission */
};

typedef struct loggp_message loggp_message;

struct loggp_message
{
    int magic; /* magic number */
    enum loggp_event_type event_type;
    tw_lpid src_gid; /* who transmitted this msg? */
    tw_lpid final_dest_gid; /* who is eventually targetted with this msg? */
    uint64_t net_msg_size_bytes;     /* size of modeled network message */
    int event_size_bytes;     /* size of simulator event message that will be tunnelled to destination */
    int local_event_size_bytes;     /* size of simulator event message that delivered locally upon local completion */
    char category[CATEGORY_NAME_MAX]; /* category for communication */
    int is_pull;
    uint64_t pull_size;

    // scheduling parameters used in this message. Necessary for receiver-side
    // queueing
    mn_sched_params sched_params;

    /* for reverse computation */
    tw_stime net_send_next_idle_saved;
    tw_stime net_recv_next_idle_saved;
    tw_stime xmit_time_saved;
    tw_stime recv_time_saved;
};

#endif /* end of include guard: LOGGP_H */

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
