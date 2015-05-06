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

#define MN_SCHED_DEBUG_VERBOSE 0

#if MN_SCHED_DEBUG_VERBOSE
#define dprintf(_fmt, ...) printf(_fmt, ##__VA_ARGS__)
#else
#define dprintf(_fmt, ...)
#endif

/// scheduler-specific data structures 

typedef struct mn_sched_qitem {
    model_net_request req;
    mn_sched_params sched_params;
    // remaining bytes to send
    uint64_t rem;
    tw_stime entry_time;
    // pointers to event structures 
    // sizes are given in the request struct
    void * remote_event;
    void * local_event;
    struct qlist_head ql;
} mn_sched_qitem;

// fcfs and round-robin each use a single queue
typedef struct mn_sched_queue {
    // method containing packet event to call
    const struct model_net_method *method;
    int is_recv_queue;
    int queue_len;
    struct qlist_head reqs; // of type mn_sched_qitem
} mn_sched_queue;

// priority scheduler consists of a bunch of rr/fcfs queues
typedef struct mn_sched_prio {
    mn_prio_params params;
    const model_net_sched_interface *sub_sched_iface;
    mn_sched_queue ** sub_scheds; // one for each params.num_prios
} mn_sched_prio;

/// scheduler-specific function decls and tables

/// FCFS
// void used to avoid ptr-to-ptr conv warnings
static void fcfs_init (
        const struct model_net_method     * method, 
        const model_net_sched_cfg_params  * params,
        int                                 is_recv_queue,
        void                             ** sched);
static void fcfs_destroy (void *sched);
static void fcfs_add (
        model_net_request     * req,
        const mn_sched_params * sched_params,
        int                     remote_event_size,
        void                  * remote_event,
        int                     local_event_size,
        void                  * local_event,
        void                  * sched,
        model_net_sched_rc    * rc,
        tw_lp                 * lp);
static void fcfs_add_rc(void *sched, model_net_sched_rc *rc, tw_lp *lp);
static int  fcfs_next(
        tw_stime              * poffset,
        void                  * sched,
        void                  * rc_event_save,
        model_net_sched_rc    * rc,
        tw_lp                 * lp);
static void fcfs_next_rc(
        void               * sched,
        void               * rc_event_save,
        model_net_sched_rc * rc,
        tw_lp              * lp);

// ROUND-ROBIN
static void rr_init (
        const struct model_net_method     * method, 
        const model_net_sched_cfg_params  * params,
        int                                 is_recv_queue,
        void                             ** sched);
static void rr_destroy (void *sched);
static void rr_add (
        model_net_request     * req,
        const mn_sched_params * sched_params,
        int                     remote_event_size,
        void                  * remote_event,
        int                     local_event_size,
        void                  * local_event,
        void                  * sched,
        model_net_sched_rc    * rc,
        tw_lp                 * lp);
static void rr_add_rc(void *sched, model_net_sched_rc *rc, tw_lp *lp);
static int  rr_next(
        tw_stime              * poffset,
        void                  * sched,
        void                  * rc_event_save,
        model_net_sched_rc    * rc,
        tw_lp                 * lp);
static void rr_next_rc (
        void               * sched,
        void               * rc_event_save,
        model_net_sched_rc * rc,
        tw_lp              * lp);
static void prio_init (
        const struct model_net_method     * method, 
        const model_net_sched_cfg_params  * params,
        int                                 is_recv_queue,
        void                             ** sched);
static void prio_destroy (void *sched);
static void prio_add (
        model_net_request     * req,
        const mn_sched_params * sched_params,
        int                     remote_event_size,
        void                  * remote_event,
        int                     local_event_size,
        void                  * local_event,
        void                  * sched,
        model_net_sched_rc    * rc,
        tw_lp                 * lp);
static void prio_add_rc(void *sched, model_net_sched_rc *rc, tw_lp *lp);
static int  prio_next(
        tw_stime              * poffset,
        void                  * sched,
        void                  * rc_event_save,
        model_net_sched_rc    * rc,
        tw_lp                 * lp);
static void prio_next_rc (
        void               * sched,
        void               * rc_event_save,
        model_net_sched_rc * rc,
        tw_lp              * lp);

/// function tables (names defined by X macro in model-net-sched.h)
static const model_net_sched_interface fcfs_tab = 
{ &fcfs_init, &fcfs_destroy, &fcfs_add, &fcfs_add_rc, &fcfs_next, &fcfs_next_rc};
static const model_net_sched_interface rr_tab = 
{ &rr_init, &rr_destroy, &rr_add, &rr_add_rc, &rr_next, &rr_next_rc};
static const model_net_sched_interface prio_tab =
{ &prio_init, &prio_destroy, &prio_add, &prio_add_rc, &prio_next, &prio_next_rc};

#define X(a,b,c) c,
const model_net_sched_interface * sched_interfaces[] = {
    SCHEDULER_TYPES
};
#undef X

/// FCFS implementation 

