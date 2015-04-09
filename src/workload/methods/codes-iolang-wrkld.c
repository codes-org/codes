/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */
#include <ross.h>
#include "src/iokernellang/CodesIOKernelTypes.h"
#include "src/iokernellang/CodesIOKernelParser.h"
#include "src/iokernellang/CodesIOKernelContext.h"
#include "src/iokernellang/codesparser.h"
#include "src/iokernellang/CodesKernelHelpers.h"
#include "src/iokernellang/codeslexer.h"

#include "codes/codes-workload.h"
#include "src/workload/codes-workload-method.h"
#include "codes/quickhash.h"

#define RANK_HASH_TABLE_SIZE 400 

/* This file implements the CODES workload API for the I/O kernel language of
the BG/P storage model */

/* load the workload file */
int iolang_io_workload_load(const char* params, int rank);

/* get next operation */
void iolang_io_workload_get_next(int rank, struct codes_workload_op *op);

/* mapping from bg/p operation enums to CODES workload operations enum */
static int convertTypes(int inst);
static int hash_rank_compare(void *key, struct qhash_head *link);

typedef struct codes_iolang_wrkld_state_per_rank codes_iolang_wrkld_state_per_rank;
static struct qhash_table *rank_tbl = NULL;
static int rank_tbl_pop = 0;
int num_ranks = -1;

/* implements the codes workload method */
struct codes_workload_method iolang_workload_method =
{
    .method_name = "iolang_workload",
    .codes_workload_load = iolang_io_workload_load,
    .codes_workload_get_next = iolang_io_workload_get_next,
};

/* state of the I/O workload that each simulated compute node/MPI rank will have */
struct codes_iolang_wrkld_state_per_rank
{
    int rank;
    CodesIOKernelContext codes_context;
    CodesIOKernel_pstate * codes_pstate;
    codeslang_inst next_event;
    struct qhash_head hash_link;
    codes_workload_info task_info;
};

/* loads the workload file for each simulated MPI rank/ compute node LP */
int iolang_io_workload_load(const char* params, int rank)
{
    int t = -1;
    iolang_params* i_param = (struct iolang_params*)params;

    /* we have to get the number of compute nodes/ranks from the bg/p model parameters
     * because the number of ranks are specified in the iolang config file not the
     * workload files */
    if(num_ranks == -1)
        num_ranks = i_param->num_cns;

    codes_iolang_wrkld_state_per_rank* wrkld_per_rank = NULL;
    if(!rank_tbl)
    {
	  rank_tbl = qhash_init(hash_rank_compare, quickhash_32bit_hash, RANK_HASH_TABLE_SIZE);
	  if(!rank_tbl)
	  {
		  return -1;
	  }
    }
    wrkld_per_rank = (codes_iolang_wrkld_state_per_rank*)malloc(sizeof(*wrkld_per_rank));
    if(!wrkld_per_rank)
	    return -1;

    wrkld_per_rank->codes_pstate = CodesIOKernel_pstate_new();
    wrkld_per_rank->rank = rank;
    t = codes_kernel_helper_bootstrap(i_param->io_kernel_path, 
				      i_param->io_kernel_meta_path,
        			      rank, 
                          num_ranks,
                      i_param->use_relpath,
				      &(wrkld_per_rank->codes_context), 
				      &(wrkld_per_rank->codes_pstate), 
				      &(wrkld_per_rank->task_info), 
				      &(wrkld_per_rank->next_event));
    qhash_add(rank_tbl, &(wrkld_per_rank->rank), &(wrkld_per_rank->hash_link));
    rank_tbl_pop++;
    return t;
}

