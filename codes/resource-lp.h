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

#ifdef __cplusplus
extern "C" {
#endif

#include <ross.h>
#include <stdint.h>

#include "lp-msg.h"
#include "resource.h"
#include "codes-callback.h"
#include "codes-mapping-context.h"

#define RESOURCE_LP_NM "resource"

typedef struct {
    int ret;
    /* in the case of a reserve, need to return the token */
    resource_token_t tok;
} resource_return;


/* registers the resource LP with CODES/ROSS */
void resource_lp_init();

/* reads the resource LP configuration */
void resource_lp_configure();

/* Wrappers for the underlying resource structure.
 * Implicitly maps the given LPID to it's group and repetition, then messages
 * the resource LP with the request.
 * The following functions expect the sending LP to put its magic and callback
 * event type into the header parameter (lpid not necessary, it's grabbed from
 * sender)
 *
 * block_on_unavail - flag whether to wait to message the requester if
 *                    request cannot be satisfied
 * return_tag, return_header - set in the return client event, using the offsets in cb
 */
void resource_lp_get(
        uint64_t req,
        int block_on_unavail,
        tw_lp *sender,
        struct codes_mctx const * map_ctx,
        int return_tag,
        msg_header const *return_header,
        struct codes_cb_info const *cb);
/* no callback for frees thus far */
void resource_lp_free(
        uint64_t req,
        tw_lp *sender,
        struct codes_mctx const * map_ctx);
void resource_lp_reserve(
        uint64_t req,
        int block_on_unavail,
        tw_lp *sender,
        struct codes_mctx const * map_ctx,
        int return_tag,
        msg_header const *return_header,
        struct codes_cb_info const *cb);
void resource_lp_get_reserved(
        uint64_t req,
        resource_token_t tok,
        int block_on_unavail,
        tw_lp *sender,
        struct codes_mctx const * map_ctx,
        int return_tag,
        msg_header const *return_header,
        struct codes_cb_info const *cb);
void resource_lp_free_reserved(
        uint64_t req,
        resource_token_t tok,
        tw_lp *sender,
        struct codes_mctx const * map_ctx);

/* rc functions - thankfully, they only use codes-local-latency, so no need 
 * to pass in any arguments */
void resource_lp_get_rc(tw_lp *sender);
void resource_lp_free_rc(tw_lp *sender);
void resource_lp_reserve_rc(tw_lp *sender);
void resource_lp_get_reserved_rc(tw_lp *sender);
void resource_lp_free_reserved_rc(tw_lp *sender);

#ifdef __cplusplus
}
#endif

#endif /* end of include guard: RESOURCE_LP_H */

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
