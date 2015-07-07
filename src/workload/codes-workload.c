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
    int app;
    int rank;
    struct rc_op *lifo;
    struct rank_queue *next;
};

static struct rank_queue *ranks = NULL;

int codes_workload_load(
        const char* type,
        const char* params,
        int app_id,
        int rank)
{
    int i;
    int ret;
    struct rank_queue *tmp;

    for(i=0; method_array[i] != NULL; i++)
    {
        if(strcmp(method_array[i]->method_name, type) == 0)
        {
            /* load appropriate workload generator */
            ret = method_array[i]->codes_workload_load(params, app_id, rank);
            if(ret < 0)
            {
                return(-1);
            }

            /* are we tracking information for this rank yet? */
            tmp = ranks;
            while(tmp)
            {
                if(tmp->rank == rank && tmp->app == app_id)
                    break;
                tmp = tmp->next;
            }
            if(tmp == NULL)
            {
                tmp = (struct rank_queue*)malloc(sizeof(*tmp));
                assert(tmp);
                tmp->app  = app_id;
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

void codes_workload_get_next(
        int wkld_id,
        int app_id,
        int rank,
        struct codes_workload_op *op)
{
    struct rank_queue *tmp;
    struct rc_op *tmp_op;

    /* first look to see if we have a reversed operation that we can
     * re-issue
     */
    tmp = ranks;
    while(tmp)
    {
        if(tmp->rank == rank && tmp->app == app_id)
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
        return;
    }

    /* ask generator for the next operation */
    method_array[wkld_id]->codes_workload_get_next(app_id, rank, op);

    return;
}

void codes_workload_get_next_rc(
        int wkld_id,
        int app_id,
        int rank,
        const struct codes_workload_op *op)
{
    struct rank_queue *tmp;
    struct rc_op *tmp_op;

    tmp = ranks;
    while(tmp)
    {
        if(tmp->rank == rank && tmp->app == app_id)
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

int codes_workload_get_rank_cnt(
        const char* type,
        const char* params,
        int app_id)
{
    int i;
    int rank_cnt;

    for(i=0; method_array[i] != NULL; i++)
    {
        if(strcmp(method_array[i]->method_name, type) == 0)
        {
            rank_cnt =
                method_array[i]->codes_workload_get_rank_cnt(params, app_id);
            assert(rank_cnt > 0);
            return(rank_cnt);
        }
    }

    fprintf(stderr, "Error: failed to find workload generator %s\n", type);
    return(-1);
}

void codes_workload_print_op(
        FILE *f,
        struct codes_workload_op *op,
        int app_id,
        int rank)
{
    switch(op->op_type){
        case CODES_WK_END:
            fprintf(f, "op: app:%d rank:%d type:end\n", app_id, rank);
            break;
        case CODES_WK_DELAY:
            fprintf(f, "op: app:%d rank:%d type:delay seconds:%lf\n",
                    app_id, rank, op->u.delay.seconds);
            break;
        case CODES_WK_BARRIER:
            fprintf(f, "op: app:%d rank:%d type:barrier count:%d root:%d\n",
                    app_id, rank, op->u.barrier.count, op->u.barrier.root);
            break;
        case CODES_WK_OPEN:
            fprintf(f, "op: app:%d rank:%d type:open file_id:%lu flag:%d\n",
                    app_id, rank, op->u.open.file_id, op->u.open.create_flag);
            break;
        case CODES_WK_CLOSE:
            fprintf(f, "op: app:%d rank:%d type:close file_id:%lu\n",
                    app_id, rank, op->u.close.file_id);
            break;
        case CODES_WK_WRITE:
            fprintf(f, "op: app:%d rank:%d type:write "
                       "file_id:%lu off:%lu size:%lu\n",
                    app_id, rank, op->u.write.file_id, op->u.write.offset,
                    op->u.write.size);
            break;
        case CODES_WK_READ:
            fprintf(f, "op: app:%d rank:%d type:read "
                       "file_id:%lu off:%lu size:%lu\n",
                    app_id, rank, op->u.read.file_id, op->u.read.offset,
                    op->u.read.size);
            break;
        case CODES_WK_SEND:
            fprintf(f, "op: app:%d rank:%d type:send "
                    "src:%d dst:%d bytes:%d type:%d count:%d tag:%d "
                    "start:%.5e end:%.5e\n",
                    app_id, rank,
                    op->u.send.source_rank, op->u.send.dest_rank,
                    op->u.send.num_bytes, op->u.send.data_type,
                    op->u.send.count, op->u.send.tag,
                    op->start_time, op->end_time);
            break;
        case CODES_WK_RECV:
            fprintf(f, "op: app:%d rank:%d type:recv "
                    "src:%d dst:%d bytes:%d type:%d count:%d tag:%d\n"
                    "start:%.5e end:%.5e\n",
                    app_id, rank,
                    op->u.recv.source_rank, op->u.recv.dest_rank,
                    op->u.recv.num_bytes, op->u.recv.data_type,
                    op->u.recv.count, op->u.recv.tag,
                    op->start_time, op->end_time);
            break;
        case CODES_WK_ISEND:
            fprintf(f, "op: app:%d rank:%d type:isend "
                    "src:%d dst:%d bytes:%d type:%d count:%d tag:%d\n"
                    "start:%.5e end:%.5e\n",
                    app_id, rank,
                    op->u.send.source_rank, op->u.send.dest_rank,
                    op->u.send.num_bytes, op->u.send.data_type,
                    op->u.send.count, op->u.send.tag,
                    op->start_time, op->end_time);
            break;
        case CODES_WK_IRECV:
            fprintf(f, "op: app:%d rank:%d type:irecv "
                    "src:%d dst:%d bytes:%d type:%d count:%d tag:%d\n"
                    "start:%.5e end:%.5e\n",
                    app_id, rank,
                    op->u.recv.source_rank, op->u.recv.dest_rank,
                    op->u.recv.num_bytes, op->u.recv.data_type,
                    op->u.recv.count, op->u.recv.tag,
                    op->start_time, op->end_time);
            break;
#define PRINT_COL(_type_str) \
            fprintf(f, "op: app:%d rank:%d type:%s" \
                    " bytes:%d, start:%.5e, end:%.5e\n", app_id, rank, \
                    _type_str, op->u.collective.num_bytes, op->start_time, \
                    op->end_time)
        case CODES_WK_BCAST:
            PRINT_COL("bcast");
            break;
        case CODES_WK_ALLGATHER:
            PRINT_COL("allgather");
            break;
        case CODES_WK_ALLGATHERV:
            PRINT_COL("allgatherv");
            break;
        case CODES_WK_ALLTOALL:
            PRINT_COL("alltoall");
            break;
        case CODES_WK_ALLTOALLV:
            PRINT_COL("alltoallv");
            break;
        case CODES_WK_REDUCE:
            PRINT_COL("reduce");
            break;
        case CODES_WK_ALLREDUCE:
            PRINT_COL("allreduce");
            break;
        case CODES_WK_COL:
            PRINT_COL("collective");
            break;
#undef PRINT_COL
#define PRINT_WAIT(_type_str, _ct) \
            fprintf(f, "op: app:%d rank:%d type:%s" \
                    "num reqs:%d, start:%.5e, end:%.5e\n", \
                    app_id, rank, _type_str, _ct, op->start_time, op->end_time)
        case CODES_WK_WAITALL:
            PRINT_WAIT("waitall", op->u.waits.count);
            break;
        case CODES_WK_WAIT:
            PRINT_WAIT("wait", 1);
            break;
        case CODES_WK_WAITSOME:
            PRINT_WAIT("waitsome", op->u.waits.count);
            break;
        case CODES_WK_WAITANY:
            PRINT_WAIT("waitany", op->u.waits.count);
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
 *  indent-tabs-mode: nil
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