/* Maps the enum types from I/O language to the CODES workload API */
static int convertTypes(int inst)
{
    switch(inst)
    {
        case CL_WRITEAT: /* write to file */
	    return CODES_WK_WRITE;
	case CL_READAT:
	    return CODES_WK_READ;
	case CL_CLOSE:
	    return CODES_WK_CLOSE; /* close the file */
	case CL_OPEN:
	    return CODES_WK_OPEN; /* open file */
	case CL_SYNC:
	    return CODES_WK_BARRIER; /* barrier in CODES workload is similar to sync in I/O lang? */
	case CL_SLEEP:
	    return CODES_WK_DELAY; /* sleep or delay */
	case CL_EXIT:
	    return CODES_WK_END; /* end of the operations/ no more operations in file */
	case CL_DELETE:
	    return CODES_WK_IGNORE;
	case CL_GETRANK: 
	    return CODES_WK_IGNORE; /* defined in I/O lang but not in workloads API*/
	case CL_GETSIZE: 
	    return CODES_WK_IGNORE; /* defined in I/O lang but not in workload API */
	default:
	   return CODES_WK_IGNORE;
    } 
}

/* Gets the next operation specified in the workload file for the simulated MPI rank */
void iolang_io_workload_get_next(int rank, struct codes_workload_op *op)
{
    /* If the number of simulated compute nodes per LP is initialized only then we get the next operation
	else we return an error code may be?  */
        codes_iolang_wrkld_state_per_rank* next_wrkld;
	struct qhash_head *hash_link = NULL; 
	hash_link = qhash_search(rank_tbl, &rank);
	if(!hash_link)
	{
		op->op_type = CODES_WK_END;
		return;
	}
	next_wrkld = qhash_entry(hash_link, struct codes_iolang_wrkld_state_per_rank, hash_link);

	int type = codes_kernel_helper_parse_input(next_wrkld->codes_pstate, &(next_wrkld->codes_context),&(next_wrkld->next_event));
        op->op_type = (enum codes_workload_op_type) convertTypes(type);
        if (op->op_type == CODES_WK_IGNORE)
            return;
	switch(op->op_type)
	{
	    case CODES_WK_WRITE:
	    {
                op->u.write.file_id = (next_wrkld->next_event).var[0];
		op->u.write.offset = (next_wrkld->next_event).var[2];
		op->u.write.size = (next_wrkld->next_event).var[1];
	    }
	    break;
	    case CODES_WK_DELAY:
	    {
            /* io language represents delays in nanoseconds */
            op->u.delay.seconds = (double)(next_wrkld->next_event).var[0] / (1000 * 1000 * 1000);
	    }
	    break;
	    case CODES_WK_END:
	    {
		/* delete the hash entry*/
		  qhash_del(hash_link); 
		  rank_tbl_pop--;

		  /* if no more entries are there, delete the hash table */
		  if(!rank_tbl_pop)
          {
            qhash_finalize(rank_tbl);
            rank_tbl = NULL;
          }
	    }
	    break;
	    case CODES_WK_CLOSE:
	    {
	        op->u.close.file_id = (next_wrkld->next_event).var[0];
	    }
	    break;
	    case CODES_WK_BARRIER:
	    {
	       op->u.barrier.count = num_ranks;
	       op->u.barrier.root = 0;
	    }
	    break;
	    case CODES_WK_OPEN:
	    {
	        op->u.open.file_id =  (next_wrkld->next_event).var[0];
            op->u.open.create_flag = 1;
	    }
	    break;
	    case CODES_WK_READ:
	    {
                op->u.read.file_id = (next_wrkld->next_event).var[0];
		op->u.read.offset = (next_wrkld->next_event).var[2];
		op->u.read.size = (next_wrkld->next_event).var[1];
	    }
	    break;
	    default:
	     {

		// Return error code
		//printf("\n Invalid operation specified %d ", op->op_type);
	     }
	}
    return;
}

static int hash_rank_compare(void *key, struct qhash_head *link)
{
    int *in_rank = (int *)key;
    codes_iolang_wrkld_state_per_rank *tmp;

    tmp = qhash_entry(link, codes_iolang_wrkld_state_per_rank, hash_link);
    if (tmp->rank == *in_rank)
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
