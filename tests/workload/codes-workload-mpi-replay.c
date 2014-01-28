/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

/* SUMMARY:
 *
 *  MPI replay tool for replaying workloads from the codes workload API. 
 *
 */

#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include <inttypes.h>
#include <math.h>
#include <getopt.h>
#include <fcntl.h>
#include <mpi.h>

#include "codes/codes-workload.h"
#include "codes/quickhash.h"

struct file_info_list
{
    struct qlist_head hash_link;
    uint64_t file_hash;
    int file_descriptor;
};

int replay_workload_op(struct codes_workload_op replay_op, int rank, long long int op_number);
int hash_file_compare(void *key, struct qlist_head *link);

/* command line options */
static int opt_verbose = 0;
static int opt_noop = 0;

/* hash table for storing file descriptors of opened files */
static struct qhash_table *fd_table = NULL;

void usage(char *exename)
{
    fprintf(stderr, "Usage: %s [OPTIONS] --wkld-type <workload_type>\n       "
            "[--wkld-params <workload_params>] --test-dir <workload_test_dir>\n\n", exename);
    fprintf(stderr, "\t<workload_type> : a valid codes workload generator name\n");
    fprintf(stderr, "\t<workload_params> : parameters for the workload generator (possibly none)\n");
    fprintf(stderr, "\t<workload_test_dir> : the directory to replay the workload I/O in\n");
    fprintf(stderr, "\n\t[OPTIONS] includes:\n");
    fprintf(stderr, "\t\t--noop : do not perform i/o\n");
    fprintf(stderr, "\t\t    -v : verbose (output i/o details)\n");   

    exit(1);
}

void parse_args(int argc, char **argv, char **workload_type, char **workload_params, char **test_dir)
{
    int index;
    static struct option long_opts[] =
    {
        {"wkld-type", 1, NULL, 't'},
        {"wkld-params", 1, NULL, 'p'},
        {"test-dir", 1, NULL, 'd'},
        {"noop", 0, NULL, 'n'},
        {"help", 0, NULL, 0},
        {0, 0, 0, 0}
    };

    *workload_type = NULL;
    *workload_params = NULL;
    *test_dir = NULL;
    while (1)
    {
        int c = getopt_long(argc, argv, "v", long_opts, &index);

        if (c == -1)
            break;

        switch (c)
        {
            case 'v':
                opt_verbose = 1;
                break;
            case 'n':
                opt_noop = 1;
                break;
            case 't':
                *workload_type = optarg;
                break;
            case 'p':
                *workload_params = optarg;
                break;
            case 'd':
                *test_dir = optarg;
                break;
            case 0:
            case '?':
            default:
                usage(argv[0]);
                break;
        }
    }

    if (optind < argc || !(*workload_type) || !(*test_dir))
    {
        usage(argv[0]);
    }

    return;
}

int main(int argc, char *argv[])
{
    char *workload_type;
    char *workload_params;
    char *replay_test_path;
    int nprocs;
    int myrank;
    int workload_id;
    struct codes_workload_op next_op;
    long long int replay_op_number = 1;
    int ret = 0;

    /* parse command line args */
    parse_args(argc, argv, &workload_type, &workload_params, &replay_test_path);

    /* initialize MPI */
    MPI_Init(&argc, &argv);
    MPI_Comm_size(MPI_COMM_WORLD, &nprocs);
    MPI_Comm_rank(MPI_COMM_WORLD, &myrank);

    /* initialize given workload generator */
    workload_id = codes_workload_load(workload_type, workload_params, myrank);
    if (workload_id < 0)
    {
        fprintf(stderr, "Unable to initialize workload %s (params = %s)\n",
                workload_type, workload_params);
        goto error_exit;
    }

    /* change the working directory to be the test directory */
    ret = chdir(replay_test_path);
    if (ret < 0)
    {
        fprintf(stderr, "Unable to change to testing directory (%s)\n", strerror(errno));
        goto error_exit;
    }

    /* initialize hash table for storing file descriptors */
    fd_table = qhash_init(hash_file_compare, quickhash_64bit_hash, 29);
    if (!fd_table)
    {
        fprintf(stderr, "File descriptor hash table memory error\n");
        goto error_exit;
    }

    /* replay loop */
    while (1)
    {
        /* get the next replay operation from the workload generator */
        codes_workload_get_next(workload_id, myrank, &next_op);

        if (next_op.op_type != CODES_WK_END)
        {
            /* replay the next workload operation */
            ret = replay_workload_op(next_op, myrank, replay_op_number++);
            if (ret < 0)
            {
                break;
            }
        }
        else
        {
            /* workload replay for this rank is complete */
            break;
        }
    }

    /* destroy and finalize the file descriptor hash table */
    qhash_destroy_and_finalize(fd_table, struct file_info_list, hash_link, free);

error_exit:

    MPI_Finalize();
    return ret;
}

