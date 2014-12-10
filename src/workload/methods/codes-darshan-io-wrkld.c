/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */
#include <assert.h>
#include <math.h>

#include "codes/codes-workload.h"
#include "codes/quickhash.h"
#include "src/workload/codes-workload-method.h"

#include "darshan-logutils.h"

#define DEF_INTER_IO_DELAY_PCT 0.2
#define DEF_INTER_CYC_DELAY_PCT 0.4

#define DARSHAN_NEGLIGIBLE_DELAY 0.00001

#define RANK_HASH_TABLE_SIZE 397

#define MIN(a, b) (((a) < (b)) ? (a) : (b))

#define ALIGN_BY_8(x) ((x) + ((x) % 8))

/* structure for storing a darshan workload operation (a codes op with 2 timestamps) */
struct darshan_io_op
{
    struct codes_workload_op codes_op;
    double start_time;
    double end_time;
};

/* I/O context structure managed by each rank in the darshan workload */
struct rank_io_context
{
    int64_t my_rank;
    double last_op_time;
    void *io_op_dat;

    off_t next_off;

    struct qhash_head hash_link;
};

/* Darshan workload generator's implementation of the CODES workload API */
static int darshan_io_workload_load(const char *params, int rank);
static void darshan_io_workload_get_next(int rank, struct codes_workload_op *op);
static int darshan_io_workload_get_rank_cnt(const char *params);
static int darshan_rank_hash_compare(void *key, struct qhash_head *link);

/* Darshan I/O op data structure access (insert, remove) abstraction */
static void *darshan_init_io_op_dat(void);
static void darshan_insert_next_io_op(void *io_op_dat, struct darshan_io_op *io_op);
static void darshan_remove_next_io_op(void *io_op_dat, struct darshan_io_op *io_op,
                                      double last_op_time);
static void darshan_finalize_io_op_dat(void *io_op_dat);
static int darshan_io_op_compare(const void *p1, const void *p2);

/* Helper functions for implementing the Darshan workload generator */
static void generate_psx_ind_file_events(struct darshan_file *file,
                                         struct rank_io_context *io_context);
static void generate_psx_coll_file_events(struct darshan_file *file,
                                          struct rank_io_context *io_context,
                                          int64_t nprocs, int64_t aggregator_cnt);
static double generate_psx_open_event(struct darshan_file *file, int create_flag,
                                      double meta_op_time, double cur_time,
                                      struct rank_io_context *io_context, int insert_flag);
static double generate_psx_close_event(struct darshan_file *file, double meta_op_time,
                                       double cur_time, struct rank_io_context *io_context,
                                       int insert_flag);
static double generate_barrier_event(struct darshan_file *file, int64_t root, double cur_time,
                                     struct rank_io_context *io_context);
static double generate_psx_ind_io_events(struct darshan_file *file, int64_t io_ops_this_cycle,
                                         double inter_io_delay, double cur_time,
                                         struct rank_io_context *io_context);
static double generate_psx_coll_io_events(struct darshan_file *file, int64_t ind_io_ops_this_cycle,
                                          int64_t coll_io_ops_this_cycle, int64_t nprocs,
                                          int64_t aggregator_cnt, double inter_io_delay,
                                          double meta_op_time, double cur_time,
                                          struct rank_io_context *io_context);
static void determine_ind_io_params(struct darshan_file *file, int write_flag, size_t *io_sz,
                                    off_t *io_off, struct rank_io_context *io_context);
static void determine_coll_io_params(struct darshan_file *file, int write_flag, int64_t coll_op_cnt,
                                     int64_t agg_cnt, int64_t agg_ndx, size_t *io_sz, off_t *io_off,
                                     struct rank_io_context *io_context);
static void calc_io_delays(struct darshan_file *file, int64_t num_opens, int64_t num_io_ops,
                           double total_delay, double *first_io_delay, double *close_delay,
                           double *inter_open_delay, double *inter_io_delay);
static void file_sanity_check(struct darshan_file *file, struct darshan_job *job);

/* workload method name and function pointers for the CODES workload API */
struct codes_workload_method darshan_io_workload_method =
{
    .method_name = "darshan_io_workload",
    .codes_workload_load = darshan_io_workload_load,
    .codes_workload_get_next = darshan_io_workload_get_next,
    .codes_workload_get_rank_cnt = darshan_io_workload_get_rank_cnt,
};

static int total_rank_cnt = 0;

/* hash table to store per-rank workload contexts */
static struct qhash_table *rank_tbl = NULL;
static int rank_tbl_pop = 0;

/* load the workload generator for this rank, given input params */
static int darshan_io_workload_load(const char *params, int rank)
{
    darshan_params *d_params = (darshan_params *)params;
    darshan_fd logfile_fd;
    struct darshan_job job;
    struct darshan_file next_file;
    struct rank_io_context *my_ctx;
    int ret;

    if (!d_params)
        return -1;

    /* open the darshan log to begin reading in file i/o info */
    logfile_fd = darshan_log_open(d_params->log_file_path, "r");
    if (logfile_fd < 0)
        return -1;

    /* get the per-job stats from the log */
    ret = darshan_log_getjob(logfile_fd, &job);
    if (ret < 0)
    {
        darshan_log_close(logfile_fd);
        return -1;
    }
    if (!total_rank_cnt)
    {
        total_rank_cnt = job.nprocs;
    }
    assert(rank < total_rank_cnt);

    /* allocate the i/o context needed by this rank */
    my_ctx = malloc(sizeof(struct rank_io_context));
    if (!my_ctx)
    {
        darshan_log_close(logfile_fd);
        return -1;
    }
    my_ctx->my_rank = (int64_t)rank;
    my_ctx->last_op_time = 0.0;
    my_ctx->io_op_dat = darshan_init_io_op_dat();
    my_ctx->next_off = 0;

    /* loop over all files contained in the log file */
    while ((ret = darshan_log_getfile(logfile_fd, &job, &next_file)) > 0)
    {
        /* generate all i/o events contained in this independent file */
        if (next_file.rank == rank)
        {
            /* make sure the file i/o counters are valid */
            file_sanity_check(&next_file, &job);

            /* generate i/o events and store them in this rank's workload context */
            generate_psx_ind_file_events(&next_file, my_ctx);
        }
        /* generate all i/o events involving this rank in this collective file */
        else if (next_file.rank == -1)
        {
            /* make sure the file i/o counters are valid */
            file_sanity_check(&next_file, &job);

            /* generate collective i/o events and store them in the rank context */
            generate_psx_coll_file_events(&next_file, my_ctx, job.nprocs, d_params->aggregator_cnt);
        }
        else if (next_file.rank < rank)
            continue;
        else
            break;

        assert(next_file.counters[CP_POSIX_OPENS] == 0);
        assert(next_file.counters[CP_POSIX_READS] == 0);
        assert(next_file.counters[CP_POSIX_WRITES] == 0);
    }
    if (ret < 0)
        return -1;

    darshan_log_close(logfile_fd);

    /* finalize the rank's i/o context so i/o ops may be retrieved later (in order) */
    darshan_finalize_io_op_dat(my_ctx->io_op_dat);

    /* initialize the hash table of rank contexts, if it has not been initialized */
    if (!rank_tbl)
    {
        rank_tbl = qhash_init(darshan_rank_hash_compare, quickhash_64bit_hash, RANK_HASH_TABLE_SIZE);
        if (!rank_tbl)
            return -1;
    }

    /* add this rank context to the hash table */
    qhash_add(rank_tbl, &(my_ctx->my_rank), &(my_ctx->hash_link));
    rank_tbl_pop++;

    return 0;
}

