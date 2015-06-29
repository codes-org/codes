/*
 * Copyright (C) 2015 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#include <stdio.h>

#include "codes/codes-jobmap.h"

#define ERR(str, ...) \
    do { \
        fprintf(stderr, "ERROR at %s:%d: " str "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
        return 1; \
    } while(0)

static int test_jobmap_dummy(int num_jobs)
{
    struct codes_jobmap_ctx *c;
    struct codes_jobmap_params_dummy p;
    p.num_jobs = num_jobs;

    /* initialize */
    c = codes_jobmap_configure(CODES_JOBMAP_DUMMY, &p);
    if (!c) ERR("jobmap configure failure");

    struct codes_jobmap_id lid;

    /* successful local lookups */
    for (int i = 0; i < num_jobs; i++) {
        lid = codes_jobmap_to_local_id(i, c);
        if (lid.job != i || lid.rank != 0)
            ERR("lookup failure for %d: expected (%d,%d), got (%d,%d)",
                    i, i, 0, lid.job, lid.rank);
        else {
            lid.job = -1;
            lid.rank = -1;
        }
    }
    /* bad local lookup */
    lid = codes_jobmap_to_local_id(num_jobs, c);
    if (lid.job != -1 || lid.rank != -1)
        ERR("lookup expected failure for %d: expected (%d,%d), got (%d,%d)",
                num_jobs, -1,-1, lid.job,lid.rank);

    /* successful global lookups */
    int gid;
    lid.rank = 0;
    for (lid.job = 0; lid.job < num_jobs; lid.job++) {
        gid = codes_jobmap_to_global_id(lid, c);
        if (gid != lid.job)
            ERR("lookup failure for (%d,%d): expected %d, got %d",
                    lid.job, lid.rank, lid.job, gid);
    }
    /* bad global lookup */
    lid.job = num_jobs;
    gid = codes_jobmap_to_global_id(lid, c);
    if (gid != -1)
        ERR("lookup expected failure for (%d,0): expected -1, got %d",
                num_jobs, gid);

    /* cleanup */
    codes_jobmap_destroy(c);

    return 0;
}

int main(int argc, char *argv[])
{
    int rc;
    rc = test_jobmap_dummy(10);
    if (rc) return rc;

    return 0;
}
