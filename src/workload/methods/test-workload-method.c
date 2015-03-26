/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

/* Example of a workload generator that plugs into the codes workload
 * generator API.  This example just produces a hard-coded pattern for
 * testing/validation purposes.
 */

#include <assert.h>

#include "ross.h"
#include "codes/codes-workload.h"
#include "src/workload/codes-workload-method.h"

static int test_workload_load(const char* params, int rank);
static void test_workload_get_next(int rank, struct codes_workload_op *op);

/* state information for each rank that is retrieving requests */
struct wkload_stream_state
{
    int rank;
    struct wkload_stream_state* next;
    struct codes_workload_op op_array[16];
    int op_array_len;
    int op_array_index;
};

struct wkload_stream_state* wkload_streams = NULL;

/* fill in function pointers for this method */
struct codes_workload_method test_workload_method = 
{
    .method_name = "test",
    .codes_workload_load = test_workload_load,
    .codes_workload_get_next = test_workload_get_next,
};

static int test_workload_load(const char* params, int rank)
{
    /* no params in this case; this example will work with any number of
     * ranks
     */
    struct wkload_stream_state* newv;

    newv = (struct wkload_stream_state*)malloc(sizeof(*newv));
    if(!newv)
        return(-1);

    newv->rank = rank;

    /* arbitrary synthetic workload for testing purposes */
    /* rank 0 sleeps 43 seconds, then does open and barrier, while all other
     * ranks immediately do open and barrier
     */
    if(rank == 0)
    {
        newv->op_array_len = 3;
        newv->op_array_index = 0;
        newv->op_array[0].op_type = CODES_WK_DELAY;
        newv->op_array[0].u.delay.seconds = 43;
        newv->op_array[1].op_type = CODES_WK_OPEN;
        newv->op_array[1].u.open.file_id = 3;
        newv->op_array[1].u.open.create_flag = 1;
        newv->op_array[2].op_type = CODES_WK_BARRIER;
        newv->op_array[2].u.barrier.root = 0;
        newv->op_array[2].u.barrier.count = -1; /* all ranks */
    }
    else
    {
        newv->op_array_len = 2;
        newv->op_array_index = 0;
        newv->op_array[0].op_type = CODES_WK_OPEN;
        newv->op_array[0].u.open.file_id = 3;
        newv->op_array[0].u.open.create_flag = 1;
        newv->op_array[1].op_type = CODES_WK_BARRIER;
        newv->op_array[1].u.barrier.root = 0;
        newv->op_array[1].u.barrier.count = -1; /* all ranks */
    }

    /* add to front of list of streams that we are tracking */
    newv->next = wkload_streams;
    wkload_streams = newv;

    return(0);
}

/* find the next workload operation to issue for this rank */
static void test_workload_get_next(int rank, struct codes_workload_op *op)
{
    struct wkload_stream_state* tmp = wkload_streams;
    struct wkload_stream_state* tmp2 = wkload_streams;

    /* find stream associated with this rank */
    while(tmp)
    {
        if(tmp->rank == rank)
            break;
        tmp = tmp->next;
    }

    if(!tmp)
    {
        op->op_type = CODES_WK_END;
        return;
    }

    assert(tmp->rank == rank);

    if(tmp->op_array_index < tmp->op_array_len)
    {
        *op = tmp->op_array[tmp->op_array_index];
        tmp->op_array_index++;
    }
    else
    {
        /* no more operations */
        op->op_type = CODES_WK_END;

        /* destroy this instance */
        if(wkload_streams == tmp)
        {
            wkload_streams = tmp->next;
        }
        else
        {
            while(tmp2)
            {
                if(tmp2->next == tmp)
                {
                    tmp2->next = tmp->next;
                    break;
                }
                tmp2 = tmp2->next;
            }
        }

        free(tmp);
    }

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
