/*
 * Copyright (C) 2015 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#include <stdlib.h>
#include <assert.h>

#include "../codes-jobmap-method-impl.h"

static int jobmap_identity_configure(void const * params, void ** ctx)
{
    int *num_ranks = malloc(sizeof(*num_ranks));
    assert(num_ranks);
    *ctx = num_ranks;

    struct codes_jobmap_params_identity const * p = params;

    *num_ranks = p->num_ranks;

    return 0;
}

static void jobmap_identity_destroy(void * ctx)
{
    free(ctx);
}

static struct codes_jobmap_id jobmap_identity_to_local(int id, void const * ctx)
{
    int const * num_ranks = ctx;
    struct codes_jobmap_id rtn;

    if (id < 0 || id >= *num_ranks) {
        rtn.job = -1;
        rtn.rank = -1;
    }
    else {
        rtn.job  = 0;
        rtn.rank = id;
    }

    return rtn;
}

static int jobmap_identity_to_global(struct codes_jobmap_id id, void const * ctx)
{
    int const * num_ranks = ctx;

    if (id.job >= 1 || id.rank >= *num_ranks || id.job < 0 || id.rank < 0)
        return -1;
    else
        return id.rank;
}

int jobmap_identity_get_num_jobs(void const * ctx)
{
    (void)ctx;
    return 1;
}

int jobmap_identity_get_num_ranks(int job_id, void const * ctx)
{
    return (job_id == 0) ? *(int const *) ctx : -1;
}

struct codes_jobmap_impl jobmap_identity_impl = {
    jobmap_identity_configure,
    jobmap_identity_destroy,
    jobmap_identity_to_local,
    jobmap_identity_to_global,
    jobmap_identity_get_num_jobs,
    jobmap_identity_get_num_ranks
};

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 *  indent-tabs-mode: nil
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
