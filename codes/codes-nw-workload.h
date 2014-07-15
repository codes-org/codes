/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#ifndef CODES_NW_WORKLOAD_H
#define CODES_NW_WORKLOAD_H

#include "ross.h"

#define MAX_NAME_LENGTH 512

/* struct to hold the actual data from a single MPI event*/
typedef struct mpi_event_list mpi_event_list;
typedef struct scala_trace_params scala_trace_params;

#ifdef USE_DUMPI
typedef struct dumpi_trace_params dumpi_trace_params;
#endif

struct scala_trace_params
{
   char offset_file_name[MAX_NAME_LENGTH];
   char nw_wrkld_file_name[MAX_NAME_LENGTH];
};

#ifdef USE_DUMPI
struct dumpi_trace_params
{
   char file_name[MAX_NAME_LENGTH];
};
#endif

enum NW_WORKLOADS
{
   SCALA_TRACE = 1,
#ifdef USE_DUMPI
   DUMPI,
#endif
   OTHERS, /* add the names of other workload generators here */
};
enum mpi_workload_type
{
    /* terminator; there are no more operations for this rank */
     CODES_NW_END = 1,
    /* sleep/delay to simulate computation or other activity */
     CODES_NW_DELAY,
    /* MPI blocking send operation */
     CODES_NW_SEND,
    /* MPI blocking recv operation */
     CODES_NW_RECV,
    /* MPI non-blocking send operation */
     CODES_NW_ISEND,
    /* MPI non-blocking receive operation */
     CODES_NW_IRECV,
    /* MPI broadcast operation */
     CODES_NW_BCAST,
    /* MPI Allgather operation */
     CODES_NW_ALLGATHER,
     /* MPI Allgatherv operation */
     CODES_NW_ALLGATHERV,
    /* MPI Alltoall operation */
     CODES_NW_ALLTOALL,
    /* MPI Alltoallv operation */
     CODES_NW_ALLTOALLV,
    /* MPI Reduce operation */
     CODES_NW_REDUCE,
    /* MPI Allreduce operation */
     CODES_NW_ALLREDUCE,
    /* Generic collective operation */
    CODES_NW_COL
};

/* data structure for holding data from a MPI event (coming through scala-trace) 
*  can be a delay, isend, irecv or a collective call */
struct mpi_event_list
{
    /* what type of operation this is */
    enum mpi_workload_type op_type;
    double start_time;
    double end_time;

   /* parameters for each operation type */
    union
    {
  	struct
  	{
	   double nsecs;
	   double seconds;
  	} delay;
        struct
  	{
      	    int source_rank;/* source rank of MPI send message */
            int dest_rank; /* dest rank of MPI send message */
	    int num_bytes;
            int blocking; /* boolean value to indicate if message is blocking or non-blocking*/
	} send;
       struct
       {
     	    int source_rank;/* source rank of MPI recv message */
     	    int dest_rank;/* dest rank of MPI recv message */
	    int num_bytes;
     	    int blocking;/* boolean value to indicate if message is blocking or non-blocking*/
       } recv; 
      struct
      {
	  int num_bytes;
      } collective;
    }u;
};


/* read in the metadata file about the MPI event information
   and populate the MPI events array */
int codes_nw_workload_load(const char* type_name, const char* params, int rank);

/* retrieves the next network operation to execute. the wkld_id is the 
   identifier returned by the init() function.  The op argument is a pointer
   to a structure to be filled in with network operation information */
void codes_nw_workload_get_next(int wkld_id, int rank, struct mpi_event_list *op); 

/* Reverse of the above function */
void codes_nw_workload_get_next_rc(int wkld_id, int rank, const struct mpi_event_list* op);

void codes_nw_workload_print_op(FILE* f, struct mpi_event_list* op, int rank);
#endif /* CODES_NW_WORKLOAD_H */

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
