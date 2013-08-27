/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#include "util/codes-callbacks.h"

int codes_callback_create(uint64_t srclp, uint64_t event, uint64_t reqid, 
                codes_callback_t * cb)
{
    if(cb == NULL)
        return -1;

    cb->srclp = srclp;
    cb->event = event;
    cb->reqid = reqid;

    return 0;
}

int codes_callback_destroy(codes_callback_t * cb)
{
    if(cb == NULL)
        return -1;

    cb->srclp = 0;
    cb->event = 0;
    cb->reqid = 0;

    return 0;
}

int codes_callback_invoke(codes_callback_t * cb, tw_lp * lp)
{
    tw_event * e = NULL;

    if(cb == NULL)
        return -1;

    /* generate callback event */
    tw_event_new(cb->srclp, CODES_CALLBACK_TIME, lp);

    /* get the event msg */
    tw_event_data(e);

    /* send the event */
    tw_event_send(e);

    return 0;
}

int codes_callback_copy(codes_callback_t * dest, codes_callback_t * src)
{
    if(dest == NULL)
        return -1;

    if(src == NULL)
        return - 1;

    dest->srclp = src->srclp;
    dest->event = src->event;
    dest->reqid = src->reqid;

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
