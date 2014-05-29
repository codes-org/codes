/*
 * Copyright (C) 2014 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

/* A scheduler interface for use by model-net. */

#ifndef MODEL_NET_SCHED_H
#define MODEL_NET_SCHED_H

#include <ross.h>
#include "model-net.h"
#include "model-net-method.h"

/// types of schedulers
/// format: enum type, config string, function pointer names
/// fcfs-full eschews packetization
#define SCHEDULER_TYPES \
    X(MN_SCHED_FCFS,      "fcfs",        &fcfs_tab) \
    X(MN_SCHED_FCFS_FULL, "fcfs-full",   &fcfs_tab) \
    X(MN_SCHED_RR,        "round-robin", &rr_tab) \
    X(MAX_SCHEDS,         NULL,          NULL)

#define X(a,b,c) a,
enum sched_type {
    SCHEDULER_TYPES
};
#undef X

extern char * sched_names[];

/// global for scheduler
/// TODO: move away from using the global for when we have multiple networks
extern enum sched_type mn_sched_type;

/// scheduler decls

typedef struct model_net_sched_s model_net_sched;
typedef struct model_net_sched_rc_s model_net_sched_rc;

/// interface to be implemented by schedulers
/// see corresponding general functions
typedef struct model_net_sched_interface {
    void (*init)(struct model_net_method *method, void ** sched);
    void (*destroy)(void * sched);
    void (*add)(
            model_net_request *req,
            int remote_event_size,
            void * remote_event,
            int local_event_size,
            void * local_event,
            void *sched,
            model_net_sched_rc *rc,
            tw_lp *lp);
    void (*add_rc)(void *sched, model_net_sched_rc *rc, tw_lp *lp);
    int  (*next)(
            tw_stime *poffset,
            void *sched,
            // NOTE: copy here when deleting remote/local events for rc
            void *rc_event_save,
            model_net_sched_rc *rc,
            tw_lp *lp);
    void (*next_rc)(void * sched, void *rc_event_save, 
            model_net_sched_rc *rc, tw_lp *lp);
} model_net_sched_interface;

/// overall scheduler struct - type puns the actual data structure

struct model_net_sched_s {
    enum sched_type type;
    void * dat;
    model_net_sched_interface *impl;
};

/// reverse computation structure - this is expected to be held by upper-level
/// model-net LPs and passed to the scheduler functions to allow proper rc
/// NOTE: since modelnet LPs will be stashing this in their event structs, 
/// need to provide full definition in header
struct model_net_sched_rc_s {
    // NOTE: sched implementations may need different types, but for now they
    // are equivalent 
    model_net_request req; // request gets deleted...
    int rtn; // return code from a sched_next 
};

void model_net_sched_init(
        enum sched_type type, 
        struct model_net_method *method,
        model_net_sched *sched);

/// schedules the next chunk, storing any info needed for rc in sched_rc
/// packet issue time is placed in poffset to be able to separate packet calls
/// between multiple scheduler events
/// returns: 
/// * 0 on success,
/// * 1 on success and the corresponding request is finished. In this case,
///   out_req is set to the underlying request
/// * -1 when there is nothing to be scheduled
int model_net_sched_next(
        tw_stime *poffset,
        model_net_sched *sched,
        void *rc_event_save,
        model_net_sched_rc *sched_rc,
        tw_lp *lp);

void model_net_sched_next_rc(
        model_net_sched *sched,
        void *rc_event_save,
        model_net_sched_rc *sched_rc,
        tw_lp *lp);

/// enter a new request into the scheduler, storing any info needed for rc in
/// sched_rc
void model_net_sched_add(
        model_net_request *req,
        int remote_event_size,
        void * remote_event,
        int local_event_size,
        void * local_event,
        model_net_sched *sched,
        model_net_sched_rc *sched_rc,
        tw_lp *lp);

void model_net_sched_add_rc(
        model_net_sched *sched,
        model_net_sched_rc *sched_rc,
        tw_lp *lp);

#endif /* end of include guard: MODEL_NET_SCHED_H */

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