/* pull the next event (independent or collective) for this rank from its event context */
static void darshan_io_workload_get_next(int rank, struct codes_workload_op *op)
{
    int64_t my_rank = (int64_t)rank;
    struct qhash_head *hash_link = NULL;
    struct rank_io_context *tmp = NULL;
    struct darshan_io_op next_io_op;

    assert(rank < total_rank_cnt);

    /* find i/o context for this rank in the rank hash table */
    hash_link = qhash_search(rank_tbl, &my_rank);

    /* terminate the workload if there is no valid rank context */
    if (!hash_link)
    {
        op->op_type = CODES_WK_END;
        return;
    }

    /* get access to the rank's io_context data */
    tmp = qhash_entry(hash_link, struct rank_io_context, hash_link);
    assert(tmp->my_rank == my_rank);

    /* get the next darshan i/o op out of this rank's context */
    darshan_remove_next_io_op(tmp->io_op_dat, &next_io_op, tmp->last_op_time);

    /* free the rank's i/o context if this is the last i/o op */
    if (next_io_op.codes_op.op_type == CODES_WK_END)
    {
        qhash_del(hash_link);
        free(tmp);
 
        rank_tbl_pop--;
        if (!rank_tbl_pop)
        {
            qhash_finalize(rank_tbl);
            rank_tbl = NULL;
        }
    }
    else
    {
        /* else, set the last op time to be the end of the returned op */
        tmp->last_op_time = next_io_op.end_time;
    }

    /* return the codes op contained in the darshan i/o op */
    *op = next_io_op.codes_op;

    return;
}

static int darshan_io_workload_get_rank_cnt(const char *params)
{
    darshan_params *d_params = (darshan_params *)params;
    darshan_fd logfile_fd;
    struct darshan_job job;
    int ret;

    if (!d_params)
        return -1;

    /* open the darshan log to begin reading in file i/o info */
    logfile_fd = darshan_log_open(d_params->log_file_path, "r");
    if (logfile_fd < 0)
        return -1;

    /* get the per-job stats from the log */
    ret = darshan_log_getjob(logfile_fd, &job);
    if (ret < 0)
    {
        darshan_log_close(logfile_fd);
        return -1;
    }

    darshan_log_close(logfile_fd);

    return job.nprocs;
}

/* comparison function for comparing two hash keys (used for storing multiple io contexts) */
static int darshan_rank_hash_compare(
    void *key, struct qhash_head *link)
{
    int64_t *in_rank = (int64_t *)key;
    struct rank_io_context *tmp;

    tmp = qhash_entry(link, struct rank_io_context, hash_link);
    if (tmp->my_rank == *in_rank)
        return 1;

    return 0;
}

/*****************************************/
/*                                       */
/*   Darshan I/O op storage abstraction  */
/*                                       */
/*****************************************/

#define DARSHAN_IO_OP_INC_CNT 100000

/* dynamically allocated array data structure for storing darshan i/o events */
struct darshan_io_dat_array
{
    struct darshan_io_op *op_array;
    int64_t op_arr_ndx;
    int64_t op_arr_cnt;
};

/* initialize the dynamic array data structure */
static void *darshan_init_io_op_dat()
{
    struct darshan_io_dat_array *tmp;

    /* initialize the array data structure */
    tmp = malloc(sizeof(struct darshan_io_dat_array));
    assert(tmp);
    tmp->op_array = malloc(DARSHAN_IO_OP_INC_CNT * sizeof(struct darshan_io_op));
    assert(tmp->op_array);
    tmp->op_arr_ndx = 0;
    tmp->op_arr_cnt = DARSHAN_IO_OP_INC_CNT;

    /* return the array info for this rank's i/o context */
    return (void *)tmp;
}

/* store the i/o event in this rank's i/o context */
static void darshan_insert_next_io_op(
    void *io_op_dat, struct darshan_io_op *io_op)
{
    struct darshan_io_dat_array *array = (struct darshan_io_dat_array *)io_op_dat;
    struct darshan_io_op *tmp;

    /* realloc array if it is already full */
    if (array->op_arr_ndx == array->op_arr_cnt)
    {
        tmp = malloc((array->op_arr_cnt + DARSHAN_IO_OP_INC_CNT) * sizeof(struct darshan_io_op));
        assert(tmp);
        memcpy(tmp, array->op_array, array->op_arr_cnt * sizeof(struct darshan_io_op));
        free(array->op_array);
        array->op_array = tmp;
        array->op_arr_cnt += DARSHAN_IO_OP_INC_CNT;
    }

    /* add the darshan i/o op to the array */
    array->op_array[array->op_arr_ndx++] = *io_op;

    return;
}

/* pull the next i/o event out of this rank's i/o context */
static void darshan_remove_next_io_op(
    void *io_op_dat, struct darshan_io_op *io_op, double last_op_time)
{
    struct darshan_io_dat_array *array = (struct darshan_io_dat_array *)io_op_dat;

    /* if the array has been scanned completely already */
    if (array->op_arr_ndx == array->op_arr_cnt)
    {
        /* no more events just end the workload */
        io_op->codes_op.op_type = CODES_WK_END;
    }
    else
    {
        struct darshan_io_op *tmp = &(array->op_array[array->op_arr_ndx]);

        if ((tmp->start_time - last_op_time) <= DARSHAN_NEGLIGIBLE_DELAY)
        {
            /* there is no delay, just return the next op in the array */
            *io_op = *tmp;
            array->op_arr_ndx++;
        }
        else
        {
            /* there is a nonnegligible delay, so generate and return a delay event */
            io_op->codes_op.op_type = CODES_WK_DELAY;
            io_op->codes_op.u.delay.seconds = tmp->start_time - last_op_time;
            io_op->start_time = last_op_time;
            io_op->end_time = tmp->start_time;
        }
    }

    /* if this is the end op, free data structures */
    if (io_op->codes_op.op_type == CODES_WK_END)
    {
        free(array->op_array);
        free(array);
    }

    return;
}

/* sort the dynamic array in order of i/o op start time */
static void darshan_finalize_io_op_dat(
    void *io_op_dat)
{
    struct darshan_io_dat_array *array = (struct darshan_io_dat_array *)io_op_dat;

    /* sort this rank's i/o op list */
    qsort(array->op_array, array->op_arr_ndx, sizeof(struct darshan_io_op), darshan_io_op_compare);
    array->op_arr_cnt = array->op_arr_ndx;
    array->op_arr_ndx = 0;

    return;
}

/* comparison function for sorting darshan_io_ops in order of start timestamps */
static int darshan_io_op_compare(
    const void *p1, const void *p2)
{
    struct darshan_io_op *a = (struct darshan_io_op *)p1;
    struct darshan_io_op *b = (struct darshan_io_op *)p2;

    if (a->start_time < b->start_time)
        return -1;
    else if (a->start_time > b->start_time)
        return 1;
    else
        return 0;
}

/*****************************************/
/*                                       */
/* Darshan workload generation functions */
/*                                       */
/*****************************************/

/* generate events for an independently opened file, and store these events */
static void generate_psx_ind_file_events(
    struct darshan_file *file, struct rank_io_context *io_context)
{
    int64_t io_ops_this_cycle;
    double cur_time = file->fcounters[CP_F_OPEN_TIMESTAMP];
    double total_delay;
    double first_io_delay = 0.0;
    double close_delay = 0.0;
    double inter_open_delay = 0.0;
    double inter_io_delay = 0.0;
    double meta_op_time;
    int create_flag;
    int64_t i;

    /* if the file was never really opened, just return because we have no timing info */
    if (file->counters[CP_POSIX_OPENS] == 0)
        return;

    /* determine delay available per open-io-close cycle */
    total_delay = file->fcounters[CP_F_CLOSE_TIMESTAMP] - file->fcounters[CP_F_OPEN_TIMESTAMP] -
                  file->fcounters[CP_F_POSIX_READ_TIME] - file->fcounters[CP_F_POSIX_WRITE_TIME] -
                  file->fcounters[CP_F_POSIX_META_TIME];

    /* calculate synthetic delay values */
    calc_io_delays(file, file->counters[CP_POSIX_OPENS],
                   file->counters[CP_POSIX_READS] + file->counters[CP_POSIX_WRITES], total_delay,
                   &first_io_delay, &close_delay, &inter_open_delay, &inter_io_delay);

    /* calculate average meta op time (for i/o and opens/closes) */
    /* TODO: this needs to be updated when we add in stat, seek, etc. */
    meta_op_time = file->fcounters[CP_F_POSIX_META_TIME] / (2 * file->counters[CP_POSIX_OPENS]);

