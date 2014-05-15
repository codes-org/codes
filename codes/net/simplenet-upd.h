/*
 * Copyright (C) 2014 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#ifndef SIMPLENET_UPD_H
#define SIMPLENET_UPD_H

typedef struct sn_message sn_message;

/* types of events that will constitute triton requests */
enum sn_event_type 
{
    SN_MSG_READY = 1,  /* sender has transmitted msg to receiver */
    SN_MSG_START,      /* initiate a transmission */
};

struct sn_message
{
    int magic; /* magic number */
    enum sn_event_type event_type;
    tw_lpid src_gid; /* who transmitted this msg? */
    tw_lpid final_dest_gid; /* who is eventually targetted with this msg? */
    uint64_t net_msg_size_bytes;     /* size of modeled network message */
    int event_size_bytes;     /* size of simulator event message that will be tunnelled to destination */
    int local_event_size_bytes;     /* size of simulator event message that delivered locally upon local completion */
    char category[CATEGORY_NAME_MAX]; /* category for communication */
    int is_pull; /* this message represents a pull request from the destination LP to the source */
    uint64_t pull_size; /* data size to pull from dest LP */

    /* for reverse computation */
    tw_stime net_send_next_idle_saved;
    tw_stime net_recv_next_idle_saved;
    tw_stime send_time_saved;
    tw_stime recv_time_saved;
};

#endif /* end of include guard: SIMPLENET_UPD_H */
/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
