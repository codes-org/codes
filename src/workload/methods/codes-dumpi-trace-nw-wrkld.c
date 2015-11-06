/*
 * Copyright (C) 2014 University of Chicago
 * See COPYRIGHT notice in top-level directory.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mpi.h>
#include <ross.h>
#include <assert.h>

#include "dumpi/libundumpi/bindings.h"
#include "dumpi/libundumpi/libundumpi.h"
#include "codes/codes-workload.h"
#include "codes/quickhash.h"

#define MAX_LENGTH 512
#define MAX_OPERATIONS 32768
#define DUMPI_IGNORE_DELAY 100
#define RANK_HASH_TABLE_SIZE 400

static struct qhash_table *rank_tbl = NULL;
static int rank_tbl_pop = 0;

/* context of the MPI workload */
typedef struct rank_mpi_context
{
    int my_app_id;
    // whether we've seen an init op (needed for timing correctness)
    int is_init;
    int64_t my_rank;
    double last_op_time;
    double init_time;
    void* dumpi_mpi_array;	
    struct qhash_head hash_link;
} rank_mpi_context;

typedef struct rank_mpi_compare
{
    int app;
    int rank;
} rank_mpi_compare;

/* Holds all the data about MPI operations from the log */
typedef struct dumpi_op_data_array
{
	struct codes_workload_op* op_array;
        int64_t op_arr_ndx;
        int64_t op_arr_cnt;
} dumpi_op_data_array;

/* timing utilities */

#ifdef __GNUC__
__attribute__((unused))
#endif
static dumpi_clock timediff(
        dumpi_clock end,
        dumpi_clock start)
{
    dumpi_clock temp;
    if ((end.nsec-start.nsec)<0) {
        temp.sec = end.sec-start.sec-1;
        temp.nsec = 1000000000+end.nsec-start.nsec;
    } else {
        temp.sec = end.sec-start.sec;
        temp.nsec = end.nsec-start.nsec;
    }
    return temp;
}

static inline double time_to_ms_lf(dumpi_clock t){
        return (double) t.sec * 1e3 + (double) t.nsec / 1e6;
}
static inline double time_to_us_lf(dumpi_clock t){
        return (double) t.sec * 1e6 + (double) t.nsec / 1e3;
}
static inline double time_to_ns_lf(dumpi_clock t){
        return (double) t.sec * 1e9 + (double) t.nsec;
}
static inline double time_to_s_lf(dumpi_clock t){
        return (double) t.sec + (double) t.nsec / 1e9;
}

/* load the trace */
static int dumpi_trace_nw_workload_load(const char* params, int app_id, int rank);

/* dumpi implementation of get next operation in the workload */
static void dumpi_trace_nw_workload_get_next(int app_id, int rank, struct codes_workload_op *op);

/* get number of bytes from the workload data type and count */
static int get_num_bytes(dumpi_datatype dt);

/* computes the delay between MPI operations */
static void update_compute_time(const dumpi_time* time, rank_mpi_context* my_ctx);

/* initializes the data structures */
static void* dumpi_init_op_data();

/* removes next operations from the dynamic array */
static void dumpi_remove_next_op(void *mpi_op_array, struct codes_workload_op *mpi_op,
                                      double last_op_time);

/* resets the counters for the dynamic array once the workload is completely loaded*/
static void dumpi_finalize_mpi_op_data(void *mpi_op_array);

/* insert next operation */
static void dumpi_insert_next_op(void *mpi_op_array, struct codes_workload_op *mpi_op);

/* initialize the array data structure */
static void* dumpi_init_op_data()
{
	dumpi_op_data_array* tmp;
	
	tmp = malloc(sizeof(dumpi_op_data_array));
	assert(tmp);
	tmp->op_array = malloc(MAX_OPERATIONS * sizeof(struct codes_workload_op));
	assert(tmp->op_array);
        tmp->op_arr_ndx = 0;
	tmp->op_arr_cnt = MAX_OPERATIONS;

	return (void *)tmp;	
}

