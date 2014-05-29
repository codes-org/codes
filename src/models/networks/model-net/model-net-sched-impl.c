/*
 * Copyright (C) 2014 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#include <stdlib.h>
#include <assert.h>

#include "model-net-sched-impl.h"
#include "codes/model-net-sched.h"
#include "codes/model-net-method.h"
#include "codes/quicklist.h"

/// scheduler-specific data structures 
/// NOTE: for now, scheduler data structures are the same - this may change in
/// later versions

typedef struct mn_sched {
    // method containing packet event to call
    struct model_net_method *method;
    struct qlist_head reqs; // of type mn_sched_qitem
} mn_sched;

typedef struct mn_sched_qitem {
    model_net_request req;
    // remaining bytes to send
    uint64_t rem;
    // pointers to event structures 
    // sizes are given in the request struct
    void * remote_event;
    void * local_event;
    struct qlist_head ql;
} mn_sched_qitem;

/// scheduler-specific function decls and function table

/// FCFS
// void used to avoid ptr-to-ptr conv warnings
static void fcfs_init (struct model_net_method *method, void ** sched);
static void fcfs_destroy (void *sched);
static void fcfs_add (
        model_net_request *req, 
        int remote_event_size,
        void * remote_event,
        int local_event_size,
        void * local_event,
        void *sched, 
        model_net_sched_rc *rc, 
        tw_lp *lp);
static void fcfs_add_rc(void *sched, model_net_sched_rc *rc, tw_lp *lp);
static int  fcfs_next(tw_stime *poffset, void *sched, void *rc_event_save, 
        model_net_sched_rc *rc, tw_lp *lp);
static void fcfs_next_rc(void *sched, void *rc_event_save,
        model_net_sched_rc *rc, tw_lp *lp);

static void rr_init (struct model_net_method *method, void ** sched);
static void rr_destroy (void *sched);
static void rr_add (
        model_net_request *req,
        int remote_event_size,
        void * remote_event,
        int local_event_size,
        void * local_event,
        void *sched,
        model_net_sched_rc *rc,
        tw_lp *lp);
static void rr_add_rc(void *sched, model_net_sched_rc *rc, tw_lp *lp);
static int  rr_next(tw_stime *poffset, void *sched, void *rc_event_save,
        model_net_sched_rc *rc, tw_lp *lp);
static void rr_next_rc (void *sched, void *rc_event_save,
        model_net_sched_rc *rc, tw_lp *lp);

/// function tables (names defined by X macro in model-net-sched.h)
static model_net_sched_interface fcfs_tab = 
{ &fcfs_init, &fcfs_destroy, &fcfs_add, &fcfs_add_rc, &fcfs_next, &fcfs_next_rc};
static model_net_sched_interface rr_tab = 
{ &rr_init, &rr_destroy, &rr_add, &rr_add_rc, &rr_next, &rr_next_rc};

#define X(a,b,c) c,
model_net_sched_interface * sched_interfaces[] = {
    SCHEDULER_TYPES
};
#undef X

/// FCFS implementation 

void fcfs_init(struct model_net_method *method, void ** sched){
    *sched = malloc(sizeof(mn_sched));
    mn_sched *ss = *sched;
    ss->method = method;
    INIT_QLIST_HEAD(&ss->reqs);
}

void fcfs_destroy(void *sched){
    free(sched);
}

void fcfs_add (
        model_net_request *req, 
        int remote_event_size,
        void * remote_event,
        int local_event_size,
        void * local_event,
        void *sched, 
        model_net_sched_rc *rc, 
        tw_lp *lp){
    // NOTE: in optimistic mode, we currently do not have a good way to
    // reliably free and re-initialize the q item and the local/remote events
    // when processing next/next_rc events. Hence, the memory leaks. Later on
    // we'll figure out a better way to handle this.
    mn_sched_qitem *q = malloc(sizeof(mn_sched_qitem));
    assert(q);
    q->req = *req;
    q->rem = req->is_pull ? PULL_MSG_SIZE : req->msg_size;
    if (remote_event_size > 0){
        q->remote_event = malloc(remote_event_size);
        memcpy(q->remote_event, remote_event, remote_event_size);
    }
    else { q->remote_event = NULL; }
    if (local_event_size > 0){
        q->local_event = malloc(local_event_size);
        memcpy(q->local_event, local_event, local_event_size);
    }
    else { q->local_event = NULL; }
    mn_sched *s = sched;
    qlist_add_tail(&q->ql, &s->reqs);
}

void fcfs_add_rc(void *sched, model_net_sched_rc *rc, tw_lp *lp){
    mn_sched *s = sched;
    struct qlist_head *ent = qlist_pop_back(&s->reqs);
    assert(ent != NULL);
    mn_sched_qitem *q = qlist_entry(ent, mn_sched_qitem, ql);
    // free'ing NULLs is a no-op 
    free(q->remote_event);
    free(q->local_event);
    free(q);
}

int fcfs_next(tw_stime *poffset, void *sched, void *rc_event_save,
        model_net_sched_rc *rc, tw_lp *lp){
    mn_sched *s = sched;
    struct qlist_head *ent = s->reqs.next;
    if (ent == &s->reqs){
        rc->rtn = -1;
        return -1;
    }
    mn_sched_qitem *q = qlist_entry(ent, mn_sched_qitem, ql);
    
    // issue the next packet
    int is_last_packet;
    uint64_t psize;
    if (q->req.packet_size >= q->rem) {
        psize = q->rem;
        is_last_packet = 1;
    }
    else{
        psize = q->req.packet_size;
        is_last_packet = 0;
    }

    *poffset = s->method->model_net_method_packet_event(q->req.category,
            q->req.final_dest_lp, psize, q->req.is_pull, q->req.msg_size, 0.0,
            q->req.remote_event_size, q->remote_event, q->req.self_event_size,
            q->local_event, q->req.src_lp, lp, is_last_packet);

    // if last packet - remove from list, free, save for rc
    if (is_last_packet){
        qlist_pop(&s->reqs);
        rc->req = q->req;
        void *e_dat = rc_event_save;
        if (q->req.remote_event_size > 0){
            memcpy(e_dat, q->remote_event, q->req.remote_event_size);
            e_dat = (char*) e_dat + q->req.remote_event_size;
            free(q->remote_event);
        }
        if (q->req.self_event_size > 0){
            memcpy(e_dat, q->local_event, q->req.self_event_size);
            free(q->local_event);
        }
        free(q);
        rc->rtn = 1;
    }
    else{
        q->rem -= psize;
        rc->rtn = 0;
    }
    return rc->rtn;
}

void fcfs_next_rc(void *sched, void *rc_event_save, model_net_sched_rc *rc,
        tw_lp *lp){
    mn_sched *s = sched;
    if (rc->rtn == -1){
        // no op
    }
    else{
        s->method->model_net_method_packet_event_rc(lp);
        if (rc->rtn == 0){
            // just get the front and increment rem
            mn_sched_qitem *q = qlist_entry(s->reqs.next, mn_sched_qitem, ql);
            // just increment rem
            q->rem += q->req.packet_size;
        }
        else if (rc->rtn == 1){
            // re-create the q item
            mn_sched_qitem *q = malloc(sizeof(mn_sched_qitem));
            assert(q);
            q->req = rc->req;
            q->rem = q->req.msg_size % q->req.packet_size;
            if (q->rem == 0){ // processed exactly a packet's worth of data
                q->rem = q->req.packet_size;
            }
            void * e_dat = rc_event_save;
            if (q->req.remote_event_size > 0){
                q->remote_event = malloc(q->req.remote_event_size);
                memcpy(q->remote_event, e_dat, q->req.remote_event_size);
                e_dat = (char*) e_dat + q->req.remote_event_size;
            }
            else { q->remote_event = NULL; }
            if (q->req.self_event_size > 0) {
                q->local_event = malloc(q->req.self_event_size);
                memcpy(q->local_event, e_dat, q->req.self_event_size);
            }
            else { q->local_event = NULL; }
            // add back to front of list
            qlist_add(&q->ql, &s->reqs);
        }
        else {
            assert(0);
        }
    }
}

void rr_init (struct model_net_method *method, void ** sched){
    *sched = malloc(sizeof(mn_sched));
    mn_sched *ss = *sched;
    ss->method = method;
    INIT_QLIST_HEAD(&ss->reqs);
}

void rr_destroy (void *sched){
    free(sched);
}

void rr_add (
        model_net_request *req,
        int remote_event_size,
        void * remote_event,
        int local_event_size,
        void * local_event,
        void *sched,
        model_net_sched_rc *rc,
        tw_lp *lp){
    // NOTE: in optimistic mode, we currently do not have a good way to
    // reliably free and re-initialize the q item and the local/remote events
    // when processing next/next_rc events. Hence, the memory leaks. Later on
    // we'll figure out a better way to handle this.
    mn_sched_qitem *q = malloc(sizeof(mn_sched_qitem));
    q->req = *req;
    q->rem = req->is_pull ? PULL_MSG_SIZE : req->msg_size;
    if (remote_event_size > 0){
        q->remote_event = malloc(remote_event_size);
        memcpy(q->remote_event, remote_event, remote_event_size);
    }
    else { q->remote_event = NULL; }
    if (local_event_size > 0){
        q->local_event = malloc(local_event_size);
        memcpy(q->local_event, local_event, local_event_size);
    }
    else{ q->local_event = NULL; }
    mn_sched *s = sched;
    qlist_add_tail(&q->ql, &s->reqs);
}
void rr_add_rc(void *sched, model_net_sched_rc *rc, tw_lp *lp){
    mn_sched *s = sched;
    struct qlist_head *ent = qlist_pop_back(&s->reqs);
    assert(ent != NULL);
    mn_sched_qitem *q = qlist_entry(ent, mn_sched_qitem, ql);
    if (q->remote_event) free(q->remote_event);
    if (q->local_event)  free(q->local_event);
    free(q);
}

int rr_next(tw_stime *poffset, void *sched, void *rc_event_save,
        model_net_sched_rc *rc, tw_lp *lp){
    mn_sched *s = sched;
    struct qlist_head *ent = qlist_pop(&s->reqs);
    if (ent == NULL){
        rc->rtn = -1;
        return -1;
    }
    mn_sched_qitem *q = qlist_entry(ent, mn_sched_qitem, ql);

    // issue the next packet
    int is_last_packet;
    uint64_t psize;
    if (q->req.packet_size >= q->rem) {
        psize = q->rem;
        is_last_packet = 1;
    }
    else{
        psize = q->req.packet_size;
        is_last_packet = 0;
    }

    *poffset = s->method->model_net_method_packet_event(q->req.category,
            q->req.final_dest_lp, psize, q->req.is_pull, q->req.msg_size, 0.0,
            q->req.remote_event_size, q->remote_event, q->req.self_event_size,
            q->local_event, q->req.src_lp, lp, is_last_packet);

    // if last packet - remove from list, free, save for rc
    if (is_last_packet){
        rc->req = q->req;
        void *e_dat = rc_event_save;
        if (q->req.remote_event_size > 0){
            memcpy(e_dat, q->remote_event, q->req.remote_event_size);
            e_dat = (char*) e_dat + q->req.remote_event_size;
            free(q->remote_event);
        }
        if (q->req.self_event_size > 0){
            memcpy(e_dat, q->local_event, q->req.self_event_size);
            free(q->local_event);
        }
        free(q);
        rc->rtn = 1;
    }
    else{
        q->rem -= psize;
        qlist_add_tail(&q->ql, &s->reqs);
        rc->rtn = 0;
    }
    return rc->rtn;
}

void rr_next_rc (void *sched, void *rc_event_save, model_net_sched_rc *rc, tw_lp *lp){
    mn_sched *s = sched;
    if (rc->rtn == -1){
        // no op
    }
    else {
        s->method->model_net_method_packet_event_rc(lp);
        if (rc->rtn == 0){
            // increment rem and put item back to front of list
            struct qlist_head *ent = qlist_pop_back(&s->reqs);
            qlist_add(ent, &s->reqs);
            mn_sched_qitem *q = qlist_entry(ent, mn_sched_qitem, ql);
            q->rem += q->req.packet_size;
        }
        else if (rc->rtn == 1){
            // re-create the q item
            mn_sched_qitem *q = malloc(sizeof(mn_sched_qitem));
            assert(q);
            q->req = rc->req;
            q->rem = q->req.msg_size % q->req.packet_size;
            if (q->rem == 0){ // processed exactly a packet's worth of data
                q->rem = q->req.packet_size;
            }
            void * e_dat = rc_event_save;
            if (q->req.remote_event_size > 0){
                q->remote_event = malloc(q->req.remote_event_size);
                memcpy(q->remote_event, e_dat, q->req.remote_event_size);
                e_dat = (char*) e_dat + q->req.remote_event_size;
            }
            else { q->remote_event = NULL; }
            if (q->req.self_event_size > 0) {
                q->local_event = malloc(q->req.self_event_size);
                memcpy(q->local_event, e_dat, q->req.self_event_size);
            }
            else { q->local_event = NULL; }
            // add back to front of list
            qlist_add(&q->ql, &s->reqs);
        }
        else {
            assert(0);
        }
    }
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
