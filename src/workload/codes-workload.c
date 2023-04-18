/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#include <assert.h>

#include <ross.h>
#include <codes/codes-workload.h>
#include <codes/codes.h>
#include "codes_config.h"

/* list of available methods.  These are statically compiled for now, but we
 * could make generators optional via autoconf tests etc. if needed
 */
/* added by pj: differ POSIX and MPI IO in darshan 3.00*/
#define	DARSHAN_POSIX_IO 	1
#define	DARSHAN_MPI_IO		0

extern struct codes_workload_method test_workload_method;
extern struct codes_workload_method iolang_workload_method;
#ifdef USE_DUMPI
extern struct codes_workload_method dumpi_trace_workload_method;
#endif

#ifdef USE_DARSHAN
#if DARSHAN_POSIX_IO
extern struct codes_workload_method darshan_posix_io_workload_method;
#elif DARSHAN_MPI_IO
extern struct codes_workload_method darshan_mpi_io_workload_method;
#endif
#endif

#ifdef USE_RECORDER
extern struct codes_workload_method recorder_io_workload_method;
#endif

#ifdef USE_SWM
extern struct codes_workload_method swm_online_comm_workload_method;
#endif
#ifdef USE_UNION
extern struct codes_workload_method conc_online_comm_workload_method;
#endif

extern struct codes_workload_method checkpoint_workload_method;
extern struct codes_workload_method iomock_workload_method;

static struct codes_workload_method const * method_array_default[] =
{
    &test_workload_method,
    &iolang_workload_method,
#ifdef USE_DUMPI
    &dumpi_trace_workload_method,
#endif

#ifdef USE_DARSHAN
/* added by pj: posix and mpi io */
#if	DARSHAN_POSIX_IO
    &darshan_posix_io_workload_method,
#elif DARNSHAN_MPI_IO
	/* TODO: MPI_IO */
	&darshan_mpi_io_workload_method,
#endif

#endif
#ifdef USE_SWM
    &swm_online_comm_workload_method,
#endif
#ifdef USE_UNION
    &conc_online_comm_workload_method,
#endif
#ifdef USE_RECORDER
    &recorder_io_workload_method,
#endif
    &checkpoint_workload_method,
    &iomock_workload_method,
    NULL
};

// once initialized, adding a workload generator is an error
static int is_workloads_init = 0;
static int num_user_methods = 0;
static struct codes_workload_method const ** method_array = NULL;

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

// only call this once
static void init_workload_methods(void)
{
    if (is_workloads_init)
        return;
    if (method_array == NULL)
        method_array = method_array_default;
    else {
        // note - includes null char
        int num_default_methods =
            (sizeof(method_array_default) / sizeof(method_array_default[0]));
        printf("\n Num default methods %d ", num_default_methods);
        method_array = realloc(method_array,
                (num_default_methods + num_user_methods + 1) *
                sizeof(*method_array));
        memcpy(method_array+num_user_methods, method_array_default,
                num_default_methods * sizeof(*method_array_default));
    }
    is_workloads_init = 1;
}

codes_workload_config_return codes_workload_read_config(
        ConfigHandle * handle,
        char const * section_name,
        char const * annotation,
        int num_ranks)
{
    init_workload_methods();

    char type[MAX_NAME_LENGTH_WKLD];
    codes_workload_config_return r;
    r.type = NULL;
    r.params = NULL;

    int rc = configuration_get_value(handle, section_name, "workload_type",
            annotation, type, MAX_NAME_LENGTH_WKLD);
    if (rc <= 0)
        return r;

    for (int i = 0; method_array[i] != NULL; i++){
        struct codes_workload_method const * m = method_array[i];
        if (strcmp(m->method_name, type) == 0) {
            r.type = m->method_name;
            if (m->codes_workload_read_config == NULL)
                r.params = NULL;
            else
                r.params = m->codes_workload_read_config(handle, section_name,
                        annotation, num_ranks);
        }
    }

    return r;
}

void codes_workload_free_config_return(codes_workload_config_return *c)
{
    free(c->params);
    c->type = NULL;
    c->params = NULL;
}

