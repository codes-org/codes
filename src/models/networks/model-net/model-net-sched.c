/*
 * Copyright (C) 2014 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

/* A scheduler interface for use by model-net. */

#include <assert.h>
#include <ross.h>

#include "model-net-sched-impl.h"
#include "codes/model-net-sched.h"
#include "codes/model-net-lp.h"
#include "codes/quicklist.h"

enum sched_type mn_sched_type = -1;

#define X(a,b,c) b,
char * sched_names [] = {
    SCHEDULER_TYPES
};
#undef X

/// general scheduler functions

void model_net_sched_init(
        enum sched_type type, 
        struct model_net_method *method,
        model_net_sched *sched){
    if (type >= MAX_SCHEDS){
        fprintf(stderr, "unknown scheduler type");
        abort();
    }
    else{
        sched->impl = sched_interfaces[type];
    }
    sched->type = type;
    sched->impl->init(method, &sched->dat);
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
        int remote_event_size,
        void * remote_event,
        int local_event_size,
        void * local_event,
        model_net_sched *sched,
        model_net_sched_rc *sched_rc,
        tw_lp *lp){
    sched->impl->add(req, remote_event_size, remote_event, local_event_size,
            local_event, sched->dat, sched_rc, lp);
}

void model_net_sched_add_rc(
        model_net_sched *sched,
        model_net_sched_rc *sched_rc,
        tw_lp *lp){
    sched->impl->add_rc(sched->dat, sched_rc, lp);
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
