/*
 * Copyright (C) 2014 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */
#include <mpi.h>
#include "codes/codes-mpi-replay.h"


int main(int argc, char** argv) {
  MPI_Init(&argc,&argv);
  
//  int rank, size;
//  MPI_Comm_rank(MPI_COMM_WORLD,&rank);
//  MPI_Comm_size(MPI_COMM_WORLD,&size);

//  MPI_Comm comm;
//  MPI_Comm_split(MPI_COMM_WORLD, rank < 2, rank, &comm);

//  if(rank < 2)
//  	modelnet_mpi_replay(comm,&argc,&argv);

   modelnet_mpi_replay(MPI_COMM_WORLD,&argc,&argv);
	MPI_Finalize();
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
