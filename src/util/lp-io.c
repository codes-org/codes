/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#include <assert.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "codes/lp-io.h"

struct io_buffer
{
    tw_lpid gid;
    int size;
    void* buffer;
    struct io_buffer *next;
};

struct identifier
{
    char identifier[64];
    struct io_buffer *buffers;
    int buffers_count;
    int buffers_total_size;
    struct identifier *next;
};

/* local list of identifiers */
static struct identifier* identifiers = NULL;
int identifiers_count = 0;

/* array of all identifiers used across all procs */
struct identifier global_identifiers[64];
int global_identifiers_count = 0;

static int write_id(char* directory, char* identifier, MPI_Comm comm);

int lp_io_write(tw_lpid gid, char* identifier, int size, void* buffer)
{
    struct identifier* id;
    struct io_buffer *buf;

    if(strlen(identifier) >= 64)
    {
        fprintf(stderr, "Error: identifier %s too big.\n", identifier);
        return(-1);
    }

    buf = (struct io_buffer*)malloc(sizeof(*buf));
    if(!buf)
        return(-1);

    buf->gid = gid;
    buf->size = size;
    /* allocate copy of data being written */
    buf->buffer = malloc(size);
    if(!buf->buffer)
    {
        free(buf);
        return(-1);
    }
    memcpy(buf->buffer, buffer, size);
    buf->next = NULL;

    /* see if we have this identifier already */
    id = identifiers;
    while(id && (strcmp(identifier, id->identifier) != 0))
        id = id->next;

    if(id)
    {
        /* add buffer to front of list */
        buf->next = id->buffers;
        id->buffers  = buf;
        id->buffers_count++;
        id->buffers_total_size += size;
    }
    else
    {
        /* new identifier */
        id = (struct identifier*)malloc(sizeof(*id));
        if(!id)
        {
            free(buf->buffer);
            free(buf);
            return(-1);
        }
        strcpy(id->identifier, identifier);
        id->next = identifiers;
        identifiers = id;
        id->buffers = buf;
        id->buffers_count = 1;
        id->buffers_total_size = size;
        buf->next = NULL;
        identifiers_count++;
    }

    return(0);
}

int lp_io_write_rev(tw_lpid gid, char* identifier){
    struct identifier* id, *id_prev;
    struct io_buffer *buf, *buf_prev;

    /* find given identifier */
    if(strlen(identifier) >= 32)
    {
        fprintf(stderr, "Error: identifier %s too big.\n", identifier);
        return(-1);
    }
    id = identifiers;
    while (id && (strcmp(identifier, id->identifier) != 0)){
        id_prev = id;
        id = id->next;
    }
    if (!id){
        fprintf(stderr, "Error: identifier %s not found on reverse for LP %lu.",
                identifier,gid);
        return(-1);
    }

    /* attempt to find previous LP's write. This is made easier by the fact 
     * that the list is stored newest-to-oldest */
    buf = id->buffers; 
    buf_prev = NULL;
    while (buf){
        if (buf->gid == gid){ break; }
        buf_prev = buf;
        buf = buf->next;
    }
    if (!buf){
        fprintf(stderr, "Error: no lp-io write buffer found for LP %lu (reverse write)\n", gid);
        return(-1);
    }

    id->buffers_count--;
    id->buffers_total_size -= buf->size;
    if (id->buffers_count == 0){
        /* seg faults caused with empty identifiers for some reason - remove
         * this ID */
        if (id == identifiers){
            identifiers = id->next;
        }
        else{
            id_prev->next = id->next;
        }
        free(id);
        identifiers_count--;
    }
    /* remove the buffer from the list 
     * (NULLs for end-of-list are preserved) */
    else if (buf == id->buffers) { /* buf is head of list */
        id->buffers = buf->next;
    }
    else { /* buf is in list, has a previous element */
        buf_prev->next = buf->next; 
    }
    free(buf->buffer);
    free(buf);
    return(0);
}

int lp_io_prepare(char *directory, int flags, lp_io_handle* handle, MPI_Comm comm)
{
    int ret;
    int rank;
    int save_err;

    MPI_Comm_rank(comm, &rank);

    if(flags & LP_IO_UNIQ_SUFFIX)
    {
        *handle = (lp_io_handle)malloc(256);
        if(!(*handle))
        {
            return(-1);
        }
        if(rank == 0)
            sprintf(*handle, "%s-%ld-%ld", directory, (long)getpid(), (long)time(NULL));
        MPI_Bcast(*handle, 256, MPI_CHAR, 0, comm);
    }
    else
    {
        *handle = strdup(directory);
        if(!(*handle))
        {
            return(-1);
        }
    }

    if(rank == 0)
    {
        /* create output directory */
        ret = mkdir(*handle, 0775);
        if(ret != 0)
        {
            save_err = errno;

            fprintf(stderr, "mkdir(\"%s\") failed: %s\n", *handle, strerror(save_err));
            return(-1);
        }
    }

    MPI_Barrier(comm);

    return(0);
}

