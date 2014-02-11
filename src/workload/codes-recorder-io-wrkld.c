/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

/* Recorder workload generator that plugs into the general CODES workload
 * generator API. This generator consumes a set of input files of Recorder I/O
 * traces and passes these traces to the underlying simulator.
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <sys/stat.h>
#include <dirent.h>

#include "ross.h"
#include "codes/codes-workload.h"
#include "codes-workload-method.h"
#include "codes/quickhash.h"

#define RECORDER_MAX_TRACE_READ_COUNT 1024

#define RANK_HASH_TABLE_SIZE 397

typedef enum
{
    POSIX_OPEN = 0,
    POSIX_CLOSE,
    POSIX_READ,
    POSIX_WRITE,
    BARRIER,
} recorder_trace_type;

struct recorder_trace
{
    int64_t rank;
    recorder_trace_type type;
    double start_time;
    union
    {
        struct
        {
            uint64_t file;
            char file_path[2048];
            int create_flag;
        } open;
        struct
        {
            uint64_t file;
            char file_path[2048];
        } close;
        struct
        {
            uint64_t file;
            char file_path[2048];
            off_t offset;
            size_t size;
        } read;
        struct
        {
            uint64_t file;
            char file_path[2048];
            off_t offset;
            size_t size;
        } write;
        struct
        {
            int64_t proc_count;
            int64_t root;
        } barrier;
    } trace_params;
};

/* structure for storing all context needed to retrieve traces for this rank */
struct rank_traces_context
{
    int rank;

    FILE *trace_file;

    int64_t trace_cnt;
    struct recorder_trace traces[RECORDER_MAX_TRACE_READ_COUNT];

    int last_trace_in_memory_index;
    long last_line_read;

    int trace_list_ndx;
    int trace_list_max;

    struct qhash_head hash_link;
};


/* CODES workload API functions for workloads generated from recorder traces*/
static int recorder_io_workload_load(const char *params, int rank);
static void recorder_io_workload_get_next(int rank, struct codes_workload_op *op);

/* helper functions for recorder workload CODES API */
static struct codes_workload_op recorder_trace_to_codes_workload_op(struct recorder_trace trace);
static int hash_rank_compare(void *key, struct qhash_head *link);

/* workload method name and function pointers for the CODES workload API */
struct codes_workload_method recorder_io_workload_method =
{
    .method_name = "recorder_io_workload",
    .codes_workload_load = recorder_io_workload_load,
    .codes_workload_get_next = recorder_io_workload_get_next,
};

static struct qhash_table *rank_tbl = NULL;
static int rank_tbl_pop = 0;

/* load the workload generator for this rank, given input params */
static int recorder_io_workload_load(const char *params, int rank)
{
    const char *trace_dir = params; /* for now, params is just the directory name of the trace files */


    int64_t nprocs = 0;
    struct rank_traces_context *new = NULL;

    if(!trace_dir)
        return -1;

    /* allocate a new trace context for this rank */
    new = malloc(sizeof(*new));
    if(!new)
        return -1;

    new->rank = rank;
    new->trace_list_ndx = 0;
    new->trace_list_max = 0;

    DIR *dirp;
    struct dirent *entry;
    dirp = opendir(trace_dir);
    while((entry = readdir(dirp)) != NULL) {
        if(entry->d_type == DT_REG)
            nprocs++;
    }

    char *trace_file_name = (char*) malloc(sizeof(char) * 1024);
    sprintf(trace_file_name, "%s/log.%d", trace_dir, rank);

    FILE *trace_file = fopen(trace_file_name, "r");
    if(trace_file == NULL)
        return -1;

    printf("rank %d out of %ld procs: Opened %s\n", rank, nprocs, trace_file_name);

    double start_time;
    char *function_name = (char*) malloc(sizeof(char) * 128);

    /* Read the first chunk of data (of size RECORDER_MAX_TRACE_READ_COUNT) */
    char *line;
    size_t len;
    ssize_t ret_value;
    int i;
    for(i = 0; i < RECORDER_MAX_TRACE_READ_COUNT; i++) {
        ret_value = getline(&line, &len, trace_file);
        if(ret_value == -1) {
            new->trace_list_max = i;
            break;
        }
        else {
            char *token = strtok(line, ", ");

            start_time = atof(token);
            token = strtok(NULL, ", ");
            strcpy(function_name, token);

            // printf("function_name=%s:\n", function_name);

            struct recorder_trace rt;

            if(!strcmp(function_name, "open") || !strcmp(function_name, "open64")) {
                rt.rank = rank;
                rt.type = POSIX_OPEN;
                rt.start_time = start_time;

                char *file_path = (char*) malloc(sizeof(char) * 2048);
                token = strtok(NULL, ", (");
                strcpy(file_path, token);

                int create_flag;
                token = strtok(NULL, ", )");
                create_flag = atoi(token);

                strcpy(rt.trace_params.open.file_path, file_path);
                rt.trace_params.open.create_flag = create_flag;

            }
            else if(!strcmp(function_name, "close")) {
                rt.rank = rank;
                rt.type = POSIX_CLOSE;
                rt.start_time = start_time;

                char *file_path = (char*) malloc(sizeof(char) * 2048);
                token = strtok(NULL, ", ()");
                strcpy(file_path, token);

                strcpy(rt.trace_params.close.file_path, file_path);

                // printf("\t rt = [%ld %d %f close (%s)\n", rt.rank, rt.type, rt.start_time, rt.trace_params.open.file_path);
            }
            else if(!strcmp(function_name, "read") || !strcmp(function_name, "read64")) {
                rt.rank = rank;
                rt.type = POSIX_READ;
                rt.start_time = start_time;

                char *file_path = (char*) malloc(sizeof(char) * 2048);
                token = strtok(NULL, ", (");
                strcpy(file_path, token);

                // Throw out the buffer
                token = strtok(NULL, ", ");

                size_t size;
                token = strtok(NULL, ", )");
                size = atol(token);

                strcpy(rt.trace_params.read.file_path, file_path);
                rt.trace_params.read.size = size;

            }
            else if(!strcmp(function_name, "write") || !strcmp(function_name, "write")) {
                rt.rank = rank;
                rt.type = POSIX_WRITE;
                rt.start_time = start_time;

                char *file_path = (char*) malloc(sizeof(char) * 2048);
                token = strtok(NULL, ", (");
                strcpy(file_path, token);

                // Throw out the buffer
                token = strtok(NULL, ", ");

                size_t size;
                token = strtok(NULL, ", )");
                size = atol(token);

                strcpy(rt.trace_params.write.file_path, file_path);
                rt.trace_params.write.size = size;

            }

            new->traces[i] = rt;
        }

        new->last_trace_in_memory_index = i;
        new->last_line_read = ftell(trace_file);
        new->trace_list_ndx = 0;

    }

    /* initialize the hash table of rank contexts, if it has not been initialized */
    if (!rank_tbl) {
        rank_tbl = qhash_init(hash_rank_compare, quickhash_32bit_hash, RANK_HASH_TABLE_SIZE);

        if (!rank_tbl) {
            free(new);
            fclose(trace_file);
            return -1;
        }
    }

    /* add this rank context to the hash table */
    qhash_add(rank_tbl, &(new->rank), &(new->hash_link));
    rank_tbl_pop++;

    return 0;
}