    /* set the create flag if the file was written to */
    if (file->counters[CP_BYTES_WRITTEN])
    {
        create_flag = 1;
    }

    /* generate open/io/close events for all cycles */
    /* TODO: add stats */
    for (i = 0; file->counters[CP_POSIX_OPENS]; i++, file->counters[CP_POSIX_OPENS]--)
    {
        /* generate an open event */
        cur_time = generate_psx_open_event(file, create_flag, meta_op_time, cur_time,
                                           io_context, 1);
        create_flag = 0;

        /* account for potential delay from first open to first io */
        cur_time += first_io_delay;

        io_ops_this_cycle = ceil((double)(file->counters[CP_POSIX_READS] +
                                 file->counters[CP_POSIX_WRITES]) /
                                 file->counters[CP_POSIX_OPENS]);

        /* perform the calculated number of i/o operations for this file open */
        cur_time = generate_psx_ind_io_events(file, io_ops_this_cycle, inter_io_delay,
                                              cur_time, io_context);

        /* account for potential delay from last io to close */
        cur_time += close_delay;

        /* generate a close for the open event at the start of the loop */
        cur_time = generate_psx_close_event(file, meta_op_time, cur_time, io_context, 1);

        /* account for potential interopen delay if more than one open */
        if (file->counters[CP_POSIX_OPENS] > 1)
        {
            cur_time += inter_open_delay;
        }
    }

    return;
}

/* generate events for the i/o ops stored in a collectively opened file for this rank */
void generate_psx_coll_file_events(
    struct darshan_file *file, struct rank_io_context *io_context,
    int64_t nprocs, int64_t in_agg_cnt)
{
    int64_t open_cycles;
    int64_t total_ind_opens;
    int64_t total_coll_opens;
    int64_t ind_opens_this_cycle;
    int64_t coll_opens_this_cycle;
    int64_t extra_opens = 0;
    int64_t extra_io_ops = 0;
    int64_t total_io_ops = file->counters[CP_POSIX_READS] + file->counters[CP_POSIX_WRITES];
    int64_t total_ind_io_ops;
    int64_t total_coll_io_ops;
    int64_t ind_io_ops_this_cycle;
    int64_t coll_io_ops_this_cycle;
    int64_t rank_cnt;
    int create_flag = 0;
    double cur_time = file->fcounters[CP_F_OPEN_TIMESTAMP];
    double total_delay;
    double first_io_delay = 0.0;
    double close_delay = 0.0;
    double inter_cycle_delay = 0.0;
    double inter_io_delay = 0.0;
    double meta_op_time;
    int64_t i;

    /* the collective file was never opened (i.e., just stat-ed), so return */
    if (!(file->counters[CP_POSIX_OPENS]))
        return;

    /*  in this case, posix opens are less than mpi opens...
     *  this is probably a mpi deferred open -- assume app will not use this, currently.
     */
    assert(file->counters[CP_POSIX_OPENS] >= nprocs);

    total_delay = file->fcounters[CP_F_CLOSE_TIMESTAMP] -
                  file->fcounters[CP_F_OPEN_TIMESTAMP] -
                  ((file->fcounters[CP_F_MPI_META_TIME] + file->fcounters[CP_F_MPI_READ_TIME] +
                  file->fcounters[CP_F_MPI_WRITE_TIME]) / nprocs);

    if (file->counters[CP_COLL_OPENS] || file->counters[CP_INDEP_OPENS])
    {
        extra_opens = file->counters[CP_POSIX_OPENS] - file->counters[CP_COLL_OPENS] -
                      file->counters[CP_INDEP_OPENS];

        total_coll_opens = file->counters[CP_COLL_OPENS];
        total_ind_opens = file->counters[CP_POSIX_OPENS] - total_coll_opens - extra_opens;

        total_ind_io_ops = file->counters[CP_INDEP_READS] + file->counters[CP_INDEP_WRITES];
        total_coll_io_ops = (file->counters[CP_COLL_READS] + file->counters[CP_COLL_WRITES]) / nprocs;

        if (file->counters[CP_COLL_OPENS])
        {
            int tmp_ind_io_cycles;
            if (total_ind_io_ops < total_coll_io_ops)
            {
                tmp_ind_io_cycles = total_ind_io_ops;
            }
            else
            {
                tmp_ind_io_cycles = ceil(((double)total_ind_io_ops / total_coll_io_ops) / nprocs) *
                                    total_coll_io_ops;
            }

            open_cycles = total_coll_opens / nprocs;
            calc_io_delays(file, ceil(((double)(total_coll_opens + total_ind_opens)) / nprocs),
                           total_coll_io_ops + tmp_ind_io_cycles, total_delay,
                           &first_io_delay, &close_delay, &inter_cycle_delay, &inter_io_delay);
        }
        else
        {
            open_cycles = ceil((double)total_ind_opens / nprocs);
            calc_io_delays(file, open_cycles, ceil((double)total_ind_io_ops / nprocs), total_delay,
                           &first_io_delay, &close_delay, &inter_cycle_delay, &inter_io_delay);
        }
    }
    else
    {
        extra_opens = file->counters[CP_POSIX_OPENS] % nprocs;
        if (extra_opens && ((file->counters[CP_POSIX_OPENS] / nprocs) % extra_opens))
        {
            extra_opens = 0;
        }
        else
        {
            extra_io_ops = total_io_ops % nprocs;
            if (extra_io_ops != extra_opens)
            {
                extra_opens = 0;
                extra_io_ops = 0;
            }
        }

        total_coll_opens = 0;
        total_ind_opens = file->counters[CP_POSIX_OPENS] - extra_opens;

        total_ind_io_ops = total_io_ops - extra_io_ops;
        total_coll_io_ops = 0;

        open_cycles = ceil((double)total_ind_opens / nprocs);
        calc_io_delays(file, open_cycles, ceil((double)total_ind_io_ops / nprocs), total_delay,
                       &first_io_delay, &close_delay, &inter_cycle_delay, &inter_io_delay);
    }
    assert(extra_opens <= open_cycles);

    /* calculate average meta op time (for i/o and opens/closes) */
    meta_op_time = file->fcounters[CP_F_POSIX_META_TIME] / ((2 * file->counters[CP_POSIX_OPENS]) +
                   file->counters[CP_POSIX_READS] + file->counters[CP_POSIX_WRITES]);

    /* it is rare to overwrite existing files, so set the create flag */
    if (file->counters[CP_BYTES_WRITTEN])
    {
        create_flag = 1;
    }

    /* generate all events for this collectively opened file */
    for (i = 0; i < open_cycles; i++)
    {
        ind_opens_this_cycle = ceil((double)total_ind_opens / (open_cycles - i));
        coll_opens_this_cycle = total_coll_opens / (open_cycles - i);

        /* assign any extra opens to rank 0 (these may correspond to file creations or
         * header reads/writes)
         */
        if (extra_opens && !(i % (open_cycles / extra_opens)))
        {
            cur_time = generate_psx_open_event(file, create_flag, meta_op_time, cur_time,
                                               io_context, (io_context->my_rank == 0));
            create_flag = 0;

            if (!file->counters[CP_COLL_OPENS] && !file->counters[CP_INDEP_OPENS])
            {
                cur_time = generate_psx_coll_io_events(file, 1, 0, nprocs, nprocs, 0.0,
                                                       meta_op_time, cur_time, io_context);
                extra_io_ops--;
            }

            cur_time = generate_psx_close_event(file, meta_op_time, cur_time, io_context,
                                                (io_context->my_rank == 0));
            file->counters[CP_POSIX_OPENS]--;
        }

        while (ind_opens_this_cycle)
        {
            if (ind_opens_this_cycle >= nprocs)
                rank_cnt = nprocs;
            else
                rank_cnt = ind_opens_this_cycle;

            cur_time = generate_psx_open_event(file, create_flag, meta_op_time, cur_time,
                                               io_context, (io_context->my_rank < rank_cnt));
            create_flag = 0;

            cur_time += first_io_delay;

            ind_io_ops_this_cycle = ceil(((double)total_ind_io_ops / total_ind_opens) * rank_cnt);
            cur_time = generate_psx_coll_io_events(file, ind_io_ops_this_cycle, 0, nprocs,
                                                   nprocs, inter_io_delay, meta_op_time,
                                                   cur_time, io_context);
            total_ind_io_ops -= ind_io_ops_this_cycle;

            cur_time += close_delay;

            cur_time = generate_psx_close_event(file, meta_op_time, cur_time, io_context,
                                                (io_context->my_rank < rank_cnt));

            file->counters[CP_POSIX_OPENS] -= rank_cnt;
            ind_opens_this_cycle -= rank_cnt;
            total_ind_opens -= rank_cnt;

            if (file->counters[CP_POSIX_OPENS])
                cur_time += inter_cycle_delay;
        }

        while (coll_opens_this_cycle)
        {
            assert(!create_flag);

            cur_time = generate_barrier_event(file, 0, cur_time, io_context);

            cur_time = generate_psx_open_event(file, create_flag, meta_op_time,
                                               cur_time, io_context, 1);

            cur_time += first_io_delay;

            if (file->counters[CP_INDEP_OPENS])
                ind_io_ops_this_cycle = 0;
            else
                ind_io_ops_this_cycle = ceil((double)total_ind_io_ops / 
                                             (file->counters[CP_COLL_OPENS] / nprocs));

            coll_io_ops_this_cycle = ceil((double)total_coll_io_ops / 
                                          (file->counters[CP_COLL_OPENS] / nprocs));
            cur_time = generate_psx_coll_io_events(file, ind_io_ops_this_cycle,
                                                   coll_io_ops_this_cycle, nprocs, in_agg_cnt,
                                                   inter_io_delay, meta_op_time, cur_time,
                                                   io_context);
            total_ind_io_ops -= ind_io_ops_this_cycle;
            total_coll_io_ops -= coll_io_ops_this_cycle;

            cur_time += close_delay;

            cur_time = generate_psx_close_event(file, meta_op_time, cur_time, io_context, 1);

            file->counters[CP_POSIX_OPENS] -= nprocs;
            file->counters[CP_COLL_OPENS] -= nprocs;
            coll_opens_this_cycle -= nprocs;
            total_coll_opens -= nprocs;

            if (file->counters[CP_POSIX_OPENS])
                cur_time += inter_cycle_delay;
        }
    }

    return;
}