int replay_workload_op(struct codes_workload_op replay_op, int rank, long long int op_number)
{
    unsigned int secs;
    unsigned int usecs;
    int open_flags = O_RDWR;
    char file_name[50];
    int fildes;
    struct file_info_list *tmp_list = NULL;
    struct qlist_head *hash_link = NULL;
    char *buf = NULL;
    int ret;

    switch (replay_op.op_type)
    {
        case CODES_WK_DELAY:
            if (opt_verbose)
                printf("[Rank %d] Operation %lld : DELAY %lf seconds\n",
                       rank, op_number, replay_op.u.delay.seconds);

            if (!opt_noop)
            {
                /* satisfy delay using second delay then microsecond delay */
                secs = floor(replay_op.u.delay.seconds);
                usecs = round((replay_op.u.delay.seconds - secs) * 1000 * 1000);
                ret = sleep(secs);
                if (ret)
                {
                    /* error in sleep */
                    errno = EINTR;
                    fprintf(stderr, "Rank %d failure on operation %lld [DELAY: %s]\n",
                            rank, op_number, strerror(errno));
                    return -1;
                }
                ret = usleep(usecs);
                if (ret < 0)
                {
                    /* error in usleep */
                    fprintf(stderr, "Rank %d failure on operation %lld [DELAY: %s]\n",
                            rank, op_number, strerror(errno));
                    return -1;
                }
            }
            return 0;
        case CODES_WK_BARRIER:
            if (opt_verbose)
                printf("[Rank %d] Operation %lld : BARRIER\n", rank, op_number);

            if (!opt_noop)
            {
                /* implement barrier using MPI global barrier on all ranks */
                ret = MPI_Barrier(MPI_COMM_WORLD);
                if (ret != MPI_SUCCESS)
                {
                    /* error in MPI_Barrier */
                    fprintf(stderr, "Rank %d failure on operation %lld [BARRIER: %s]\n",
                            rank, op_number, "Invalid communicator");
                    return -1;
                }
            }
            return 0;
        case CODES_WK_OPEN:
            if (opt_verbose)
                printf("[Rank %d] Operation %lld: %s file %"PRIu64"\n", rank, op_number,
                       (replay_op.u.open.create_flag) ? "CREATE" : "OPEN", replay_op.u.open.file_id);

            if (!opt_noop)
            {
                /* set the create flag, if necessary */
                if (replay_op.u.open.create_flag)
                    open_flags |= O_CREAT;

                /* write the file hash to string to be used as the actual file name */
                snprintf(file_name, sizeof(file_name), "%"PRIu64, replay_op.u.open.file_id);

                /* perform the open operation */
                fildes = open(file_name, open_flags, 0666);
                if (fildes < 0)
                {
                    fprintf(stderr, "Rank %d failure on operation %lld [%s: %s]\n",
                            rank, op_number, (replay_op.u.open.create_flag) ? "CREATE" : "OPEN",
                            strerror(errno));
                    return -1;
                }

                /* save the file descriptor for this file in a hash table to be retrieved later */
                tmp_list = malloc(sizeof(struct file_info_list));
                if (!tmp_list)
                {
                    fprintf(stderr, "No memory available for file hash entry\n");
                    return -1;
                }

                tmp_list->file_hash = replay_op.u.open.file_id;
                tmp_list->file_descriptor = fildes;
                qhash_add(fd_table, &(replay_op.u.open.file_id), &(tmp_list->hash_link));
            }
            return 0;
        case CODES_WK_CLOSE:
            if (opt_verbose)
                printf("[Rank %d] Operation %lld : CLOSE file %"PRIu64"\n", rank, op_number,
                       replay_op.u.close.file_id);

            if (!opt_noop)
            {
                /* search for the corresponding file descriptor in the hash table */
                hash_link = qhash_search_and_remove(fd_table, &(replay_op.u.close.file_id));
                assert(hash_link);
                tmp_list = qhash_entry(hash_link, struct file_info_list, hash_link);
                fildes = tmp_list->file_descriptor;
                free(tmp_list);
     
                /* perform the close operation */
                ret = close(fildes);
                if (ret < 0)
                {
                    fprintf(stderr, "Rank %d failure on operation %lld [CLOSE: %s]\n",
                            rank, op_number, strerror(errno));
                    return -1;
                }
            }
            return 0;
        case CODES_WK_WRITE:
            if (opt_verbose)
                printf("[Rank %d] Operation %lld : WRITE file %"PRIu64" (sz = %"PRId64
                       ", off = %"PRId64")\n",
                       rank, op_number, replay_op.u.write.file_id, replay_op.u.write.size,    
                       replay_op.u.write.offset);

            if (!opt_noop)
            {
                /* search for the corresponding file descriptor in the hash table */
                hash_link = qhash_search(fd_table, &(replay_op.u.write.file_id));
                assert(hash_link);
                tmp_list = qhash_entry(hash_link, struct file_info_list, hash_link);
                fildes = tmp_list->file_descriptor;

                /* perform the write operation */
                buf = malloc(replay_op.u.write.size);
                if (!buf)
                {
                    fprintf(stderr, "No memory available for write buffer\n");
                    return -1;
                }
                ret = pwrite(fildes, buf, replay_op.u.write.size, replay_op.u.write.offset);
                free(buf);
                if (ret < 0)
                {
                    fprintf(stderr, "Rank %d failure on operation %lld [WRITE: %s]\n",
                            rank, op_number, strerror(errno));
                    return -1;
                }
            }
            return 0;
        case CODES_WK_READ:
            if (opt_verbose)
                printf("[Rank %d] Operation %lld : READ file %"PRIu64" (sz = %"PRId64", off = %"
                       PRId64")\n", rank, op_number, replay_op.u.read.file_id,
                       replay_op.u.read.size, replay_op.u.read.offset);

            if (!opt_noop)
            {
                /* search for the corresponding file descriptor in the hash table */
                hash_link = qhash_search(fd_table, &(replay_op.u.read.file_id));
                assert(hash_link);
                tmp_list = qhash_entry(hash_link, struct file_info_list, hash_link);
                fildes = tmp_list->file_descriptor;

                /* perform the write operation */
                buf = malloc(replay_op.u.read.size);
                if (!buf)
                {
                    fprintf(stderr, "No memory available for write buffer\n");
                    return -1;
                }
                ret = pread(fildes, buf, replay_op.u.read.size, replay_op.u.read.offset);
                free(buf);
                if (ret < 0)
                {
                    fprintf(stderr, "Rank %d failure on operation %lld [READ: %s]\n",
                            rank, op_number, strerror(errno));
                    return -1;
                }
            }
            return 0;
        default:
            fprintf(stderr, "** Rank %d: INVALID OPERATION (op count = %lld) **\n", rank, op_number);
            return -1;
    }

}

int hash_file_compare(void *key, struct qlist_head *link)
{
    uint64_t *in_file_hash = (uint64_t *)key;
    struct file_info_list *tmp_file;

    tmp_file = qlist_entry(link, struct file_info_list, hash_link);
    if (tmp_file->file_hash == *in_file_hash)
        return 1;

    return 0;
}
