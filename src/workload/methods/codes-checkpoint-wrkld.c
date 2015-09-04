/*
 * Copyright (C) 2015 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <assert.h>

#include "codes/quickhash.h"

#include "codes/codes-workload.h"

#define CHECKPOINT_HASH_TABLE_SIZE 251

#define DEFAULT_WR_BUF_SIZE (16 * 1024 * 1024)   /* 16 MiB default */

enum checkpoint_status
{
    CHECKPOINT_COMPUTE,
    CHECKPOINT_OPEN_FILE,
    CHECKPOINT_WRITE,
    CHECKPOINT_CLOSE_FILE,
    CHECKPOINT_INACTIVE,
};

static void * checkpoint_workload_read_config(
        ConfigHandle *handle,
        char const * section_name,
        char const * annotation,
        int num_ranks);
static int checkpoint_workload_load(const char* params, int app_id, int rank);
static void checkpoint_workload_get_next(int app_id, int rank, struct codes_workload_op *op);

static int checkpoint_state_compare(void *key, struct qhash_head *link);

/* TODO: fpp or shared file, or option? affects offsets and file ids */

/* state for each process participating in this checkpoint */
struct checkpoint_state
{
    int rank;
    int app_id;
    enum checkpoint_status status;
    /* optimal interval to checkpoint (s) */
    double checkpoint_interval;
    /* how much this rank contributes to checkpoint (bytes) */
    long long io_per_checkpoint;
    /* which checkpointing iteration are we on */
    int checkpoint_number;
    /* how much we have checkpointed to file in current iteration (bytes) */
    long long cur_checkpoint_sz;
    /* how many remaining iterations of compute/checkpoint phases are there */
    int remaining_iterations;
    struct qhash_head hash_link;
};

struct checkpoint_id
{
    int rank;
    int app_id;
};

static struct qhash_table *chkpoint_state_tbl = NULL;
static int chkpoint_tbl_pop = 0;

/* function pointers for this method */
struct codes_workload_method checkpoint_workload_method = 
{
    .method_name = "checkpoint_io_workload",
    .codes_workload_read_config = &checkpoint_workload_read_config,
    .codes_workload_load = &checkpoint_workload_load,
    .codes_workload_get_next = &checkpoint_workload_get_next,
};

static void * checkpoint_workload_read_config(
        ConfigHandle *handle,
        char const * section_name,
        char const * annotation,
        int num_ranks)
{
    checkpoint_wrkld_params *p = malloc(sizeof(*p));
    assert(p);

    int rc;

    rc = configuration_get_value_double(&config, section_name, "checkpoint_sz",
            annotation, &p->checkpoint_sz);
    assert(!rc);

    rc = configuration_get_value_double(&config, section_name,
            "checkpoint_wr_bw", annotation, &p->checkpoint_wr_bw);
    assert(!rc);

    rc = configuration_get_value_double(&config, section_name, "app_run_time",
            annotation, &p->app_runtime);
    assert(!rc);

    rc = configuration_get_value_double(&config, section_name, "mtti",
            annotation, &p->mtti);
    assert(!rc);

    p->nprocs = num_ranks;

    return p;
}

static int checkpoint_workload_load(const char* params, int app_id, int rank)
{
    checkpoint_wrkld_params *c_params = (checkpoint_wrkld_params *)params;
    struct checkpoint_state* new_state;
    double checkpoint_wr_time;
    double checkpoint_phase_time;
    struct checkpoint_id this_chkpoint_id;

    if (!c_params)
        return(-1);

    /* TODO: app_id unsupported? */
    APP_ID_UNSUPPORTED(app_id, "checkpoint_workload")

    if (!chkpoint_state_tbl)
    {
        chkpoint_state_tbl = qhash_init(checkpoint_state_compare,
            quickhash_64bit_hash, CHECKPOINT_HASH_TABLE_SIZE);
        if(!chkpoint_state_tbl)
            return(-1);
    }

    new_state = (struct checkpoint_state*)malloc(sizeof(*new_state));
    if (!new_state)
        return(-1);

    new_state->rank = rank;
    new_state->app_id = app_id;
    new_state->status = CHECKPOINT_COMPUTE;
    new_state->checkpoint_number = 0;

    /* calculate the time (in seconds) taken to write the checkpoint to file */
    checkpoint_wr_time = (c_params->checkpoint_sz * 1024) /* checkpoint size (GiB) */
        / c_params->checkpoint_wr_bw; /* write bw (GiB/s) */

    /* set the optimal checkpoint interval (in seconds), according to the
     * equation given in the conclusion of Daly's "A higher order estimate
     * of the optimum checkpoint interval for restart dumps"
     */
    new_state->checkpoint_interval =
        sqrt(2 * checkpoint_wr_time * (c_params->mtti * 60 * 60))
            - checkpoint_wr_time;

    /* calculate how many bytes each rank contributes to total checkpoint */
    new_state->io_per_checkpoint = (c_params->checkpoint_sz * pow(1024, 4))
        / c_params->nprocs;

    /* calculate how many iterations based on how long the app should run for
     * and how long it takes to compute + checkpoint the file
     */
    checkpoint_phase_time = checkpoint_wr_time + new_state->checkpoint_interval;
    new_state->remaining_iterations =
        round(c_params->app_runtime / (checkpoint_phase_time / 60 / 60));

    /* add state for this checkpoint to hash table */
    this_chkpoint_id.rank = rank;
    this_chkpoint_id.app_id = app_id;
    qhash_add(chkpoint_state_tbl, &this_chkpoint_id, &(new_state->hash_link));
    chkpoint_tbl_pop++;

    return(0);
}

