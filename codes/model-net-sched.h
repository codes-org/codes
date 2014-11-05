/*
 * Copyright (C) 2014 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

/* A scheduler interface for use by model-net models. */

#ifndef MODEL_NET_SCHED_H
#define MODEL_NET_SCHED_H

#include <ross.h>
#include "model-net.h"

// forward decl of mn_sched_params since we currently have a circular include
// (method needs sched def, sched needs method def)
typedef struct mn_sched_params_s mn_sched_params;

#include "model-net-method.h"

/// types of schedulers
/// format: enum type, config string, function pointer names
/// fcfs-full eschews packetization
#define SCHEDULER_TYPES \
    X(MN_SCHED_FCFS,      "fcfs",        &fcfs_tab) \
    X(MN_SCHED_FCFS_FULL, "fcfs-full",   &fcfs_tab) \
    X(MN_SCHED_RR,        "round-robin", &rr_tab) \
    X(MN_SCHED_PRIO,      "priority",    &prio_tab) \
    X(MAX_SCHEDS,         NULL,          NULL)

#define X(a,b,c) a,
enum sched_type {
    SCHEDULER_TYPES
};
#undef X

/// scheduler decls

typedef struct model_net_sched_s model_net_sched;
typedef struct model_net_sched_rc_s model_net_sched_rc;

// priority scheduler configurtion parameters
typedef struct mn_prio_params_s {
    int num_prios; // number of priorities
    // sub-scheduler to use. can be any but prio
    enum sched_type sub_stype;
} mn_prio_params;

// TODO: other scheduler config params

// initialization parameter set
typedef struct model_net_sched_cfg_params_s {
    enum sched_type type;
    union {
        mn_prio_params prio;
    } u;
} model_net_sched_cfg_params;

typedef struct mn_sched_cfg_params {
    mn_prio_params prio;
} mn_sched_cfg_params;

/// message-specific parameters
enum sched_msg_param_type {
    MN_SCHED_PARAM_PRIO, // currently, only the priority scheduler has params
    MAX_SCHED_MSG_PARAM_TYPES
};

// scheduler-specific parameter definitions must go here
struct mn_sched_params_s {
    int prio; // MN_SCHED_PARAM_PRIO (currently the only one)
} ;

/// interface to be implemented by schedulers
/// see corresponding general functions
typedef struct model_net_sched_interface {
    // initialize the scheduler
    // params - scheduler specific params (currently only prio q uses)
    void (*init)(
            const struct model_net_method     * method, 
            const model_net_sched_cfg_params  * params,
            int                                 is_recv_queue,
            void                             ** sched);
    // finalize the scheduler
    void (*destroy)(void * sched);
    // add a new request to the scheduler
    // sched_params - per-message parameters distinct to each scheduler:
    //                prio (currently the only user): int priority
    //              - NULL arguments should be treated as "use default value" 
    void (*add)(
            model_net_request     * req,
            const mn_sched_params * sched_params,
            int                     remote_event_size,
            void                  * remote_event,
            int                     local_event_size,
            void                  * local_event,
            void                  * sched,
            model_net_sched_rc    * rc,
            tw_lp                 * lp);
    // reverse the previous request addition
    void (*add_rc)(void *sched, model_net_sched_rc *rc, tw_lp *lp);
    // schedule the next packet for processing by the model
    int  (*next)(
            tw_stime              * poffset,
            void                  * sched,
            // NOTE: copy here when deleting remote/local events for rc
            void                  * rc_event_save,
            model_net_sched_rc    * rc,
            tw_lp                 * lp);
    // reverse schedule the previous packet
    void (*next_rc)(
            void               * sched,
            void               * rc_event_save,
            model_net_sched_rc * rc,
            tw_lp              * lp);
} model_net_sched_interface;

/// overall scheduler struct - type puns the actual data structure

struct model_net_sched_s {
    enum sched_type type;
    // data for the underlying scheduler implementation (see
    // model-net-sched-impl*)
    void * dat;
    const model_net_sched_interface * impl;
};

/// scheduler-specific structures go here

/// reverse computation structure - this is expected to be held by upper-level
/// model-net LPs and passed to the scheduler functions to allow proper rc
/// NOTE: since modelnet LPs will be stashing this in their event structs, 
/// need to provide full definition in header
struct model_net_sched_rc_s {
    // NOTE: sched implementations may need different types, but for now they
    // are equivalent 
    model_net_request req; // request gets deleted...
    mn_sched_params sched_params; // along with msg params
    int rtn; // return code from a sched_next 
    int prio; // prio when doing priority queue events
};

// initialize the scheduler
// - params is created by the configuration routine and can be different from
//   type to type. Currently only priority scheduler uses it
void model_net_sched_init(
        const model_net_sched_cfg_params * params,
        int is_recv_queue,
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
/// sched_msg_params is scheduler-specific parameters (currently only used by
/// prio scheduler)
void model_net_sched_add(
        model_net_request *req,
        const mn_sched_params * sched_params,
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

// set default parameters for messages that don't specify any
void model_net_sched_set_default_params(mn_sched_params *sched_params);

extern char * sched_names[];

#endif /* end of include guard: MODEL_NET_SCHED_H */

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
