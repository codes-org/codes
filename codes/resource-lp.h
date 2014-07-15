/*
 * Copyright (C) 2014 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
*/

/* this header defines the user-facing functionality needed to interact with a
 * resource LP. All of the LP-specific implementation details are in the
 * corresponding C file */

#ifndef RESOURCE_LP_H
#define RESOURCE_LP_H

#include "ross.h"
#include "codes/lp-msg.h"
#include "codes/resource.h"
#include <stdint.h>

#define RESOURCE_MAX_CALLBACK_PAYLOAD 64

#define RESOURCE_LP_NM "resource"

typedef struct {
    int ret;
    /* in the case of a reserve, need to return the token */
    resource_token_t tok;
} resource_callback;

void resource_lp_init();
void resource_lp_configure();

/* Wrappers for the underlying resource structure.
 * Implicitly maps the given LPID to it's group and repetition, then messages
 * the resource LP with the request.
 * The following functions expect the sending LP to put its magic and callback
 * event type into the header parameter (lpid not necessary, it's grabbed from
 * sender) */
void resource_lp_get(
        msg_header *header,
        uint64_t req, 
        int block_on_unavail,
        int msg_size, 
        int msg_header_offset,
        int msg_callback_offset,
        int msg_callback_misc_size,
        int msg_callback_misc_offset,
        void *msg_callback_misc_data,
        tw_lp *sender);
/* no callback for frees thus far */
void resource_lp_free(uint64_t req, tw_lp *sender);
void resource_lp_reserve(
        msg_header *header, 
        uint64_t req,
        int block_on_unavail,
        int msg_size,
        int msg_header_offset,
        int msg_callback_offset,
        int msg_callback_misc_size,
        int msg_callback_misc_offset,
        void *msg_callback_misc_data,
        tw_lp *sender);
void resource_lp_get_reserved(
        msg_header *header,
        uint64_t req,
        resource_token_t tok,
        int block_on_unavail,
        int msg_size, 
        int msg_header_offset,
        int msg_callback_offset,
        int msg_callback_misc_size,
        int msg_callback_misc_offset,
        void *msg_callback_misc_data,
        tw_lp *sender);
void resource_lp_free_reserved(
        uint64_t req,
        resource_token_t tok,
        tw_lp *sender);

/* rc functions - thankfully, they only use codes-local-latency, so no need 
 * to pass in any arguments */
void resource_lp_get_rc(tw_lp *sender);
void resource_lp_free_rc(tw_lp *sender);
void resource_lp_reserve_rc(tw_lp *sender);
void resource_lp_get_reserved_rc(tw_lp *sender);
void resource_lp_free_reserved_rc(tw_lp *sender);

#endif /* end of include guard: RESOURCE_LP_H */

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
