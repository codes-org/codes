/*
 * Copyright (C) 2017 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#ifndef CODES_CONC_ADDON_H
#define CODES_CONC_ADDON_H

#ifdef __cplusplus
extern "C" {
#endif

#ifdef USE_CONC
#include <ncptl/ncptl.h> 
#endif
#include <mpi.h>

#define MAX_CONC_ARGV 128

typedef struct conc_bench_param conc_bench_param;


struct conc_bench_param {
    char conc_program[MAX_CONC_ARGV];
    int conc_argc;
    char config_in[MAX_CONC_ARGV][MAX_CONC_ARGV];
    char *conc_argv[MAX_CONC_ARGV];
};


int codes_conc_bench_load(
        const char* program,
        int argc, 
        char *argv[]);

void CODES_MPI_Comm_size (MPI_Comm comm, int *size);
void CODES_MPI_Comm_rank( MPI_Comm comm, int *rank );
void CODES_MPI_Finalize();
void CODES_MPI_Send(const void *buf, 
            int count, 
            MPI_Datatype datatype, 
            int dest, 
            int tag,
            MPI_Comm comm);
void CODES_MPI_Recv(void *buf, 
            int count, 
            MPI_Datatype datatype, 
            int source, 
            int tag,
            MPI_Comm comm, 
            MPI_Status *status);
void CODES_MPI_Sendrecv(const void *sendbuf, 
            int sendcount, 
            MPI_Datatype sendtype,
            int dest, 
            int sendtag,
            void *recvbuf, 
            int recvcount, 
            MPI_Datatype recvtype,
            int source, 
            int recvtag,
            MPI_Comm comm, 
            MPI_Status *status);
void CODES_MPI_Barrier(MPI_Comm comm);
void CODES_MPI_Isend(const void *buf, 
            int count, 
            MPI_Datatype datatype, 
            int dest, 
            int tag,
            MPI_Comm comm, 
            MPI_Request *request);
void CODES_MPI_Irecv(void *buf, 
            int count, 
            MPI_Datatype datatype, 
            int source, 
            int tag,
            MPI_Comm comm, 
            MPI_Request *request);
void CODES_MPI_Waitall(int count, 
            MPI_Request array_of_requests[], 
            MPI_Status array_of_statuses[]);
void CODES_MPI_Reduce(const void *sendbuf, 
            void *recvbuf, 
            int count, 
            MPI_Datatype datatype,
            MPI_Op op, 
            int root, 
            MPI_Comm comm);
void CODES_MPI_Allreduce(const void *sendbuf, 
            void *recvbuf, 
            int count, 
            MPI_Datatype datatype,
            MPI_Op op, 
            MPI_Comm comm);
void CODES_MPI_Bcast(void *buffer, 
            int count, 
            MPI_Datatype datatype, 
            int root, 
            MPI_Comm comm);
void CODES_MPI_Alltoall(const void *sendbuf, 
            int sendcount, 
            MPI_Datatype sendtype, 
            void *recvbuf,
            int recvcount, 
            MPI_Datatype recvtype, 
            MPI_Comm comm);
void CODES_MPI_Alltoallv(const void *sendbuf, 
            const int *sendcounts, 
            const int *sdispls,
            MPI_Datatype sendtype, 
            void *recvbuf, 
            const int *recvcounts,
            const int *rdispls, 
            MPI_Datatype recvtype, 
            MPI_Comm comm);

/* implementation structure */
struct codes_conceptual_bench {
    char *program_name; /* name of the conceptual program */
    int (*conceptual_main)(int argc, char *argv[]);
};


void codes_conceptual_add_bench(struct codes_conceptual_bench const * method);

#ifdef __cplusplus
}
#endif

#endif /* CODES_CONC_ADDON_H */
