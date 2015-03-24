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
#include "src/workload/codes-workload-method.h"
#include "codes/quickhash.h"
#include "codes/jenkins-hash.h"

#define RECORDER_NEGLIGIBLE_DELAY .00001

#define RANK_HASH_TABLE_SIZE 397

struct recorder_io_op
{
    double start_time;
    double end_time;
    struct codes_workload_op codes_op;
};

struct file_entry
{
    uint64_t file_id;
    int fd;
    struct qhash_head hash_link;
};

/* structure for storing all context needed to retrieve traces for this rank */
struct rank_traces_context
{
    int rank;
    double last_op_time;

    struct recorder_io_op trace_ops[2048]; /* TODO: this should be extendable */
    int trace_list_ndx;
    int trace_list_max;

    /* hash table link to next rank (if any) */
    struct qhash_head hash_link;
};

/* CODES workload API functions for workloads generated from recorder traces*/
static int recorder_io_workload_load(const char *params, int rank);
static void recorder_io_workload_get_next(int rank, struct codes_workload_op *op);

/* helper functions for recorder workload CODES API */
static int hash_rank_compare(void *key, struct qhash_head *link);
static int hash_file_compare(void *key, struct qhash_head *link);

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
    recorder_params *r_params = (recorder_params *) params;
    struct rank_traces_context *newv = NULL;
    struct qhash_table *file_id_tbl = NULL;
    struct qhash_head *link = NULL;
    struct file_entry *file;

    int64_t nprocs = r_params->nprocs;
    char *trace_dir = r_params->trace_dir_path;
    if(!trace_dir)
        return -1;

    /* allocate a new trace context for this rank */
    newv = (struct rank_traces_context*)malloc(sizeof(*newv));
    if(!newv)
        return -1;

    newv->rank = rank;
    newv->trace_list_ndx = 0;
    newv->trace_list_max = 0;

#if 0
    DIR *dirp;
    struct dirent *entry;
    dirp = opendir(trace_dir);
    while((entry = readdir(dirp)) != NULL) {
        if(entry->d_type == DT_REG)
            nprocs++;
    }
    closedir(dirp);