/* inserts next operation in the array */
static void dumpi_insert_next_op(void *mpi_op_array, struct codes_workload_op *mpi_op)
{
	dumpi_op_data_array *array = (dumpi_op_data_array*)mpi_op_array;
	struct codes_workload_op *tmp;

	/*check if array is full.*/
	if (array->op_arr_ndx == array->op_arr_cnt)
	{
		tmp = malloc((array->op_arr_cnt + MAX_OPERATIONS) * sizeof(struct codes_workload_op));
		assert(tmp);
		memcpy(tmp, array->op_array, array->op_arr_cnt * sizeof(struct codes_workload_op));
		free(array->op_array);
	        array->op_array = tmp;
	        array->op_arr_cnt += MAX_OPERATIONS;
	}

	/* add the MPI operation to the op array */
	array->op_array[array->op_arr_ndx] = *mpi_op;
	//printf("\n insert time %f end time %f ", array->op_array[array->op_arr_ndx].start_time, array->op_array[array->op_arr_ndx].end_time);
	array->op_arr_ndx++;
	return;
}

/* resets the counters after file is fully loaded */
static void dumpi_finalize_mpi_op_data(void *mpi_op_array)
{
	struct dumpi_op_data_array* array = (struct dumpi_op_data_array*)mpi_op_array;

	array->op_arr_cnt = array->op_arr_ndx;	
	array->op_arr_ndx = 0;
}

/* removes the next operation from the array */
static void dumpi_remove_next_op(void *mpi_op_array, struct codes_workload_op *mpi_op,
                                      double last_op_time)
{
	dumpi_op_data_array *array = (dumpi_op_data_array*)mpi_op_array;
	//printf("\n op array index %d array count %d ", array->op_arr_ndx, array->op_arr_cnt);
	if (array->op_arr_ndx == array->op_arr_cnt)
	 {
		mpi_op->op_type = CODES_WK_END;
	 }
	else
	{
		struct codes_workload_op *tmp = &(array->op_array[array->op_arr_ndx]);
		//printf("\n tmp end time %f ", tmp->end_time);
		*mpi_op = *tmp;
		array->op_arr_ndx++;
	}
	if(mpi_op->op_type == CODES_WK_END)
	{
		free(array->op_array);
		free(array);
	}
}

/* check for initialization and normalize reported time */
static inline void check_set_init_time(const dumpi_time *t, rank_mpi_context * my_ctx)
{
    if (!my_ctx->is_init) {
        my_ctx->is_init = 1;
        my_ctx->init_time = time_to_ns_lf(t->start);
        my_ctx->last_op_time = time_to_ns_lf(t->stop) - my_ctx->init_time;
    }
}

/* introduce delay between operations: delay is the compute time NOT spent in MPI operations*/
void update_compute_time(const dumpi_time* time, rank_mpi_context* my_ctx)
{
    double start = time_to_ns_lf(time->start) - my_ctx->init_time;
    double stop = time_to_ns_lf(time->stop) - my_ctx->init_time;
    if((start - my_ctx->last_op_time) > DUMPI_IGNORE_DELAY)
    {
        struct codes_workload_op wrkld_per_rank;

        wrkld_per_rank.op_type = CODES_WK_DELAY;
        wrkld_per_rank.start_time = my_ctx->last_op_time;
        wrkld_per_rank.end_time = start;
        wrkld_per_rank.u.delay.seconds = (start - my_ctx->last_op_time) / 1e9;
        dumpi_insert_next_op(my_ctx->dumpi_mpi_array, &wrkld_per_rank); 
    }
    my_ctx->last_op_time = stop;
}

static int handleDUMPIInit(
        const dumpi_init *prm,
        uint16_t thread,
        const dumpi_time *cpu,
        const dumpi_time *wall,
        const dumpi_perfinfo *perf,
        void *uarg)
{
    rank_mpi_context *myctx = (rank_mpi_context*)uarg;
    check_set_init_time(wall, myctx);
    return 0;
}

int handleDUMPIIgnore(const void* prm, uint16_t thread, const dumpi_time *cpu, const dumpi_time *wall, const dumpi_perfinfo *perf, void *uarg)
{
	rank_mpi_context* myctx = (rank_mpi_context*)uarg;

        check_set_init_time(wall, myctx);
	update_compute_time(wall, myctx);

	return 0;
}

static void update_times_and_insert(
        struct codes_workload_op *op,
        const dumpi_time *t,
        rank_mpi_context *ctx)
{
    check_set_init_time(t, ctx);
    op->start_time = time_to_ns_lf(t->start) - ctx->init_time;
    op->end_time = time_to_ns_lf(t->stop) - ctx->init_time;
    update_compute_time(t, ctx);
    dumpi_insert_next_op(ctx->dumpi_mpi_array, op);
}


int handleDUMPIWait(const dumpi_wait *prm, uint16_t thread,
                    const dumpi_time *cpu, const dumpi_time *wall,
                    const dumpi_perfinfo *perf, void *userarg)
{
        rank_mpi_context* myctx = (rank_mpi_context*)userarg;
        struct codes_workload_op wrkld_per_rank;

        wrkld_per_rank.op_type = CODES_WK_WAIT;
        wrkld_per_rank.u.wait.req_id = prm->request;

        update_times_and_insert(&wrkld_per_rank, wall, myctx);

        return 0;
}

int handleDUMPIWaitsome(const dumpi_waitsome *prm, uint16_t thread,
                    const dumpi_time *cpu, const dumpi_time *wall,
                    const dumpi_perfinfo *perf, void *userarg)
{
        int i;
        rank_mpi_context* myctx = (rank_mpi_context*)userarg;
        struct codes_workload_op wrkld_per_rank;

        wrkld_per_rank.op_type = CODES_WK_WAITSOME;
        wrkld_per_rank.u.waits.count = prm->count;
        wrkld_per_rank.u.waits.req_ids =
            malloc(prm->count * sizeof(*wrkld_per_rank.u.waits.req_ids));

        for( i = 0; i < prm->count; i++ )
                wrkld_per_rank.u.waits.req_ids[i] = (int16_t)prm->requests[i];

        update_times_and_insert(&wrkld_per_rank, wall, myctx);
        return 0;

}

int handleDUMPIWaitany(const dumpi_waitany *prm, uint16_t thread,
                    const dumpi_time *cpu, const dumpi_time *wall,
                    const dumpi_perfinfo *perf, void *userarg)
{
        int i;
        rank_mpi_context* myctx = (rank_mpi_context*)userarg;
        struct codes_workload_op wrkld_per_rank;

        wrkld_per_rank.op_type = CODES_WK_WAITANY;
        wrkld_per_rank.u.waits.count = prm->count;
        wrkld_per_rank.u.waits.req_ids =
            malloc(prm->count * sizeof(*wrkld_per_rank.u.waits.req_ids));

        for( i = 0; i < prm->count; i++ )
                wrkld_per_rank.u.waits.req_ids[i] = (int16_t)prm->requests[i];

        update_times_and_insert(&wrkld_per_rank, wall, myctx);
        return 0;
}

int handleDUMPIWaitall(const dumpi_waitall *prm, uint16_t thread,
                    const dumpi_time *cpu, const dumpi_time *wall,
                    const dumpi_perfinfo *perf, void *userarg)
{
        int i;
        rank_mpi_context* myctx = (rank_mpi_context*)userarg;
        struct codes_workload_op wrkld_per_rank;

        wrkld_per_rank.op_type = CODES_WK_WAITALL;

        wrkld_per_rank.u.waits.count = prm->count;
        wrkld_per_rank.u.waits.req_ids =
            malloc(prm->count * sizeof(*wrkld_per_rank.u.waits.req_ids));
        for( i = 0; i < prm->count; i++ )
                wrkld_per_rank.u.waits.req_ids[i] = prm->requests[i];

        update_times_and_insert(&wrkld_per_rank, wall, myctx);
        return 0;
}

int handleDUMPIISend(const dumpi_isend *prm, uint16_t thread, const dumpi_time *cpu, const dumpi_time *wall, const dumpi_perfinfo *perf, void *userarg)
{
	rank_mpi_context* myctx = (rank_mpi_context*)userarg;

	struct codes_workload_op wrkld_per_rank;

	wrkld_per_rank.op_type = CODES_WK_ISEND;
	wrkld_per_rank.u.send.tag = prm->tag;
	wrkld_per_rank.u.send.count = prm->count;
	wrkld_per_rank.u.send.data_type = prm->datatype;
        wrkld_per_rank.u.send.num_bytes = prm->count * get_num_bytes(prm->datatype);
	assert(wrkld_per_rank.u.send.num_bytes > 0);
    	wrkld_per_rank.u.send.req_id = prm->request;
        wrkld_per_rank.u.send.dest_rank = prm->dest;
        wrkld_per_rank.u.send.source_rank = myctx->my_rank;

        update_times_and_insert(&wrkld_per_rank, wall, myctx);
	return 0;
}

int handleDUMPIIRecv(const dumpi_irecv *prm, uint16_t thread, const dumpi_time *cpu, const dumpi_time *wall, const dumpi_perfinfo *perf, void *userarg)
{
	//printf("\n irecv source %d count %d data type %d", prm->source, prm->count, prm->datatype);
	rank_mpi_context* myctx = (rank_mpi_context*)userarg;
        struct codes_workload_op wrkld_per_rank;

        wrkld_per_rank.op_type = CODES_WK_IRECV;
	wrkld_per_rank.u.recv.data_type = prm->datatype;
	wrkld_per_rank.u.recv.count = prm->count;
	wrkld_per_rank.u.recv.tag = prm->tag;
        wrkld_per_rank.u.recv.num_bytes = prm->count * get_num_bytes(prm->datatype);
	assert(wrkld_per_rank.u.recv.num_bytes > 0);
        wrkld_per_rank.u.recv.source_rank = prm->source;
        wrkld_per_rank.u.recv.dest_rank = -1;
	wrkld_per_rank.u.recv.req_id = prm->request;

        update_times_and_insert(&wrkld_per_rank, wall, myctx);
        return 0;
}

int handleDUMPISend(const dumpi_send *prm, uint16_t thread,
                      const dumpi_time *cpu, const dumpi_time *wall,
                      const dumpi_perfinfo *perf, void *uarg)
{
	rank_mpi_context* myctx = (rank_mpi_context*)uarg;
        struct codes_workload_op wrkld_per_rank;

        wrkld_per_rank.op_type = CODES_WK_SEND;
	wrkld_per_rank.u.send.tag = prm->tag;
        wrkld_per_rank.u.send.count = prm->count;
        wrkld_per_rank.u.send.data_type = prm->datatype;
        wrkld_per_rank.u.send.num_bytes = prm->count * get_num_bytes(prm->datatype);
	if(wrkld_per_rank.u.send.num_bytes < 0)
		printf("\n Number of bytes %d count %d data type %d num_bytes %d", prm->count * get_num_bytes(prm->datatype), prm->count, prm->datatype, get_num_bytes(prm->datatype));
	assert(wrkld_per_rank.u.send.num_bytes > 0);
        wrkld_per_rank.u.send.dest_rank = prm->dest;
        wrkld_per_rank.u.send.source_rank = myctx->my_rank;
         wrkld_per_rank.u.send.req_id = -1;

        update_times_and_insert(&wrkld_per_rank, wall, myctx);
        return 0;
}

int handleDUMPIRecv(const dumpi_recv *prm, uint16_t thread,
                      const dumpi_time *cpu, const dumpi_time *wall,
                      const dumpi_perfinfo *perf, void *uarg)
{
	//printf("\n irecv source %d count %d data type %d", prm->source, prm->count, prm->datatype);
	rank_mpi_context* myctx = (rank_mpi_context*)uarg;
	struct codes_workload_op wrkld_per_rank;

	wrkld_per_rank.op_type = CODES_WK_RECV;
        wrkld_per_rank.u.recv.num_bytes = prm->count * get_num_bytes(prm->datatype);
	assert(wrkld_per_rank.u.recv.num_bytes > 0);
        wrkld_per_rank.u.recv.source_rank = prm->source;
        wrkld_per_rank.u.recv.dest_rank = -1;

        update_times_and_insert(&wrkld_per_rank, wall, myctx);
        return 0;

}

int handleDUMPIBcast(const dumpi_bcast *prm, uint16_t thread,
                       const dumpi_time *cpu, const dumpi_time *wall,
                       const dumpi_perfinfo *perf, void *uarg)
{
	rank_mpi_context* myctx = (rank_mpi_context*)uarg;
	struct codes_workload_op wrkld_per_rank;

        wrkld_per_rank.op_type = CODES_WK_BCAST;
        wrkld_per_rank.u.collective.num_bytes = prm->count * get_num_bytes(prm->datatype);
	assert(wrkld_per_rank.u.collective.num_bytes > 0);

        update_times_and_insert(&wrkld_per_rank, wall, myctx);
        return 0;
}

int handleDUMPIAllgather(const dumpi_allgather *prm, uint16_t thread,
                           const dumpi_time *cpu, const dumpi_time *wall,
                           const dumpi_perfinfo *perf, void *uarg)
{
	rank_mpi_context* myctx = (rank_mpi_context*)uarg;
	struct codes_workload_op wrkld_per_rank;

        wrkld_per_rank.op_type = CODES_WK_ALLGATHER;
        wrkld_per_rank.u.collective.num_bytes = prm->sendcount * get_num_bytes(prm->sendtype);
	assert(wrkld_per_rank.u.collective.num_bytes > 0);

        update_times_and_insert(&wrkld_per_rank, wall, myctx);
        return 0;
}

int handleDUMPIAllgatherv(const dumpi_allgatherv *prm, uint16_t thread,
                            const dumpi_time *cpu, const dumpi_time *wall,
                            const dumpi_perfinfo *perf, void *uarg)
{
	rank_mpi_context* myctx = (rank_mpi_context*)uarg;
	struct codes_workload_op wrkld_per_rank;

        wrkld_per_rank.op_type = CODES_WK_ALLGATHERV;
        wrkld_per_rank.u.collective.num_bytes = prm->sendcount * get_num_bytes(prm->sendtype);
	assert(wrkld_per_rank.u.collective.num_bytes > 0);

        update_times_and_insert(&wrkld_per_rank, wall, myctx);
        return 0;
}

int handleDUMPIAlltoall(const dumpi_alltoall *prm, uint16_t thread,
                          const dumpi_time *cpu, const dumpi_time *wall,
                          const dumpi_perfinfo *perf, void *uarg)
{
	rank_mpi_context* myctx = (rank_mpi_context*)uarg;
	struct codes_workload_op wrkld_per_rank;

        wrkld_per_rank.op_type = CODES_WK_ALLTOALL;
        wrkld_per_rank.u.collective.num_bytes = prm->sendcount * get_num_bytes(prm->sendtype);
	assert(wrkld_per_rank.u.collective.num_bytes > 0);

        update_times_and_insert(&wrkld_per_rank, wall, myctx);
        return 0;
}

int handleDUMPIAlltoallv(const dumpi_alltoallv *prm, uint16_t thread,
                           const dumpi_time *cpu, const dumpi_time *wall,
                           const dumpi_perfinfo *perf, void *uarg)
{
	rank_mpi_context* myctx = (rank_mpi_context*)uarg;
	struct codes_workload_op wrkld_per_rank;

        wrkld_per_rank.op_type = CODES_WK_ALLTOALLV;
        wrkld_per_rank.u.collective.num_bytes = prm->sendcounts[0] * get_num_bytes(prm->sendtype);
	assert(wrkld_per_rank.u.collective.num_bytes > 0);

        update_times_and_insert(&wrkld_per_rank, wall, myctx);
        return 0;
}

int handleDUMPIReduce(const dumpi_reduce *prm, uint16_t thread,
                        const dumpi_time *cpu, const dumpi_time *wall,
                        const dumpi_perfinfo *perf, void *uarg)
{
	rank_mpi_context* myctx = (rank_mpi_context*)uarg;
	struct codes_workload_op wrkld_per_rank;

        wrkld_per_rank.op_type = CODES_WK_REDUCE;
        wrkld_per_rank.u.collective.num_bytes = prm->count * get_num_bytes(prm->datatype);
	assert(wrkld_per_rank.u.collective.num_bytes > 0);

        update_times_and_insert(&wrkld_per_rank, wall, myctx);
        return 0;
}

int handleDUMPIAllreduce(const dumpi_allreduce *prm, uint16_t thread,
                           const dumpi_time *cpu, const dumpi_time *wall,
                           const dumpi_perfinfo *perf, void *uarg)
{
	rank_mpi_context* myctx = (rank_mpi_context*)uarg;
	struct codes_workload_op wrkld_per_rank;

        wrkld_per_rank.op_type = CODES_WK_ALLREDUCE;
        wrkld_per_rank.u.collective.num_bytes = prm->count * get_num_bytes(prm->datatype);
	assert(wrkld_per_rank.u.collective.num_bytes > 0);

        update_times_and_insert(&wrkld_per_rank, wall, myctx);
        return 0;

}

int handleDUMPIFinalize(const dumpi_finalize *prm, uint16_t thread, const dumpi_time *cpu, const dumpi_time *wall, const dumpi_perfinfo *perf, void *uarg)
{
	rank_mpi_context* myctx = (rank_mpi_context*)uarg;
	struct codes_workload_op wrkld_per_rank;

        wrkld_per_rank.op_type = CODES_WK_END;

        update_times_and_insert(&wrkld_per_rank, wall, myctx);
        return 0;
}

static int hash_rank_compare(void *key, struct qhash_head *link)
{
    rank_mpi_compare *in = key;
    rank_mpi_context *tmp;

    tmp = qhash_entry(link, rank_mpi_context, hash_link);
    if (tmp->my_rank == in->rank && tmp->my_app_id == in->app)
        return 1;

    return 0;
}

int dumpi_trace_nw_workload_load(const char* params, int app_id, int rank)
{
	libundumpi_callbacks callbacks;
	libundumpi_cbpair callarr[DUMPI_END_OF_STREAM];
	dumpi_profile* profile;
	dumpi_trace_params* dumpi_params = (dumpi_trace_params*)params;
	char file_name[MAX_LENGTH];

	if(rank >= dumpi_params->num_net_traces)
		return -1;

	if(!rank_tbl)
    	{
            rank_tbl = qhash_init(hash_rank_compare, quickhash_64bit_hash, RANK_HASH_TABLE_SIZE);
            if(!rank_tbl)
                  return -1;
    	}
	
	rank_mpi_context *my_ctx;
	my_ctx = malloc(sizeof(rank_mpi_context));
	assert(my_ctx);
	my_ctx->my_rank = rank;
        my_ctx->my_app_id = app_id;
	my_ctx->last_op_time = 0.0;
        my_ctx->is_init = 0;
	my_ctx->dumpi_mpi_array = dumpi_init_op_data();

	if(rank < 10)
            sprintf(file_name, "%s000%d.bin", dumpi_params->file_name, rank);
         else if(rank >=10 && rank < 100)
            sprintf(file_name, "%s00%d.bin", dumpi_params->file_name, rank);
           else if(rank >=100 && rank < 1000)
             sprintf(file_name, "%s0%d.bin", dumpi_params->file_name, rank);
             else
              sprintf(file_name, "%s%d.bin", dumpi_params->file_name, rank);
	profile =  undumpi_open(file_name);
        if(NULL == profile) {
                printf("Error: unable to open DUMPI trace: %s", file_name);
                exit(-1);
        }
	
	memset(&callbacks, 0, sizeof(libundumpi_callbacks));
        memset(&callarr, 0, sizeof(libundumpi_cbpair) * DUMPI_END_OF_STREAM);

	/* handle MPI function calls */	        
        callbacks.on_init = handleDUMPIInit;
	callbacks.on_send = (dumpi_send_call)handleDUMPISend;
        callbacks.on_recv = (dumpi_recv_call)handleDUMPIRecv;
        callbacks.on_isend = (dumpi_isend_call)handleDUMPIISend;
        callbacks.on_irecv = (dumpi_irecv_call)handleDUMPIIRecv;
        callbacks.on_allreduce = (dumpi_allreduce_call)handleDUMPIAllreduce;
	callbacks.on_bcast = (dumpi_bcast_call)handleDUMPIBcast;
	callbacks.on_get_count = (dumpi_get_count_call)handleDUMPIIgnore;
	callbacks.on_bsend = (dumpi_bsend_call)handleDUMPIIgnore;
	callbacks.on_ssend = (dumpi_ssend_call)handleDUMPIIgnore;
	callbacks.on_rsend = (dumpi_rsend_call)handleDUMPIIgnore;
	callbacks.on_buffer_attach = (dumpi_buffer_attach_call)handleDUMPIIgnore;
	callbacks.on_buffer_detach = (dumpi_buffer_detach_call)handleDUMPIIgnore;
	callbacks.on_ibsend = (dumpi_ibsend_call)handleDUMPIIgnore;
	callbacks.on_issend = (dumpi_issend_call)handleDUMPIIgnore;
	callbacks.on_irsend = (dumpi_irsend_call)handleDUMPIIgnore;
	callbacks.on_wait = (dumpi_wait_call)handleDUMPIWait;
	callbacks.on_test = (dumpi_test_call)handleDUMPIIgnore;
	callbacks.on_request_free = (dumpi_request_free_call)handleDUMPIIgnore;
	callbacks.on_waitany = (dumpi_waitany_call)handleDUMPIWaitany;
	callbacks.on_testany = (dumpi_testany_call)handleDUMPIIgnore;
	callbacks.on_waitall = (dumpi_waitall_call)handleDUMPIWaitall;
	callbacks.on_testall = (dumpi_testall_call)handleDUMPIIgnore;
	callbacks.on_waitsome = (dumpi_waitsome_call)handleDUMPIWaitsome;
	callbacks.on_testsome = (dumpi_testsome_call)handleDUMPIIgnore;
	callbacks.on_iprobe = (dumpi_iprobe_call)handleDUMPIIgnore;
	callbacks.on_probe = (dumpi_probe_call)handleDUMPIIgnore;
	callbacks.on_cancel = (dumpi_cancel_call)handleDUMPIIgnore;
	callbacks.on_test_cancelled = (dumpi_test_cancelled_call)handleDUMPIIgnore;
	callbacks.on_send_init = (dumpi_send_init_call)handleDUMPIIgnore;
	callbacks.on_bsend_init = (dumpi_bsend_init_call)handleDUMPIIgnore;
	callbacks.on_ssend_init = (dumpi_ssend_init_call)handleDUMPIIgnore;
	callbacks.on_rsend_init = (dumpi_rsend_init_call)handleDUMPIIgnore;
	callbacks.on_recv_init = (dumpi_recv_init_call)handleDUMPIIgnore;
	callbacks.on_start = (dumpi_start_call)handleDUMPIIgnore;
	callbacks.on_startall = (dumpi_startall_call)handleDUMPIIgnore;
	callbacks.on_sendrecv = (dumpi_sendrecv_call)handleDUMPIIgnore;
	callbacks.on_sendrecv_replace = (dumpi_sendrecv_replace_call)handleDUMPIIgnore;
	callbacks.on_type_contiguous = (dumpi_type_contiguous_call)handleDUMPIIgnore;
	callbacks.on_barrier = (dumpi_barrier_call)handleDUMPIIgnore;
        callbacks.on_gather = (dumpi_gather_call)handleDUMPIIgnore;
        callbacks.on_gatherv = (dumpi_gatherv_call)handleDUMPIIgnore;
        callbacks.on_scatter = (dumpi_scatter_call)handleDUMPIIgnore;
        callbacks.on_scatterv = (dumpi_scatterv_call)handleDUMPIIgnore;
        callbacks.on_allgather = (dumpi_allgather_call)handleDUMPIIgnore;
        callbacks.on_allgatherv = (dumpi_allgatherv_call)handleDUMPIIgnore;
        callbacks.on_alltoall = (dumpi_alltoall_call)handleDUMPIIgnore;
        callbacks.on_alltoallv = (dumpi_alltoallv_call)handleDUMPIIgnore;
        callbacks.on_alltoallw = (dumpi_alltoallw_call)handleDUMPIIgnore;
        callbacks.on_reduce = (dumpi_reduce_call)handleDUMPIIgnore;
        callbacks.on_reduce_scatter = (dumpi_reduce_scatter_call)handleDUMPIIgnore;
        callbacks.on_group_size = (dumpi_group_size_call)handleDUMPIIgnore;
        callbacks.on_group_rank = (dumpi_group_rank_call)handleDUMPIIgnore;
        callbacks.on_comm_size = (dumpi_comm_size_call)handleDUMPIIgnore;
        callbacks.on_comm_rank = (dumpi_comm_rank_call)handleDUMPIIgnore;
        callbacks.on_comm_get_attr = (dumpi_comm_get_attr_call)handleDUMPIIgnore;
        callbacks.on_comm_create = (dumpi_comm_create_call)handleDUMPIIgnore;
        callbacks.on_wtime = (dumpi_wtime_call)handleDUMPIIgnore;
        callbacks.on_finalize = (dumpi_finalize_call)handleDUMPIFinalize;

        libundumpi_populate_callbacks(&callbacks, callarr);

        dumpi_start_stream_read(profile);
        //dumpi_header* trace_header = undumpi_read_header(profile);
        //dumpi_free_header(trace_header);

        int finalize_reached = 0;
        int active = 1;
        int num_calls = 0;
        while(active && !finalize_reached)
        {
           num_calls++;
           active = undumpi_read_single_call(profile, callarr, (void*)my_ctx, &finalize_reached);
        }
	undumpi_close(profile);
	dumpi_finalize_mpi_op_data(my_ctx->dumpi_mpi_array);
	/* add this rank context to hash table */	
        rank_mpi_compare cmp;
        cmp.app = my_ctx->my_app_id;
        cmp.rank = my_ctx->my_rank;
	qhash_add(rank_tbl, &cmp, &(my_ctx->hash_link));
	rank_tbl_pop++;

	return 0;
}

int get_num_bytes(dumpi_datatype dt)
{
   switch(dt)
   {
	case DUMPI_DATATYPE_ERROR:
	case DUMPI_DATATYPE_NULL:
		return -1; /* error state */
	break;

	case DUMPI_CHAR:
	case DUMPI_UNSIGNED_CHAR:
	case DUMPI_SIGNED_CHAR:
	case DUMPI_BYTE:
		return 1; /* 1 byte for char */
	break;

	case DUMPI_WCHAR:
		return 4; /* 4 bytes for a 64-bit version */
	break;

	case DUMPI_SHORT:
	case DUMPI_SHORT_INT:
	case DUMPI_UNSIGNED_SHORT:
		return 2;
	break;

	 case DUMPI_INT:
	 case DUMPI_UNSIGNED:
	 case DUMPI_FLOAT:
	 case DUMPI_FLOAT_INT:
		return 4;
	 break;

	case DUMPI_DOUBLE:
	case DUMPI_LONG:
	case DUMPI_LONG_INT:
	case DUMPI_UNSIGNED_LONG:
	case DUMPI_LONG_LONG_INT:
	case DUMPI_UNSIGNED_LONG_LONG:
	case DUMPI_LONG_LONG:
	case DUMPI_DOUBLE_INT:
		return 8;
	break;

	case DUMPI_LONG_DOUBLE:
	case DUMPI_LONG_DOUBLE_INT:
		return 16;
	break;
	
	default:
	  {
		printf("\n Undefined data type ");
		return 0;	
	  }	
   } 
}

void dumpi_trace_nw_workload_get_next(int app_id, int rank, struct codes_workload_op *op)
{
   rank_mpi_context* temp_data;
   struct qhash_head *hash_link = NULL;
   struct codes_workload_op mpi_op;
   rank_mpi_compare cmp;
   cmp.rank = rank;
   cmp.app = app_id;
   hash_link = qhash_search(rank_tbl, &cmp);
   if(!hash_link)
   {
      printf("\n not found for rank id %d , %d", rank, app_id);
      op->op_type = CODES_WK_END;
      return;
   }
  temp_data = qhash_entry(hash_link, rank_mpi_context, hash_link);
  assert(temp_data);

  dumpi_remove_next_op(temp_data->dumpi_mpi_array, &mpi_op, temp_data->last_op_time); 
  if( mpi_op.op_type == CODES_WK_END)
  {
	qhash_del(hash_link);
        free(temp_data);

        rank_tbl_pop--;
        if (!rank_tbl_pop)
        {
            qhash_finalize(rank_tbl);
            rank_tbl = NULL;
        }
  }
  *op = mpi_op;
  return;
}

/* implements the codes workload method */
struct codes_workload_method dumpi_trace_workload_method =
{
    .method_name = "dumpi-trace-workload",
    .codes_workload_read_config = NULL,
    .codes_workload_load = dumpi_trace_nw_workload_load,
    .codes_workload_get_next = dumpi_trace_nw_workload_get_next,
};

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 *  indent-tabs-mode: nil
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
