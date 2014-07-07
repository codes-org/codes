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
 * Modified Jul 7: We want to make sure that the event time stamp generated
is always greater than the default g_tw_lookahead value. Multiplying by 1.1
ensures that if tw_rand_exponential generates a zero time-stamped event, we
still have a timestamp that is greater than g_tw_lookahead. 
 */
#define CODES_MEAN_LOCAL_LATENCY 0.01
static inline tw_stime codes_local_latency(tw_lp *lp)
{
    tw_stime tmp;

    tmp = (1.1 * g_tw_lookahead) + tw_rand_exponential(lp->rng, CODES_MEAN_LOCAL_LATENCY);

    return(tmp);
}

static inline void codes_local_latency_reverse(tw_lp *lp)
{
    tw_rand_reverse_unif(lp->rng);
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