int lp_io_flush(lp_io_handle handle, MPI_Comm comm)
{
    int comm_size;
    int rank;
    MPI_Status status;
    int ret;
    int i;
    struct identifier *id;
    int match = 0;

    char* directory  = handle;

    memset(global_identifiers, 0, 64*sizeof(global_identifiers[0]));

    MPI_Comm_size(comm, &comm_size);
    MPI_Comm_rank(comm, &rank);

    /* The first thing we need to do is come up with a global list of
     * identifiers.  We can't really guarantee that every MPI proc had data
     * written to every identifier, but we want to collectively write each
     * ID.
     */

    /* To do this, each rank will receive an initial set of ideas from
     * rank+1 and add its own identifiers to the set before transmitting to
     * rank-1.  Rank 0 will then broadcast the results to everyone.
     */

    /* get array so far from rank +1 */
    if(rank != comm_size-1)
    {
        ret = MPI_Recv(&global_identifiers_count, 1, MPI_INT, rank+1, 0, comm, &status);
        assert(ret == 0);
        ret = MPI_Recv(global_identifiers, 64*sizeof(global_identifiers[0]), MPI_BYTE,
            rank+1, 0, comm, &status);
        assert(ret == 0);
    }

    /* add our own entries to the array */
    id = identifiers;
    while(id)
    {
        match = 0;
        for(i = 0; i<global_identifiers_count; i++)
        {
            if(strcmp(id->identifier, global_identifiers[i].identifier) == 0)
            {
                match = 1;
                break;
            }
        }
        if(!match)
        {
            assert(global_identifiers_count < 64);
            global_identifiers[global_identifiers_count] = *id;
            global_identifiers_count++;
        }

        id = id->next;
    }

    /* send results to rank-1 */
    if(rank != 0)
    {
        ret = MPI_Send(&global_identifiers_count, 1, MPI_INT, rank-1, 0, comm);
        assert(ret == 0);
        ret = MPI_Send(global_identifiers, 64*sizeof(global_identifiers[0]), MPI_BYTE,
            rank-1, 0, comm);
        assert(ret == 0);

    }

    /* broadcast results to everyone */
    ret = MPI_Bcast(&global_identifiers_count, 1, MPI_INT, 0, comm);
    assert(ret == 0);
    ret = MPI_Bcast(global_identifiers, 64*sizeof(global_identifiers[0]), MPI_BYTE, 0, comm);
    assert(ret == 0);

    if(rank == 0)
    {
        printf("LP-IO: writing output to %s/\n", directory);
        printf("LP-IO: data files:\n");
    }

    for(i=0; i<global_identifiers_count; i++)
    {
        if(rank == 0)
        {
            printf("   %s/%s\n", directory, global_identifiers[i].identifier);
        }

        ret = write_id(directory, global_identifiers[i].identifier, comm);
        if(ret < 0)
        {
            return(ret);
        }
    }

    free(handle);

    return(0);
}

static int write_id(char* directory, char* identifier, MPI_Comm comm)
{
    char file[256];
    MPI_File fh;
    int ret;
    long my_size = 0;
    long my_offset = 0;
    struct identifier* id;
    struct io_buffer *buf;
    char err_string[MPI_MAX_ERROR_STRING];
    int err_len;
    MPI_Datatype mtype;
    int *lengths;
    MPI_Aint *displacements;
    MPI_Aint base;
    int i;
    MPI_Status status;

    sprintf(file, "%s/%s", directory, identifier);
    ret = MPI_File_open(comm, file, MPI_MODE_CREATE|MPI_MODE_WRONLY|MPI_MODE_EXCL, MPI_INFO_NULL, &fh);
    if(ret != 0)
    {
        MPI_Error_string(ret, err_string, &err_len);
        fprintf(stderr, "Error: MPI_File_open(%s) failure: %s\n", file, err_string);
        return(-1);
    }

    /* see if we have any data for this id */
    id = identifiers;
    while(id && (strcmp(identifier, id->identifier) != 0))
        id = id->next;

    /* find my offset */
    if(id)
        my_size = id->buffers_total_size;
    MPI_Scan(&my_size, &my_offset, 1, MPI_LONG, MPI_SUM, comm);
    my_offset -= my_size;

    /* build datatype for our buffers */
    if(id)
    {
        lengths = (int*)malloc(id->buffers_count*sizeof(int));
        assert(lengths);
        displacements = (MPI_Aint*)malloc(id->buffers_count*sizeof(MPI_Aint));
        assert(displacements);

        /* NOTE: some versions of MPI-IO have a bug related to using
         * MPI_BOTTOM with hindexed types. We therefore use first pointer as
         * base and adjust the others accordingly.
         */
        buf = id->buffers;
        base = (MPI_Aint)buf->buffer;
        /* NOTE: filling in datatype arrays backwards to get buffers in the
         * order they were added on each process
         */
        i = id->buffers_count-1;
        while(buf)
        {
            displacements[i] = (MPI_Aint)buf->buffer - (MPI_Aint)base;
            lengths[i] = buf->size;
            i--;
            buf = buf->next;
        }
        MPI_Type_hindexed(id->buffers_count, lengths, displacements,
            MPI_BYTE, &mtype);
        MPI_Type_commit(&mtype);
        free(lengths);
        free(displacements);

        ret = MPI_File_write_at_all(fh, my_offset, id->buffers->buffer, 1, mtype, &status);

        MPI_Type_free(&mtype);
    }
    else
    {
        /* nothing to write, but participate in collective anyway */
        ret = MPI_File_write_at_all(fh, my_offset, NULL, 0, MPI_BYTE, &status);
    }
    if(ret != 0)
    {
        fprintf(stderr, "Error: MPI_File_write_at(%s) failure.\n", file);
        return(-1);
    }
    
    ret = MPI_File_close(&fh);
    if(ret != 0)
    {
        fprintf(stderr, "Error: MPI_File_close(%s) failure.\n", file);
        return(-1);
    }

    return(0);
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
