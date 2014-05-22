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
#include "codes/quicklist.h"

/// scheduler-specific data structures (TODO: split specific schedulers into
/// their own files if we move beyond just these two)
/// NOTE: for now, scheduler data structures are the same - this may change in
/// later versions

typedef struct mn_sched {
    // method containing packet event to call
    struct model_net_method *method;
    struct qlist_head reqs; // of type mn_sched_qitem
    // this is an unfortunate result - we have to basically not free anything
    // in order to keep around the remote and local events
    // we desperately need GVT hooks to run our own garbage collection
    struct qlist_head free_reqs;
} mn_sched;

// at the moment, rr and fcfs only differ in how req queue is modified, queue
// items themselves are equivalent

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
static int  fcfs_next(tw_stime *poffset, void *sched, model_net_sched_rc *rc, tw_lp *lp);
static void fcfs_next_rc(void *sched, model_net_sched_rc *rc, tw_lp *lp);

static model_net_sched_interface fcfs_tab = 
{ &fcfs_init, &fcfs_destroy, &fcfs_add, &fcfs_add_rc, &fcfs_next, &fcfs_next_rc};

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
static int  rr_next(tw_stime *poffset, void *sched, model_net_sched_rc *rc, tw_lp *lp);
static void rr_next_rc (void *sched, model_net_sched_rc *rc, tw_lp *lp);

static model_net_sched_interface rr_tab = 
{ &rr_init, &rr_destroy, &rr_add, &rr_add_rc, &rr_next, &rr_next_rc};

/// general scheduler functions

void model_net_sched_init(
        enum sched_type type, 
        struct model_net_method *method,
        model_net_sched *sched){
    sched->type = type;
    switch (type){
        case MN_SCHED_FCFS:
            sched->impl = &fcfs_tab;
            break;
        case MN_SCHED_RR:
            sched->impl = &rr_tab;
            break;
        default:
            fprintf(stderr, "unknown scheduler type");
            abort();
    }
    sched->impl->init(method, &sched->dat);
}

int model_net_sched_next(
        tw_stime *poffset,
        model_net_sched *sched,
        model_net_sched_rc *sched_rc,
        tw_lp *lp){
    return sched->impl->next(poffset, sched->dat, sched_rc, lp);
}

void model_net_sched_next_rc(
        model_net_sched *sched,
        model_net_sched_rc *sched_rc,
        tw_lp *lp) {
    sched->impl->next_rc(sched->dat, sched_rc, lp);
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

/// specific scheduler implementations

void fcfs_init(struct model_net_method *method, void ** sched){
    *sched = malloc(sizeof(mn_sched));
    mn_sched *ss = *sched;
    ss->method = method;
    INIT_QLIST_HEAD(&ss->reqs);
    INIT_QLIST_HEAD(&ss->free_reqs);
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
    mn_sched_qitem *q = malloc(sizeof(mn_sched_qitem));
    q->req = *req;
    q->rem = req->is_pull ? PULL_MSG_SIZE : req->msg_size;
    if (remote_event_size > 0){
        q->remote_event = malloc(remote_event_size);
        memcpy(q->remote_event, remote_event, remote_event_size);
    }
    if (local_event_size > 0){
        q->local_event = malloc(local_event_size);
        memcpy(q->local_event, local_event, local_event_size);
    }
    mn_sched *s = sched;
    qlist_add_tail(&q->ql, &s->reqs);
}

void fcfs_add_rc(void *sched, model_net_sched_rc *rc, tw_lp *lp){
    mn_sched *s = sched;
    struct qlist_head *ent = qlist_pop_back(&s->reqs);
    assert(ent != NULL);
    mn_sched_qitem *q = qlist_entry(ent, mn_sched_qitem, ql);
    if (q->remote_event) free(q->remote_event);
    if (q->local_event)  free(q->local_event);
    free(q);
}

int fcfs_next(tw_stime *poffset, void *sched, model_net_sched_rc *rc, tw_lp *lp){
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

    // if last packet - remove from list, put into free list
    if (is_last_packet){
        qlist_pop(&s->reqs);
        qlist_add_tail(&q->ql, &s->free_reqs);
        rc->rtn = 1;
    }
    else{
        q->rem -= psize;
        rc->rtn = 0;
    }
    return rc->rtn;
}

void fcfs_next_rc(void *sched, model_net_sched_rc *rc, tw_lp *lp){
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
            qlist_add(qlist_pop_back(&s->free_reqs), &s->reqs);
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
    INIT_QLIST_HEAD(&ss->free_reqs);
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
    mn_sched_qitem *q = malloc(sizeof(mn_sched_qitem));
    q->req = *req;
    q->rem = req->is_pull ? PULL_MSG_SIZE : req->msg_size;
    if (remote_event_size > 0){
        q->remote_event = malloc(remote_event_size);
        memcpy(q->remote_event, remote_event, remote_event_size);
    }
    if (local_event_size > 0){
        q->local_event = malloc(local_event_size);
        memcpy(q->local_event, local_event, local_event_size);
    }
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

int rr_next(tw_stime *poffset, void *sched, model_net_sched_rc *rc, tw_lp *lp){
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

    // if last packet - remove from list, put into free list
    if (is_last_packet){
        qlist_add_tail(&q->ql, &s->free_reqs);
        rc->rtn = 1;
    }
    else{
        q->rem -= psize;
        qlist_add_tail(&q->ql, &s->reqs);
        rc->rtn = 0;
    }
    return rc->rtn;
}

void rr_next_rc (void *sched, model_net_sched_rc *rc, tw_lp *lp){
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
            // put back to *front* of list. We know it's the front because it was
            // in the front when it was deleted
            qlist_add(qlist_pop_back(&s->free_reqs), &s->reqs);
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
