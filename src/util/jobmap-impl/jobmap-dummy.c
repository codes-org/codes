/*
 * Copyright (C) 2015 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#include <stdlib.h>
#include <assert.h>

#include "src/util/codes-jobmap-method-impl.h"

static int jobmap_dummy_configure(void const * params, void ** ctx)
{
    int *num_jobs = *ctx;
    num_jobs = malloc(sizeof(*num_jobs));
    assert(num_jobs);

    struct codes_jobmap_params_dummy const * p = params;

    *num_jobs = p->num_jobs;

    return 0;
}

static void jobmap_dummy_destroy(void * ctx)
{
    free(ctx);
}

static struct codes_jobmap_id jobmap_dummy_lookup(int id, void const * ctx)
{
    int const * num_jobs = ctx;
    struct codes_jobmap_id rtn = {-1, -1};

    if (id >= *num_jobs) {
        rtn.job  = id;
        rtn.rank = 0;
    }
    return rtn;
}

int jobmap_dummy_get_num_jobs(void const * ctx)
{
    return *(int const *) ctx;
}

struct codes_jobmap_impl jobmap_dummy_impl = {
    jobmap_dummy_configure,
    jobmap_dummy_destroy,
    jobmap_dummy_lookup,
    jobmap_dummy_get_num_jobs
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