int codes_workload_load(
        const char* type,
        const char* params,
        int app_id,
        int rank)
{
    init_workload_methods();

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
    if(tmp==NULL)
        printf("tmp is NULL, rank=%d, app_id = %d", rank, app_id);
    assert(tmp);
    if(tmp->lifo)
    {
        tmp_op = tmp->lifo;
        tmp->lifo = tmp_op->next;

        *op = tmp_op->op;
        free(tmp_op);
        return;
    }

    method_array[wkld_id]->codes_workload_get_next(app_id, rank, op);

    assert(op->op_type);
    return;
}

void codes_workload_get_next_rc(
        int wkld_id,
        int app_id,
        int rank,
        const struct codes_workload_op *op)
{
    (void)wkld_id; // currently unused
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

    tmp_op = (struct rc_op*)malloc(sizeof(struct rc_op));
    assert(tmp_op);
    tmp_op->op = *op;
    tmp_op->next = tmp->lifo;
    tmp->lifo = tmp_op;

    return;
}

void codes_workload_get_next_rc2(
                int wkld_id,
                int app_id,
                int rank)
{
    assert(method_array[wkld_id]->codes_workload_get_next_rc2);
    method_array[wkld_id]->codes_workload_get_next_rc2(app_id, rank);
}

/* Finalize the workload */
int codes_workload_finalize(
        const char* type,
        const char* params,
        int app_id, 
        int rank)
{
    int i;

    for(i=0; method_array[i] != NULL; i++)
    {
        if(strcmp(method_array[i]->method_name, type) == 0)
        {
                return method_array[i]->codes_workload_finalize(
                        params, app_id, rank);
        }
    }

    fprintf(stderr, "Error: failed to find workload generator %s\n", type);
    return(-1);
}
int codes_workload_get_time(const char *type, const char *params, int app_id,
		int rank, double *read_time, double *write_time, int64_t *read_bytes, int64_t *written_bytes)
{
	int i;
	init_workload_methods();

	//printf("entering rank count, method_array = %p \n", method_array);
	for(i=0; method_array[i] != NULL; i++)
	{
		//printf("%p\n", method_array[i]);
		//printf(" geting time:: method_array[%d]->method_name = %s, type = %s\n", i, method_array[i]->method_name, type);
		if(strcmp(method_array[i]->method_name, type) == 0)
		{
			if (method_array[i]->codes_workload_get_time != NULL)
				return method_array[i]->codes_workload_get_time(
						params, app_id, rank, read_time, write_time, read_bytes, written_bytes);
			else
				return -1;
		}
	}
	return 0;
}