#endif

    char trace_file_name[1024] = {'\0'};
    sprintf(trace_file_name, "%s/log.%d", trace_dir, rank);

    FILE *trace_file = fopen(trace_file_name, "r");
    if(trace_file == NULL)
        return -1;

    char *line = NULL;
    size_t len;
    ssize_t ret_value;
    char function_name[128] = {'\0'};
    double wkld_start_time = 0.0;
    double io_start_time = 0.0;
    while((ret_value = getline(&line, &len, trace_file)) != -1) {
        struct recorder_io_op r_op;
        char *token = strtok(line, ", \n");
        int fd;

        if (strcmp(token, "BARRIER") && strcmp(token, "0"))
        {
            if (wkld_start_time == 0.0)
                wkld_start_time = atof(token);

            r_op.start_time = atof(token) - wkld_start_time;
            token = strtok(NULL, ", ");
        }
        strcpy(function_name, token);

        if(!strcmp(function_name, "open") || !strcmp(function_name, "open64")) {
            char *filename = NULL;
            char *open_flags = NULL;

            filename = strtok(NULL, ", (");
            open_flags = strtok(NULL, ", )");

            if (!(atoi(open_flags) & O_CREAT))
            {
                r_op.codes_op.op_type = CODES_WK_BARRIER;
                r_op.end_time = r_op.start_time;

                r_op.codes_op.u.barrier.count = nprocs;
                r_op.codes_op.u.barrier.root = 0;

                newv->trace_ops[newv->trace_list_ndx++] = r_op;
                if (newv->trace_list_ndx == 2048) break;
            }

            token = strtok(NULL, ", )");
            token = strtok(NULL, ", ");
            fd = atoi(token);

            token = strtok(NULL, ", \n");
            r_op.end_time = r_op.start_time + atof(token);

            if (!file_id_tbl)
            {
                file_id_tbl = qhash_init(hash_file_compare, quickhash_32bit_hash, 11);
                if (!file_id_tbl)
                {
                    free(newv);
                    return -1;
                }
            }

            file = (struct file_entry*)malloc(sizeof(struct file_entry));
            if (!file)
            {
                free(newv);
                return -1;
            }
            
            uint32_t h1 = 0x00000000, h2 = 0xFFFFFFFF;
            bj_hashlittle2(filename, strlen(filename), &h1, &h2);
            file->file_id = h1 + (((uint64_t)h2)<<32);
            file->fd = fd;
            r_op.codes_op.op_type = CODES_WK_OPEN;
            r_op.codes_op.u.open.file_id = file->file_id;
            r_op.codes_op.u.open.create_flag = atoi(open_flags) & O_CREAT;

            qhash_add(file_id_tbl, &fd, &(file->hash_link));
        }
        else if(!strcmp(function_name, "close")) {
            r_op.codes_op.op_type = CODES_WK_CLOSE;

            token = strtok(NULL, ", ()");
            fd = atoi(token);

            token = strtok(NULL, ", ");
            token = strtok(NULL, ", \n");
            r_op.end_time = r_op.start_time + atof(token);

            link = qhash_search(file_id_tbl, &fd);
            if (!link)
            {
                free(newv);
                return -1;
            }

            file = qhash_entry(link, struct file_entry, hash_link);
            r_op.codes_op.u.close.file_id = file->file_id;

            qhash_del(link);
            free(file);
        }
        else if(!strcmp(function_name, "read") || !strcmp(function_name, "read64")) {
            r_op.codes_op.op_type = CODES_WK_READ;

            token = strtok(NULL, ", (");
            fd = atoi(token);

            // Throw out the buffer
            token = strtok(NULL, ", ");

            token = strtok(NULL, ", )");
            r_op.codes_op.u.read.size = atol(token);

            token = strtok(NULL, ", )");
            r_op.codes_op.u.read.offset = atol(token);

            token = strtok(NULL, ", ");
            token = strtok(NULL, ", \n");

            if (io_start_time == 0.0)
            {
                r_op.end_time = r_op.start_time + atof(token);
            }
            else
            {
                r_op.start_time = r_op.end_time = io_start_time;
            }

            link = qhash_search(file_id_tbl, &fd);
            if (!link)
            {
                free(newv);
                return -1;
            }

            file = qhash_entry(link, struct file_entry, hash_link);
            r_op.codes_op.u.read.file_id = file->file_id;
        }
        else if(!strcmp(function_name, "write") || !strcmp(function_name, "write64")) {
            r_op.codes_op.op_type = CODES_WK_WRITE;

            token = strtok(NULL, ", (");
            fd = atoi(token);

            // Throw out the buffer
            token = strtok(NULL, ", ");

            token = strtok(NULL, ", )");
            r_op.codes_op.u.write.size = atol(token);

            token = strtok(NULL, ", )");
            r_op.codes_op.u.write.offset = atol(token);

            token = strtok(NULL, ", ");
            token = strtok(NULL, ", \n");

            if (io_start_time == 0.0)
            {
                r_op.end_time = r_op.start_time + atof(token);
            }
            else
            {
                r_op.start_time = r_op.end_time = io_start_time;
            }

            link = qhash_search(file_id_tbl, &fd);
            if (!link)
            {
                free(newv);
                return -1;
            }

            file = qhash_entry(link, struct file_entry, hash_link);
            r_op.codes_op.u.write.file_id = file->file_id;
        }
        else if(!strcmp(function_name, "BARRIER")) {
            r_op.start_time = r_op.end_time = io_start_time;
            
            r_op.codes_op.op_type = CODES_WK_BARRIER;
            r_op.codes_op.u.barrier.count = nprocs;
            r_op.codes_op.u.barrier.root = 0;
        }
        else if(!strcmp(function_name, "0")) {
            token = strtok (NULL, ", \n");
            newv->trace_ops[newv->trace_list_ndx-1].end_time += atof(token);

            io_start_time = 0.0;
            continue;
        }
        else{
            if (!strcmp(function_name, "MPI_File_write_at_all") ||
                !strcmp(function_name, "MPI_File_read_at_all")) {
                io_start_time = r_op.start_time;
            }
            continue;
        }

        newv->trace_ops[newv->trace_list_ndx++] = r_op;
        if (newv->trace_list_ndx == 2048) break;
    }

    fclose(trace_file);
    qhash_finalize(file_id_tbl);

    /* reset ndx to 0 and set max to event count */
    /* now we can read all events by counting through array from 0 - max */
    newv->trace_list_max = newv->trace_list_ndx;
    newv->trace_list_ndx = 0;
    newv->last_op_time = 0.0;

    /* initialize the hash table of rank contexts, if it has not been initialized */
    if (!rank_tbl) {
        rank_tbl = qhash_init(hash_rank_compare, quickhash_32bit_hash, RANK_HASH_TABLE_SIZE);

        if (!rank_tbl) {
            free(newv);
            return -1;
        }
    }

    /* add this rank context to the hash table */
    qhash_add(rank_tbl, &(newv->rank), &(newv->hash_link));
    rank_tbl_pop++;

    return 0;
}

/* pull the next trace (independent or collective) for this rank from its trace context */
static void recorder_io_workload_get_next(int rank, struct codes_workload_op *op)
{
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

    if(tmp->trace_list_ndx == tmp->trace_list_max) {
        /* no more events -- just end the workload */
        op->op_type = CODES_WK_END;
        qhash_del(hash_link);
        free(tmp);

        rank_tbl_pop--;
        if(!rank_tbl_pop)
        {
            qhash_finalize(rank_tbl);
            rank_tbl = NULL;
        }
    }
    else {
        struct recorder_io_op *next_r_op = &(tmp->trace_ops[tmp->trace_list_ndx]);

        if ((next_r_op->start_time - tmp->last_op_time) <= RECORDER_NEGLIGIBLE_DELAY) {
            *op = next_r_op->codes_op;

            tmp->trace_list_ndx++;
            tmp->last_op_time = next_r_op->end_time;
        }
        else {
            op->op_type = CODES_WK_DELAY;
            op->u.delay.seconds = next_r_op->start_time - tmp->last_op_time;

            tmp->last_op_time = next_r_op->start_time;
        }
    }

    return;
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

static int hash_file_compare(void *key, struct qhash_head *link)
{
    int *in_file = (int *)key;
    struct file_entry *tmp;

    tmp = qhash_entry(link, struct file_entry, hash_link);
    if (tmp->fd == *in_file)
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
