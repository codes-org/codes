/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

/* Darshan workload generator that plugs into the general CODES workload
 * generator API. This generator consumes an input file of Darshan I/O
 * events and passes these events to the underlying simulator.
 */
#include <fcntl.h>
#include <assert.h>
#include <sys/stat.h>

#include "codes/codes-workload.h"
#include "codes-workload-method.h"
#include "darshan-io-events.h"
#include "codes/quickhash.h"

#define DARSHAN_MAX_EVENT_READ_COUNT 100
#define DARSHAN_NEGLIGIBLE_DELAY .001

#define RANK_HASH_TABLE_SIZE 397

/* structure for storing all context needed to retrieve events for this rank */
/* TODO: if many ross LPs are mapped to this PE, then memory usage is going to increase
 *       linearly, obviously due to the static events arrays used here to store darshan events.
 *       This really needs to be fixed by coming up with a more intelligent way of representing
 *       this workload (i.e. no intermediary format). Until then, you can lower the 
 *       DARSHAN_MAX_EVENT_READ_COUNT to as low as 1 to save memory but restrict performance.
 */
struct rank_events_context
{
    int rank;
    int ind_file_fd;
    int64_t ind_event_cnt;
    int coll_file_fd;
    int64_t coll_event_cnt;
    struct darshan_event ind_events[DARSHAN_MAX_EVENT_READ_COUNT];
    struct darshan_event coll_events[DARSHAN_MAX_EVENT_READ_COUNT];
    int ind_list_ndx;
    int ind_list_max;
    int coll_list_ndx;
    int coll_list_max;
    double last_event_time;
    struct qhash_head hash_link;
};

/* CODES workload API functions for workloads generated from darshan logs*/
static int darshan_io_workload_load(const char *params, int rank);
static void darshan_io_workload_get_next(int rank, struct codes_workload_op *op);

/* helper functions for darshan workload CODES API */
static void darshan_read_events(struct rank_events_context *rank_context, int ind_flag);
static struct codes_workload_op darshan_event_to_codes_workload_op(struct darshan_event event);
static int hash_rank_compare(void *key, struct qhash_head *link);

/* workload method name and function pointers for the CODES workload API */
struct codes_workload_method darshan_io_workload_method =
{
    .method_name = "darshan_io_workload",
    .codes_workload_load = darshan_io_workload_load,
    .codes_workload_get_next = darshan_io_workload_get_next,
};

static struct qhash_table *rank_tbl = NULL;
static int rank_tbl_pop = 0;

/* info about this darshan workload group needed by bgp model */
/* TODO: is this needed for darshan workloads? */
/* TODO: does this need to be stored in the rank context to support multiple workloads? */