/* fill in an open event structure and store it with the rank context */
static double generate_psx_open_event(
    struct darshan_file *file, int create_flag, double meta_op_time,
    double cur_time, struct rank_io_context *io_context, int insert_flag)
{
    struct darshan_io_op next_io_op = 
    {
        .codes_op.op_type = CODES_WK_OPEN,
        .codes_op.u.open.file_id = file->hash,
        .codes_op.u.open.create_flag = create_flag,
        .start_time = cur_time
    };

    /* set the end time of the event based on time spent in POSIX meta operations */
    cur_time += meta_op_time;
    next_io_op.end_time = cur_time;

    /* store the open event (if this rank performed it) */
    if (insert_flag)
        darshan_insert_next_io_op(io_context->io_op_dat, &next_io_op);

    return cur_time;
}

/* fill in a close event structure and store it with the rank context */
static double generate_psx_close_event(
    struct darshan_file *file, double meta_op_time, double cur_time,
    struct rank_io_context *io_context, int insert_flag)
{
    struct darshan_io_op next_io_op =
    {
        .codes_op.op_type = CODES_WK_CLOSE,
        .codes_op.u.close.file_id = file->hash,
        .start_time = cur_time
    };

    /* set the end time of the event based on time spent in POSIX meta operations */
    cur_time += meta_op_time;
    next_io_op.end_time = cur_time;

    /* store the close event (if this rank performed it) */
    if (insert_flag)
        darshan_insert_next_io_op(io_context->io_op_dat, &next_io_op);

    io_context->next_off = 0;

    return cur_time;
}

/* fill in a barrier event structure and store it with the rank context */
static double generate_barrier_event(
    struct darshan_file *file, int64_t root, double cur_time, struct rank_io_context *io_context)
{
    struct darshan_io_op next_io_op =
    {
        .codes_op.op_type = CODES_WK_BARRIER, 
        .codes_op.u.barrier.count = 8192, /* all processes */
        .codes_op.u.barrier.root = root,
        .start_time = cur_time
    };

    cur_time += .000001; /* small synthetic delay representing time to barrier */
    next_io_op.end_time = cur_time;

    /* store the barrier event */
    if (file->rank == -1)
        darshan_insert_next_io_op(io_context->io_op_dat, &next_io_op);

    return cur_time;
}

/* generate all i/o events for one independent file open and store them with the rank context */
static double generate_psx_ind_io_events(
    struct darshan_file *file, int64_t io_ops_this_cycle, double inter_io_delay,
    double cur_time, struct rank_io_context *io_context)
{
    static int rw = -1; /* rw = 1 for write, 0 for read, -1 for uninitialized */
    static int64_t io_ops_this_rw;
    static double rd_bw = 0.0, wr_bw = 0.0;
    int64_t psx_rw_ops_remaining = file->counters[CP_POSIX_READS] + file->counters[CP_POSIX_WRITES];
    double io_op_time;
    size_t io_sz;
    off_t io_off;
    int64_t i;
    struct darshan_io_op next_io_op;

    /* if there are no i/o ops, just return immediately */
    if (!io_ops_this_cycle)
        return cur_time;

    /* initialze static variables when a new file is opened */
    if (rw == -1)
    {
        /* initialize rw to be the first i/o operation found in the log */
        if (file->fcounters[CP_F_WRITE_START_TIMESTAMP] == 0.0)
            rw = 0;
        else if (file->fcounters[CP_F_READ_START_TIMESTAMP] == 0.0)
            rw = 1;
        else
            rw = (file->fcounters[CP_F_READ_START_TIMESTAMP] <
                  file->fcounters[CP_F_WRITE_START_TIMESTAMP]) ? 0 : 1;

        /* determine how many io ops to do before next rw switch */
        if (!rw)
            io_ops_this_rw = file->counters[CP_POSIX_READS] /
                             ((file->counters[CP_RW_SWITCHES] / 2) + 1);
        else
            io_ops_this_rw = file->counters[CP_POSIX_WRITES] /
                             ((file->counters[CP_RW_SWITCHES] / 2) + 1);

        /* initialize the rd and wr bandwidth values using total io size and time */
        if (file->fcounters[CP_F_POSIX_READ_TIME])
            rd_bw = file->counters[CP_BYTES_READ] / file->fcounters[CP_F_POSIX_READ_TIME];
        if (file->fcounters[CP_F_POSIX_WRITE_TIME])
            wr_bw = file->counters[CP_BYTES_WRITTEN] / file->fcounters[CP_F_POSIX_WRITE_TIME];
    }

    /* loop to generate all reads/writes for this open/close sequence */
    for (i = 0; i < io_ops_this_cycle; i++)
    {
        /* calculate what value to use for i/o size and offset */
        determine_ind_io_params(file, rw, &io_sz, &io_off, io_context);
        if (!rw)
        {
            /* generate a read event */
            next_io_op.codes_op.op_type = CODES_WK_READ;
            next_io_op.codes_op.u.read.file_id = file->hash;
            next_io_op.codes_op.u.read.size = io_sz;
            next_io_op.codes_op.u.read.offset = io_off;
            next_io_op.start_time = cur_time;

            /* set the end time based on observed bandwidth and io size */
            if (rd_bw == 0.0)
                io_op_time = 0.0;
            else
                io_op_time = (io_sz / rd_bw);

            /* update time, accounting for metadata time */
            cur_time += io_op_time;
            next_io_op.end_time = cur_time;
            file->counters[CP_POSIX_READS]--;
        }
        else
        {
            /* generate a write event */
            next_io_op.codes_op.op_type = CODES_WK_WRITE;
            next_io_op.codes_op.u.write.file_id = file->hash;
            next_io_op.codes_op.u.write.size = io_sz;
            next_io_op.codes_op.u.write.offset = io_off;
            next_io_op.start_time = cur_time;

            /* set the end time based on observed bandwidth and io size */
            if (wr_bw == 0.0)
                io_op_time = 0.0;
            else
                io_op_time = (io_sz / wr_bw);

            /* update time, accounting for metadata time */
            cur_time += io_op_time;
            next_io_op.end_time = cur_time;
            file->counters[CP_POSIX_WRITES]--;
        }
        psx_rw_ops_remaining--;
        io_ops_this_rw--;
        assert(file->counters[CP_POSIX_READS] >= 0);
        assert(file->counters[CP_POSIX_WRITES] >= 0);

        /* store the i/o event */
        darshan_insert_next_io_op(io_context->io_op_dat, &next_io_op);

        /* determine whether to toggle between reads and writes */
        if (!io_ops_this_rw && psx_rw_ops_remaining)
        {
            /* toggle the read/write flag */
            rw ^= 1;
            file->counters[CP_RW_SWITCHES]--;

            /* determine how many io ops to do before next rw switch */
            if (!rw)
                io_ops_this_rw = file->counters[CP_POSIX_READS] /
                                 ((file->counters[CP_RW_SWITCHES] / 2) + 1);
            else
                io_ops_this_rw = file->counters[CP_POSIX_WRITES] /
                                 ((file->counters[CP_RW_SWITCHES] / 2) + 1);
        }

        if (i != (io_ops_this_cycle - 1))
        {
            /* update current time to account for possible delay between i/o operations */
            cur_time += inter_io_delay;
        }
    }

    /* reset the static rw flag if this is the last open-close cycle for this file */
    if (file->counters[CP_POSIX_OPENS] == 1)
    {
        rw = -1;
    }

    return cur_time;
}

