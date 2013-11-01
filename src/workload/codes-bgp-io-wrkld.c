/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */
#include <ross.h>
#include <codes/CodesIOKernelTypes.h>
#include <codes/CodesIOKernelParser.h>
#include <codes/CodesIOKernelContext.h>
#include <codes/codesparser.h>
#include <codes/codeslogging.h>
#include <codes/CodesKernelHelpers.h>
#include <codes/codeslexer.h>

#include "codes/codes-workload.h"
#include "codes-workload-method.h"

/* This file implements the CODES workload API for the I/O kernel language of
the BG/P storage model */

/* load the workload file */
int bgp_io_workload_load(const char* params, int rank);

/* get next operation */
void bgp_io_workload_get_next(int rank, struct codes_workload_op *op);

/* mapping from bg/p operation enums to CODES workload operations enum */
static int convertTypes(int inst);

/* get additional workload information if any*/
void* bgp_io_workload_get_info(int rank);

typedef struct codes_bgp_wrkld_state_per_cn codes_bgp_wrkld_state_per_cn;

/* I/O language state data structure for each simulated compute node / MPI rank */
static codes_bgp_wrkld_state_per_cn* wrkld_arr = NULL;

/* number of compute nodes/ simulated MPI ranks per LP */
static int num_cns_per_lp = -1;

/* implements the codes workload method */
struct codes_workload_method bgp_io_workload_method =
{
    .method_name = "bgp_io_workload",
    .codes_workload_load = bgp_io_workload_load,
    .codes_workload_get_next = bgp_io_workload_get_next,
    .codes_workload_get_info = bgp_io_workload_get_info,
};

/* state of the I/O workload that each simulated compute node/MPI rank will have */
struct codes_bgp_wrkld_state_per_cn
{
    CodesIOKernelContext codes_context;
    CodesIOKernel_pstate * codes_pstate;
    codeslang_inst next_event;
    app_cf_info_t task_info;
};

/* returns information that is used further by the BG/P model for running multiple jobs */
void* bgp_io_workload_get_info(int rank)
{
    int local_rank = (rank - g_tw_mynode) / tw_nnodes();
    return &(wrkld_arr[local_rank].task_info);
}

/* loads the workload file for each simulated MPI rank/ compute node LP */
int bgp_io_workload_load(const char* params, int rank)
{
    int t = -1;
    bgp_params* b_param = (bgp_params*)params;
 
    if(!wrkld_arr)
    {
	num_cns_per_lp = b_param->num_cns_per_lp;
       	wrkld_arr = malloc(num_cns_per_lp * sizeof(codes_bgp_wrkld_state_per_cn));    
    }
    int local_rank = (rank - g_tw_mynode)/tw_nnodes(); 
    wrkld_arr[local_rank].codes_pstate = CodesIOKernel_pstate_new();
    t = codes_kernel_helper_bootstrap(b_param->io_kernel_path, 
				      b_param->io_kernel_def_path, 
				      b_param->io_kernel_meta_path,
        			      rank, 
				      &(wrkld_arr[local_rank].codes_context), 
				      &(wrkld_arr[local_rank].codes_pstate), 
				      &(wrkld_arr[local_rank].task_info), 
				      &(wrkld_arr[local_rank].next_event));
    return t;
}

/* Maps the enum types from I/O language to the CODES workload API */
static int convertTypes(int inst)
{
    int op_type = -1;
    switch(inst)
    {
        case CL_WRITEAT: /* write to file */
        {
	    op_type = CODES_WK_WRITE;
        }
	break;
	case CL_CLOSE:
	{
	    op_type = CODES_WK_CLOSE; /* close the file */
	}
	break;
	case CL_OPEN:
	{
	    op_type = CODES_WK_OPEN; /* open file */
	}
	break;
	case CL_SYNC:
	{
	    op_type = CODES_WK_BARRIER; /* barrier in CODES workload is similar to sync in I/O lang? */
	}
	break;
	case CL_SLEEP:
	{
	    op_type = CODES_WK_DELAY; /* sleep or delay */
	}
	break;
	case CL_EXIT:
	{
	    op_type = CODES_WK_END; /* end of the operations/ no more operations in file */
	}
	break;
	case CL_DELETE:
	{
	    op_type = -2;
	}
	break;
	case CL_GETRANK: 
	{
	    op_type = -3; /* defined in I/O lang but not in workloads API*/
	}
	break;
	case CL_GETSIZE: 
	{
	    op_type = -4; /* defined in I/O lang but not in workload API */
	}
	break;
	default:
	{
	   //printf("\n convert type undefined %d ", inst);
	   op_type = -1; 
	}
    }
    return op_type;
}

/* Gets the next operation specified in the workload file for the simulated MPI rank
TODO: some of the workloads are yet to be added like the READ operation */
void bgp_io_workload_get_next(int rank, struct codes_workload_op *op)
{
    /* If the number of simulated compute nodes per LP is initialized only then we get the next operation
	else we return an error code may be?  */
    if(num_cns_per_lp > -1)
    {
        int local_rank = (rank - g_tw_mynode) / tw_nnodes();
        int type = codes_kernel_helper_parse_input(wrkld_arr[local_rank].codes_pstate, &(wrkld_arr[local_rank].codes_context),&(wrkld_arr[local_rank].next_event));
        op->op_type = convertTypes(type);
	switch(op->op_type)
	{
	    case CODES_WK_WRITE:
	    {
                op->u.write.file_id = (wrkld_arr[local_rank].next_event).var[0];
		op->u.write.offset = (wrkld_arr[local_rank].next_event).var[2];
		op->u.write.size = (wrkld_arr[local_rank].next_event).var[1];
		//printf("\n Write size %d offset %d file id %d ", op->write.size, op->write.offset, op->write.file_id);	
	    }
	    break;
	    case CODES_WK_DELAY:
	    {
		op->u.delay.seconds = (wrkld_arr[local_rank].next_event).var[0];
	    }
	    break;
	    case CODES_WK_END:
	    {
		/* do nothing */
	    }
	    break;
	    case CODES_WK_CLOSE:
	    {
	        op->u.close.file_id = (wrkld_arr[local_rank].next_event).var[0];
	    }
	    break;
	    case CODES_WK_BARRIER:
	    {
	       op->u.barrier.count = wrkld_arr[local_rank].task_info.num_lrank;
	       op->u.barrier.root = 0;
	    }
	    break;
	    case CODES_WK_OPEN:
	    {
	        op->u.open.file_id =  (wrkld_arr[local_rank].next_event).var[0];
	    }
	    break;
	    case CODES_WK_READ:
	    {
		/* to be added (the BG/P model does not supports read operations right now) */
	    }
	    break;
	    default:
	     {
		// Return error code
		//printf("\n Invalid operation specified %d ", op->op_type);
	     }
	}
    }
    return;
}
