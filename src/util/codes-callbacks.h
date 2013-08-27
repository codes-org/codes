/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#ifndef CODES_CALLBACKS_H
#define CODES_CALLBACKS_H

#include <ross.h>
#include <stdint.h>
#include <stdlib.h>

#define CODES_CALLBACK_TIME 1

typedef struct
{
    uint64_t srclp;
    uint64_t event;
    uint64_t reqid;
} codes_callback_t;

/* callback function prototypes */
int codes_callback_create(uint64_t srclp, uint64_t event, uint64_t reqid,
        codes_callback_t * cb);
int codes_callback_destroy(codes_callback_t * cb);
int codes_callback_invoke(codes_callback_t * cb, tw_lp * lp);
int codes_callback_copy(codes_callback_t * dest, codes_callback_t * src);

#endif

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