static double generate_psx_coll_io_events(
    struct darshan_file *file, int64_t ind_io_ops_this_cycle, int64_t coll_io_ops_this_cycle,
    int64_t nprocs, int64_t aggregator_cnt, double inter_io_delay, double meta_op_time,
    double cur_time, struct rank_io_context *io_context)
{
    static int rw = -1; /* rw = 1 for write, 0 for read, -1 for uninitialized */
    static int64_t io_ops_this_rw;
    static double rd_bw = 0.0, wr_bw = 0.0;
    int64_t psx_rw_ops_remaining = file->counters[CP_POSIX_READS] + file->counters[CP_POSIX_WRITES];
    int64_t total_io_ops_this_cycle = ind_io_ops_this_cycle + coll_io_ops_this_cycle;
    int64_t tmp_rank;
    int64_t next_ind_io_rank = 0;
    int64_t io_cnt;
    double ranks_per_aggregator = (double)(nprocs - 1) / (aggregator_cnt - 1);
    int64_t ind_ops_remaining = 0;
    double io_op_time;
    size_t io_sz;
    off_t io_off;
    int64_t i, j;
    struct darshan_io_op next_io_op;

    if (!total_io_ops_this_cycle)
        return cur_time;

    /* initialze static variables when a new file is opened */
    if (rw == -1)
    {
        /* initialize rw to be the first i/o operation found in the log */
        if (file->fcounters[CP_F_WRITE_START_TIMESTAMP] == 0.0)
            rw = 0;
        else if (file->fcounters[CP_F_READ_START_TIMESTAMP] == 0.0)
            rw = 1;
        else
            rw = (file->fcounters[CP_F_READ_START_TIMESTAMP] <
                  file->fcounters[CP_F_WRITE_START_TIMESTAMP]) ? 0 : 1;

        /* determine how many io ops to do before next rw switch */
        if (!rw)
        {
            if (file->counters[CP_COLL_OPENS])
                io_ops_this_rw =
                    ((file->counters[CP_COLL_READS] / nprocs) + file->counters[CP_INDEP_READS]) /
                    ((file->counters[CP_RW_SWITCHES] / (2 * aggregator_cnt)) + 1);
            else
                io_ops_this_rw = file->counters[CP_POSIX_READS] /
                                 ((file->counters[CP_RW_SWITCHES] / (2 * aggregator_cnt)) + 1);
        }
        else
        {
            if (file->counters[CP_COLL_OPENS])
                io_ops_this_rw =
                    ((file->counters[CP_COLL_WRITES] / nprocs) + file->counters[CP_INDEP_WRITES]) /
                    ((file->counters[CP_RW_SWITCHES] / (2 * aggregator_cnt)) + 1);
            else
                io_ops_this_rw = file->counters[CP_POSIX_WRITES] /
                                 ((file->counters[CP_RW_SWITCHES] / (2 * aggregator_cnt)) + 1);
        }

        /* initialize the rd and wr bandwidth values using total io size and time */
        if (file->fcounters[CP_F_POSIX_READ_TIME])
            rd_bw = file->counters[CP_BYTES_READ] / file->fcounters[CP_F_POSIX_READ_TIME];
        if (file->fcounters[CP_F_POSIX_WRITE_TIME])
            wr_bw = file->counters[CP_BYTES_WRITTEN] / file->fcounters[CP_F_POSIX_WRITE_TIME];
    }

    if (coll_io_ops_this_cycle)
        ind_ops_remaining = ceil((double)ind_io_ops_this_cycle / coll_io_ops_this_cycle);
    else
        ind_ops_remaining = ind_io_ops_this_cycle;

    for (i = 0; i < total_io_ops_this_cycle; i++)
    {
        if (ind_ops_remaining)
        {
            tmp_rank = (next_ind_io_rank++) % nprocs;
            ind_io_ops_this_cycle--;

            determine_ind_io_params(file, rw, &io_sz, &io_off, io_context);
            if (!rw)
            {
                /* generate a read event */
                next_io_op.codes_op.op_type = CODES_WK_READ;
                next_io_op.codes_op.u.read.file_id = file->hash;
                next_io_op.codes_op.u.read.size = io_sz;
                next_io_op.codes_op.u.read.offset = io_off;

                /* set the end time based on observed bandwidth and io size */
                if (rd_bw == 0.0)
                    io_op_time = 0.0;
                else
                    io_op_time = (io_sz / rd_bw);
                
                file->counters[CP_POSIX_READS]--;
                file->counters[CP_INDEP_READS]--;
            }
            else
            {
                /* generate a write event */
                next_io_op.codes_op.op_type = CODES_WK_WRITE;
                next_io_op.codes_op.u.write.file_id = file->hash;
                next_io_op.codes_op.u.write.size = io_sz;
                next_io_op.codes_op.u.write.offset = io_off;

                /* set the end time based on observed bandwidth and io size */
                if (wr_bw == 0.0)
                    io_op_time = 0.0;
                else
                    io_op_time = (io_sz / wr_bw);

                file->counters[CP_POSIX_WRITES]--;
                file->counters[CP_INDEP_WRITES]--;
            }
            next_io_op.start_time = cur_time;
            next_io_op.end_time = cur_time + io_op_time + meta_op_time;

            psx_rw_ops_remaining--;
            assert(file->counters[CP_POSIX_READS] >= 0);
            assert(file->counters[CP_POSIX_WRITES] >= 0);

            /* store the io operation if it belongs to this rank */
            if (tmp_rank == io_context->my_rank)
            {
                darshan_insert_next_io_op(io_context->io_op_dat, &next_io_op);
                cur_time = next_io_op.end_time;
                if (coll_io_ops_this_cycle || (ind_ops_remaining > nprocs))
                    cur_time += inter_io_delay;
            }

            ind_ops_remaining--;
            if (!ind_ops_remaining) next_ind_io_rank = 0;
        }
        else
        {
            tmp_rank = 0;
            coll_io_ops_this_cycle--;

            if (!rw)
            {
                io_cnt = ceil((double)(file->counters[CP_POSIX_READS] -
                              file->counters[CP_INDEP_READS]) / 
                              (file->counters[CP_COLL_READS] / nprocs));
                file->counters[CP_COLL_READS] -= nprocs;
            }
            else
            {
                io_cnt = ceil((double)(file->counters[CP_POSIX_WRITES] -
                              file->counters[CP_INDEP_WRITES]) / 
                              (file->counters[CP_COLL_WRITES] / nprocs));
                file->counters[CP_COLL_WRITES] -= nprocs;
            }

            if (coll_io_ops_this_cycle)
                ind_ops_remaining = ceil((double)ind_io_ops_this_cycle / coll_io_ops_this_cycle);
            else
                ind_ops_remaining = ind_io_ops_this_cycle;

            for (j = 0; j < io_cnt; j += aggregator_cnt)
            {
                int64_t tmp_coll_cnt = MIN(io_cnt - j, aggregator_cnt);
                int64_t tmp_agg_ndx;

                cur_time = generate_barrier_event(file, 0, cur_time, io_context);

                tmp_agg_ndx = (int64_t)round(io_context->my_rank / ranks_per_aggregator);
                if ((round(tmp_agg_ndx * ranks_per_aggregator) == io_context->my_rank) &&
                    (tmp_agg_ndx < tmp_coll_cnt))
                {
                    determine_coll_io_params(file, rw, io_cnt, tmp_coll_cnt, tmp_agg_ndx + 1, 
                                             &io_sz, &io_off, io_context);
                    if (!rw)
                    {
                        /* generate a read event */
                        next_io_op.codes_op.op_type = CODES_WK_READ;
                        next_io_op.codes_op.u.read.file_id = file->hash;
                        next_io_op.codes_op.u.read.size = io_sz;
                        next_io_op.codes_op.u.read.offset = io_off;

                        /* set the end time based on observed bandwidth and io size */
                        if (rd_bw == 0.0)
                            io_op_time = 0.0;
                        else
                            io_op_time = (io_sz / rd_bw);
                        
                        file->counters[CP_POSIX_READS] -= tmp_coll_cnt;
                    }
                    else
                    {
                        /* generate a write event */
                        next_io_op.codes_op.op_type = CODES_WK_WRITE;
                        next_io_op.codes_op.u.write.file_id = file->hash;
                        next_io_op.codes_op.u.write.size = io_sz;
                        next_io_op.codes_op.u.write.offset = io_off;

                        /* set the end time based on observed bandwidth and io size */
                        if (wr_bw == 0.0)
                            io_op_time = 0.0;
                        else
                            io_op_time = (io_sz / wr_bw);

                        file->counters[CP_POSIX_WRITES] -= tmp_coll_cnt;
                    }
                    next_io_op.start_time = cur_time;
                    next_io_op.end_time = cur_time + io_op_time + meta_op_time;

                    darshan_insert_next_io_op(io_context->io_op_dat, &next_io_op);

                    cur_time = next_io_op.end_time;
                }
                else
                {
                    if (!rw)
                    {
                        file->counters[CP_POSIX_READS] -= tmp_coll_cnt;
                    }
                    else
                    {
                        file->counters[CP_POSIX_WRITES] -= tmp_coll_cnt;
                    }
                }
                psx_rw_ops_remaining -= tmp_coll_cnt;
                assert(file->counters[CP_POSIX_READS] >= 0);
                assert(file->counters[CP_POSIX_WRITES] >= 0);
            }

            if (i != (total_io_ops_this_cycle - 1))
                cur_time += inter_io_delay;
        }
        io_ops_this_rw--;


        /* determine whether to toggle between reads and writes */
        if (!io_ops_this_rw && psx_rw_ops_remaining)
        {
            /* toggle the read/write flag */
            rw ^= 1;
            file->counters[CP_RW_SWITCHES] -= aggregator_cnt;

            /* determine how many io ops to do before next rw switch */
            if (!rw)
            {
                if (file->counters[CP_COLL_OPENS])
                    io_ops_this_rw =
                        ((file->counters[CP_COLL_READS] / nprocs) +
                        file->counters[CP_INDEP_READS]) / ((file->counters[CP_RW_SWITCHES] /
                        (2 * aggregator_cnt)) + 1);
                else
                    io_ops_this_rw = file->counters[CP_POSIX_READS] /
                                     ((file->counters[CP_RW_SWITCHES] / (2 * aggregator_cnt)) + 1);
            }
            else
            {
                if (file->counters[CP_COLL_OPENS])
                    io_ops_this_rw =
                        ((file->counters[CP_COLL_WRITES] / nprocs) +
                        file->counters[CP_INDEP_WRITES]) / ((file->counters[CP_RW_SWITCHES] /
                        (2 * aggregator_cnt)) + 1);
                else
                    io_ops_this_rw = file->counters[CP_POSIX_WRITES] /
                                     ((file->counters[CP_RW_SWITCHES] / (2 * aggregator_cnt)) + 1);
            }
        }
    }

    /* reset the static rw flag if this is the last open-close cycle for this file */
    if (file->counters[CP_POSIX_OPENS] <= nprocs)
    {
        rw = -1;
    }

    return cur_time;
}

