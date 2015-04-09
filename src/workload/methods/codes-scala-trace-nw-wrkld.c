/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */
#include <assert.h>

#include "ross.h"
#include "codes/codes-workload.h"
#include "src/workload/codes-workload-method.h"

int scala_trace_nw_workload_load(const char* params, int rank);
void scala_trace_nw_workload_get_next(int rank, struct codes_workload_op *op);

/* implements the codes workload method */
struct codes_workload_method scala_trace_workload_method =
{
    .method_name = "scala-trace-workload",
    .codes_workload_load = scala_trace_nw_workload_load,
    .codes_workload_get_next = scala_trace_nw_workload_get_next,
};

struct st_write_data
{
   char mpi_type[128];
   int source_rank;
   int dest_rank;
   int data_type;
   int count;
   long time_stamp;
};

struct mpi_event_info
{
   long offset;
   long events_per_rank;
};

static struct st_write_data * event_array;
static struct mpi_event_info mpi_info;
static int current_counter = 0;

int scala_trace_nw_workload_load(const char* params, int rank)
{	
    MPI_Datatype MPI_EVENTS_INFO;
    MPI_Datatype MPI_WRITE_INFO;
    
    MPI_Type_contiguous(2, MPI_LONG, &MPI_EVENTS_INFO);
    MPI_Type_commit(&MPI_EVENTS_INFO);

    MPI_Datatype data_type[6] = {MPI_CHAR, MPI_INT, MPI_INT, MPI_INT, MPI_INT, MPI_LONG};
    int blocklen[6] = {128, 1, 1, 1, 1, 1};
    MPI_Aint disp[6];

    disp[0] = 0;
    disp[1] = sizeof(char) * 128;
    disp[2] = disp[1] + sizeof(int);
    disp[3] = disp[2] + sizeof(int);
    disp[4] = disp[3] + sizeof(int);
    disp[5] = disp[4] + sizeof(int);
    MPI_Type_create_struct(6, blocklen, disp, data_type, &MPI_WRITE_INFO);
    MPI_Type_commit(&MPI_WRITE_INFO);

    scala_trace_params* st_params = (scala_trace_params*)params;

    char offset_file[MAX_NAME_LENGTH_WKLD];
    char wrkld_file[MAX_NAME_LENGTH_WKLD];

    strcpy(offset_file, st_params->offset_file_name);
    strcpy(wrkld_file, st_params->nw_wrkld_file_name);

    MPI_File fh;
    MPI_File_open(MPI_COMM_WORLD, offset_file, MPI_MODE_RDONLY, MPI_INFO_NULL, &fh);
    MPI_File_seek(fh, rank * sizeof(struct mpi_event_info), MPI_SEEK_SET);
    MPI_File_read(fh, &mpi_info, 1, MPI_EVENTS_INFO, MPI_STATUS_IGNORE);
    MPI_File_close(&fh);

    event_array = (struct st_write_data*) malloc(sizeof(struct st_write_data) * mpi_info.events_per_rank);

    //printf("\n rank %d allocated array of size %d ", rank, mpi_info.events_per_rank);
    MPI_File_open(MPI_COMM_WORLD, wrkld_file, MPI_MODE_RDONLY, MPI_INFO_NULL, &fh);
    MPI_File_set_view(fh, mpi_info.offset * sizeof(struct st_write_data), MPI_WRITE_INFO, MPI_WRITE_INFO, "derived", MPI_INFO_NULL);
    MPI_File_read(fh, event_array, mpi_info.events_per_rank, MPI_WRITE_INFO, MPI_STATUS_IGNORE);
    MPI_File_close(&fh);
    return 0;
}

void scala_trace_nw_workload_get_next(int rank, struct codes_workload_op *op)
{
   assert(current_counter <= mpi_info.events_per_rank);

   if(current_counter == mpi_info.events_per_rank)
    {
	op->op_type = CODES_WK_END;
	return;
    }
   struct st_write_data temp_data = event_array[current_counter];   

    if(strcmp( temp_data.mpi_type, "Delay") == 0)
 	{
		op->op_type = CODES_WK_DELAY;
		op->u.delay.seconds = temp_data.time_stamp;
	}
     else if (strcmp( temp_data.mpi_type, "MPI_Isend") == 0)
	{
		op->op_type = CODES_WK_SEND;
		op->u.send.source_rank = temp_data.source_rank;
		op->u.send.dest_rank = temp_data.dest_rank;
		//op->u.send.blocking = 0; /* non-blocking operation */
	}
     else if(strcmp( temp_data.mpi_type, "MPI_Irecv") == 0)
	{
		op->op_type = CODES_WK_RECV;
		op->u.recv.source_rank = temp_data.source_rank;    
		op->u.recv.dest_rank = temp_data.dest_rank; 
		//op->u.recv.blocking = 0; /* non-blocking recv operation */
	}
     else if(strcmp( temp_data.mpi_type, "MPI_Send") == 0)
	{
		op->op_type = CODES_WK_SEND;
		op->u.send.source_rank = temp_data.source_rank;
		op->u.send.dest_rank = temp_data.dest_rank;
		//op->u.send.blocking = 1; /* blocking send operation */
	}
     else if(strcmp( temp_data.mpi_type, "MPI_Recv") == 0)
	{
		op->op_type = CODES_WK_RECV;
		op->u.recv.source_rank = temp_data.source_rank;    
		op->u.recv.dest_rank = temp_data.dest_rank; 
		//op->u.recv.blocking = 1; /* blocking recv operation */
	}

   /* increment current counter */
   current_counter++;
   assert(current_counter <= mpi_info.events_per_rank);
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
