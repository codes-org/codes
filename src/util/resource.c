/*
 * Copyright (C) 2014 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
*/

#include "codes/resource.h"
#include <assert.h>

/* initialize with avail capacity, all unreserved */
void resource_init(uint64_t avail, resource *r){
    r->avail[0] = avail;
    r->num_tokens = 0;
}

/* Acquire req units of the resource. 
 * Returns 0 on success, 1 on failure (not enough available), 2 on invalid
 * token. */
int resource_get(uint64_t req, resource_token_t tok, resource *r){
    if (tok > r->num_tokens){ 
        return 2;
    }
    else if (req > r->avail[tok]){
        return 1;
    }
    else{
        r->avail[tok] -= req;
        return 0;
    }
}

/* Release req units of the resource.
 * Returns 0 on success, 2 on invalid token */
int resource_free(uint64_t req, resource_token_t tok, resource *r){
    if (tok > r->num_tokens){ 
        return 2;
    }
    else{
        r->avail[tok] += req;
        return 0;
    }
}

/* Reservation functions, same return value as get. 
 * These functions expect exactly one caller per LP group as 
 * defined by the codes configuration 
 * TODO: "un-reserving" not yet supported */
int resource_reserve(uint64_t req, resource_token_t *tok, resource *r){
    if (req > r->avail[0]){
        return 1;
    }
    else{
        /* reserved tokens start from 1 */ 
        *tok = ++(r->num_tokens);
        r->avail[*tok] = req;
        return 0;
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
