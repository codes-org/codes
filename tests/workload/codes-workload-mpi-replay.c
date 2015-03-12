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
#include <sys/stat.h>
#include <mpi.h>

#include "codes/codes-workload.h"
#include "codes/quickhash.h"
#include "codes/configuration.h"

#define DEBUG_PROFILING 0

/* hash table entry for looking up file descriptor of a workload file id */
struct file_info
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
static double opt_delay_pct = 1.0;

/* hash table for storing file descriptors of opened files */
static struct qhash_table *fd_table = NULL;

/* file stream to log rank events to, if verbose turned on */
static FILE *log_stream = NULL;

/* global variables for profiling different portions of the replay, if enabled */
#if DEBUG_PROFILING
static double total_open_time = 0.0;
static double total_close_time = 0.0;
static double total_read_time = 0.0;
static double total_write_time = 0.0;
static double total_delay_time = 0.0;
static double total_barrier_time = 0.0;
#endif

void usage(char *exename)
{
    fprintf(stderr, "Usage: %s [OPTIONS] --conf <conf_file_path>\n       "
            "--test-dir <workload_test_dir>\n\n", exename);
    fprintf(stderr, "\t<conf_file_path> : (absolute) path to a valid workload configuration file\n");
    fprintf(stderr, "\t<workload_test_dir> : the directory to replay the workload I/O in\n");
    fprintf(stderr, "\n\t[OPTIONS] includes:\n");
    fprintf(stderr, "\t\t--noop : do not perform i/o\n");
    fprintf(stderr, "\t\t    -v : verbose (output i/o details)\n");

    exit(1);
}

void parse_args(int argc, char **argv, char **conf_path, char **test_dir)
{
    int index;
    static struct option long_opts[] =
    {
        {"conf", 1, NULL, 'c'},
        {"test-dir", 1, NULL, 'd'},
        {"noop", 0, NULL, 'n'},
        {"delay", 1, NULL, 'p'},
        {"help", 0, NULL, 0},
        {0, 0, 0, 0}
    };

    *conf_path = NULL;
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
            case 'c':
                *conf_path = optarg;
                break;
            case 'd':
                *test_dir = optarg;
                break;
            case 'p':
                opt_delay_pct = atof(optarg);
                break;
            case 0:
            case '?':
            default:
                usage(argv[0]);
                break;
        }
    }

    if (optind < argc || !(*conf_path) || !(*test_dir))
    {
        usage(argv[0]);
    }

    return;
}

int load_workload(char *conf_path, int rank)
{
    char workload_type[MAX_NAME_LENGTH_WKLD];

    /* load the config file across all ranks */
    configuration_load(conf_path, MPI_COMM_WORLD, &config);

    /* get the workload type out of PARAMS */
    configuration_get_value(&config, "PARAMS", "workload_type",
                            NULL, workload_type, MAX_NAME_LENGTH_WKLD);

    /* set up the workload parameters and load into the workload API */
    if (strcmp(workload_type, "darshan_io_workload") == 0)
    {
        struct darshan_params d_params;
        char aggregator_count[10];

        /* get the darshan params from the config file */
        configuration_get_value(&config, "PARAMS", "log_file_path",
                                NULL, d_params.log_file_path, MAX_NAME_LENGTH_WKLD);
        configuration_get_value(&config, "PARAMS", "aggregator_count", NULL, aggregator_count, 10);
        d_params.aggregator_cnt = atol(aggregator_count);

        return codes_workload_load(workload_type, (char *)&d_params, rank);
    }
    else if (strcmp(workload_type, "iolang_workload") == 0)
    {
        struct iolang_params i_params;
        char rank_count[10];

        /* get the iolang i/o params from the config file */
        configuration_get_value(&config, "PARAMS", "io_kernel_meta_path",
                                NULL, i_params.io_kernel_meta_path, MAX_NAME_LENGTH_WKLD);
        configuration_get_value(&config, "PARAMS", "rank_count", NULL, rank_count, 10);
        strcpy(i_params.io_kernel_path, "");
        i_params.num_cns = atoi(rank_count);
        i_params.use_relpath = 1;

        return codes_workload_load(workload_type, (char *)&i_params, rank);
    }
    else if (strcmp(workload_type, "recorder_io_workload") == 0) {
        struct recorder_params r_params;
        char nprocs[10];

        /* get the recorder params from the config file */
        configuration_get_value(&config, "PARAMS", "trace_dir_path",
                                NULL, r_params.trace_dir_path, MAX_NAME_LENGTH_WKLD);
        configuration_get_value(&config, "PARAMS", "nprocs", NULL, nprocs, 10);
        r_params.nprocs = atol(nprocs);

        return codes_workload_load(workload_type, (char *)&r_params, rank);

	}
    else
    {
        fprintf(stderr, "Error: Invalid workload type specified (%s)\n", workload_type);
        return -1;
    }
}

