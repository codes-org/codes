/*
 * Copyright (C) 2014 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
*/

#include "codes/lp-msg.h"
#include "ross.h"

void msg_set_header(int magic, int event_type, tw_lpid src, msg_header *h){
    h->magic = magic;
    h->event_type = event_type;
    h->src = src;
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