/* load the workload generator for this rank, given input params */
static int darshan_io_workload_load(const char *params, int rank)
{
    const char *event_file = params; /* for now, params is just the input trace file name */
    int event_file_fd;
    int64_t nprocs;
    ssize_t bytes_read;
    int64_t *rank_offsets = NULL;
    int64_t off_beg = 0;
    int64_t off_end = 0;
    int64_t i;
    int ret;
    struct stat stat_buf;
    struct rank_events_context *new = NULL;

    if (!event_file)
        return -1;

    /*  allocate a new event context for this rank */
    new = malloc(sizeof(*new));
    if (!new)
        return -1;

    new->rank = rank;
    new->last_event_time = 0.0;

    /* open the input events file */
    event_file_fd = open(event_file, O_RDONLY);
    if (event_file_fd < 0)
    {
        free(new);
        return -1;
    }

    /* read the event file header to determine the number of original ranks */
    bytes_read = read(event_file_fd, &nprocs, sizeof(int64_t));
    if (bytes_read != sizeof(int64_t))
    {
        free(new);
        close(event_file_fd);
        return -1;
    }

    rank_offsets = malloc((nprocs + 1) * sizeof(int64_t));
    if (!rank_offsets)
    {
        free(new);
        close(event_file_fd);
        return -1;
    }

    /* read the event list offset for all ranks from the event file header */
    bytes_read = read(event_file_fd, rank_offsets, (nprocs + 1) * sizeof(int64_t));
    if (bytes_read != ((nprocs + 1) * sizeof(int64_t)))
    {
        free(new);
        free(rank_offsets);
        close(event_file_fd);
        return -1;
    }

    /* determine the beginning and ending offsets for this rank's independent events */
    off_beg = rank_offsets[rank];
    if (off_beg)
    {
        /* set ending offset to be the next nonzero offset in the rank_offsets list */
        for (i = rank + 1; i < nprocs + 1; i++)
        {
            if (rank_offsets[i])
            {
                off_end = rank_offsets[i];
                break;
            }
        }

        /* if no ending is found in rank_offsets, set ending offset to be the end of file */
        if (i == (nprocs + 1))
        {
            ret = fstat(event_file_fd, &stat_buf);
            if (!ret)
                off_end = stat_buf.st_size;
            else
            {
                free(new);
                free(rank_offsets);
                close(event_file_fd);
                return -1;
            }
        }

        /* set the independent event count */
        assert(((off_end - off_beg) % sizeof(struct darshan_event)) == 0);
        new->ind_event_cnt = (off_end - off_beg) / sizeof(struct darshan_event);

        /* set independent file descriptor to the open events file and seek to this rank's offset */
        new->ind_file_fd = event_file_fd;
        ret = lseek(new->ind_file_fd, (off_t)off_beg, SEEK_SET);
        if (ret < 0)
        {
            free(new);
            free(rank_offsets);
            close(event_file_fd);
            return -1;
        }
    }
    else
    {
        new->ind_event_cnt = 0;
        new->ind_file_fd = -1;
    }

    /* determine the beginning and ending offsets for the collective events */
    off_beg = rank_offsets[nprocs];
    if (off_beg)
    {
        /* use fstat to find the ending offset of the file */
        ret = fstat(event_file_fd, &stat_buf);
        if (ret < 0)
        {
            free(new);
            free(rank_offsets);
            close(event_file_fd);
            return -1;
        }
        else
            off_end = (int64_t)stat_buf.st_size;

        /* set the collective event count */
        assert(((off_end - off_beg) % sizeof(struct darshan_event)) == 0);
        new->coll_event_cnt = (off_end - off_beg) / sizeof(struct darshan_event);

        /* open a new file descriptor for collective events, and seek to the beginning offset */
        new->coll_file_fd = open(event_file, O_RDONLY);
        ret = lseek(new->coll_file_fd, (off_t)off_beg, SEEK_SET);
        if (ret < 0)
        {
            free(new);
            free(rank_offsets);
            close(event_file_fd);
            return -1;
        }
    }
    else
    {
        new->coll_event_cnt = 0;
        new->coll_file_fd = -1;
    }

    /* initialize list variables to zero -- events are only read in the get_next function */
    new->ind_list_ndx = 0;
    new->ind_list_max = 0;
    new->coll_list_ndx = 0;
    new->coll_list_max = 0;

    /* initialize the hash table of rank contexts, if it has not been initialized */
    if (!rank_tbl)
    {
        rank_tbl = qhash_init(hash_rank_compare, quickhash_32bit_hash, RANK_HASH_TABLE_SIZE);
        if (!rank_tbl)
        {
            free(new);
            free(rank_offsets);
            close(event_file_fd);
            close(new->coll_file_fd);
            return -1;
        }
    }

    /* add this rank context to the hash table */
    qhash_add(rank_tbl, &(new->rank), &(new->hash_link));
    rank_tbl_pop++;

    free(rank_offsets);
    return 0;
}

/* pull the next event (independent or collective) for this rank from its event context */
static void darshan_io_workload_get_next(int rank, struct codes_workload_op *op)
{
    struct darshan_event next_event;
    int ind_event_flag;
    struct qhash_head *hash_link = NULL;
    struct rank_events_context *tmp = NULL;

    /* find event context for this rank in the rank hash table */
    hash_link = qhash_search(rank_tbl, &rank);

    /* terminate the workload if there is no valid rank context */
    if (!hash_link)
    {
        op->op_type = CODES_WK_END;
        return;
    }

    tmp = qhash_entry(hash_link, struct rank_events_context, hash_link);
    assert(tmp->rank == rank);

    /* read in more independent events if necessary */
    if ((tmp->ind_list_ndx == tmp->ind_list_max) && tmp->ind_event_cnt)
    {
        darshan_read_events(tmp, 1);
    }

    /* read in more collective events if necessary */
    while (1)
    {
        if ((tmp->coll_list_ndx == tmp->coll_list_max) && tmp->coll_event_cnt)
        {
            darshan_read_events(tmp, 0);
        }
        else if ((tmp->coll_list_ndx == tmp->coll_list_max) && !tmp->coll_event_cnt)
        {
            break;
        }

        /* search through collective events until one for this rank is found */
        if (tmp->coll_list_ndx < tmp->coll_list_max)
        {
            if ((tmp->coll_events[tmp->coll_list_ndx].rank == tmp->rank) ||
                (tmp->coll_events[tmp->coll_list_ndx].rank == -1))
            {
                break;
            }
            else
            {
                tmp->coll_list_ndx++;
            }
        }
    }

    if ((tmp->ind_list_ndx == tmp->ind_list_max) && (tmp->coll_list_ndx == tmp->coll_list_max))
    {
        /* no more events -- just end the workload */
        op->op_type = CODES_WK_END;
        qhash_del(hash_link);
        free(tmp);
        rank_tbl_pop--;
        if (!rank_tbl_pop)
            qhash_finalize(rank_tbl);
    }
    else
    {
        /* determine whether the next event is from the independent or collective list */
        if (tmp->coll_list_ndx == tmp->coll_list_max)
        {
            next_event = tmp->ind_events[tmp->ind_list_ndx];
            ind_event_flag = 1;
        }
        else if (tmp->ind_list_ndx == tmp->ind_list_max)
        {
            next_event = tmp->coll_events[tmp->coll_list_ndx];
            ind_event_flag = 0;
        }
        else
        {
            /* both lists have events -- choose the event with the lowest timestamp */
            if (tmp->ind_events[tmp->ind_list_ndx].start_time <
                tmp->coll_events[tmp->coll_list_ndx].start_time)
            {
                next_event = tmp->ind_events[tmp->ind_list_ndx];
                ind_event_flag = 1;
            }
            else
            {
                next_event = tmp->coll_events[tmp->coll_list_ndx];
                ind_event_flag = 0;
            }
        }

        if ((next_event.start_time - tmp->last_event_time) < DARSHAN_NEGLIGIBLE_DELAY)
        {
            /* return the next event -- there is no delay */
            *op = darshan_event_to_codes_workload_op(next_event);
            tmp->last_event_time = next_event.end_time;
            if (ind_event_flag)
                tmp->ind_list_ndx++;
            else
                tmp->coll_list_ndx++;
        }
        else
        {
            /* there is a non-negligible delay, so pass back a delay event */
            op->op_type = CODES_WK_DELAY;
            op->u.delay.seconds = next_event.start_time - tmp->last_event_time;
            tmp->last_event_time = next_event.start_time;
        }
    }

    return;
}