char* buf = NULL;
int fd = -1;
int op_num = 0;

int main(int argc, char *argv[])
{
    char *conf_path;
    char *replay_test_path;
    char *log_dir = "log";
    char my_log_path[MAX_NAME_LENGTH_WKLD];
    int nprocs;
    int myrank;
    int workload_id;
    struct codes_workload_op next_op;
    long long int replay_op_number = 1;
    double load_start, load_end;
    int ret = 0;
    int i;

    /* parse command line args */
    parse_args(argc, argv, &conf_path, &replay_test_path);

    /* initialize MPI */
    MPI_Init(&argc, &argv);
    MPI_Comm_size(MPI_COMM_WORLD, &nprocs);
    MPI_Comm_rank(MPI_COMM_WORLD, &myrank);

    /* start workload load timer */
    load_start = MPI_Wtime();

    /* change the working directory to be the test directory */
    ret = chdir(replay_test_path);
    if (ret < 0)
    {
        fprintf(stderr, "Unable to change to testing directory (%s)\n", strerror(errno));
        goto error_exit;
    }

    /* set the path for logging this rank's events, if verbose is turned on */
    if (opt_verbose)
    {
        mkdir(log_dir, 0755);
        snprintf(my_log_path, MAX_NAME_LENGTH_WKLD, "%s/rank-%d.log", log_dir, myrank);
        log_stream = fopen(my_log_path, "w");
        if (log_stream == NULL)
        {
            fprintf(stderr, "Unable to open log file %s\n", my_log_path);
            goto error_exit;
        }
    }

    /* initialize workload generator from config file */
    workload_id = load_workload(conf_path, myrank);
    if (workload_id < 0)
    {
        goto error_exit;
    }

    /* initialize hash table for storing file descriptors */
    fd_table = qhash_init(hash_file_compare, quickhash_64bit_hash, 29);
    if (!fd_table)
    {
        fprintf(stderr, "File descriptor hash table memory error\n");
        goto error_exit;
    }

    /* synchronize before replay */
    MPI_Barrier(MPI_COMM_WORLD);

    /* loading is finished */
    load_end = MPI_Wtime();

    if (myrank == 0) printf("Note: Workload took %.2lf seconds to load.\n", load_end - load_start);

    /* replay loop */
    while (1)
    {
        /* get the next replay operation from the workload generator */
        codes_workload_get_next(workload_id, myrank, &next_op);

        if (next_op.op_type != CODES_WK_END)
        {

            if (next_op.op_type == CODES_WK_DELAY)
                next_op.u.delay.seconds *= opt_delay_pct;

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

    if (log_stream)
        fclose(log_stream);

    /* destroy and finalize the file descriptor hash table */
    qhash_destroy_and_finalize(fd_table, struct file_info, hash_link, free);

#if DEBUG_PROFILING
    printf("Rank %d:\td=%.4lf, b=%.4lf, o=%.4lf, c=%.4lf, r=%.4lf, w=%.4lf\n",
           myrank, total_delay_time, total_barrier_time, total_open_time,
           total_close_time, total_read_time, total_write_time);
#endif

error_exit:
    MPI_Finalize();

    return ret;
}

int replay_workload_op(struct codes_workload_op replay_op, int rank, long long int op_number)
{
    struct timespec delay;
    int open_flags = O_RDWR;
    char file_name[250];
    int fildes;
    struct file_info *tmp_list = NULL;
    struct qlist_head *hash_link = NULL;
    int ret;
    int i;

#if DEBUG_PROFILING
    double start, end;
    start = MPI_Wtime();
#endif

    switch (replay_op.op_type)
    {
        case CODES_WK_DELAY:
            if (opt_verbose)
                fprintf(log_stream, "[Rank %d] Operation %lld : DELAY %lf seconds\n",
                       rank, op_number, replay_op.u.delay.seconds);

            if (!opt_noop)
            {
                /* satisfy delay using second delay then microsecond delay */
                delay.tv_sec = (long long)replay_op.u.delay.seconds;
                delay.tv_nsec = (unsigned int)((replay_op.u.delay.seconds - delay.tv_sec) *
                                               1000 * 1000 * 1000);
                ret = nanosleep(&delay, NULL);
                if (ret)
                {
                    /* error in sleep */
                    errno = EINTR;
                    fprintf(stderr, "Rank %d failure on operation %lld [DELAY: %s]\n",
                            rank, op_number, strerror(errno));
                    return -1;
                }
#if DEBUG_PROFILING
                end = MPI_Wtime();
                total_delay_time += (end - start);
#endif
            }
            return 0;
        case CODES_WK_BARRIER:
            if (opt_verbose)
                fprintf(log_stream, "[Rank %d] Operation %lld : BARRIER\n", rank, op_number);

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

#if DEBUG_PROFILING
                end = MPI_Wtime();
                total_barrier_time += (end - start);
#endif
            }
            return 0;
        case CODES_WK_OPEN:
            if (opt_verbose)
                fprintf(log_stream, "[Rank %d] Operation %lld: %s file %"PRIu64"\n", rank, op_number,
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
                tmp_list = malloc(sizeof(struct file_info));
                if (!tmp_list)
                {
                    fprintf(stderr, "No memory available for file hash entry\n");
                    return -1;
                }

                tmp_list->file_hash = replay_op.u.open.file_id;
                tmp_list->file_descriptor = fildes;
                qhash_add(fd_table, &(replay_op.u.open.file_id), &(tmp_list->hash_link));

                if (!buf)
                {
                    buf = malloc(16*1024*1024);
                    assert(buf);
                    for(i=0; i<16*1024*1024; i++)
                    {
                        buf[i] = '1';
                    }
                }

#if DEBUG_PROFILING
                end = MPI_Wtime();
                total_open_time += (end - start);
#endif
            }
            return 0;
        case CODES_WK_CLOSE:
            if (opt_verbose)
                fprintf(log_stream, "[Rank %d] Operation %lld : CLOSE file %"PRIu64"\n",
                        rank, op_number, replay_op.u.close.file_id);

            if (!opt_noop)
            {
                /* search for the corresponding file descriptor in the hash table */
                hash_link = qhash_search_and_remove(fd_table, &(replay_op.u.close.file_id));
                assert(hash_link);
                tmp_list = qhash_entry(hash_link, struct file_info, hash_link);
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

#if DEBUG_PROFILING
                end = MPI_Wtime();
                total_close_time += (end - start);
#endif
            }
            return 0;
        case CODES_WK_WRITE:
            if (opt_verbose)
                fprintf(log_stream, "[Rank %d] Operation %lld : WRITE file %"PRIu64" (sz = %"PRId64
                       ", off = %"PRId64")\n",
                       rank, op_number, replay_op.u.write.file_id, replay_op.u.write.size,
                       replay_op.u.write.offset);

            if (!opt_noop)
            {
                /* search for the corresponding file descriptor in the hash table */
                hash_link = qhash_search(fd_table, &(replay_op.u.write.file_id));
                assert(hash_link);
                tmp_list = qhash_entry(hash_link, struct file_info, hash_link);
                fildes = tmp_list->file_descriptor;

                ret = pwrite(fildes, buf, replay_op.u.write.size, replay_op.u.write.offset);

                if (ret < 0)
                {
                    fprintf(stderr, "Rank %d failure on operation %lld [WRITE: %s]\n",
                            rank, op_number, strerror(errno));
                    return -1;
                }

#if DEBUG_PROFILING
                end = MPI_Wtime();
                total_write_time += (end - start);
#endif
            }
            return 0;
        case CODES_WK_READ:
            if (opt_verbose)
                fprintf(log_stream, "[Rank %d] Operation %lld : READ file %"PRIu64" (sz = %"PRId64
                        ", off = %"PRId64")\n", rank, op_number, replay_op.u.read.file_id,
                       replay_op.u.read.size, replay_op.u.read.offset);

            if (!opt_noop)
            {
                /* search for the corresponding file descriptor in the hash table */
                hash_link = qhash_search(fd_table, &(replay_op.u.read.file_id));
                assert(hash_link);
                tmp_list = qhash_entry(hash_link, struct file_info, hash_link);
                fildes = tmp_list->file_descriptor;

                ret = pread(fildes, buf, replay_op.u.read.size, replay_op.u.read.offset);

                if (ret < 0)
                {
                    fprintf(stderr, "Rank %d failure on operation %lld [READ: %s]\n",
                            rank, op_number, strerror(errno));
                    return -1;
                }

#if DEBUG_PROFILING
                end = MPI_Wtime();
                total_read_time += (end - start);
#endif
            }
            return 0;
        default:
            fprintf(stderr, "** Rank %d: INVALID OPERATION (op count = %lld) **\n", rank, op_number);
            return 0;
    }

}

int hash_file_compare(void *key, struct qlist_head *link)
{
    uint64_t *in_file_hash = (uint64_t *)key;
    struct file_info *tmp_file;

    tmp_file = qlist_entry(link, struct file_info, hash_link);
    if (tmp_file->file_hash == *in_file_hash)
        return 1;

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