static void determine_coll_io_params(
    struct darshan_file *file, int write_flag, int64_t coll_op_cnt, int64_t agg_cnt,
    int64_t agg_ndx, size_t *io_sz, off_t *io_off, struct rank_io_context *io_context)
{
    static int64_t size_bins_left = 0;
    static int64_t agg_size_bins[10] = { 0 };
    static off_t agg_off = 0;
    int i, j;
    int start_ndx, end_ndx;
    int64_t *all_size_bins = NULL;
    int64_t *common_accesses = &(file->counters[CP_ACCESS1_ACCESS]); /* 4 common accesses */
    int64_t *common_access_counts = &(file->counters[CP_ACCESS1_COUNT]); /* common access counts */
    int64_t *total_io_size = NULL;
    int64_t tmp_cnt;
    int64_t switch_cnt;
    int64_t leftover;
    int64_t bins_to_use;
    const int64_t size_bin_min_vals[10] = { 0, 100, 1024, 10 * 1024, 100 * 1024, 1024 * 1024,
                                            4 * 1024 * 1024, 10 * 1024 * 1024, 100 * 1024 * 1024,
                                            1024 * 1024 * 1024 };
    const int64_t size_bin_max_vals[10] = { 100, 1024, 10 * 1024, 100 * 1024, 1024 * 1024,
                                            4 * 1024 * 1024, 10 * 1024 * 1024, 100 * 1024 * 1024,
                                            1024 * 1024 * 1024, INT64_MAX };
    
    if (write_flag)
    {
        all_size_bins = &(file->counters[CP_SIZE_WRITE_0_100]);
        total_io_size = &(file->counters[CP_BYTES_WRITTEN]);
    }
    else
    {
        all_size_bins = &(file->counters[CP_SIZE_READ_0_100]);
        total_io_size = &(file->counters[CP_BYTES_READ]);
    }

    /* we enter this if statement if we have not yet calculated which size bins to use for the
     * current collective I/O operation.
     */
    if (!size_bins_left)
    {
        int64_t total_agg_size_bin_cnt = 0;
        int tmp_ndx = 9;

        /* find some size bins that we can assign accesses out of.
         * Note: we require the bins be reasonably close to each other in size, and be less
         * than 100 MiB.
         */
        for (i = 7; i >= 0; i--)
        {
            if (all_size_bins[i])
            {
                if (total_agg_size_bin_cnt)
                {
                    if ((tmp_ndx - 3) <= i)
                    {
                        tmp_ndx = i;
                        total_agg_size_bin_cnt += all_size_bins[i];
                    }
                    break;
                }
                else
                {
                    tmp_ndx = i;
                    total_agg_size_bin_cnt += all_size_bins[i];
                }
            }
        }

        /* assign accesses from found bins proportional to bin size */
        for (i = 7; i >= tmp_ndx; i--)
        {
            if (all_size_bins[i])
            {
                agg_size_bins[i] = ((double)all_size_bins[i] / total_agg_size_bin_cnt) *
                                   MIN(total_agg_size_bin_cnt, coll_op_cnt);
                size_bins_left += agg_size_bins[i];
                all_size_bins[i] -= agg_size_bins[i];
            }

            if (size_bins_left == coll_op_cnt) break;
        }

        /* if we still haven't assigned enough accesses, just assign them stupidly */
        if (size_bins_left < coll_op_cnt)
        {
            for (i = 9; i >= 0; i--)
            {
                if (all_size_bins[i])
                {
                    tmp_cnt = MIN(all_size_bins[i], coll_op_cnt - size_bins_left);
                    agg_size_bins[i] += tmp_cnt;
                    size_bins_left += tmp_cnt;
                    all_size_bins[i] -= tmp_cnt;
                }

                if (size_bins_left == coll_op_cnt) break;
            }
        }
        assert(size_bins_left == coll_op_cnt);

        ssize_t tmp_size;
        int64_t tmp_agg_ndx = 1; /* start at aggregator 1 */
        int64_t tmp_agg_cnt;
        int64_t tmp_common_cnts[4];
        int64_t tmp_size_bins[10];
        memcpy(tmp_common_cnts, common_access_counts, 4 * sizeof(int64_t));
        memcpy(tmp_size_bins, agg_size_bins, 10 * sizeof(int64_t));
        tmp_cnt = coll_op_cnt;
        agg_off = io_context->next_off;

        while (tmp_cnt)
        {
            start_ndx = -1;
            end_ndx = -1;
            leftover = 0;
            tmp_agg_cnt = MIN(tmp_cnt, agg_cnt);
        
            for (i = 9; i >= 0; i--)
            {
                if (tmp_size_bins[i])
                {
                    if (start_ndx == -1) start_ndx = i;

                    tmp_agg_cnt -= tmp_size_bins[i];
                    if (tmp_agg_cnt <= 0)
                    {
                        end_ndx = i;
                        break;
                    }
                }
            }

            i = start_ndx;
            tmp_agg_cnt = MIN(tmp_cnt, agg_cnt);
            while (tmp_agg_cnt)
            {
                if ((tmp_size_bins[i] >= tmp_agg_cnt) && !leftover)
                {
                    switch_cnt = tmp_agg_cnt;
                }
                else if (tmp_size_bins[i])
                {
                    bins_to_use = MIN(tmp_size_bins[i], tmp_agg_cnt - leftover);
                    switch_cnt = ceil((double)bins_to_use / (tmp_agg_cnt - bins_to_use));

                    leftover += (bins_to_use - switch_cnt);
                }
                else
                {
                    switch_cnt = 0;
                }

                /* assign i/o operations until we need to move to the next size bin */
                while (switch_cnt)
                {
                    /* assign access sizes, starting with common and falling back to default */
                    tmp_size = -1;
                    for (j = 0; j < 4; j++)
                    {
                        if (tmp_common_cnts[j] &&
                            (common_accesses[j] > size_bin_min_vals[i]) &&
                            (common_accesses[j] <= size_bin_max_vals[i]))
                        {
                            tmp_size = common_accesses[j];
                            tmp_common_cnts[j]--;
                            break;
                        }
                    }

                    if (tmp_size == -1)
                    {
                        tmp_size = ALIGN_BY_8((size_bin_max_vals[i] - size_bin_min_vals[i]) / 2);
                    }

                    /* only increment offset for aggregators less than me */
                    if (tmp_agg_ndx < agg_ndx)
                    {
                        agg_off += tmp_size;
                    }
                    io_context->next_off += tmp_size;

                    tmp_cnt--;
                    tmp_agg_cnt--;
                    switch_cnt--;
                    tmp_size_bins[i]--;
                    tmp_agg_ndx++;
                    if (tmp_agg_ndx > agg_cnt) tmp_agg_ndx = 1;
                }

                i--;
                if (i < end_ndx)
                {
                    i = start_ndx;
                    leftover = 0;
                }
            }
        }
    }

    /* assign an actual access size, according to already initialized agg size bins */
    *io_sz = 0;
    if (*total_io_size > 0)
    {
        tmp_cnt = agg_cnt;
        start_ndx = -1;
        end_ndx = -1;
        leftover = 0;
        int my_ndx = -1;

        for (i = 9; i >= 0; i--)
        {
            if (agg_size_bins[i])
            {
                if (start_ndx == -1) start_ndx = i;

                tmp_cnt -= agg_size_bins[i];
                if (tmp_cnt <= 0)
                {
                    end_ndx = i;
                    break;
                }
            }
        }

        i = start_ndx;
        tmp_cnt = agg_cnt;
        while (tmp_cnt)
        {
            if ((agg_size_bins[i] >= tmp_cnt) && !leftover)
            {
                switch_cnt = tmp_cnt;
            }
            else if (agg_size_bins[i])
            {
                bins_to_use = MIN(agg_size_bins[i], tmp_cnt - leftover);
                switch_cnt = ceil((double)bins_to_use / (tmp_cnt - bins_to_use));

                leftover += (bins_to_use - switch_cnt);
            }
            else
            {
                switch_cnt = 0;
            }

            if (switch_cnt)
            {
                if (my_ndx == -1)
                {
                    if (agg_ndx <= switch_cnt)
                        my_ndx = i;
                    else
                        agg_ndx -= switch_cnt;
                }

                agg_size_bins[i] -= switch_cnt;
                if (tmp_cnt > switch_cnt)
                    tmp_cnt -= switch_cnt;
                else
                    tmp_cnt = 0;
            }

            i--;
            if (i < end_ndx)
            {
                i = start_ndx;
                leftover = 0;
            }
        }
        assert((my_ndx >= 0) && (my_ndx < 10));

        /* first try a common access size */
        for (j = 0; j < 4; j++)
        {
            if (common_access_counts[j] &&
                (common_accesses[j] > size_bin_min_vals[my_ndx]) &&
                (common_accesses[j] <= size_bin_max_vals[my_ndx])) 
            {
                *io_sz = common_accesses[j];
                common_access_counts[j]--;
                break;
            }
        }

        /* if no common access size found, assign a default size */
        if (j == 4)
        {
            /* default size is the median of the range, aligned to be multiple of 8 */
            size_t gen_size = (size_bin_max_vals[my_ndx] - size_bin_min_vals[my_ndx]) / 2;
            *io_sz = ALIGN_BY_8(gen_size);
        }
    }
    *total_io_size -= *io_sz;

    /* decrement size bins counter */
    size_bins_left -= agg_cnt;
    if (size_bins_left < agg_ndx)
        size_bins_left = 0;

    /* we simply assign offsets sequentially through an aggregator's file view */
    *io_off = agg_off;
    agg_off += *io_sz;

    return;
}