/* read events from the event file -- ind_flag is set if reading independent events */
static void darshan_read_events(struct rank_events_context *rank_context, int ind_flag)
{
    int fd;
    int64_t *event_cnt;
    struct darshan_event *event_list;
    int *list_ndx;
    int *list_max;
    ssize_t bytes_read;

    /* set variable values depending on the value of ind_flag */
    if (ind_flag)
    {
        fd = rank_context->ind_file_fd;
        event_cnt = &(rank_context->ind_event_cnt);
        event_list = rank_context->ind_events;
        list_ndx = &(rank_context->ind_list_ndx);
        list_max = &(rank_context->ind_list_max);
    }
    else
    {
        fd = rank_context->coll_file_fd;
        event_cnt = &(rank_context->coll_event_cnt);
        event_list = rank_context->coll_events;
        list_ndx = &(rank_context->coll_list_ndx);
        list_max = &(rank_context->coll_list_max);
    }

    /* try to read maximum number of events, if possible */
    if (*event_cnt >= DARSHAN_MAX_EVENT_READ_COUNT)
    {
        bytes_read = read(fd, event_list, DARSHAN_MAX_EVENT_READ_COUNT *
                          sizeof(struct darshan_event));
        assert(bytes_read == (DARSHAN_MAX_EVENT_READ_COUNT * sizeof(struct darshan_event)));

        /* update event list variables */
        *list_max = DARSHAN_MAX_EVENT_READ_COUNT;
        *list_ndx = 0;
        *event_cnt -= DARSHAN_MAX_EVENT_READ_COUNT;
    }
    else
    {
        bytes_read = read(fd, event_list, *event_cnt * sizeof(struct darshan_event));
        assert(bytes_read == (*event_cnt * sizeof(struct darshan_event)));

        /* update event list variables */
        *list_max = *event_cnt;
        *list_ndx = 0;
        *event_cnt = 0;
    }

    return;
}

/* take a darshan event struct as input and convert it to a codes workload op */
static struct codes_workload_op darshan_event_to_codes_workload_op(struct darshan_event event)
{
    struct codes_workload_op codes_op;

    switch (event.type)
    {
        case POSIX_OPEN:
            codes_op.op_type = CODES_WK_OPEN;
            codes_op.u.open.file_id = event.event_params.open.file;
            codes_op.u.open.create_flag = event.event_params.open.create_flag;
            break;
        case POSIX_CLOSE:
            codes_op.op_type = CODES_WK_CLOSE;
            codes_op.u.close.file_id = event.event_params.close.file;
            break;
        case POSIX_READ:
            codes_op.op_type = CODES_WK_READ;
            codes_op.u.read.file_id = event.event_params.read.file;
            codes_op.u.read.offset = event.event_params.read.offset;
            codes_op.u.read.size = event.event_params.read.size;
            break;
        case POSIX_WRITE:
            codes_op.op_type = CODES_WK_WRITE;
            codes_op.u.write.file_id = event.event_params.write.file;
            codes_op.u.write.offset = event.event_params.write.offset;
            codes_op.u.write.size = event.event_params.write.size;
            break;
        case BARRIER:
            codes_op.op_type = CODES_WK_BARRIER;
            codes_op.u.barrier.count = event.event_params.barrier.proc_count;
            codes_op.u.barrier.root = event.event_params.barrier.root;
            break;
        default:
            assert(0);
            break;
    }

    return codes_op;
}

static int hash_rank_compare(void *key, struct qhash_head *link)
{
    int *in_rank = (int *)key;
    struct rank_events_context *tmp;

    tmp = qhash_entry(link, struct rank_events_context, hash_link);
    if (tmp->rank == *in_rank)
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
