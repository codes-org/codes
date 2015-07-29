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

static int test_jobmap_identity(int num_ranks)
{
    struct codes_jobmap_ctx *c;
    struct codes_jobmap_params_identity p;

    c = codes_jobmap_configure(CODES_JOBMAP_IDENTITY, &p);
    if (!c) ERR("jobmap-identity: configure failure");

    int num_jobs = codes_jobmap_get_num_jobs(c);
    if (1 != num_jobs)
        ERR("jobmap-identity: expected exactly 1 job, got %d\n", num_jobs);

    struct codes_jobmap_id lid;
    int gid;
    for (int i = 0; i < num_ranks; i++) {
        lid.job = -1; lid.rank = -1;
        lid = codes_jobmap_to_local_id(i, c);
        if (lid.job != 0 || lid.rank != i)
            ERR("jobmap-identity: expected lid (%d,%d), got (%d,%d) for gid %d",
                    0,i, lid.job,lid.rank, i);
        gid = codes_jobmap_to_global_id(lid, c);
        if (gid != i)
            ERR("jobmap-identity: expected gid %d, got %d for lid (%d,%d)",
                    i, gid, lid.job, lid.rank);
    }
    return 0;
}
/* THIS TEST IS HARDCODED AGAINST jobmap-test-list.conf */
static int test_jobmap_list(char * fname)
{
    struct codes_jobmap_ctx *c;
    struct codes_jobmap_params_list p;
    p.alloc_file = fname;

    c = codes_jobmap_configure(CODES_JOBMAP_LIST, &p);
    if (!c) ERR("jobmap-list: configure failure");

    int rank_count_per_job[] = {10, 5, 2, 1, 1, 1, 1, 1, 1};

    int num_jobs = codes_jobmap_get_num_jobs(c);
    if (num_jobs != 9)
        ERR("jobmap-list: expected %d jobs, got %d", 9, num_jobs);

    int gid, gid_expected = 0;
    struct codes_jobmap_id lid_expected, lid;
    for (int i = 0; i < num_jobs; i++) {
        for (int j = 0; j < rank_count_per_job[i]; j++) {
            lid_expected.job = i;
            lid_expected.rank = j;
            gid = codes_jobmap_to_global_id(lid_expected, c);
            if (gid != gid_expected)
                ERR("jobmap-list: expected gid %d for lid (%d,%d), got %d",
                        gid_expected, lid_expected.job, lid_expected.rank, gid);
            lid = codes_jobmap_to_local_id(gid_expected, c);
            if (lid.job != lid_expected.job || lid.rank != lid_expected.rank)
                ERR("jobmap-list: expected lid (%d,%d) for gid %d, got (%d,%d)",
                        lid_expected.job, lid_expected.rank, gid_expected,
                        lid.job, lid.rank);
            gid_expected++;
        }
    }
    codes_jobmap_destroy(c);
    return 0;
}
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
    if (argc != 2)
        ERR("usage: jobmap-test <jobmap-list alloc file>");
    int rc;
    rc = test_jobmap_dummy(10);
    if (rc) return rc;
    rc = test_jobmap_identity(10);
    if (rc) return rc;
    rc = test_jobmap_list(argv[1]);
    if (rc) return rc;
    return 0;
}
