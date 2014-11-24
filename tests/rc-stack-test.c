/*
 * Copyright (C) 2014 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#include <assert.h>
#include <ross.h>
#include "codes/rc-stack.h"

int main(int argc, char *argv[])
{
    /* mock up a dummy lp for testing */
    tw_lp lp;
    tw_kp kp;
    tw_pe pe;
    memset(&lp, 0, sizeof(lp));
    memset(&kp, 0, sizeof(kp));
    memset(&pe, 0, sizeof(pe));

    lp.pe = &pe;
    lp.kp = &kp;

    struct rc_stack *s;
    rc_stack_create(&s);
    assert(s != NULL);

    int *a = malloc(sizeof(*a));
    int *b = malloc(sizeof(*b));
    int *c = malloc(sizeof(*c));
    *a = 1;
    *b = 2;
    *c = 3;

#define PUSH_ALL() \
    do { \
        kp.last_time = 1.0; \
        rc_stack_push(&lp, a, s); \
        kp.last_time = 2.0; \
        rc_stack_push(&lp, b, s); \
        kp.last_time = 3.0; \
        rc_stack_push(&lp, c, s); \
    } while (0)

    PUSH_ALL();

    void *dat;
    assert(3 == rc_stack_count(s));
    dat = rc_stack_pop(s);
    assert(c == dat);
    dat = rc_stack_pop(s);
    assert(b == dat);
    dat = rc_stack_pop(s);
    assert(a == dat);
    assert(0 == rc_stack_count(s));

    PUSH_ALL();
    /* garbage collect the first two (NOT freeing the pointers first) */
    pe.GVT = 2.5;
    rc_stack_gc(&lp, 0, s);
    assert(1 == rc_stack_count(s));

    dat = rc_stack_pop(s);
    assert(c == dat);
    assert(0 == rc_stack_count(s));

    /* destroy everything */
    PUSH_ALL();
    rc_stack_destroy(1, s);

    return 0;
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