int codes_workload_get_rank_cnt(
        const char* type,
        const char* params,
        int app_id)
{
    int i;
    init_workload_methods();

    //printf("entering rank count, method_array = %p \n", method_array);
    for(i=0; method_array[i] != NULL; i++)
    {
    	//printf("%p\n", method_array[i]);
    	//printf("method_array[%d]->method_name = %s, type = %s\n", i, method_array[i]->method_name, type);
        if(strcmp(method_array[i]->method_name, type) == 0)
        {
            if (method_array[i]->codes_workload_get_rank_cnt != NULL)
                return method_array[i]->codes_workload_get_rank_cnt(
                        params, app_id);
            else
                return -1;
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
    char *name;

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
        case CODES_WK_MPI_OPEN:
        case CODES_WK_MPI_COLL_OPEN:
            if(op->op_type == CODES_WK_OPEN) name = "open";
            if(op->op_type == CODES_WK_MPI_OPEN) name = "mpi_open";
            if(op->op_type == CODES_WK_MPI_COLL_OPEN) name = "mpi_coll_open";
            fprintf(f, "op: app:%d rank:%d type:%s file_id:%llu flag:%d\n",
                    app_id, rank, name, LLU(op->u.open.file_id), op->u.open.create_flag);
            break;
        case CODES_WK_CLOSE:
        case CODES_WK_MPI_CLOSE:
            if(op->op_type == CODES_WK_CLOSE) name = "close";
            if(op->op_type == CODES_WK_MPI_CLOSE) name = "mpi_close";
            fprintf(f, "op: app:%d rank:%d type:%s file_id:%llu\n",
                    app_id, rank, name, LLU(op->u.close.file_id));
            break;
        case CODES_WK_WRITE:
        case CODES_WK_MPI_WRITE:
        case CODES_WK_MPI_COLL_WRITE:
            if(op->op_type == CODES_WK_WRITE) name = "write";
            if(op->op_type == CODES_WK_MPI_WRITE) name = "mpi_write";
            if(op->op_type == CODES_WK_MPI_COLL_WRITE) name = "mpi_coll_write";
            fprintf(f, "op: app:%d rank:%d type:%s "
                       "file_id:%llu off:%llu size:%llu\n",
                    app_id, rank, name, LLU(op->u.write.file_id), LLU(op->u.write.offset),
                    LLU(op->u.write.size));
            break;
        case CODES_WK_READ:
        case CODES_WK_MPI_READ:
        case CODES_WK_MPI_COLL_READ:
            if(op->op_type == CODES_WK_READ) name = "read";
            if(op->op_type == CODES_WK_MPI_READ) name = "mpi_read";
            if(op->op_type == CODES_WK_MPI_COLL_READ) name = "mpi_coll_read";
            fprintf(f, "op: app:%d rank:%d type:%s "
                       "file_id:%llu off:%llu size:%llu\n",
                    app_id, rank, name, LLU(op->u.read.file_id), LLU(op->u.read.offset),
                    LLU(op->u.read.size));
            break;
        case CODES_WK_SEND:
            fprintf(f, "op: app:%d rank:%d type:send "
                    "src:%d dst:%d bytes:%"PRIu64" type:%d count:%d tag:%d "
                    "start:%.5e end:%.5e\n",
                    app_id, rank,
                    op->u.send.source_rank, op->u.send.dest_rank,
                    op->u.send.num_bytes, op->u.send.data_type,
                    op->u.send.count, op->u.send.tag,
                    op->start_time, op->end_time);
            break;
        case CODES_WK_RECV:
            fprintf(f, "op: app:%d rank:%d type:recv "
                    "src:%d dst:%d bytes:%"PRIu64" type:%d count:%d tag:%d "
                    "start:%.5e end:%.5e\n",
                    app_id, rank,
                    op->u.recv.source_rank, op->u.recv.dest_rank,
                    op->u.recv.num_bytes, op->u.recv.data_type,
                    op->u.recv.count, op->u.recv.tag,
                    op->start_time, op->end_time);
            break;
        case CODES_WK_ISEND:
            fprintf(f, "op: app:%d rank:%d type:isend "
                    "src:%d dst:%d req_id:%"PRIu32" bytes:%"PRIu64" type:%d count:%d tag:%d "
                    "start:%.5e end:%.5e\n",
                    app_id, rank,
                    op->u.send.source_rank, op->u.send.dest_rank,
                    op->u.send.req_id,
                    op->u.send.num_bytes, op->u.send.data_type,
                    op->u.send.count, op->u.send.tag,
                    op->start_time, op->end_time);
            break;
        case CODES_WK_IRECV:
            fprintf(f, "op: app:%d rank:%d type:irecv "
                    "src:%d dst:%d req_id:%"PRIu32" bytes:%"PRIu64" type:%d count:%d tag:%d "
                    "start:%.5e end:%.5e\n",
                    app_id, rank,
                    op->u.recv.source_rank, op->u.recv.dest_rank,
                    op->u.recv.req_id,
                    op->u.recv.num_bytes, op->u.recv.data_type,
                    op->u.recv.count, op->u.recv.tag,
                    op->start_time, op->end_time);
            break;
       case CODES_WK_REQ_FREE:
            fprintf(f, "op: app:%d rank:%d type:req free "
                    " req:%d ",
                    app_id, rank,
                    op->u.free.req_id);
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

void codes_workload_add_method(struct codes_workload_method const * method)
{
    static int method_array_cap = 10;
    if (is_workloads_init)
        tw_error(TW_LOC,
                "adding a workload method after initialization is forbidden");
    else if (method_array == NULL){
        method_array = malloc(method_array_cap * sizeof(*method_array));
        assert(method_array);
    }

    if (num_user_methods == method_array_cap) {
        method_array_cap *= 2;
        method_array = realloc(method_array,
                method_array_cap * sizeof(*method_array));
        assert(method_array);
    }

    method_array[num_user_methods++] = method;
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