static void determine_ind_io_params(
    struct darshan_file *file, int write_flag, size_t *io_sz, off_t *io_off,
    struct rank_io_context *io_context)
{
    int size_bin_ndx = 0;
    int64_t *rd_size_bins = &(file->counters[CP_SIZE_READ_0_100]);
    int64_t *wr_size_bins = &(file->counters[CP_SIZE_WRITE_0_100]);
    int64_t *size_bins = NULL;
    int64_t *common_accesses = &(file->counters[CP_ACCESS1_ACCESS]); /* 4 common accesses */
    int64_t *common_access_counts = &(file->counters[CP_ACCESS1_COUNT]); /* common access counts */
    int64_t *total_io_size = NULL;
    int i, j = 0;
    const int64_t size_bin_min_vals[10] = { 0, 100, 1024, 10 * 1024, 100 * 1024, 1024 * 1024,
                                            4 * 1024 * 1024, 10 * 1024 * 1024, 100 * 1024 * 1024,
                                            1024 * 1024 * 1024 };
    const int64_t size_bin_max_vals[10] = { 100, 1024, 10 * 1024, 100 * 1024, 1024 * 1024,
                                            4 * 1024 * 1024, 10 * 1024 * 1024, 100 * 1024 * 1024,
                                            1024 * 1024 * 1024, INT64_MAX };

    /* assign data values depending on whether the operation is a read or write */
    if (write_flag)
    {
        total_io_size = &(file->counters[CP_BYTES_WRITTEN]);
        size_bins = wr_size_bins;
    }
    else
    {
        total_io_size = &(file->counters[CP_BYTES_READ]);
        size_bins = rd_size_bins;
    }

    for (i = 0; i < 10; i++)
    {
        if (size_bins[i])
        {
            size_bin_ndx = i;
            break;
        }
    }

