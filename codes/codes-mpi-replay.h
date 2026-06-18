/*
 * Copyright (C) 2017 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */


#ifndef CODES_MPI_REPLAY_H
#define CODES_MPI_REPLAY_H

#ifdef __cplusplus
extern "C" {
#endif

#include <mpi.h>

int modelnet_mpi_replay(MPI_Comm comm, int* argc, char*** argv);

#ifdef __cplusplus
}
#endif

#endif /* CODES_H */
