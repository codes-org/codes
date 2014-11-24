/*
 * Copyright (C) 2014 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#ifndef RC_STACK_H
#define RC_STACK_H

#include <ross.h>

/* A simple stack data structure that is GVT-aware for cleanup purposes. It's
 * meant to use as an alternative to event-stuffing for allocating data that
 * would be too large to put into the event.
 *
 * It's currently overly simple and will not perform well for workloads that
 * hit the RC stack often. Just currently meant to act as a simple placeholder
 * until we have time to do a high-performance implementation.
 *
 * TODO:
 * - use dedicated memory (pass out mem for data, use singly-linked-list of
 *   large-ish chunks to prevent lots of malloc/free calls)
 * - provide better options for invoking garbage collection (enter collection
 *   loop if more than N entries present, every X events, etc.). Don't want to
 *   enter the loop every event or modify the list every time gvt changes.
 */

struct rc_stack;

void rc_stack_create(struct rc_stack **s);
/* setting free_data to non-zero will free the data associated with each entry
 */
void rc_stack_destroy(int free_data, struct rc_stack *s);

/* push data to the stack with time given by tw_now(lp) */
void rc_stack_push(
        tw_lp *lp,
        void *data,
        struct rc_stack *s);

/* pop data from the stack for rc (tw_error if stack empty) */
void * rc_stack_pop(struct rc_stack *s);

/* get the number of entries on the stack (mostly for debug) */
int rc_stack_count(struct rc_stack *s);

/* remove entries from the stack with generation time < GVT (lp->pe->GVT)
 * 
 * setting free_data to non-zero will free the associated data */
void rc_stack_gc(
        tw_lp *lp,
        int free_data,
        struct rc_stack *s);

#endif /* end of include guard: RC-STACK_H */

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
