/*
 * Copyright (C) 2015 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

/* SUMMARY - namespace support for multiple groups of ids (jobs) with respect to a
 * flat namespace. Note that this API is meant for static job creation - a more
 * sophisticated method would need to be used to modify job mappings at
 * runtime.
 *
 * Example:
 *
 * job  0      1    2
 * rank 0 1 2  0 1  0 1 2 3
 * ID   0 1 2  3 4  5 6 7 8  (<-- LP relative ID)
 * LP   A B C  D E  F G H I  (<-- provided by codes-mapping)
 * */

#ifndef CODES_JOBMAP_H
#define CODES_JOBMAP_H

/** type markers and parameter defs for jobmaps **/

enum codes_jobmap_type {
    /* the "dummy" jobmap is an example implementation. It simply specifies N
     * jobs, with exactly one rank per job, with a trivial mapping */
    CODES_JOBMAP_DUMMY
};

struct codes_jobmap_params_dummy {
    int num_jobs;
};

/** jobmap interface **/

struct codes_jobmap_ctx;

struct codes_jobmap_id {
    int job;
    int rank; // relative to job
};

struct codes_jobmap_ctx *
codes_jobmap_configure(enum codes_jobmap_type t, void const * params);

void codes_jobmap_destroy(struct codes_jobmap_ctx *c);

struct codes_jobmap_id codes_jobmap_lookup(
        int id,
        struct codes_jobmap_ctx const * c);

int codes_jobmap_get_num_jobs(struct codes_jobmap_ctx const * c);

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
