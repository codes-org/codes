/*
 * Copyright (C) 2015 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#include <assert.h>
#include <stdlib.h>

#include "codes-jobmap-method-impl.h"
#include "codes/codes-jobmap.h"

extern struct codes_jobmap_impl jobmap_dummy_impl;
extern struct codes_jobmap_impl jobmap_list_impl;
extern struct codes_jobmap_impl jobmap_identity_impl;

struct codes_jobmap_ctx *
codes_jobmap_configure(enum codes_jobmap_type t, void const * params)
{
    struct codes_jobmap_ctx *c = malloc(sizeof(*c));
    assert(c);
    int rc;

    c->type = t;
    switch(t) {
        case CODES_JOBMAP_IDENTITY:
            c->impl = &jobmap_identity_impl;
            break;
        case CODES_JOBMAP_LIST:
            c->impl = &jobmap_list_impl;
            break;
        case CODES_JOBMAP_DUMMY:
            c->impl = &jobmap_dummy_impl;
            break;
        default:
            free(c);
            fprintf(stderr, "ERROR: unknown jobmap type %d\n", t);
            return NULL;
    }
    rc = c->impl->configure(params, &c->ctx);
    if (rc) {
        fprintf(stderr, "ERROR: failed to configure jobmap type %d\n", t);
        free(c);
        return NULL;
    }
    else
        return c;
}

void codes_jobmap_destroy(struct codes_jobmap_ctx *c)
{
    c->impl->destroy(c->ctx);
    free(c);
}

struct codes_jobmap_id codes_jobmap_to_local_id(
        int id,
        struct codes_jobmap_ctx const * c)
{
    return c->impl->to_local(id, c->ctx);
}

int codes_jobmap_to_global_id(
        struct codes_jobmap_id id,
        struct codes_jobmap_ctx const * c)
{
    return c->impl->to_global(id, c->ctx);
}

int codes_jobmap_get_num_jobs(struct codes_jobmap_ctx const * c)
{
    return c->impl->get_num_jobs(c->ctx);
}

int codes_jobmap_get_num_ranks(int job_id, struct codes_jobmap_ctx const * c)
{
    return c->impl->get_num_ranks(job_id, c->ctx);
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 *  indent-tabs-mode: nil
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
