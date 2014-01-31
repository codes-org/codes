/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#include "codes/codes-workload.h"
#include "codes/quickhash.h"
#include "codes-workload-method.h"

#include "darshan-logutils.h"

/* CODES workload API functions for workloads generated from darshan logs*/
static int darshan_io_workload_load(const char *params, int rank);
static void darshan_io_workload_get_next(int rank, struct codes_workload_op *op);

/* workload method name and function pointers for the CODES workload API */
struct codes_workload_method darshan_io_workload_method =
{
    .method_name = "darshan_io_workload",
    .codes_workload_load = darshan_io_workload_load,
    .codes_workload_get_next = darshan_io_workload_get_next,
};

/* info about this darshan workload group needed by bgp model */
/* TODO: is this needed for darshan workloads? */
/* TODO: does this need to be stored in the rank context to support multiple workloads? */

/* hash table to store per-rank workload contexts */
//static struct qhash_table *rank_tbl = NULL;
//static int rank_tbl_pop = 0;

/* load the workload generator for this rank, given input params */
static int darshan_io_workload_load(const char *params, int rank)
{
    darshan_params *d_params = (darshan_params *)params;
    darshan_fd logfile_fd;

    if (!d_params)
        return -1;

    /* (re)seed the random number generator */
    srand(time(NULL));

    logfile_fd = darshan_log_open(d_params->log_file_path, "r");
    if (logfile_fd < 0)
        return -1;

    darshan_log_close(logfile_fd);

    return 0;
}

/* pull the next event (independent or collective) for this rank from its event context */
static void darshan_io_workload_get_next(int rank, struct codes_workload_op *op)
{

    return;
}

/* return the workload info needed by the bgp model */
/* TODO: do we really need this? */
static void *darshan_io_workload_get_info(int rank)
{
    return &(darshan_workload_info);
}
