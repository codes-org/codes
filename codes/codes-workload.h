/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

/* I/O workload generator API to be used for reading I/O operations into
 * storage system simulations.  This API just describes the operations to be
 * executed; it does not service the operations.
 */

#ifndef CODES_WORKLOAD_H
#define CODES_WORKLOAD_H

#include "ross.h"
#define MAX_NAME_LENGTH_WKLD 512

typedef struct bgp_params bgp_params;
typedef struct darshan_params darshan_params;
typedef struct recorder_params recorder_params;
typedef struct codes_workload_info codes_workload_info;

struct bgp_params
{
    /* We have the number of ranks passed in from the bg/p model because
     * the I/O lang workloads have no information about the number of ranks.
     * Only the bg/p config file knows the number of ranks. */
    int num_cns;
    /* flag - use path to find kernel files relative to the metafile */
    int use_relpath;
    char io_kernel_meta_path[MAX_NAME_LENGTH_WKLD];
    char bgp_config_file[MAX_NAME_LENGTH_WKLD];
    char io_kernel_path[MAX_NAME_LENGTH_WKLD];
    char io_kernel_def_path[MAX_NAME_LENGTH_WKLD];
};

struct darshan_params
{
    char log_file_path[MAX_NAME_LENGTH_WKLD];
    int64_t aggregator_cnt;
};

struct recorder_params
{
    char trace_dir_path[MAX_NAME_LENGTH_WKLD];
    int64_t nprocs;
};


struct codes_workload_info
{
    int group_id; /* group id */
    int min_rank; /* minimum rank in the collective operation */
    int max_rank; /* maximum rank in the collective operation */
    int local_rank; /* local rank? never being used in the bg/p model */
    int num_lrank; /* number of ranks participating in the collective operation*/
};

/* supported I/O operations */
enum codes_workload_op_type
{
    /* terminator; there are no more operations for this rank */
    CODES_WK_END = 1,
    /* sleep/delay to simulate computation or other activity */
    CODES_WK_DELAY,
    /* block until specified ranks have reached the same point */
    CODES_WK_BARRIER,
    /* open */
    CODES_WK_OPEN,
    /* close */
    CODES_WK_CLOSE,
    /* write */
    CODES_WK_WRITE,
    /* read */
    CODES_WK_READ
};

/* I/O operation paramaters */
struct codes_workload_op
{
    /* TODO: do we need different "classes" of operations to differentiate
     * between different APIs?
     */

    /* what type of operation this is */
    enum codes_workload_op_type op_type;

    /* parameters for each operation type */
    union
    {
        struct {
            double seconds;
        } delay;
        struct {
            int count;  /* num ranks in barrier, -1 means "all" */
            int root;   /* root rank */
        } barrier;
        struct {
            uint64_t file_id;      /* integer identifier for the file */
            int create_flag;  /* file must be created, not just opened */
        } open;
        struct {
            uint64_t file_id;  /* file to operate on */
            off_t offset; /* offset and size */
            size_t size;
        } write;
        struct {
            uint64_t file_id;  /* file to operate on */
            off_t offset; /* offset and size */
            size_t size;
        } read;
        struct {
            uint64_t file_id;  /* file to operate on */
        } close;
    }u;
};

/* load and initialize workload of of type "type" with parameters specified by
 * "params".  The rank is the caller's relative rank within the collection
 * of processes that will participate in this workload.
 *
 * This function is intended to be called by a compute node LP in a model
 * and may be called multiple times over the course of a
 * simulation in order to execute different application workloads.
 *
 * Returns and identifier that can be used to retrieve operations later.
 * Returns -1 on failure.
 */
int codes_workload_load(const char* type, const char* params, int rank);

/* Retrieves the next I/O operation to execute.  the wkld_id is the
 * identifier returned by the init() function.  The op argument is a pointer
 * to a structure to be filled in with I/O operation information.
 */
void codes_workload_get_next(int wkld_id, int rank, struct codes_workload_op *op);

/* Reverse of the above function. */
void codes_workload_get_next_rc(int wkld_id, int rank, const struct codes_workload_op *op);

/* Retrieve the number of ranks contained in a workload */
int codes_workload_get_rank_cnt(const char* type, const char* params);

/* for debugging/logging: print an individual operation to the specified file */
void codes_workload_print_op(FILE *f, struct codes_workload_op *op, int rank);

/* NOTE: there is deliberately no finalize function; we don't have any
 * reliable way to tell when a workload is truly done and will not
 * participate in further reverse computation.   The underlying generators
 * will shut down automatically once they have issued their last event.
 */

#endif /* CODES_WORKLOAD_H */

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
