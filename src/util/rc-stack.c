/*
 * Copyright (C) 2014 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#include <assert.h>
#include <ross.h>
#include "codes/rc-stack.h"
#include "codes/quicklist.h"

typedef struct rc_entry_s {
    tw_stime time;
    void * data;
    struct qlist_head ql;
} rc_entry;

struct rc_stack {
    int count;
    struct qlist_head head;
};

void rc_stack_create(struct rc_stack **s){
    struct rc_stack *ss = (struct rc_stack*)malloc(sizeof(*ss));
    if (ss) {
        INIT_QLIST_HEAD(&ss->head);
        ss->count = 0;
    }
    *s = ss;
}

void rc_stack_destroy(int free_data, struct rc_stack *s) {
#define FREE_ENTRY(_e, _free_data)\
    do { \
        if (_e != NULL){\
            rc_entry *r = qlist_entry(_e, rc_entry, ql);\
            if (_free_data) free(r->data);\
            free(r);\
        } \
    } while(0)

    struct qlist_head *ent, *ent_prev = NULL;
    qlist_for_each(ent, &s->head) {
        FREE_ENTRY(ent_prev, free_data);
        ent_prev = ent;
    }
    FREE_ENTRY(ent_prev, free_data);
    free(s);

#undef FREE_ENTRY
}

void rc_stack_push(
        tw_lp *lp,
        void * data,
        struct rc_stack *s){
    rc_entry * ent = (rc_entry*)malloc(sizeof(*ent));
    assert(ent);
    ent->time = tw_now(lp);
    ent->data = data;
    qlist_add_tail(&ent->ql, &s->head);
    s->count++;
}

void* rc_stack_pop(struct rc_stack *s){
    struct qlist_head *item = qlist_pop_back(&s->head);
    if (item == NULL)
        tw_error(TW_LOC,
                "could not pop item from rc stack (stack likely empty)\n");
    s->count--;
    return qlist_entry(item, rc_entry, ql)->data;
}

int rc_stack_count(struct rc_stack *s) { return s->count; }

void rc_stack_gc(tw_lp *lp, int free_data, struct rc_stack *s) {
    struct qlist_head *ent = s->head.next;
    while (ent != &s->head) {
        rc_entry *r = qlist_entry(ent, rc_entry, ql);
        if (r->time < lp->pe->GVT){
            qlist_del(ent);
            if (free_data) free(r->data);
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
