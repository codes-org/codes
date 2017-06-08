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

#ifdef __cplusplus
extern "C" {
#endif

#include <ross.h>
#include "configuration.h"

#define MAX_NAME_LENGTH_WKLD 512

/* implementations included with codes */
typedef struct iomock_params iomock_params;
typedef struct iolang_params iolang_params;
typedef struct darshan_params darshan_params;
typedef struct recorder_params recorder_params;

/* struct to hold the actual data from a single MPI event*/
typedef struct dumpi_trace_params dumpi_trace_params;
typedef struct checkpoint_wrkld_params checkpoint_wrkld_params;

struct iomock_params
{
    uint64_t file_id;
    int use_uniq_file_ids;
    int is_write;
    int num_requests;
    int request_size;
    // for optimizing lookup - set higher (>= num ranks) to reduce collisions
    // and 0 to use the default
    int rank_table_size;
};

struct iolang_params
{
    /* the rank count is defined in the workload config file */
    int num_cns;
    /* flag - use path to find kernel files relative to the metafile */
    int use_relpath;
    char io_kernel_meta_path[MAX_NAME_LENGTH_WKLD];
    /* set by config in the metadata path */
    char io_kernel_path[MAX_NAME_LENGTH_WKLD];
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

struct dumpi_trace_params {
   char file_name[MAX_NAME_LENGTH_WKLD];
   int num_net_traces;
#ifdef ENABLE_CORTEX_PYTHON
   char cortex_script[MAX_NAME_LENGTH_WKLD];
   char cortex_class[MAX_NAME_LENGTH_WKLD];
   char cortex_gen[MAX_NAME_LENGTH_WKLD];
#endif
};

struct checkpoint_wrkld_params
{
    int nprocs; /* number of workload processes */
    double checkpoint_sz; /* size of checkpoint, in TiB */
    double checkpoint_wr_bw; /* checkpoint write b/w, in GiB/s */
    int total_checkpoints; /* total number of checkpoint phases */
    double mtti; /* mean time to interrupt, in hours */
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

    /* IO operations */
    /* open */
    CODES_WK_OPEN,
    /* close */
    CODES_WK_CLOSE,
    /* write */
    CODES_WK_WRITE,
    /* read */
    CODES_WK_READ,

    /* network operations (modelled after MPI operations) */
    /* blocking send operation */
    CODES_WK_SEND,
    /* blocking recv operation */
    CODES_WK_RECV,
    /* non-blocking send operation */
    CODES_WK_ISEND,
    /* non-blocking receive operation */
    CODES_WK_IRECV,
    /* broadcast operation */
    CODES_WK_BCAST,
    /* Allgather operation */
    CODES_WK_ALLGATHER,
    /* Allgatherv operation */
    CODES_WK_ALLGATHERV,
    /* Alltoall operation */
    CODES_WK_ALLTOALL,
    /* Alltoallv operation */
    CODES_WK_ALLTOALLV,
    /* Reduce operation */
    CODES_WK_REDUCE,
    /* Allreduce operation */
    CODES_WK_ALLREDUCE,
    /* Generic collective operation */
    CODES_WK_COL,
    /* Waitall operation */
    CODES_WK_WAITALL,
    /* Wait operation */
    CODES_WK_WAIT,
    /* Waitsome operation */
    CODES_WK_WAITSOME,
    /* Waitany operation */
    CODES_WK_WAITANY,
    /* Testall operation */
    CODES_WK_TESTALL,
    /* MPI request free operation*/
    CODES_WK_REQ_FREE,

    /* for workloads that have events not yet handled
     * (eg the workload language) */
    CODES_WK_IGNORE
};

/* I/O operation paramaters */
struct codes_workload_op
{
    /* TODO: do we need different "classes" of operations to differentiate
     * between different APIs?
     */

    /* what type of operation this is */
    enum codes_workload_op_type op_type;
    /* currently only used by network workloads */
    double start_time;
    double end_time;
    double sim_start_time;

    /* parameters for each operation type */
    union
    {
        struct {
            double seconds;
	    double nsecs;
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
        struct {
            /* TODO: not sure why source rank is here */
            int source_rank;/* source rank of MPI send message */
            int dest_rank; /* dest rank of MPI send message */
            uint64_t num_bytes; /* number of bytes to be transferred over the network */
            int16_t data_type; /* MPI data type to be matched with the recv */
            int count; /* number of elements to be received */
            int tag; /* tag of the message */
            int32_t req_id;
        } send;
        struct {
            /* TODO: not sure why source rank is here */
            int source_rank;/* source rank of MPI recv message */
            int dest_rank;/* dest rank of MPI recv message */
            uint64_t num_bytes; /* number of bytes to be transferred over the network */
            int16_t data_type; /* MPI data type to be matched with the send */
            int count; /* number of elements to be sent */
            int tag; /* tag of the message */
            int32_t req_id;
        } recv;
        /* TODO: non-stub for other collectives */
        struct {
            int num_bytes;
        } collective;
        struct {
            int count;
            int32_t* req_ids;
        } waits;
        struct {
            int32_t req_id;
        } wait;
        struct
        {
            int32_t req_id;
        }
        free;
    }u;
};

// helper macro for implementations - call this if multi-app support not
// available
#define APP_ID_UNSUPPORTED(id, name) \
    if (id != 0) \
        tw_error(TW_LOC,\
                "APP IDs not supported for %s generator, 0 required", name);

/* read workload configuration from a CODES configuration file and return the
 * workload name and parameters, which can then be passed to
 * codes_workload_load */
typedef struct
{
    char const * type;
    void * params;
} codes_workload_config_return;

// NOTE: some workloads (iolang, checkpoint) require information about the
// total number of ranks to correctly process traces/config files, etc. Other
// workload generators (darshan) ignore it
codes_workload_config_return codes_workload_read_config(
        ConfigHandle * handle,
        char const * section_name,
        char const * annotation,
        int num_ranks);

void codes_workload_free_config_return(codes_workload_config_return *c);

/* load and initialize workload of of type "type" with parameters specified by
 * "params".  The rank is the caller's relative rank within the collection
 * of processes that will participate in this workload. The app_id is the
 * "application" that the rank is participating in, used to differentiate
 * between multiple, concurrent workloads
 *
 * This function is intended to be called by a compute node LP in a model
 * and may be called multiple times over the course of a
 * simulation in order to execute different application workloads.
 *
 * Returns and identifier that can be used to retrieve operations later.
 * Returns -1 on failure.
 */
int codes_workload_load(
        const char* type,
        const char* params,
        int app_id,
        int rank);

/* Retrieves the next I/O operation to execute.  the wkld_id is the
 * identifier returned by the init() function.  The op argument is a pointer
 * to a structure to be filled in with I/O operation information.
 */
void codes_workload_get_next(
        int wkld_id,
        int app_id,
        int rank,
        struct codes_workload_op *op);

/* Reverse of the above function. */
void codes_workload_get_next_rc(
        int wkld_id,
        int app_id,
        int rank,
        const struct codes_workload_op *op);

/* Another version of reverse handler. */
void codes_workload_get_next_rc2(
                int wkld_id,
                int app_id,
                int rank);

/* Retrieve the number of ranks contained in a workload */
int codes_workload_get_rank_cnt(
        const char* type,
        const char* params,
        int app_id);

/* for debugging/logging: print an individual operation to the specified file */
void codes_workload_print_op(
        FILE *f,
        struct codes_workload_op *op,
        int app_id,
        int rank);

/* implementation structure */
struct codes_workload_method
{
    char *method_name; /* name of the generator */
    void * (*codes_workload_read_config) (
            ConfigHandle *handle, char const * section_name,
            char const * annotation, int num_ranks);
    int (*codes_workload_load)(const char* params, int app_id, int rank);
    void (*codes_workload_get_next)(int app_id, int rank, struct codes_workload_op *op);
    void (*codes_workload_get_next_rc2)(int app_id, int rank);
    int (*codes_workload_get_rank_cnt)(const char* params, int app_id);
};


/* dynamically add to the workload implementation table. Must be done BEFORE
 * calls to codes_workload_read_config or codes_workload_load */
void codes_workload_add_method(struct codes_workload_method const * method);

/* NOTE: there is deliberately no finalize function; we don't have any
 * reliable way to tell when a workload is truly done and will not
 * participate in further reverse computation.   The underlying generators
 * will shut down automatically once they have issued their last event.
 */

#ifdef __cplusplus
}
#endif

#endif /* CODES_WORKLOAD_H */

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 *  indent-tabs-mode: nil
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
