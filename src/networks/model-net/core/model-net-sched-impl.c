/*
 * Copyright (C) 2014 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#include <stdlib.h>
#include <assert.h>

#include <codes/model-net-sched-impl.h>
#include <codes/model-net-sched.h>
#include <codes/model-net-method.h>
#include <codes/quicklist.h>
#include <codes/codes.h>

#define MN_SCHED_DEBUG_VERBOSE 0

#define dprintf(_fmt, ...) \
    do { \
        if (MN_SCHED_DEBUG_VERBOSE) printf(_fmt, ##__VA_ARGS__); \
    } while(0)

/// scheduler-specific data structures

typedef struct mn_sched_qitem {
    model_net_request req;
    mn_sched_params sched_params;
    // remaining bytes to send
    uint64_t rem;
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
        const model_net_request * req,
        const mn_sched_params   * sched_params,
        int                       remote_event_size,
        void                    * remote_event,
        int                       local_event_size,
        void                    * local_event,
        void                    * sched,
        model_net_sched_rc      * rc,
        tw_lp                   * lp);
static void fcfs_add_rc(void *sched, const model_net_sched_rc *rc, tw_lp *lp);
static int  fcfs_next(
        tw_stime              * poffset,
        void                  * sched,
        void                  * rc_event_save,
        model_net_sched_rc    * rc,
        tw_lp                 * lp);
static void fcfs_next_rc(
        void                     * sched,
        const void               * rc_event_save,
        const model_net_sched_rc * rc,
        tw_lp                    * lp);
static void save_state_fcfs_state(mn_sched_queue * into, mn_sched_queue const * from);
static void clean_state_fcfs_state(mn_sched_queue * into);
static bool check_fcfs_state(mn_sched_queue *before, mn_sched_queue *after);
static void print_fcfs_state(FILE * out, mn_sched_queue *sched);

// ROUND-ROBIN
static void rr_init (
        const struct model_net_method     * method,
        const model_net_sched_cfg_params  * params,
        int                                 is_recv_queue,
        void                             ** sched);
static void rr_destroy (void *sched);
static void rr_add (
        const model_net_request * req,
        const mn_sched_params   * sched_params,
        int                       remote_event_size,
        void                    * remote_event,
        int                       local_event_size,
        void                    * local_event,
        void                    * sched,
        model_net_sched_rc      * rc,
        tw_lp                   * lp);
static void rr_add_rc(void *sched, const model_net_sched_rc *rc, tw_lp *lp);
static int  rr_next(
        tw_stime              * poffset,
        void                  * sched,
        void                  * rc_event_save,
        model_net_sched_rc    * rc,
        tw_lp                 * lp);
static void rr_next_rc (
        void                     * sched,
        const void               * rc_event_save,
        const model_net_sched_rc * rc,
        tw_lp                    * lp);
static void prio_init (
        const struct model_net_method     * method,
        const model_net_sched_cfg_params  * params,
        int                                 is_recv_queue,
        void                             ** sched);
static void prio_destroy (void *sched);
static void prio_add (
        const model_net_request * req,
        const mn_sched_params   * sched_params,
        int                       remote_event_size,
        void                    * remote_event,
        int                       local_event_size,
        void                    * local_event,
        void                    * sched,
        model_net_sched_rc      * rc,
        tw_lp                   * lp);
static void prio_add_rc(void *sched, const model_net_sched_rc *rc, tw_lp *lp);
static int  prio_next(
        tw_stime              * poffset,
        void                  * sched,
        void                  * rc_event_save,
        model_net_sched_rc    * rc,
        tw_lp                 * lp);
static void prio_next_rc (
        void                     * sched,
        const void               * rc_event_save,
        const model_net_sched_rc * rc,
        tw_lp                    * lp);

/// function tables (names defined by X macro in model-net-sched.h)
static const model_net_sched_interface fcfs_tab =
{ &fcfs_init, &fcfs_destroy, &fcfs_add, &fcfs_add_rc, &fcfs_next, &fcfs_next_rc};
static const model_net_sched_interface rr_tab =
{ &rr_init, &rr_destroy, &rr_add, &rr_add_rc, &rr_next, &rr_next_rc};
static const model_net_sched_interface prio_tab =
{ &prio_init, &prio_destroy, &prio_add, &prio_add_rc, &prio_next, &prio_next_rc};

static const crv_checkpointer fcfs_chptr = {
    NULL,
    sizeof(mn_sched_queue),
    (save_checkpoint_state_f) save_state_fcfs_state,
    (clean_checkpoint_state_f) clean_state_fcfs_state,
    (check_states_f) check_fcfs_state,
    (print_lpstate_f) print_fcfs_state,
    (print_checkpoint_state_f) print_fcfs_state,
    NULL,
};

#define X(a,b,c,d) c,
const model_net_sched_interface * sched_interfaces[] = {
    SCHEDULER_TYPES
};
#undef X

#define X(a,b,c,d) d,
const crv_checkpointer * sched_checkpointers[] = {
    SCHEDULER_TYPES
};
#undef X

/// FCFS implementation

void fcfs_init(
        const struct model_net_method     * method,
        const model_net_sched_cfg_params  * params,
        int                                 is_recv_queue,
        void                             ** sched){
    (void)params; // unused for fcfs
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
        const model_net_request * req,
        const mn_sched_params   * sched_params,
        int                       remote_event_size,
        void                    * remote_event,
        int                       local_event_size,
        void                    * local_event,
        void                    * sched,
        model_net_sched_rc      * rc,
        tw_lp                   * lp){
    (void)rc; // unneeded for fcfs
    mn_sched_qitem *q = malloc(sizeof(mn_sched_qitem));
    q->req = *req;
    q->sched_params = *sched_params;
    q->rem = req->msg_size;
    assert(req->remote_event_size == remote_event_size);
    if (remote_event_size > 0){
        q->remote_event = malloc(remote_event_size);
        memcpy(q->remote_event, remote_event, remote_event_size);
    }
    else { q->remote_event = NULL; }
    assert(req->self_event_size == local_event_size);
    if (local_event_size > 0){
        q->local_event = malloc(local_event_size);
        memcpy(q->local_event, local_event, local_event_size);
    }
    else { q->local_event = NULL; }
    mn_sched_queue *s = sched;
    s->queue_len++;
    qlist_add_tail(&q->ql, &s->reqs);
    dprintf("%llu (mn):    adding %srequest from %llu to %llu, size %llu, at %lf\n",
            LLU(lp->gid), req->is_pull ? "pull " : "", LLU(req->src_lp),
            LLU(req->final_dest_lp), LLU(req->msg_size), tw_now(lp));
}

void fcfs_add_rc(void *sched, const model_net_sched_rc *rc, tw_lp *lp){
    (void)rc;
    mn_sched_queue *s = sched;
    s->queue_len--;
    struct qlist_head *ent = qlist_pop_back(&s->reqs);
    assert(ent != NULL);
    mn_sched_qitem *q = qlist_entry(ent, mn_sched_qitem, ql);
    dprintf("%llu (mn): rc adding request from %llu to %llu\n", LLU(lp->gid),
            LLU(q->req.src_lp), LLU(q->req.final_dest_lp));
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

    bool const is_there_another_pckt_in_queue = !is_last_packet || s->queue_len > 1;

    if (s->is_recv_queue){
        dprintf("%llu (mn):    receiving message of size %llu (of %llu) "
                "from %llu to %llu at %1.5e (last:%d)\n",
                LLU(lp->gid), LLU(psize), LLU(q->rem), LLU(q->req.src_lp),
                LLU(q->req.final_dest_lp), tw_now(lp), is_last_packet);
        // note: we overloaded on the dest_mn_lp field - it's the dest of the
        // soruce in the case of a pull
        *poffset = s->method->model_net_method_recv_msg_event(q->req.category,
                q->req.final_dest_lp, q->req.dest_mn_lp, psize,
                q->req.is_pull, q->req.pull_size, 0.0, q->req.remote_event_size,
                q->remote_event, q->req.src_lp, lp);
    }
    else{
        dprintf("%llu (mn):    issuing packet of size %llu (of %llu) "
                "from %llu to %llu at %1.5e (last:%d)\n",
                LLU(lp->gid), LLU(psize), LLU(q->rem), LLU(q->req.src_lp),
                LLU(q->req.final_dest_lp), tw_now(lp), is_last_packet);
        *poffset = s->method->model_net_method_packet_event(&q->req,
                q->req.msg_size - q->rem, psize, 0.0, &q->sched_params,
                q->remote_event, q->local_event, lp, is_last_packet,
                is_there_another_pckt_in_queue);
    }

    // if last packet - remove from list, free, save for rc
    if (is_last_packet){
        dprintf("last %spkt: %llu (%llu) to %llu, size %llu at %1.5e (pull:%d)\n",
                s->is_recv_queue ? "recv " : "send ",
                LLU(lp->gid), LLU(q->req.src_lp), LLU(q->req.final_dest_lp),
                LLU(q->req.is_pull ? q->req.pull_size : q->req.msg_size), tw_now(lp),
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
        void                     * sched,
        const void               * rc_event_save,
        const model_net_sched_rc * rc,
        tw_lp                    * lp){
    mn_sched_queue *s = sched;
    if (rc->rtn == -1){
        // no op
    }
    else{
        if (s->is_recv_queue){
            dprintf("%llu (mn): rc receiving message\n", LLU(lp->gid));
            s->method->model_net_method_recv_msg_event_rc(lp);
        }
        else {
            dprintf("%llu (mn): rc issuing packet\n", LLU(lp->gid));
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
            q->rem = q->req.msg_size % q->req.packet_size;
            // processed exactly a packet's worth of data
            if (q->rem == 0 && q->req.msg_size != 0){
                q->rem = q->req.packet_size;
            }
            const void * e_dat = rc_event_save;
            if (q->req.remote_event_size > 0){
                q->remote_event = malloc(q->req.remote_event_size);
                memcpy(q->remote_event, e_dat, q->req.remote_event_size);
                e_dat = (const char*) e_dat + q->req.remote_event_size;
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

static void save_mn_sched_qitem(mn_sched_qitem * into, mn_sched_qitem const * from) {
    into->req = from->req;
    into->sched_params = from->sched_params;
    into->rem = from->rem;
    if (from->remote_event != NULL) {
        assert(from->req.remote_event_size > 0);
        into->remote_event = malloc(from->req.remote_event_size);
        memcpy(into->remote_event, from->remote_event, from->req.remote_event_size);
    }
    if (from->local_event != NULL) {
        assert(from->req.self_event_size > 0);
        into->local_event = malloc(from->req.self_event_size);
        memcpy(into->local_event, from->local_event, from->req.self_event_size);
    }
}

static void save_state_fcfs_state(mn_sched_queue * into, mn_sched_queue const * from) {
    into->method = from->method;
    into->is_recv_queue = from->is_recv_queue;
    into->queue_len = from->queue_len;
    INIT_QLIST_HEAD(&into->reqs);

    mn_sched_qitem * sched_qitem = NULL;
    qlist_for_each_entry(sched_qitem, &from->reqs, ql) {
        mn_sched_qitem * new_sched_qitem = malloc(sizeof(mn_sched_qitem));
        save_mn_sched_qitem(new_sched_qitem, sched_qitem);
        qlist_add_tail(&new_sched_qitem->ql, &into->reqs);
    }
}

static void clean_mn_sched_qitem(mn_sched_qitem * into) {
    if (into->remote_event != NULL) {
        free(into->remote_event);
    }
    if (into->local_event != NULL) {
        free(into->local_event);
    }
}

static void clean_state_fcfs_state(mn_sched_queue * into) {
    mn_sched_qitem * sched_qitem = NULL;
    mn_sched_qitem * _ = NULL;
    qlist_for_each_entry_safe(sched_qitem, _, &into->reqs, ql) {
        clean_mn_sched_qitem(sched_qitem);
        qlist_del(&sched_qitem->ql);
        free(sched_qitem);
    }
}

static bool check_mn_sched_qitem(mn_sched_qitem * before, mn_sched_qitem * after) {
    bool is_same = true;

    is_same &= check_model_net_request(&before->req, &after->req);
    is_same &= before->sched_params.prio == after->sched_params.prio;
    is_same &= before->rem == after->rem;
    is_same &= !memcmp(before->remote_event, after->remote_event, before->req.remote_event_size);
    is_same &= !memcmp(before->local_event, after->local_event, before->req.self_event_size);
    return is_same;
}

static bool check_fcfs_state(mn_sched_queue * before, mn_sched_queue * after) {
    bool is_same = true;

    is_same &= before->is_recv_queue == after->is_recv_queue;
    is_same &= before->queue_len == after->queue_len;

    if (qlist_count(&before->reqs) != qlist_count(&before->reqs)) {
        return false;
    }

    is_same &= are_qlist_equal(&before->reqs, &after->reqs, QLIST_OFFSET(mn_sched_qitem, ql), (bool (*) (void *, void *)) check_mn_sched_qitem);

    return is_same;
}

static void print_mn_sched_qitem(FILE * out, mn_sched_qitem * item) {
    fprintf(out, "     mn_sched_qitem\n");
    fprintf(out, "       | .req\n");
    print_model_net_request(out, "       |     |.", &item->req);
    fprintf(out, "       | sched_params.prio = %d\n", item->sched_params.prio);
    fprintf(out, "       |               rem = %lu\n", item->rem);
    fprintf(out, "       |      remote_event = %p (contents below)\n", item->remote_event);
    tw_fprint_binary_array(out, item->remote_event, item->req.remote_event_size);
    fprintf(out, "       |       local_event = %p (contents below)\n", item->local_event);
    tw_fprint_binary_array(out, item->local_event, item->req.self_event_size);
}

static void print_fcfs_state(FILE * out, mn_sched_queue *sched) {
    fprintf(out, "FCFS:\n");
    fprintf(out, "   |        .method = %p\n", sched->method);
    fprintf(out, "   | .is_recv_queue = %d\n", sched->is_recv_queue);
    fprintf(out, "   |     .queue_len = %d\n", sched->queue_len);
    fprintf(out, "   |      .reqs[%d] = {\n", qlist_count(&sched->reqs));
    mn_sched_qitem * sched_qitem = NULL;
    qlist_for_each_entry(sched_qitem, &sched->reqs, ql) {
         print_mn_sched_qitem(out, sched_qitem);
    }
    fprintf(out, "}\n");
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
        const model_net_request * req,
        const mn_sched_params   * sched_params,
        int                       remote_event_size,
        void                    * remote_event,
        int                       local_event_size,
        void                    * local_event,
        void                    * sched,
        model_net_sched_rc      * rc,
        tw_lp                   * lp){
    fcfs_add(req, sched_params, remote_event_size, remote_event,
            local_event_size, local_event, sched, rc, lp);
}

void rr_add_rc(void *sched, const model_net_sched_rc *rc, tw_lp *lp){
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
        void                     * sched,
        const void               * rc_event_save,
        const model_net_sched_rc * rc,
        tw_lp                    * lp){
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
        const model_net_request * req,
        const mn_sched_params   * sched_params,
        int                       remote_event_size,
        void                    * remote_event,
        int                       local_event_size,
        void                    * local_event,
        void                    * sched,
        model_net_sched_rc      * rc,
        tw_lp                   * lp){
    // sched_msg_params is simply an int
    mn_sched_prio *ss = sched;
    int prio = sched_params->prio;
    if (prio == -1){
        // default prio - lowest possible
        prio = ss->params.num_prios-1;
    }
    else if (prio >= ss->params.num_prios){
        tw_error(TW_LOC, "sched for lp %llu: invalid prio (%d vs [%d,%d))",
                LLU(lp->gid), prio, 0, ss->params.num_prios);
    }
    dprintf("%llu (mn):    adding with prio %d\n", LLU(lp->gid), prio);
    ss->sub_sched_iface->add(req, sched_params, remote_event_size,
            remote_event, local_event_size, local_event, ss->sub_scheds[prio],
            rc, lp);
    rc->prio = prio;
}

void prio_add_rc(void * sched, const model_net_sched_rc *rc, tw_lp *lp){
    // just call the sub scheduler's add_rc
    mn_sched_prio *ss = sched;
    dprintf("%llu (mn): rc adding with prio %d\n", LLU(lp->gid), rc->prio);
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
        void                     * sched,
        const void               * rc_event_save,
        const model_net_sched_rc * rc,
        tw_lp                    * lp){
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
