/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */


#ifndef CODES_H
#define CODES_H

#include <ross.h>
#include <assert.h>

#if 0
#define codes_event_new tw_event_new
#else
static inline tw_event * codes_event_new(
    tw_lpid dest_gid, 
    tw_stime offset_ts, 
    tw_lp * sender)
{
    tw_stime abs_ts = offset_ts + tw_now(sender); 
    assert(abs_ts < g_tw_ts_end);
    //printf("codes_event_new() abs_ts: %.9f\n", abs_ts);
    return(tw_event_new(dest_gid, offset_ts, sender));
}
#endif

/* TODO: validate what value we should use here */
/* Modeled latency for communication between local software components and
 * communication between daemons and hardware devices.  Measured in
 * nanoseconds.
 */
#define CODES_MIN_LATENCY 0.5
#define CODES_MAX_LATENCY 1.0
#define CODES_LATENCY_RANGE \
    (CODES_MAX_LATENCY-CODES_MIN_LATENCY)
static inline tw_stime codes_local_latency(tw_lp *lp)
{
    int r = g_tw_nRNG_per_lp-1;
    tw_stime tmp;

    tmp = g_tw_lookahead + CODES_MIN_LATENCY +
        tw_rand_unif(&lp->rng[r]) * CODES_LATENCY_RANGE;

    if (g_tw_synchronization_protocol == CONSERVATIVE &&
            (tw_now(lp) + g_tw_lookahead) >= (tw_now(lp) + tmp))
        tw_error(TW_LOC,
                "codes_local_latency produced a precision loss "
                "sufficient to fail lookahead check (conservative mode) - "
                "increase CODES_MIN_LATENCY/CODES_MAX_LATENCY. "
                "Now:%0.5le, lookahead:%0.5le, return:%0.5le\n",
                tw_now(lp), g_tw_lookahead, tmp);

    return(tmp);
}

static inline void codes_local_latency_reverse(tw_lp *lp)
{
    int r = g_tw_nRNG_per_lp-1;
    tw_rand_reverse_unif(&lp->rng[r]);
    return;
}

#endif /* CODES_H */

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
