/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#include <assert.h>

#include "ross.h"
#include "codes/codes-nw-workload.h"
#include "codes-nw-workload-method.h"

/* list of available methods.  These are statically compiled for now, but we
 * could make generators optional via autoconf tests etc. if needed
 */
extern struct codes_nw_workload_method scala_trace_workload_method;
#ifdef USE_DUMPI
extern struct codes_nw_workload_method dumpi_trace_workload_method;
#endif

static struct codes_nw_workload_method *method_array[] =
{
    &scala_trace_workload_method,
#ifdef USE_DUMPI
    &dumpi_trace_workload_method,
#endif
    NULL};

/* This shim layer is responsible for queueing up reversed operations and
 * re-issuing them so that the underlying workload generator method doesn't
 * have to worry about reverse events.
 *
 * NOTE: we could make this faster with a smarter data structure.  For now
 * we just have a linked list of rank_queue structs, one per rank that has
 * opened the workload.  We then have a linked list off of each of those
 * to hold a lifo queue of operations that have been reversed for that rank.
 */

/* holds an operation that has been reversed */
struct rc_op
{
    struct mpi_event_list op;
    struct rc_op* next;
};

/* tracks lifo queue of reversed operations for a given rank */
struct rank_queue
{
    int rank;
    struct rc_op *lifo;
    struct rank_queue *next;
};

static struct rank_queue *ranks = NULL;

int codes_nw_workload_load(const char* type, const char* params, int rank)
{
    int i;
    int ret;
    struct rank_queue *tmp;

    for(i=0; method_array[i] != NULL; i++)
    {
        if(strcmp(method_array[i]->method_name, type) == 0)
        {
            /* load appropriate workload generator */
            ret = method_array[i]->codes_nw_workload_load(params, rank);
            if(ret < 0)
            {
                return(-1);
            }

            /* are we tracking information for this rank yet? */
            tmp = ranks;
            while(tmp)
            {
                if(tmp->rank == rank)
                    break;
                tmp = tmp->next;
            }
            if(tmp == NULL)
            {
                tmp = malloc(sizeof(*tmp));
                assert(tmp);
                tmp->rank = rank;
                tmp->lifo = NULL;
                tmp->next = ranks;
                ranks = tmp;
            }

            return(i);
        }
    }

    fprintf(stderr, "Error: failed to find workload generator %s\n", type);
    return(-1);
}

void codes_nw_workload_get_next(int wkld_id, int rank, struct mpi_event_list *op)
{
    struct rank_queue *tmp;
    struct rc_op *tmp_op;

    /* first look to see if we have a reversed operation that we can
     * re-issue
     */
    tmp = ranks;
    while(tmp)
    {
        if(tmp->rank == rank)
            break;
        tmp = tmp->next;
    }
    assert(tmp);
    if(tmp->lifo)
    {
        tmp_op = tmp->lifo;
        tmp->lifo = tmp_op->next;

        *op = tmp_op->op;
        free(tmp_op);
        //printf("codes_workload_get_next re-issuing reversed operation.\n");
        return;
    }

    /* ask generator for the next operation */
    //printf("codes_workload_get_next issuing new operation rank %d %d.\n", rank, wkld_id);
    method_array[wkld_id]->codes_nw_workload_get_next(rank, op);

    return;
}

void codes_nw_workload_get_next_rc(int wkld_id, int rank, const struct mpi_event_list *op)
{
    struct rank_queue *tmp;
    struct rc_op *tmp_op;

    tmp = ranks;
    while(tmp)
    {
        if(tmp->rank == rank)
            break;
        tmp = tmp->next;
    }
    assert(tmp);

    tmp_op = malloc(sizeof(*tmp_op));
    assert(tmp_op);
    tmp_op->op = *op;
    tmp_op->next = tmp->lifo;
    tmp->lifo = tmp_op;

    return;
}

void codes_nw_workload_print_op(FILE *f, struct mpi_event_list *op, int rank){
    switch(op->op_type){
        case CODES_NW_END:
            fprintf(f, "op: rank:%d type:end\n", rank);
            break;
        case CODES_NW_DELAY:
            fprintf(f, "op: rank:%d type:delay nsecs:%f \n",
                    rank, op->u.delay.nsecs);
            break;
        case CODES_NW_SEND:
	case CODES_NW_ISEND:
            fprintf(f, "op: rank:%d type:send "
                       "sender: %d receiver: %d number of bytes: %d "
			"start time: %f end time: %f \n",
                    rank, op->u.send.source_rank, op->u.send.dest_rank,
                    op->u.send.num_bytes,
		    op->start_time, op->end_time);
            break;
        case CODES_NW_RECV:
	case CODES_NW_IRECV:
            fprintf(f, "op: rank:%d type:recv "
                       "sender: %d receiver: %d number of bytes: %d "
			"start time: %f end time: %f  \n",
                    rank, op->u.recv.source_rank, op->u.recv.dest_rank,
                    op->u.recv.num_bytes,
		    op->start_time, op->end_time);
            break;
	case CODES_NW_COL:
	case CODES_NW_BCAST:
	case CODES_NW_ALLGATHER:
	case CODES_NW_ALLGATHERV:
	case CODES_NW_ALLTOALL:
	case CODES_NW_ALLTOALLV:
	case CODES_NW_REDUCE:
	case CODES_NW_ALLREDUCE:
            fprintf(f, "op: rank:%d type:collective "
                       "count: %d \n",
                    rank, op->u.collective.num_bytes);
            break;	
	/*case CODES_NW_TEST:
		fprintf(f, "op: rank:%d type:test "
			"request ID: %d flag: %d "
			"start time: %f end time: %f \n",
			rank, (int)op->u.test.request, op->u.test.flag,
			 op->start_time, op->end_time);
	    break; */
	/*case CODES_NW_WAITALL:
                fprintf(f, "op: rank:%d type:waitall "
                        "count: %d "
                        "start time: %f end time: %f \n",
                        rank, op->u.wait_all.count, 
                         op->start_time, op->end_time);
	break;*/
    }
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
