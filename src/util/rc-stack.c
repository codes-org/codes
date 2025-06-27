/*
 * Copyright (C) 2014 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#include <assert.h>
#include <ross.h>
#include "codes/rc-stack.h"
#include "codes/quicklist.h"

enum rc_stack_mode {
    RC_NONOPT, // not in optimistic mode
    RC_OPT, // optimistic mode
    RC_OPT_DBG, // optimistic *debug* mode (requires special handling)
    RC_SEQ_RV_DBG, // sequential rollback chek, a *debug* mode that requires special handling
};

typedef struct rc_entry_s {
#ifdef USE_RAND_TIEBREAKER
    tw_event_sig e_sig; // ROSS 2D event timestamp (.recv_ts & .event_tiebreaker)
#else
    tw_stime time;
#endif
    void * data;
    void (*free_fn)(void*);
    struct qlist_head ql;
} rc_entry;

struct rc_stack {
    int count;
    enum rc_stack_mode mode;
    struct qlist_head head;
};

void rc_stack_create(struct rc_stack **s){
    struct rc_stack *ss = (struct rc_stack*)malloc(sizeof(*ss));
    if (ss) {
        INIT_QLIST_HEAD(&ss->head);
        ss->count = 0;
    }
    switch (g_tw_synchronization_protocol) {
        case OPTIMISTIC:
        case OPTIMISTIC_REALTIME:
            ss->mode = RC_OPT;
            break;
        case SEQUENTIAL_ROLLBACK_CHECK:
            ss->mode = RC_SEQ_RV_DBG;
            break;
        case OPTIMISTIC_DEBUG:
            ss->mode = RC_OPT_DBG;
            break;
        default:
            ss->mode = RC_NONOPT;
    }
    *s = ss;
}

void rc_stack_destroy(struct rc_stack *s) {
    rc_stack_gc(NULL, s);
    free(s);
}

void rc_stack_push(
        tw_lp const *lp,
        void * data,
        void (*free_fn)(void*),
        struct rc_stack *s){
    if (s->mode != RC_NONOPT || free_fn == NULL) {
        rc_entry * ent = (rc_entry*)malloc(sizeof(*ent));
        assert(ent);
#ifdef USE_RAND_TIEBREAKER
        ent->e_sig = tw_now_sig(lp);
#else
        ent->time = tw_now(lp);
#endif
        ent->data = data;
        ent->free_fn = free_fn;
        qlist_add_tail(&ent->ql, &s->head);
        s->count++;
    }
    else
        free_fn(data);
}

void* rc_stack_pop(struct rc_stack *s){
    void * ret = NULL;
    rc_entry *ent = NULL;
    struct qlist_head *item = qlist_pop_back(&s->head);
    if (item == NULL)
        tw_error(TW_LOC,
                "could not pop item from rc stack (stack likely empty)\n");
    s->count--;
    ent = qlist_entry(item, rc_entry, ql);
    ret = ent->data;
    free(ent);
    return ret;
}

int rc_stack_count(struct rc_stack const *s) { return s->count; }

void rc_stack_gc(tw_lp const *lp, struct rc_stack *s) {
    // in optimistic debug mode, we can't gc anything, because we'll be rolling
    // back to the beginning
    if (s->mode == RC_OPT_DBG)
        return;

    // rollback until only one event is left
    if (s->mode == RC_SEQ_RV_DBG) {
        struct qlist_head *ent = s->head.next;
        while (ent->next != &s->head) {
            rc_entry *r = qlist_entry(ent, rc_entry, ql);
            qlist_del(ent);
            if (r->free_fn) r->free_fn(r->data);
            free(r);
            s->count--;
            ent = s->head.next;
        }
        return;
    }

    // Removing all stored rollback events from stack
    struct qlist_head *ent = s->head.next;
    while (ent != &s->head) {
        rc_entry *r = qlist_entry(ent, rc_entry, ql);
#ifdef USE_RAND_TIEBREAKER
        if (lp == NULL || tw_event_sig_compare_ptr(&r->e_sig, &lp->pe->GVT_sig) < 0) {
#else
        if (lp == NULL || r->time < lp->pe->GVT){
#endif
            qlist_del(ent);
            if (r->free_fn) r->free_fn(r->data);
            free(r);
            s->count--;
            ent = s->head.next;
        }
        else
            break;
    }
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
