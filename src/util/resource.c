/*
 * Copyright (C) 2014 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
*/

#include "codes/resource.h"
#include <assert.h>

/* initialize with avail capacity, all unreserved */
void resource_init(uint64_t avail, resource *r){
    r->avail = avail;
    r->num_tokens = 0;
}

/* Acquire req units of the resource from the general pool. 
 * Returns 0 on success, 1 on failure (not enough available). */
int resource_get(uint64_t req, resource *r){
    if (req > r->avail){
        return 1;
    }
    else{
        r->avail -= req;
        return 0;
    }
}

/* Release req units of the resource from the general pool. */
void resource_free(uint64_t req, resource *r){
    r->avail += req;
}

/* Reservation functions, same return value as get. 
 * These functions expect exactly one caller per LP group as 
 * defined by the codes configuration 
 * TODO: "un-reserving" not yet supported */
int resource_reserve(uint64_t req, resource_token_t *tok, resource *r){
    if (req > r->avail){
        return 1;
    }
    else{
        *tok = r->num_tokens++;
        r->reserved_avail[*tok] = req;
        r->avail -= req;
        return 0;
    }
}

/* Acquire req units of the resource from a reserved pool */ 
int reserved_get(uint64_t req, resource_token_t tok, resource *r){
    assert(tok < r->num_tokens);
    if (req > r->reserved_avail[tok]){
        return 1;
    }
    else{
        r->reserved_avail[tok] -= req;
        return 0;
    }
}

/* Release req units of the resource from the general pool. */
void reserved_free(uint64_t req, resource_token_t tok, resource *r){
    assert(tok < r->num_tokens);
    r->reserved_avail[tok] += req;
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
