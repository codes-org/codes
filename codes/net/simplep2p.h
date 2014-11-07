/*
 * Copyright (C) 2014 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#ifndef SIMPLEP2P_H
#define SIMPLEP2P_H

typedef struct sp_message sp_message;

enum sp_event_type 
{
    SP_MSG_READY = 1,  /* sender has transmitted msg to receiver */
    SP_MSG_START,      /* initiate a transmission */
};

struct sp_message
{
    int magic; /* magic number */
    enum sp_event_type event_type;
    tw_lpid src_gid; /* who transmitted this msg? */
    tw_lpid final_dest_gid; /* who is eventually targetted with this msg? */
    /* relative ID of the sending simplep2p message (for latency/bandwidth lookup) */
    int src_mn_rel_id;
    int dest_mn_rel_id; /* included to make rc easier */
    uint64_t net_msg_size_bytes;     /* size of modeled network message */
    int event_size_bytes;     /* size of simulator event message that will be tunnelled to destination */
    int local_event_size_bytes;     /* size of simulator event message that delivered locally upon local completion */
    char category[CATEGORY_NAME_MAX]; /* category for communication */
    
    int is_pull;
    uint64_t pull_size; 

    /* for reverse computation */
    // TODO: clean up
    tw_stime send_next_idle_saved;
    tw_stime recv_next_idle_saved;
    tw_stime send_time_saved;
    tw_stime recv_time_saved;
    tw_stime send_next_idle_all_saved;
    tw_stime send_prev_idle_all_saved;
    tw_stime recv_next_idle_all_saved;
    tw_stime recv_prev_idle_all_saved;
};

#endif /* end of include guard: SIMPLEP2P_H */

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */

