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
#include "codes/codes-workload.h"
#include "codes/quickhash.h"
#include "codes/cortex/dfly_bcast.h"
#include "codes/cortex/dragonfly-cortex-api.h"

#define MAX_LENGTH 512
#define MAX_OPERATIONS 32768
#define RANK_HASH_TABLE_SIZE 110000

static struct qhash_table *rank_tbl = NULL;
static int rank_tbl_pop = 0;

static int nprocs = 0;
static int algo_type = 0;

/* context of the MPI workload */
typedef struct rank_mpi_context
{
    int my_app_id;
    int64_t my_rank;
    void* cortex_mpi_array;	
    struct qhash_head hash_link;
    struct rc_stack * completed_ctx;
} rank_mpi_context;

typedef struct rank_mpi_compare
{
    int app;
    int rank;
} rank_mpi_compare;

/* Holds all the data about MPI operations from the log */
typedef struct cortex_op_data_array
{
	struct codes_workload_op* op_array;
    int64_t op_arr_ndx;
    int64_t op_arr_cnt;
} cortex_op_data_array;

/* load the trace */
static int cortex_trace_nw_workload_load(const char* params, int app_id, int rank);

/* cortex implementation of get next operation in the workload */
static void cortex_trace_nw_workload_get_next(int app_id, int rank, struct codes_workload_op *op);

/* initializes the data structures */
static void* cortex_init_op_data();

/* removes next operations from the dynamic array */
static void cortex_remove_next_op(void *mpi_op_array, struct codes_workload_op *mpi_op);

/* resets the counters for the dynamic array once the workload is completely loaded*/
static void cortex_finalize_mpi_op_data(void *mpi_op_array);

/* insert next operation */
static void cortex_insert_next_op(void *mpi_op_array, struct codes_workload_op *mpi_op);

/* initialize the array data structure */
static void* cortex_init_op_data()
{
	cortex_op_data_array* tmp;
	
	tmp = malloc(sizeof(cortex_op_data_array));
	assert(tmp);
	tmp->op_array = malloc(MAX_OPERATIONS * sizeof(struct codes_workload_op));
	assert(tmp->op_array);
    tmp->op_arr_ndx = 0;
	tmp->op_arr_cnt = MAX_OPERATIONS;

	return (void *)tmp;	
}

/* inserts next operation in the array */
static void cortex_insert_next_op(void *mpi_op_array, struct codes_workload_op *mpi_op)
{
    assert(mpi_op->op_type != 0);
	cortex_op_data_array *array = (cortex_op_data_array*)mpi_op_array;
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
	array->op_arr_ndx++;
    return;
}

/* resets the counters after file is fully loaded */
static void cortex_finalize_mpi_op_data(void *mpi_op_array)
{
	struct cortex_op_data_array* array = (struct cortex_op_data_array*)mpi_op_array;

	array->op_arr_cnt = array->op_arr_ndx;	
	array->op_arr_ndx = 0;
}

/* rolls back to previous index */
static void cortex_roll_back_prev_op(void * mpi_op_array)
{
    cortex_op_data_array *array = (cortex_op_data_array*)mpi_op_array;
    array->op_arr_ndx--;
    assert(array->op_arr_ndx >= 0);
}
/* removes the next operation from the array */
static void cortex_remove_next_op(void *mpi_op_array, struct codes_workload_op *mpi_op)
{
	cortex_op_data_array *array = (cortex_op_data_array*)mpi_op_array;
	if (array->op_arr_ndx == array->op_arr_cnt || array->op_arr_ndx == 0)
	 {
		mpi_op->op_type = CODES_WK_END;
	 }
	else
	{
        array->op_arr_ndx--;
		struct codes_workload_op *tmp = &(array->op_array[array->op_arr_ndx]);
		*mpi_op = *tmp;
	}
}


int handleCortexSend(int app_id, int rank, int size, int dest, int tag, void* uarg)
{
	rank_mpi_context* myctx = (rank_mpi_context*)uarg;
    assert(myctx->my_rank == rank);

	struct codes_workload_op wrkld_per_rank;

	wrkld_per_rank.op_type = CODES_WK_SEND;
    wrkld_per_rank.u.send.num_bytes = size;
    wrkld_per_rank.u.send.tag = tag;
    wrkld_per_rank.u.send.dest_rank = dest;
    wrkld_per_rank.u.send.source_rank = rank;

    //printf("\n op send added rank %d dest %d ", rank, dest);
    cortex_insert_next_op(myctx->cortex_mpi_array, &wrkld_per_rank);
	return 0;
}

int handleCortexRecv(int app_id, int rank, int size, int src,  int tag, void* uarg)
{
	rank_mpi_context* myctx = (rank_mpi_context*)uarg;
    assert(myctx->my_rank == rank);

    struct codes_workload_op wrkld_per_rank;

    wrkld_per_rank.op_type = CODES_WK_RECV;
	wrkld_per_rank.u.recv.tag = tag;
    wrkld_per_rank.u.recv.num_bytes = size;
    wrkld_per_rank.u.recv.source_rank = src;
    wrkld_per_rank.u.recv.dest_rank = -1;
   
    //printf("\n op recv added rank %d src %d", rank, src);
    cortex_insert_next_op(myctx->cortex_mpi_array, &wrkld_per_rank);
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

int cortex_trace_nw_workload_load(const char* params, int app_id, int rank)
{
    // initialize the topology information
    cortex_dfly_topology_init();
    
    rank_mpi_context *my_ctx;
    my_ctx = malloc(sizeof(rank_mpi_context));
    assert(my_ctx);
    my_ctx->my_rank = rank;
    my_ctx->my_app_id = app_id;
    my_ctx->cortex_mpi_array = cortex_init_op_data();
    struct cortex_wrkld_params * cortex_params = (struct cortex_wrkld_params*)params;

    if(!rank_tbl)
    {
        rank_tbl = qhash_init(hash_rank_compare, quickhash_64bit_hash, RANK_HASH_TABLE_SIZE);
        if(!rank_tbl)
            return -1;
    }
	/* add this rank context to hash table */	
    rank_mpi_compare cmp;
    cmp.app = app_id;
    cmp.rank = rank;
	qhash_add(rank_tbl, &cmp, &(my_ctx->hash_link));
	rank_tbl_pop++;

    /* setup the parameters */
    algo_type = cortex_params->algo_type;
    nprocs = cortex_params->nprocs;
    int root = cortex_params->root;
    int size = cortex_params->size;

     comm_handler comm = { 
         .my_rank = rank,
         .do_send = handleCortexSend,
         .do_recv = handleCortexRecv
     };
    /* now execute the collective algorithm */
    dfly_bcast(algo_type, app_id, root, nprocs, size, &comm, (void*)my_ctx);
	return 0;
}

int cortex_get_num_ranks(const char* params, int app_id)
{
    return nprocs;
}

void cortex_trace_nw_workload_get_next_rc2(int app_id, int rank)
{
    rank_mpi_context* temp_data; 
    struct qhash_head *hash_link = NULL;  
    rank_mpi_compare cmp;  
    cmp.rank = rank;
    cmp.app = app_id;

    hash_link = qhash_search(rank_tbl, &cmp);

    assert(hash_link);
    temp_data = qhash_entry(hash_link, rank_mpi_context, hash_link); 
    assert(temp_data);

    cortex_roll_back_prev_op(temp_data->cortex_mpi_array);
}

void cortex_trace_nw_workload_get_next(int app_id, int rank, struct codes_workload_op *op)
{
       rank_mpi_context* temp_data;
       struct qhash_head *hash_link = NULL;
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

      struct codes_workload_op mpi_op;
      cortex_remove_next_op(temp_data->cortex_mpi_array, &mpi_op);
      *op = mpi_op;
      return;
}

/* implements the codes workload method */
struct codes_workload_method cortex_workload_method =
{
    .method_name = "cortex-workload",
    .codes_workload_read_config = NULL,
    .codes_workload_load = cortex_trace_nw_workload_load,
    .codes_workload_get_next = cortex_trace_nw_workload_get_next,
    .codes_workload_get_next_rc2 = cortex_trace_nw_workload_get_next_rc2,
    .codes_workload_get_rank_cnt = cortex_get_num_ranks,
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
