/*
 * Copyright (C) 2014 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

/* A scheduler interface for use by model-net. */

#include <assert.h>
#include <ross.h>

#include "codes/model-net-sched.h"
#include "codes/model-net-lp.h"
#include "model-net-sched-impl.h"
#include "codes/quicklist.h"

#define X(a,b,c) b,
char * sched_names [] = {
    SCHEDULER_TYPES
};
#undef X

/// general scheduler functions

void model_net_sched_init(
        const model_net_sched_cfg_params * params,
        int is_recv_queue,
        struct model_net_method *method,
        model_net_sched *sched){
    if (params->type >= MAX_SCHEDS){
        fprintf(stderr, "unknown scheduler type");
        abort();
    }
    else{
        sched->impl = sched_interfaces[params->type];
    }
    sched->type = params->type;
    sched->impl->init(method, params, is_recv_queue, &sched->dat);
}

int model_net_sched_next(
        tw_stime *poffset,
        model_net_sched *sched,
        void *rc_event_save,
        model_net_sched_rc *sched_rc,
        tw_lp *lp){
    return sched->impl->next(poffset, sched->dat, rc_event_save, sched_rc, lp);
}

void model_net_sched_next_rc(
        model_net_sched *sched,
        void *rc_event_save,
        model_net_sched_rc *sched_rc,
        tw_lp *lp) {
    sched->impl->next_rc(sched->dat, rc_event_save, sched_rc, lp);
}

void model_net_sched_add(
        model_net_request *req,
        const mn_sched_params * sched_params,
        int remote_event_size,
        void * remote_event,
        int local_event_size,
        void * local_event,
        model_net_sched *sched,
        model_net_sched_rc *sched_rc,
        tw_lp *lp){
    sched->impl->add(req, sched_params, remote_event_size, remote_event,
            local_event_size, local_event, sched->dat, sched_rc, lp);
}

void model_net_sched_add_rc(
        model_net_sched *sched,
        model_net_sched_rc *sched_rc,
        tw_lp *lp){
    sched->impl->add_rc(sched->dat, sched_rc, lp);
}

void model_net_sched_set_default_params(mn_sched_params *sched_params){
    sched_params->prio = -1;
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
