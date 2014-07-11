/*
 * Copyright (C) 2014 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
*/

/* Implementation of a simple "resource" data structure for lps to get and
 * retrieve. Additionally allows 'reservations' of a resource for
 * later use, allowing claiming of the resource without requiring the LP to
 * reimplement resource functionality. General requests go through the
 * "general" pool (unreserved part of the resource and
 * reservation-specific requests go through their specific pools. */

#ifndef RESOURCE_H
#define RESOURCE_H

#include <stdint.h>


typedef struct resource_s resource;
typedef unsigned int resource_token_t;

/* initialize with avail capacity, all unreserved */
void resource_init(uint64_t avail, resource *r);

/* Acquire req units of the resource. 
 * Returns 0 on success, 1 on failure (not enough available), 2 on invalid
 * token. */
int resource_get(uint64_t req, resource_token_t tok, resource *r);

/* Release req units of the resource.
 * Returns 0 on success, 2 on invalid token */
int resource_free(uint64_t req, resource_token_t tok, resource *r);

/* Determine amount of resource units remaining
 * Returns 0 on success, 2 on invalid token */
int resource_get_avail(resource_token_t tok, uint64_t *avail, resource *r);

/* Determine amount of used resource units.
 * The number returned is based on the pool-specific maximums, for which
 * reserve calls can change */
int resource_get_used(resource_token_t tok, uint64_t *used, resource *r);

/* Get and restore minimum stat (support for RC). So that the resource
 * interface doesn't need to upgrade to a full LP, store stats in
 * caller-provided arguments. */
int resource_get_min_avail(resource_token_t tok, uint64_t *min_avail,
        resource *r);
int resource_restore_min_avail(resource_token_t tok, uint64_t min_avail,
        resource *r);

/* Reservation functions, same return value as get.
 * These functions expect exactly one caller per LP group as
 * defined by the codes configuration
 * TODO: "un-reserving" not yet supported */
int resource_reserve(uint64_t req, resource_token_t *tok, resource *r);

#define MAX_RESERVE 8
struct resource_s {
    // index 0 is the general pool, 1... are the reserved pools
    // current available 
    uint64_t avail[MAX_RESERVE+1];
    // maximums per pool (the max for the general pool is reduced on reserve!)
    uint64_t max[MAX_RESERVE+1];
    // statistics - minimum available at any time
    uint64_t min_avail[MAX_RESERVE+1];
    // global maximum
    uint64_t max_all;
    unsigned int num_tokens;
};


#endif /* end of include guard: RESOURCE_H */

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