void fcfs_init(
        const struct model_net_method     * method, 
        const model_net_sched_cfg_params  * params,
        int                                 is_recv_queue,
        void                             ** sched){
    *sched = malloc(sizeof(mn_sched_queue));
    mn_sched_queue *ss = *sched;
    ss->method = method;
    ss->is_recv_queue = is_recv_queue;
    ss->queue_len = 0;
    INIT_QLIST_HEAD(&ss->reqs);
}

void fcfs_destroy(void *sched){
    free(sched);
}

void fcfs_add (
        model_net_request     * req,
        const mn_sched_params * sched_params,
        int                     remote_event_size,
        void                  * remote_event,
        int                     local_event_size,
        void                  * local_event,
        void                  * sched,
        model_net_sched_rc    * rc,
        tw_lp                 * lp){
    mn_sched_qitem *q = malloc(sizeof(mn_sched_qitem));
    q->entry_time = tw_now(lp);
    q->req = *req;
    q->sched_params = *sched_params;
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
    mn_sched_queue *s = sched;
    s->queue_len++;
    qlist_add_tail(&q->ql, &s->reqs);
    dprintf("%lu (mn):    adding %srequest from %lu to %lu, size %lu, at %lf\n",
            lp->gid, req->is_pull ? "pull " : "", req->src_lp,
            req->final_dest_lp, req->msg_size, tw_now(lp));
}

void fcfs_add_rc(void *sched, model_net_sched_rc *rc, tw_lp *lp){
    mn_sched_queue *s = sched;
    s->queue_len--;
    struct qlist_head *ent = qlist_pop_back(&s->reqs);
    assert(ent != NULL);
    mn_sched_qitem *q = qlist_entry(ent, mn_sched_qitem, ql);
    dprintf("%lu (mn): rc adding request from %lu to %lu\n", lp->gid,
            q->req.src_lp, q->req.final_dest_lp);
    // free'ing NULLs is a no-op 
    free(q->remote_event);
    free(q->local_event);
    free(q);
}

