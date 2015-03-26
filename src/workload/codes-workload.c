/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#include <assert.h>

#include "ross.h"
#include "codes/codes-workload.h"
#include "codes-workload-method.h"

/* list of available methods.  These are statically compiled for now, but we
 * could make generators optional via autoconf tests etc. if needed
 */
extern struct codes_workload_method test_workload_method;
extern struct codes_workload_method iolang_workload_method;
extern struct codes_workload_method scala_trace_workload_method;
#ifdef USE_DUMPI
extern struct codes_workload_method dumpi_trace_workload_method;
#endif
#ifdef USE_DARSHAN
extern struct codes_workload_method darshan_io_workload_method;
#endif
#ifdef USE_RECORDER
extern struct codes_workload_method recorder_io_workload_method;
#endif

static struct codes_workload_method *method_array[] =
{
    &test_workload_method,
    &iolang_workload_method,
    &scala_trace_workload_method,
#ifdef USE_DUMPI
    &dumpi_trace_workload_method,
#endif
#ifdef USE_DARSHAN
    &darshan_io_workload_method,
#endif
#ifdef USE_RECORDER
    &recorder_io_workload_method,
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
    struct codes_workload_op op;
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

int codes_workload_load(const char* type, const char* params, int rank)
{
    int i;
    int ret;
    struct rank_queue *tmp;

    for(i=0; method_array[i] != NULL; i++)
    {
        if(strcmp(method_array[i]->method_name, type) == 0)
        {
            /* load appropriate workload generator */
            ret = method_array[i]->codes_workload_load(params, rank);
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
                tmp = (struct rank_queue*)malloc(sizeof(*tmp));
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

void codes_workload_get_next(int wkld_id, int rank, struct codes_workload_op *op)
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
    //printf("codes_workload_get_next issuing new operation.\n");
    method_array[wkld_id]->codes_workload_get_next(rank, op);

    return;
}

void codes_workload_get_next_rc(int wkld_id, int rank, const struct codes_workload_op *op)
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

    tmp_op = (struct rc_op*)malloc(sizeof(*tmp_op));
    assert(tmp_op);
    tmp_op->op = *op;
    tmp_op->next = tmp->lifo;
    tmp->lifo = tmp_op;

    return;
}

int codes_workload_get_rank_cnt(const char* type, const char* params)
{
    int i;
    int rank_cnt;

    for(i=0; method_array[i] != NULL; i++)
    {
        if(strcmp(method_array[i]->method_name, type) == 0)
        {
            rank_cnt = method_array[i]->codes_workload_get_rank_cnt(params);
            assert(rank_cnt > 0);
            return(rank_cnt);
        }
    }

    fprintf(stderr, "Error: failed to find workload generator %s\n", type);
    return(-1);
}

void codes_workload_print_op(FILE *f, struct codes_workload_op *op, int rank){
    switch(op->op_type){
        case CODES_WK_END:
            fprintf(f, "op: rank:%d type:end\n", rank);
            break;
        case CODES_WK_DELAY:
            fprintf(f, "op: rank:%d type:delay seconds:%lf\n",
                    rank, op->u.delay.seconds);
            break;
        case CODES_WK_BARRIER:
            fprintf(f, "op: rank:%d type:barrier count:%d root:%d\n",
                    rank, op->u.barrier.count, op->u.barrier.root);
            break;
        case CODES_WK_OPEN:
            fprintf(f, "op: rank:%d type:open file_id:%lu flag:%d\n",
                    rank, op->u.open.file_id, op->u.open.create_flag);
            break;
        case CODES_WK_CLOSE:
            fprintf(f, "op: rank:%d type:close file_id:%lu\n",
                    rank, op->u.close.file_id);
            break;
        case CODES_WK_WRITE:
            fprintf(f, "op: rank:%d type:write "
                       "file_id:%lu off:%lu size:%lu\n",
                    rank, op->u.write.file_id, op->u.write.offset,
                    op->u.write.size);
            break;
        case CODES_WK_READ:
            fprintf(f, "op: rank:%d type:read "
                       "file_id:%lu off:%lu size:%lu\n",
                    rank, op->u.read.file_id, op->u.read.offset,
                    op->u.read.size);
            break;
        case CODES_WK_SEND:
            fprintf(f, "op: rank:%d type:send "
                    "src:%d dst:%d bytes:%d type:%d count:%d tag:%d\n",
                    rank,
                    op->u.send.source_rank, op->u.send.dest_rank,
                    op->u.send.num_bytes, op->u.send.data_type,
                    op->u.send.count, op->u.send.tag);
            break;
        case CODES_WK_RECV:
            fprintf(f, "op: rank:%d type:recv "
                    "src:%d dst:%d bytes:%d type:%d count:%d tag:%d\n",
                    rank,
                    op->u.recv.source_rank, op->u.recv.dest_rank,
                    op->u.recv.num_bytes, op->u.recv.data_type,
                    op->u.recv.count, op->u.recv.tag);
            break;
        case CODES_WK_ISEND:
            fprintf(f, "op: rank:%d type:isend "
                    "src:%d dst:%d bytes:%d type:%d count:%d tag:%d\n",
                    rank,
                    op->u.send.source_rank, op->u.send.dest_rank,
                    op->u.send.num_bytes, op->u.send.data_type,
                    op->u.send.count, op->u.send.tag);
            break;
        case CODES_WK_IRECV:
            fprintf(f, "op: rank:%d type:irecv "
                    "src:%d dst:%d bytes:%d type:%d count:%d tag:%d\n",
                    rank,
                    op->u.recv.source_rank, op->u.recv.dest_rank,
                    op->u.recv.num_bytes, op->u.recv.data_type,
                    op->u.recv.count, op->u.recv.tag);
            break;
        case CODES_WK_BCAST:
            fprintf(f, "op: rank:%d type:bcast "
                    "bytes:%d\n", rank, op->u.collective.num_bytes);
            break;
        case CODES_WK_ALLGATHER:
            fprintf(f, "op: rank:%d type:allgather "
                    "bytes:%d\n", rank, op->u.collective.num_bytes);
            break;
        case CODES_WK_ALLGATHERV:
            fprintf(f, "op: rank:%d type:allgatherv "
                    "bytes:%d\n", rank, op->u.collective.num_bytes);
            break;
        case CODES_WK_ALLTOALL:
            fprintf(f, "op: rank:%d type:alltoall "
                    "bytes:%d\n", rank, op->u.collective.num_bytes);
            break;
        case CODES_WK_ALLTOALLV:
            fprintf(f, "op: rank:%d type:alltoallv "
                    "bytes:%d\n", rank, op->u.collective.num_bytes);
            break;
        case CODES_WK_REDUCE:
            fprintf(f, "op: rank:%d type:reduce "
                    "bytes:%d\n", rank, op->u.collective.num_bytes);
            break;
        case CODES_WK_ALLREDUCE:
            fprintf(f, "op: rank:%d type:allreduce "
                    "bytes:%d\n", rank, op->u.collective.num_bytes);
            break;
        case CODES_WK_COL:
            fprintf(f, "op: rank:? type:collective "
                    "bytes:%d\n", op->u.collective.num_bytes);
            break;
	case CODES_WK_WAITALL:
	    fprintf(f, "op: rank:? type:waitall "
                     "num reqs: :%d\n", op->u.waits.count);
	    break;
	case CODES_WK_WAIT:
	    fprintf(f, "op: rank:? type:wait "
                     "num reqs: :%d\n", op->u.wait.req_id);
	    break;
	case CODES_WK_WAITSOME:
	    fprintf(f, "op: rank:? type:waitsome "
                     "num reqs: :%d\n", op->u.waits.count);
	    break;
	case CODES_WK_WAITANY:
	    fprintf(f, "op: rank:? type:waitany "
                     "num reqs: :%d\n", op->u.waits.count);
	    break;
        case CODES_WK_IGNORE:
            break;
        default:
            fprintf(stderr,
                    "%s:%d: codes_workload_print_op: unrecognized workload type "
                    "(op code %d)\n", __FILE__, __LINE__, op->op_type);
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
