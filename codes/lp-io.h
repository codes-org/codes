/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#ifndef LP_IO_H
#define LP_IO_H

#include <ross.h>

typedef char* lp_io_handle;

#define LP_IO_UNIQ_SUFFIX 1

/* to be called (collectively) before running simulation to prepare the
 * output directory.
 */
int lp_io_prepare(char *directory, int flags, lp_io_handle* handle, MPI_Comm comm);

/* to be called within LPs to store a block of data */
int lp_io_write(tw_lpid gid, char* identifier, int size, void* buffer);

/* undo the immediately preceding write for the given LP */
int lp_io_write_rev(tw_lpid gid, char* identifier);

/* to be called (collectively) after tw_run() to flush data to disk */
int lp_io_flush(lp_io_handle handle, MPI_Comm comm);

/* retrieves the directory name for a handle */
static inline char* lp_io_handle_to_dir(lp_io_handle handle)
{
    return(strdup(handle));
}

#endif /* LP_IO_H */

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