/* pull the next trace (independent or collective) for this rank from its trace context */
static void recorder_io_workload_get_next(int rank, struct codes_workload_op *op)
{
    struct recorder_trace next_trace;
    struct qhash_head *hash_link = NULL;
    struct rank_traces_context *tmp = NULL;

    /* Find event context for this rank in the rank hash table */
    hash_link = qhash_search(rank_tbl, &rank);

    /* terminate the workload if there is no valid rank context */
    if(!hash_link) {
        op->op_type = CODES_WK_END;
        return;
    }

    tmp = qhash_entry(hash_link, struct rank_traces_context, hash_link);
    assert(tmp->rank == rank);

    /* TODO: read in more events if necessary */
    if(tmp->trace_list_max == 0 && tmp->last_trace_in_memory_index == RECORDER_MAX_TRACE_READ_COUNT) {
    }


    if(tmp->trace_list_max != 0 && tmp->trace_list_max == tmp->trace_list_ndx) {
        /* no more events -- just end the workload */
        op->op_type = CODES_WK_END;
        qhash_del(hash_link);
        free(tmp);
        rank_tbl_pop--;
        if(!rank_tbl_pop)
            qhash_finalize(rank_tbl);
    }
    else {
        /* return the next event */
        /* TODO: Do I need to check for the delay like in Darshan? */

        next_trace = tmp->traces[tmp->trace_list_ndx];
        *op = recorder_trace_to_codes_workload_op(next_trace);
        // tmp->last_event_time = next_event.end_time
        tmp->trace_list_ndx++;
    }

    return;
}

/* take a recorder trace struct as input and convert it to a codes workload op */
static struct codes_workload_op recorder_trace_to_codes_workload_op(struct recorder_trace trace)
{
    struct codes_workload_op codes_op;

    switch (trace.type)
    {
        case POSIX_OPEN:
            codes_op.op_type = CODES_WK_OPEN;
            codes_op.u.open.file_id = trace.trace_params.open.file;
            codes_op.u.open.create_flag = trace.trace_params.open.create_flag;
            break;
        case POSIX_CLOSE:
            codes_op.op_type = CODES_WK_CLOSE;
            codes_op.u.close.file_id = trace.trace_params.close.file;
            break;
        case POSIX_READ:
            codes_op.op_type = CODES_WK_READ;
            codes_op.u.read.file_id = trace.trace_params.read.file;
            codes_op.u.read.offset = trace.trace_params.read.offset;
            codes_op.u.read.size = trace.trace_params.read.size;
            break;
        case POSIX_WRITE:
            codes_op.op_type = CODES_WK_WRITE;
            codes_op.u.write.file_id = trace.trace_params.write.file;
            codes_op.u.write.offset = trace.trace_params.write.offset;
            codes_op.u.write.size = trace.trace_params.write.size;
            break;
        case BARRIER:
            codes_op.op_type = CODES_WK_BARRIER;
            codes_op.u.barrier.count = trace.trace_params.barrier.proc_count;
            codes_op.u.barrier.root = trace.trace_params.barrier.root;
            break;
        default:
            assert(0);
            break;
    }

    return codes_op;
}

static int hash_rank_compare(void *key, struct qhash_head *link)
{
    int *in_rank = (int *)key;
    struct rank_traces_context *tmp;
    tmp = qhash_entry(link, struct rank_traces_context, hash_link);

    if (tmp->rank == *in_rank)
        return 1;

    return 0;
}


/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