    *io_sz = 0;
    if (*total_io_size > 0)
    {
        /* try to assign a common access first (intelligently) */
        for (j = 0; j < 4; j++)
        {
            if (common_access_counts[j] &&
                (common_accesses[j] >= size_bin_min_vals[size_bin_ndx]) &&
                (common_accesses[j] <= size_bin_max_vals[size_bin_ndx]))
            {
                *io_sz = common_accesses[j];
                common_access_counts[j]--;
                break;
            }
        }

        /* if no common accesses left, then assign default size for this bin */
        if (*io_sz == 0)
        {
            size_t gen_size;
            gen_size = (size_bin_max_vals[size_bin_ndx] - size_bin_min_vals[size_bin_ndx]) / 2;
            *io_sz = ALIGN_BY_8(gen_size);
        }
        assert(*io_sz);
    }

    *total_io_size -= *io_sz;
    size_bins[size_bin_ndx]--;

    *io_off = io_context->next_off;
    io_context->next_off += *io_sz;

    return;
}

/* calculate the simulated "delay" between different i/o events using delay info
 * from the file counters */
static void calc_io_delays(
    struct darshan_file *file, int64_t num_opens, int64_t num_io_ops, double total_delay,
    double *first_io_delay, double *close_delay, double *inter_open_delay, double *inter_io_delay)
{
    double first_io_time, last_io_time;
    double first_io_pct, close_pct, inter_open_pct, inter_io_pct;
    double total_delay_pct;
    double tmp_inter_io_pct, tmp_inter_open_pct;

    if (total_delay > 0.0)
    {
        /* determine the time of the first io operation */
        if (!file->fcounters[CP_F_WRITE_START_TIMESTAMP])
            first_io_time = file->fcounters[CP_F_READ_START_TIMESTAMP];
        else if (!file->fcounters[CP_F_READ_START_TIMESTAMP])
            first_io_time = file->fcounters[CP_F_WRITE_START_TIMESTAMP];
        else if (file->fcounters[CP_F_READ_START_TIMESTAMP] <
                 file->fcounters[CP_F_WRITE_START_TIMESTAMP])
            first_io_time = file->fcounters[CP_F_READ_START_TIMESTAMP];
        else
            first_io_time = file->fcounters[CP_F_WRITE_START_TIMESTAMP];

        /* determine the time of the last io operation */
        if (file->fcounters[CP_F_READ_END_TIMESTAMP] > file->fcounters[CP_F_WRITE_END_TIMESTAMP])
            last_io_time = file->fcounters[CP_F_READ_END_TIMESTAMP];
        else
            last_io_time = file->fcounters[CP_F_WRITE_END_TIMESTAMP];

        /* no delay contribution for inter-open delay if there is only a single open */
        if (num_opens > 1)
            inter_open_pct = DEF_INTER_CYC_DELAY_PCT;

        /* no delay contribution for inter-io delay if there is one or less io op */
        if ((num_io_ops - num_opens) > 0)
            inter_io_pct = DEF_INTER_IO_DELAY_PCT;

        /* determine delay contribution for first io and close delays */
        if (first_io_time != 0.0)
        {
            first_io_pct = (first_io_time - file->fcounters[CP_F_OPEN_TIMESTAMP]) *
                           (num_opens / total_delay);
            close_pct = (file->fcounters[CP_F_CLOSE_TIMESTAMP] - last_io_time) *
                        (num_opens / total_delay);
        }
        else
        {
            first_io_pct = 0.0;
            close_pct = 1 - inter_open_pct;
        }

        /* adjust per open delay percentages using a simple heuristic */
        total_delay_pct = inter_open_pct + inter_io_pct + first_io_pct + close_pct;
        if ((total_delay_pct < 1) && (inter_open_pct || inter_io_pct))
        {
            /* only adjust inter-open and inter-io delays if we underestimate */
            tmp_inter_open_pct = (inter_open_pct / (inter_open_pct + inter_io_pct)) *
                                 (1 - first_io_pct - close_pct);
            tmp_inter_io_pct = (inter_io_pct / (inter_open_pct + inter_io_pct)) *
                               (1 - first_io_pct - close_pct);
            inter_open_pct = tmp_inter_open_pct;
            inter_io_pct = tmp_inter_io_pct;
        }
        else
        {
            inter_open_pct += (inter_open_pct / total_delay_pct) * (1 - total_delay_pct);
            inter_io_pct += (inter_io_pct / total_delay_pct) * (1 - total_delay_pct);
            first_io_pct += (first_io_pct / total_delay_pct) * (1 - total_delay_pct);
            close_pct += (close_pct / total_delay_pct) * (1 - total_delay_pct);
        }

        *first_io_delay = (first_io_pct * total_delay) / num_opens;
        *close_delay = (close_pct * total_delay) / num_opens;

        if (num_opens > 1)
            *inter_open_delay = (inter_open_pct * total_delay) / (num_opens - 1);
        if ((num_io_ops - num_opens) > 0)
            *inter_io_delay = (inter_io_pct * total_delay) / (num_io_ops - num_opens);
    }

    return;
}

/* check to make sure file stats are valid and properly formatted */
static void file_sanity_check(
    struct darshan_file *file, struct darshan_job *job)
{
    int64_t ops_not_implemented;
    int64_t ops_total;

    /* make sure we have log version 2.03 or greater */
    if (strcmp(job->version_string, "2.03") < 0)
    {
        fprintf(stderr, "Error: Darshan log version must be >= 2.03 (using %s)\n",
                job->version_string);
        exit(EXIT_FAILURE);
    }

    /* these counters should not be negative */
    assert(file->counters[CP_POSIX_OPENS] >= 0);
    assert(file->counters[CP_COLL_OPENS] >= 0);
    assert(file->counters[CP_POSIX_READS] >= 0);
    assert(file->counters[CP_POSIX_WRITES] >= 0);
    assert(file->counters[CP_BYTES_READ] >= 0);
    assert(file->counters[CP_BYTES_WRITTEN] >= 0);
    assert(file->counters[CP_RW_SWITCHES] >= 0);

    /* set any timestamps that happen to be negative to 0 */
    if (file->fcounters[CP_F_READ_START_TIMESTAMP] < 0.0)
        file->fcounters[CP_F_READ_START_TIMESTAMP] = 0.0;
    if (file->fcounters[CP_F_WRITE_START_TIMESTAMP] < 0.0)
        file->fcounters[CP_F_WRITE_START_TIMESTAMP] = 0.0;
    if (file->fcounters[CP_F_READ_END_TIMESTAMP] < 0.0)
        file->fcounters[CP_F_READ_END_TIMESTAMP] = 0.0;
    if (file->fcounters[CP_F_WRITE_END_TIMESTAMP] < 0.0)
        file->fcounters[CP_F_WRITE_END_TIMESTAMP] = 0.0;

    /* set file close time to the end of execution if it is not given */
    if (file->fcounters[CP_F_CLOSE_TIMESTAMP] == 0.0)
        file->fcounters[CP_F_CLOSE_TIMESTAMP] = job->end_time - job->start_time + 1;

    /* collapse fopen/fread/etc. calls into the corresponding open/read/etc. counters */
    file->counters[CP_POSIX_OPENS] += file->counters[CP_POSIX_FOPENS];
    file->counters[CP_POSIX_READS] += file->counters[CP_POSIX_FREADS];
    file->counters[CP_POSIX_WRITES] += file->counters[CP_POSIX_FWRITES];
    file->counters[CP_POSIX_SEEKS] += file->counters[CP_POSIX_FSEEKS];

    /* reduce total meta time by percentage of ops not currently implemented */
    /* NOTE: we lump fseeks and r/w operations together when setting op durations ... */
    ops_not_implemented = file->counters[CP_POSIX_STATS] + file->counters[CP_POSIX_FSYNCS];
    ops_total = ops_not_implemented + (2 * file->counters[CP_POSIX_OPENS]) +
                file->counters[CP_POSIX_SEEKS];
    file->fcounters[CP_F_POSIX_META_TIME] *= (1 - ((double)ops_not_implemented / ops_total));

    return;
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
