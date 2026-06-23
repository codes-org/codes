/*
 * Copyright (C) 2014 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */
#include <mpi.h>
#include "codes_config.h"

#if CODES_HAVE_ONLINE
#include <abt.h>
#endif

#include "codes/codes-mpi-replay.h"


int main(int argc, char** argv) {
    MPI_Init(&argc, &argv);
#if CODES_HAVE_ONLINE
    ABT_init(argc, argv);
#endif
    //  int rank, size;
    //  MPI_Comm_rank(MPI_COMM_WORLD,&rank);
    //  MPI_Comm_size(MPI_COMM_WORLD,&size);

    //  MPI_Comm comm;
    //  MPI_Comm_split(MPI_COMM_WORLD, rank < 2, rank, &comm);

    //  if(rank < 2)
    //  	modelnet_mpi_replay(comm,&argc,&argv);

    modelnet_mpi_replay(MPI_COMM_WORLD, &argc, &argv);
    int flag;
#if CODES_HAVE_ONLINE
    ABT_finalize();
#endif

    MPI_Finalized(&flag);
    if (!flag)
        MPI_Finalize();
    return 0;
}