/* find the next workload operation to issue for this rank */
static void checkpoint_workload_get_next(int app_id, int rank, struct codes_workload_op *op)
{
    struct qhash_head *hash_link = NULL;
    struct checkpoint_state *this_state = NULL;
    struct checkpoint_id tmp;
    long long remaining;

    /* find the checkpoint state for this rank/app_id combo */
    tmp.rank = rank;
    tmp.app_id = app_id;
    hash_link = qhash_search(chkpoint_state_tbl, &tmp);
    if (!hash_link)
    {
        fprintf(stderr, "No checkpoint context found for rank %d (app_id = %d)\n",
            rank, app_id);
        op->op_type = CODES_WK_END;
        return;
    }
    this_state = qhash_entry(hash_link, struct checkpoint_state, hash_link);
    assert(this_state);

    switch (this_state->status)
    {
        case CHECKPOINT_COMPUTE:
            /* the workload is just starting or the previous checkpoint
             * file was just closed, so we start the next computation
             * cycle, with duration == checkpoint_interval time
             */

            /* set delay operation parameters */
            op->op_type = CODES_WK_DELAY;
            op->u.delay.seconds = this_state->checkpoint_interval;

            /* set the next status */
            this_state->status = CHECKPOINT_OPEN_FILE;
            break;
        case CHECKPOINT_OPEN_FILE:
            /* we just finished a computation phase, so we need to
             * open the next checkpoint file to start a dump */
            /* TODO: do we synchronize before opening? */
            /* TODO: how do we get unique file_ids for different ranks, apps, and checkpoint iterations */

            /* set open parameters */
            op->op_type = CODES_WK_OPEN;
            op->u.open.file_id = this_state->checkpoint_number;
            op->u.open.create_flag = 1;

            /* set the next status */
            this_state->cur_checkpoint_sz = 0;
            this_state->status = CHECKPOINT_WRITE;
            break;
        case CHECKPOINT_WRITE:
            /* we just opened the checkpoint file, so we being looping
             * over the write operations we need to perform
             */
            remaining = this_state->io_per_checkpoint
                - this_state->cur_checkpoint_sz;

            /* set write parameters */
            op->op_type = CODES_WK_WRITE;
            op->u.write.file_id = this_state->checkpoint_number;
            op->u.write.offset = this_state->cur_checkpoint_sz;
            if (remaining >= DEFAULT_WR_BUF_SIZE)
                op->u.write.size = DEFAULT_WR_BUF_SIZE;
            else
                op->u.write.size = remaining;

            /* set the next status -- only advance to checkpoint
             * file close if we have completed the checkpoint
             * writing phase
             */
            this_state->cur_checkpoint_sz += op->u.write.size;
            if (this_state->cur_checkpoint_sz == this_state->io_per_checkpoint)
                this_state->status = CHECKPOINT_CLOSE_FILE;
            break;
        case CHECKPOINT_CLOSE_FILE:
            /* we just completed a checkpoint writing phase, so we need to
             * close the current checkpoint file
             */

            /* set close parameters */
            op->op_type = CODES_WK_CLOSE;
            op->u.close.file_id = this_state->checkpoint_number;

            /* set the next status -- if there are more iterations to
             * be completed, start the next compute/checkpoint phase;
             * otherwise, end the workload
             */
            this_state->remaining_iterations--;
            this_state->checkpoint_number++;
            if (this_state->remaining_iterations == 0)
            {
                this_state->status = CHECKPOINT_INACTIVE;
            }
            else
            {
                this_state->status = CHECKPOINT_COMPUTE;
            }
            break;
        case CHECKPOINT_INACTIVE:
            /* all compute checkpoint iterations complete, so just end
             * the workload
             */
            op->op_type = CODES_WK_END;

            /* remove hash entry */
            qhash_del(hash_link);
            free(this_state);
            chkpoint_tbl_pop--;
            if (!chkpoint_tbl_pop)
            {
                qhash_finalize(chkpoint_state_tbl);
                chkpoint_state_tbl = NULL;
            }
            break;
        default:
            fprintf(stderr, "Invalid checkpoint workload status for "
                "rank %d (app_id = %d)\n", rank, app_id);
            op->op_type = CODES_WK_END;
            return;
    }

    return;
}

static int checkpoint_state_compare(void *key, struct qhash_head *link)
{
    struct checkpoint_id *in = (struct checkpoint_id *)key;
    struct checkpoint_state *tmp = NULL;

    tmp = qhash_entry(link, struct checkpoint_state, hash_link);
    if ((tmp->rank == in->rank) && (tmp->app_id == in->app_id))
        return(1);

    return(0);
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
