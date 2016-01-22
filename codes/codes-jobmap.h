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
 * job  0      1    2        (<-- jobmap defined "job" IDs)
 * rank 0 1 2  0 1  0 1 2 3  (<-- "job local" IDs)
 * ID   0 1 2  3 4  5 6 7 8  (<-- jobmap-defined LP relative "global" IDs)
 * LP   A B C  D E  F G H I  (<-- provided by codes-mapping)
 * */

#ifndef CODES_JOBMAP_H
#define CODES_JOBMAP_H

#ifdef __cplusplus
extern "C" {
#endif

/** type markers and parameter defs for jobmaps **/

enum codes_jobmap_type {
    /* the "identity" jobmap is a shim for single-job workloads */
    CODES_JOBMAP_IDENTITY,
    /* the "list" jobmap allows the explicit specification of mappings from
     * jobs to lists of global ids through a text file, wiht one line per job
     */
    CODES_JOBMAP_LIST,
    /* the "dummy" jobmap is an example implementation for testing, and can be
     * seen as the inverse of the identity mapping.
     * It simply specifies N jobs, with exactly one rank per job, with a trivial
     * mapping */
    CODES_JOBMAP_DUMMY,
};

struct codes_jobmap_params_identity {
    int num_ranks;
};

struct codes_jobmap_params_dummy {
    int num_jobs;
};

struct codes_jobmap_params_list {
    char *alloc_file;
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

/* main mapping functions - bidirectional mapping is needed:
 * - global -> local ID for initialization
 * - local -> global ID for communication between local IDs
 *
 * functions return {-1, -1} and -1, respectively, for invalid id input */

struct codes_jobmap_id codes_jobmap_to_local_id(
        int id,
        struct codes_jobmap_ctx const * c);

int codes_jobmap_to_global_id(
        struct codes_jobmap_id id,
        struct codes_jobmap_ctx const * c);

int codes_jobmap_get_num_jobs(struct codes_jobmap_ctx const * c);

int codes_jobmap_get_num_ranks(int job_id, struct codes_jobmap_ctx const * c);

#ifdef __cplusplus
}
#endif

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
