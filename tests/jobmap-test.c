/*
 * Copyright (C) 2015 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#include <stdio.h>

#include "codes/codes-jobmap.h"

const int N = 10;

int main(int argc, char *argv[])
{
    struct codes_jobmap_ctx *c;
    struct codes_jobmap_params_dummy p;
    p.num_jobs = N;

#define ERR(str, ...) \
    do { \
        fprintf(stderr, "ERROR: " str "\n", ##__VA_ARGS__); \
        return 1; \
    } while(0)

    /* initialize */
    c = codes_jobmap_configure(CODES_JOBMAP_DUMMY, &p);
    if (!c) ERR("jobmap configure failure");

    /* successful lookups */
    struct codes_jobmap_id id;
    for (int i = 0; i < N; i++) {
        id = codes_jobmap_to_local_id(i, c);
        if (id.job != i || id.rank != 0)
            ERR("lookup failure for %d: expected (%d,%d), got (%d,%d)",
                    i, i, 0, id.job, id.rank);
        else {
            id.job = -1;
            id.rank = -1;
        }
    }
    /* bad lookup */
    id = codes_jobmap_to_local_id(10, c);
    if (id.job != -1 || id.rank != -1)
        ERR("lookup expected failure for 10: expected (%d,%d), got (%d,%d)",
                -1,-1, id.job,id.rank);

    /* cleanup */
    codes_jobmap_destroy(c);

#undef ERR
    return 0;
}