int fcfs_next(
        tw_stime              * poffset,
        void                  * sched,
        void                  * rc_event_save,
        model_net_sched_rc    * rc,
        tw_lp                 * lp){
    mn_sched_queue *s = sched;
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

    if (s->is_recv_queue){
        dprintf("%lu (mn):    receiving message of size %lu (of %lu) "
                "from %lu to %lu at %1.5e (last:%d)\n",
                lp->gid, psize, q->rem, q->req.src_lp, q->req.final_dest_lp,
                tw_now(lp), is_last_packet);
        *poffset = s->method->model_net_method_recv_msg_event(q->req.category,
                q->req.final_dest_lp, psize, q->req.is_pull, q->req.msg_size,
                0.0, q->req.remote_event_size, q->remote_event, q->req.src_lp,
                lp);
    }
    else{
        dprintf("%lu (mn):    issuing packet of size %lu (of %lu) "
                "from %lu to %lu at %1.5e (last:%d)\n",
                lp->gid, psize, q->rem, q->req.src_lp, q->req.final_dest_lp,
                tw_now(lp), is_last_packet);
        *poffset = s->method->model_net_method_packet_event(q->req.category,
                q->req.final_dest_lp, psize, q->req.is_pull, q->req.msg_size,
                0.0, &q->sched_params, q->req.remote_event_size, q->remote_event,
                q->req.self_event_size, q->local_event, q->req.src_lp, lp,
                is_last_packet);
    }

    // if last packet - remove from list, free, save for rc
    if (is_last_packet){
        dprintf("last %spkt: %lu (%lu) to %lu, size %lu at %1.5e (pull:%d)\n",
                s->is_recv_queue ? "recv " : "send ",
                lp->gid, q->req.src_lp, q->req.final_dest_lp,
                q->req.is_pull ? PULL_MSG_SIZE : q->req.msg_size, tw_now(lp),
                q->req.is_pull);
        qlist_pop(&s->reqs);
        s->queue_len--;
        rc->req = q->req;
        rc->sched_params = q->sched_params;
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

void fcfs_next_rc(
        void               * sched,
        void               * rc_event_save,
        model_net_sched_rc * rc,
        tw_lp              * lp){
    mn_sched_queue *s = sched;
    if (rc->rtn == -1){
        // no op
    }
    else{
        if (s->is_recv_queue){
            dprintf("%lu (mn): rc receiving message\n", lp->gid);
            s->method->model_net_method_recv_msg_event_rc(lp);
        }
        else {
            dprintf("%lu (mn): rc issuing packet\n", lp->gid);
            s->method->model_net_method_packet_event_rc(lp);
        }
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
            q->sched_params = rc->sched_params;
            q->rem = (q->req.is_pull ? PULL_MSG_SIZE : q->req.msg_size) % 
                q->req.packet_size;
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
            s->queue_len++;
        }
        else {
            assert(0);
        }
    }
}

void rr_init (
        const struct model_net_method     * method, 
        const model_net_sched_cfg_params  * params,
        int                                 is_recv_queue,
        void                             ** sched){
    // same underlying representation
    fcfs_init(method, params, is_recv_queue, sched);
}

void rr_destroy (void *sched){
    // same underlying representation
    fcfs_destroy(sched);
}

void rr_add (
        model_net_request     * req,
        const mn_sched_params * sched_params,
        int                     remote_event_size,
        void                  * remote_event,
        int                     local_event_size,
        void                  * local_event,
        void                  * sched,
        model_net_sched_rc    * rc,
        tw_lp                 * lp){
    fcfs_add(req, sched_params, remote_event_size, remote_event,
            local_event_size, local_event, sched, rc, lp);
}

void rr_add_rc(void *sched, model_net_sched_rc *rc, tw_lp *lp){
    fcfs_add_rc(sched, rc, lp);
}

int rr_next(
        tw_stime              * poffset,
        void                  * sched,
        void                  * rc_event_save,
        model_net_sched_rc    * rc,
        tw_lp                 * lp){
    int ret = fcfs_next(poffset, sched, rc_event_save, rc, lp);
    // if error in fcfs or the request was finished & removed, then nothing to
    // do here
    if (ret == -1 || ret == 1)
        return ret;
    // otherwise request was successful, still in the queue
    else {
        mn_sched_queue *s = sched;
        qlist_add_tail(qlist_pop(&s->reqs), &s->reqs);
        return ret;
    }
}

void rr_next_rc (
        void               * sched,
        void               * rc_event_save,
        model_net_sched_rc * rc,
        tw_lp              * lp){
    // only time we need to do something apart from fcfs is on a successful
    // rr_next that didn't remove the item from the queue
    if (rc->rtn == 0){
        mn_sched_queue *s = sched;
        qlist_add(qlist_pop_back(&s->reqs), &s->reqs);
    }
    fcfs_next_rc(sched, rc_event_save, rc, lp);
}

void prio_init (
        const struct model_net_method     * method, 
        const model_net_sched_cfg_params  * params,
        int                                 is_recv_queue,
        void                             ** sched){
    *sched = malloc(sizeof(mn_sched_prio));
    mn_sched_prio *ss = *sched;
    ss->params = params->u.prio;
    ss->sub_scheds = malloc(ss->params.num_prios*sizeof(mn_sched_queue*));
    ss->sub_sched_iface = sched_interfaces[ss->params.sub_stype];
    for (int i = 0; i < ss->params.num_prios; i++){
        ss->sub_sched_iface->init(method, params, is_recv_queue,
                (void**)&ss->sub_scheds[i]);
    }
}

void prio_destroy (void *sched){
    mn_sched_prio *ss = sched;
    for (int i = 0; i < ss->params.num_prios; i++){
        ss->sub_sched_iface->destroy(ss->sub_scheds[i]);
        free(ss->sub_scheds);
        free(ss);
    }
}

void prio_add (
        model_net_request     * req,
        const mn_sched_params * sched_params,
        int                     remote_event_size,
        void                  * remote_event,
        int                     local_event_size,
        void                  * local_event,
        void                  * sched,
        model_net_sched_rc    * rc,
        tw_lp                 * lp){
    // sched_msg_params is simply an int
    mn_sched_prio *ss = sched;
    int prio = sched_params->prio;
    if (prio == -1){
        // default prio - lowest possible 
        prio = ss->params.num_prios-1;
    }
    else if (prio >= ss->params.num_prios){
        tw_error(TW_LOC, "sched for lp %lu: invalid prio (%d vs [%d,%d))",
                lp->gid, prio, 0, ss->params.num_prios);
    }
    dprintf("%lu (mn):    adding with prio %d\n", lp->gid, prio);
    ss->sub_sched_iface->add(req, sched_params, remote_event_size,
            remote_event, local_event_size, local_event, ss->sub_scheds[prio],
            rc, lp);
    rc->prio = prio;
}

void prio_add_rc(void * sched, model_net_sched_rc *rc, tw_lp *lp){
    // just call the sub scheduler's add_rc
    mn_sched_prio *ss = sched;
    dprintf("%lu (mn): rc adding with prio %d\n", lp->gid, rc->prio);
    ss->sub_sched_iface->add_rc(ss->sub_scheds[rc->prio], rc, lp);
}

int prio_next(
        tw_stime              * poffset,
        void                  * sched,
        void                  * rc_event_save,
        model_net_sched_rc    * rc,
        tw_lp                 * lp){
    // check each priority, first one that's non-empty gets the next
    mn_sched_prio *ss = sched;
    for (int i = 0; i < ss->params.num_prios; i++){
        // TODO: this works for now while the other schedulers have the same
        // internal representation
        if (!qlist_empty(&ss->sub_scheds[i]->reqs)){
            rc->prio = i;
            return ss->sub_sched_iface->next(
                    poffset, ss->sub_scheds[i], rc_event_save, rc, lp);
        }
    }
    rc->prio = -1;
    return -1; // all sub schedulers had no work 
}

void prio_next_rc (
        void               * sched,
        void               * rc_event_save,
        model_net_sched_rc * rc,
        tw_lp              * lp){
    if (rc->prio != -1){
        // we called a next somewhere
        mn_sched_prio *ss = sched;
        ss->sub_sched_iface->next_rc(ss->sub_scheds[rc->prio], rc_event_save,
                rc, lp);
    }
    // else, no-op
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
