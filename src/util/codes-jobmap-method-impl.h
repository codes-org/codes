/*
 * Copyright (C) 2015 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#ifndef CODES_JOBMAP_METHOD_IMPL
#define CODES_JOBMAP_METHOD_IMPL

#include <stdio.h>
#include "codes/codes-jobmap.h"

struct codes_jobmap_impl {
    /* returns nonzero on failure (to distinguish between no-state (dummy) and
     * failure) */
    int                    (*configure)(void const * params, void ** ctx);
    void                   (*destroy)(void * ctx);
    struct codes_jobmap_id (*to_local) (int id, void const * ctx);
    int                    (*to_global) (struct codes_jobmap_id id, void const * ctx);
    int                    (*get_num_jobs)(void const * ctx);
    int                    (*get_num_ranks)(int job_id, void const * ctx);
};

struct codes_jobmap_ctx {
    enum codes_jobmap_type type;
    struct codes_jobmap_impl *impl;
    void * ctx;
};

#endif

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 *  indent-tabs-mode: nil
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
