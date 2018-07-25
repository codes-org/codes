/*
 * Copyright (C) 2015 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#include <mpi.h>
#include <ross.h>

MPI_Comm MPI_COMM_CODES = MPI_COMM_WORLD;

/*
 * Needs to be called AFTER tw_init() because in tw_init, 
 * ROSS may split the MPI_COMM_ROSS communicator
 */
void codes_comm_update()
{
    MPI_COMM_CODES = MPI_COMM_ROSS;
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 *  indent-tabs-mode: nil
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
