/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#ifndef CODES_NW_WORKLOAD_H
#define CODES_NW_WORKLOAD_H

#include "ross.h"

#define MAX_LENGTH 512
//#define MAX_REQUESTS 128

/* struct to hold the actual data from a single MPI event*/
typedef struct mpi_event_list mpi_event_list;
typedef struct scala_trace_params scala_trace_params;
typedef struct dumpi_trace_params dumpi_trace_params;

struct scala_trace_params
{
   char offset_file_name[MAX_LENGTH];
   char nw_wrkld_file_name[MAX_LENGTH];
};

struct dumpi_trace_params
{
   int num_net_traces;
   char file_name[MAX_LENGTH];
};

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
    /* sleep/delay to simulate computation or other activity */
     CODES_NW_DELAY = 1,
    /* MPI wait all operation */
     //CODES_NW_WAITALL,
    /* terminator; there are no more operations for this rank */
     CODES_NW_END,
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
    /* MPI test all operation */
     //CODES_NW_TESTALL,
    /* MPI test operation */
     //CODES_NW_TEST,
    /* Generic collective operation */
    CODES_NW_COL,
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
	    int num_bytes; /* number of bytes to be transferred over the network */
	    short data_type; /* MPI data type to be matched with the recv */
	    int count; /* number of elements to be received */
	    int tag; /* tag of the message */
	    //int32_t request;
	} send;
       struct
       {
     	    int source_rank;/* source rank of MPI recv message */
     	    int dest_rank;/* dest rank of MPI recv message */
	    int num_bytes; /* number of bytes to be transferred over the network */
	    short data_type; /* MPI data type to be matched with the send */
	    int count; /* number of elements to be sent */
	    int tag; /* tag of the message */
       	    //int32_t request;
	} recv; 
      struct
      {
	  int num_bytes;
      } collective;
      /*struct
      {
	int count;
        int requests[MAX_REQUESTS]; 
      } wait_all;
      struct
      {
	int32_t request;
	int flag;
      } test;*/
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
