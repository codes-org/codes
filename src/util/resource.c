/*
 * Copyright (C) 2014 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
*/

#include "codes/resource.h"
#include <assert.h>
#include <string.h>

static uint64_t min_u64(uint64_t a, uint64_t b){
    return a < b ? a : b;
}

/* initialize with avail capacity, all unreserved */
void resource_init(uint64_t avail, resource *r){
    r->num_tokens = 0;
    r->max_all  = avail;
    r->avail[0] = avail;
    r->max[0] = avail;
    r->min_avail[0] = avail;
    for (int i = 1; i < MAX_RESERVE; i++){
        r->avail[i] = 0;
        r->max[i] = 0;
        r->min_avail[i] = 0;
    }
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
        r->min_avail[tok] = min_u64(r->avail[tok], r->min_avail[tok]);
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
        // TODO: check for invalid state? (more available than possible)
        return 0;
    }
}

/* Determine amount of resource units remaining
 * Returns 0 on success, 2 on invalid token */
int resource_get_avail(resource_token_t tok, uint64_t *avail, resource *r){
    if (tok > r->num_tokens){
        return 2;
    }
    else{
        return r->avail[tok];
    }
}

/* Determine amount of used resource units.
 * The number returned is based on the pool-specific maximums, for which
 * reserve calls can change */
int resource_get_used(resource_token_t tok, uint64_t *used, resource *r){
    if (tok > r->num_tokens){
        return 2;
    }
    else{
        return r->max[tok] - r->avail[tok];
    }
}

/* Get and restore minimum stat (support for RC). So that the resource
 * interface doesn't need to upgrade to a full LP, store stats in
 * caller-provided arguments. */
int resource_get_min_avail(resource_token_t tok, uint64_t *min_avail,
        resource *r){
    if (tok > r->num_tokens){
        return 2;
    }
    else{
        *min_avail = r->min_avail[tok];
        return 0;
    }
}
int resource_restore_min_avail(resource_token_t tok, uint64_t min_avail,
        resource *r){
    if (tok > r->num_tokens){
        return 2;
    }
    else{
        r->min_avail[tok] = min_avail;
        return 0;
    }
}

/* Reservation function, same return values as get. 
 * These functions expect exactly one caller per LP group as 
 * defined by the codes configuration 
 * TODO: "un-reserving" not yet supported */
int resource_reserve(uint64_t req, resource_token_t *tok, resource *r){
    if (r->num_tokens > MAX_RESERVE){
        return 2;
    }
    else if (req > r->avail[0]){
        return 1;
    }
    else{
        /* reserved tokens start from 1 */ 
        *tok = ++(r->num_tokens);
        r->avail[*tok] = req;
        r->max[*tok] = req;
        r->min_avail[*tok] = req;
        r->max[0] -= req;
        r->avail[0] -= req;
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
