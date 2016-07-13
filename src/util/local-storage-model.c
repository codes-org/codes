/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#include <assert.h>
#include <ross.h>
#include <codes/lp-io.h>
#include <codes/jenkins-hash.h>
#include <codes/codes.h>
#include <codes/codes_mapping.h>
#include <codes/lp-type-lookup.h>
#include <codes/local-storage-model.h>
#include <codes/quicklist.h>
#include <codes/rc-stack.h>

#define CATEGORY_NAME_MAX 16
#define CATEGORY_MAX 12

int lsm_in_sequence = 0;
tw_stime lsm_msg_offset = 0.0;

/* holds statistics about disk traffic on each LP */
typedef struct lsm_stats_s
{
    char category[CATEGORY_NAME_MAX];
    long read_count;
    long read_bytes;
    long read_seeks;
    tw_stime read_time;
    long write_count;
    long write_bytes;
    long write_seeks;
    tw_stime write_time;
} lsm_stats_t;

/*
 * disk model parameters
 */
typedef struct disk_model_s
{
    unsigned int *request_sizes;
    double *write_rates;
    double *read_rates;
    double *write_overheads;
    double *read_overheads;
    double *write_seeks;
    double *read_seeks;
    unsigned int bins;
    // sched params
    //   0  - no scheduling
    //  >0  - make scheduler with use_sched priority lanes
    int use_sched;
} disk_model_t;

/*
 * lsm_message_data_t
 *   - data used for input in transfer time calculation
 *   - data comes for caller
 *   - object: id of byte stream which could be a file, object, etc.
 *   - offset: offset into byte stream
 *   - size: size in bytes of request
 */
typedef struct lsm_message_data_s
{
    uint64_t    object;
    uint64_t    offset;
    uint64_t    size;
    char category[CATEGORY_NAME_MAX]; /* category for traffic */
    int prio; // for scheduling
} lsm_message_data_t;

/*
 * lsm_sched_op_s - operation to be scheduled
 */
typedef struct lsm_sched_op_s
{
    lsm_message_data_t data;
    struct qlist_head ql;
} lsm_sched_op_t;

/*
 * lsm_sched_s - data structure for implementing scheduling loop
 */
typedef struct lsm_sched_s
{
    int num_prios;
    // number of pending requests, incremented on new and decremented on
    // complete
    int active_count;
    // scheduler mallocs data per-request - hold onto and free later
    struct rc_stack *freelist;
    struct qlist_head *queues;
} lsm_sched_t;

/*
 * lsm_state_s
 *   - state tracking structure for each LP node
 *   - next_idle: next point in time the disk will be idle
 *   - model: disk parameters
 *   - current_offset: last offset the disk operated on
 *   - current_object: last object id that operated on
 */
typedef struct lsm_state_s
{
    tw_stime next_idle;
    disk_model_t *model;
    int64_t  current_offset;
    uint64_t current_object;
    lsm_stats_t lsm_stats_array[CATEGORY_MAX];
    /* scheduling state */
    int use_sched;
    lsm_sched_t sched;
} lsm_state_t;

/*
 * lsm_message_t
 *   - holds event data
 *   - event: event type
 *   - data: IO request data
 *   - wrap: wrapped event data of caller
 */
typedef struct lsm_message_s
{
    int magic; /* magic number */
    lsm_event_t event;
    int prio; // op priority (user-set on request, used by LSM in rc for complete)
    tw_stime    prev_idle;
    lsm_stats_t prev_stat;
    int64_t     prev_offset;
    uint64_t    prev_object;
    lsm_message_data_t data;
    struct codes_cb_params cb;
} lsm_message_t;

/*
 * Prototypes
 */
static void lsm_lp_init (lsm_state_t *ns, tw_lp *lp);
static void lsm_event (lsm_state_t *ns, tw_bf *b, lsm_message_t *m, tw_lp *lp);
static void lsm_rev_event (lsm_state_t *ns, tw_bf *b, lsm_message_t *m, tw_lp *lp);
static void lsm_finalize (lsm_state_t *ns, tw_lp *lp);
static void handle_io_sched_new(lsm_state_t *ns, tw_bf *b, lsm_message_t *m_in, tw_lp *lp);
static void handle_rev_io_sched_new(lsm_state_t *ns, tw_bf *b, lsm_message_t *m_in, tw_lp *lp);
static void handle_io_request(lsm_state_t *ns, tw_bf *b, lsm_message_data_t *data, lsm_message_t *m_in, tw_lp *lp);
static void handle_rev_io_request(lsm_state_t *ns, tw_bf *b, lsm_message_data_t *data, lsm_message_t *m_in, tw_lp *lp);
static void handle_io_sched_compl(lsm_state_t *ns, tw_bf *b, lsm_message_t *m_in, tw_lp *lp);
static void handle_rev_io_sched_compl(lsm_state_t *ns, tw_bf *b, lsm_message_t *m_in, tw_lp *lp);
static void handle_io_completion (lsm_state_t *ns, tw_bf *b, lsm_message_t *m_in, tw_lp *lp);
static void handle_rev_io_completion (lsm_state_t *ns, tw_bf *b, lsm_message_t *m_in, tw_lp *lp);
static lsm_stats_t *find_stats(const char* category, lsm_state_t *ns);
static void write_stats(tw_lp* lp, lsm_stats_t* stat);

/*
 * Globals
 */

static int lsm_magic = 0;

/* configuration parameters (by annotation) */
static disk_model_t model_unanno, *models_anno = NULL;
static const config_anno_map_t *anno_map = NULL;

/* sched temporary for lsm_set_event_priority */
static int temp_prio = -1;

/*
 * lsm_lp
 *   - implements ROSS callback interfaces
 */
tw_lptype lsm_lp =
{
    (init_f) lsm_lp_init,
    (pre_run_f) NULL,
    (event_f) lsm_event,
    (revent_f) lsm_rev_event,
    (commit_f) NULL,
    (final_f) lsm_finalize,
    (map_f) codes_mapping,
    sizeof(lsm_state_t)
};

static tw_stime transfer_time_table (lsm_state_t *ns,
                                     lsm_stats_t *stat,
                                     int rw,
                                     uint64_t object,
                                     int64_t offset,
                                     uint64_t size)
{
    double mb;
    double time = 0.0;
    double disk_rate;
    double disk_seek;
    double disk_overhead;
    unsigned int i;

    /* find nearest size rounded down. */
    for (i = 0; i < ns->model->bins; i++)
    {
        if (ns->model->request_sizes[i] > size)
        {
            break;
        }
    }
    if (i > 0) i--;

    if (rw)
    {
        /* read */
        disk_rate = ns->model->read_rates[i];
        disk_seek = ns->model->read_seeks[i];
        disk_overhead = ns->model->read_overheads[i];
    }
    else
    {
        /* write */
        disk_rate = ns->model->write_rates[i];
        disk_seek = ns->model->write_seeks[i];
        disk_overhead = ns->model->write_overheads[i];

    }

    /* transfer time */
    mb = ((double)size) / (1024.0 * 1024.0);
    time += (mb / disk_rate) * 1000.0 * 1000.0 * 1000.0;

    /* request overhead */
    time += (disk_overhead * 1000.0);

    /* seek */
    if ((object != ns->current_object) ||
        (offset < ns->current_offset) ||
        (offset > (ns->current_offset+512)))
    {
        if (rw) stat->read_seeks++; else stat->write_seeks++;
        time += (disk_seek * 1000.0);
    }


    /* update statistics */
    if (rw)
    {
        stat->read_count += 1;
        stat->read_bytes += size;
        stat->read_time  += time;
    }
    else
    {
        stat->write_count += 1;
        stat->write_bytes += size;
        stat->write_time  += time;
    }

    return time;
}

void lsm_io_event_rc(tw_lp *sender)
{
    codes_local_latency_reverse(sender);
}

tw_lpid lsm_find_local_device(
        struct codes_mctx const * map_ctx,
        tw_lpid sender_gid)
{
    return codes_mctx_to_lpid(map_ctx, LSM_NAME, sender_gid);
}

void lsm_io_event(
        const char * lp_io_category,
        uint64_t io_object,
        int64_t  io_offset,
        uint64_t io_size_bytes,
        int      io_type,
        tw_stime delay,
        tw_lp *sender,
        struct codes_mctx const * map_ctx,
        int return_tag,
        msg_header const * return_header,
        struct codes_cb_info const * cb)
{
    assert(strlen(lp_io_category) < CATEGORY_NAME_MAX-1);
    assert(strlen(lp_io_category) > 0);
    SANITY_CHECK_CB(cb, lsm_return_t);

    tw_lpid lsm_id = codes_mctx_to_lpid(map_ctx, LSM_NAME, sender->gid);

    tw_stime delta = delay + codes_local_latency(sender);
    if (lsm_in_sequence) {
        tw_stime tmp = lsm_msg_offset;
        lsm_msg_offset += delta;
        delta += tmp;
    }

    tw_event *e = tw_event_new(lsm_id, delta, sender);
    lsm_message_t *m = tw_event_data(e);
    m->magic = lsm_magic;
    m->event = (lsm_event_t) io_type;
    m->data.object = io_object;
    m->data.offset = io_offset;
    m->data.size   = io_size_bytes;
    strcpy(m->data.category, lp_io_category);

    // get the priority count for checking
    int num_prios = lsm_get_num_priorities(map_ctx, sender->gid);
    // prio checks and sets
    if (num_prios <= 0) // disabled scheduler - ignore
        m->data.prio = 0;
    else if (temp_prio < 0) // unprovided priority - defer to max possible
        m->data.prio = num_prios-1;
    else if (temp_prio < num_prios) // valid priority
        m->data.prio = temp_prio;
    else
        tw_error(TW_LOC,
                "LP %lu, LSM LP %lu: Bad priority (%d supplied, %d lanes)\n",
                sender->gid, lsm_id, temp_prio, num_prios);
    // reset temp_prio
    temp_prio = -1;

    m->cb.info = *cb;
    m->cb.h = *return_header;
    m->cb.tag = return_tag;

    tw_event_send(e);
}

int lsm_get_num_priorities(
        struct codes_mctx const * map_ctx,
        tw_lpid sender_id)
{
    char const * annotation =
        codes_mctx_get_annotation(map_ctx, LSM_NAME, sender_id);

    if (annotation == NULL) {
        assert(anno_map->has_unanno_lp);
        return model_unanno.use_sched;
    }
    else {
        for (int i = 0; i < anno_map->num_annos; i++) {
            if (strcmp(anno_map->annotations[i].ptr, annotation) == 0)
                return models_anno[i].use_sched;
        }
        assert(0);
        return -1;
    }
}

void lsm_set_event_priority(int prio)
{
    temp_prio = prio;
}

/*
 * lsm_lp_init
 *   - initialize the lsm model
 *   - sets the disk to be idle now
 */
static void lsm_lp_init (lsm_state_t *ns, tw_lp *lp)
{
    memset(ns, 0, sizeof(*ns));

    ns->next_idle = tw_now(lp);

    // set the correct model
    const char *anno = codes_mapping_get_annotation_by_lpid(lp->gid);
    if (anno == NULL)
        ns->model = &model_unanno;
    else {
        int id = configuration_get_annotation_index(anno, anno_map);
        ns->model = &models_anno[id];
    }

    // initialize the scheduler if need be
    ns->use_sched = ns->model->use_sched > 0;
    if (ns->use_sched) {
        ns->sched.num_prios = ns->model->use_sched;
        ns->sched.active_count = 0;
        rc_stack_create(&ns->sched.freelist);
        ns->sched.queues =
            malloc(ns->sched.num_prios * sizeof(*ns->sched.queues));
        for (int i = 0; i < ns->sched.num_prios; i++)
            INIT_QLIST_HEAD(&ns->sched.queues[i]);
    }

    return;
}

/*
 * lsm_event
 *   - event handler callback
 *   - dispatches the events to the appropriate handlers
 *   - handles initializtion of node state
 */
static void lsm_event (lsm_state_t *ns, tw_bf *b, lsm_message_t *m, tw_lp *lp)
{
    assert(m->magic == lsm_magic);

    switch (m->event)
    {
        case LSM_WRITE_REQUEST:
        case LSM_READ_REQUEST:
            if (LSM_DEBUG)
                printf("svr(%llu): REQUEST obj:%llu off:%llu size:%llu\n",
                    (unsigned long long)lp->gid,
                    (unsigned long long)m->data.object,
                    (unsigned long long)m->data.offset,
                    (unsigned long long)m->data.size);
            assert(ns->model);
            if (ns->use_sched)
                handle_io_sched_new(ns, b, m, lp);
            else
                handle_io_request(ns, b, &m->data, m, lp);
            break;
        case LSM_WRITE_COMPLETION:
        case LSM_READ_COMPLETION:
            if (LSM_DEBUG)
                printf("svr(%llu): COMPLETION\n",
                    (unsigned long long)lp->gid);
            handle_io_completion(ns, b, m, lp);
            break;
        default:
            printf("svr(%llu): Unknown Event:%d\n",
                (unsigned long long)lp->gid,
                m->event);
            break;
    }

    return;
}

/*
 * lsm_rev_event
 *   - callback to reverse an event
 */
static void lsm_rev_event(lsm_state_t *ns,
                          tw_bf *b,
                          lsm_message_t *m,
                          tw_lp *lp)
{
    assert(m->magic == lsm_magic);

    switch (m->event)
    {
        case LSM_WRITE_REQUEST:
        case LSM_READ_REQUEST:
            if (LSM_DEBUG)
                printf("svr(%llu): reverse REQUEST obj:%llu off:%llu size:%llu\n",
                    (unsigned long long)lp->gid,
                    (unsigned long long)m->data.object,
                    (unsigned long long)m->data.offset,
                    (unsigned long long)m->data.size);
            if (ns->use_sched)
                handle_rev_io_sched_new(ns, b, m, lp);
            else
                handle_rev_io_request(ns, b, &m->data, m, lp);
            break;
        case LSM_WRITE_COMPLETION:
        case LSM_READ_COMPLETION:
            if (LSM_DEBUG)
                printf("svr(%llu): reverse COMPLETION\n",
                    (unsigned long long)lp->gid);
            handle_rev_io_completion(ns, b, m, lp);
            break;
        default:
            printf("svr(%llu): reverse Unknown Event:%d\n",
                (unsigned long long)lp->gid,
                m->event);
            break;
    }

    return;
}

/*
 * lsm_finalize
 *   - callback to release model resources
 */
static void lsm_finalize(lsm_state_t *ns,
                         tw_lp *lp)
{
    int i;
    lsm_stats_t all;

    memset(&all, 0, sizeof(all));
    sprintf(all.category, "all");

    for(i=0; i<CATEGORY_MAX; i++)
    {
        if(strlen(ns->lsm_stats_array[i].category) > 0)
        {
            all.write_count += ns->lsm_stats_array[i].write_count;
            all.write_bytes += ns->lsm_stats_array[i].write_bytes;
            all.write_time += ns->lsm_stats_array[i].write_time;
            all.write_seeks += ns->lsm_stats_array[i].write_seeks;
            all.read_count += ns->lsm_stats_array[i].read_count;
            all.read_bytes += ns->lsm_stats_array[i].read_bytes;
            all.read_seeks += ns->lsm_stats_array[i].read_seeks;
            all.read_time += ns->lsm_stats_array[i].read_time;

            write_stats(lp, &ns->lsm_stats_array[i]);
        }
    }

    write_stats(lp, &all);

    return;
}

static void handle_io_sched_new(
        lsm_state_t *ns,
        tw_bf *b,
        lsm_message_t *m_in,
        tw_lp *lp)
{
    if (LSM_DEBUG)
        printf("handle_io_sched_new called\n");
    // if nothing else is going on, then issue directly
    if (!ns->sched.active_count)
        handle_io_request(ns, b, &m_in->data, m_in, lp);
    else {
        lsm_sched_op_t *op = malloc(sizeof(*op));
        op->data = m_in->data;
        qlist_add_tail(&op->ql, &ns->sched.queues[m_in->prio]);
    }
    ns->sched.active_count++;
}

static void handle_rev_io_sched_new(
        lsm_state_t *ns,
        tw_bf *b,
        lsm_message_t *m_in,
        tw_lp *lp)
{
    if (LSM_DEBUG)
        printf("handle_rev_io_sched_new called\n");
    ns->sched.active_count--;
    if (!ns->sched.active_count)
        handle_rev_io_request(ns, b, &m_in->data, m_in, lp);
    else {
        struct qlist_head *ent = qlist_pop_back(&ns->sched.queues[m_in->prio]);
        assert(ent);
        lsm_sched_op_t *op = qlist_entry(ent, lsm_sched_op_t, ql);
        free(op);
    }
}

static void handle_io_sched_compl(
        lsm_state_t *ns,
        tw_bf *b,
        lsm_message_t *m_in,
        tw_lp *lp)
{
    if (LSM_DEBUG)
        printf("handle_io_sched_compl called\n");
    ns->sched.active_count--;
    if (ns->sched.active_count) {
        lsm_sched_op_t *next = NULL;
        struct qlist_head *ent = NULL;
        for (int i = 0; i < ns->sched.num_prios; i++) {
            ent = qlist_pop(&ns->sched.queues[i]);
            if (ent != NULL) {
                next = qlist_entry(ent, lsm_sched_op_t, ql);
                m_in->prio = i;
                break;
            }
        }
        assert(next);
        handle_io_request(ns, b, &next->data, m_in, lp);
        // now done with this request metadata
        rc_stack_push(lp, next, free, ns->sched.freelist);
    }
}

static void handle_rev_io_sched_compl(
        lsm_state_t *ns,
        tw_bf *b,
        lsm_message_t *m_in,
        tw_lp *lp)
{
    if (LSM_DEBUG)
        printf("handle_rev_io_sched_compl called\n");
    if (ns->sched.active_count) {
        lsm_sched_op_t *prev = rc_stack_pop(ns->sched.freelist);
        handle_rev_io_request(ns, b, &prev->data, m_in, lp);
        qlist_add_tail(&prev->ql, &ns->sched.queues[m_in->prio]);
    }
    ns->sched.active_count++;
}


/*
 * handle_io_request
 *   - handles the IO request events
 *   - computes the next_idle time
 *   - fires disk completion event at computed time
 */
static void handle_io_request(lsm_state_t *ns,
                              tw_bf *b,
                              lsm_message_data_t *data,
                              lsm_message_t *m_in,
                              tw_lp *lp)
{
    (void)b;
    tw_stime queue_time, t_time;
    tw_event *e;
    lsm_message_t *m_out;
    lsm_stats_t *stat;
    int rw = (m_in->event == LSM_READ_REQUEST) ? 1 : 0;

    tw_stime (*transfer_time) (lsm_state_t *, lsm_stats_t *, int, uint64_t, int64_t, uint64_t);

    transfer_time = transfer_time_table;

    stat = find_stats(data->category, ns);

    /* save history for reverse operation */
    m_in->prev_idle   = ns->next_idle;
    m_in->prev_stat   = *stat;
    m_in->prev_object = ns->current_object;
    m_in->prev_offset = ns->current_offset;

    if (ns->next_idle > tw_now(lp))
    {
        queue_time = ns->next_idle - tw_now(lp);
    }
    else
    {
        queue_time = 0;
    }


    t_time = transfer_time(ns,
                           stat,
                           rw,
                           data->object,
                           data->offset,
                           data->size);
    queue_time += t_time;
    ns->next_idle = queue_time + tw_now(lp);
    ns->current_offset = data->offset + data->size;
    ns->current_object = data->object;

    e = tw_event_new(lp->gid, queue_time, lp);
    m_out = (lsm_message_t*)tw_event_data(e);

    memcpy(m_out, m_in, sizeof(*m_in));
    if (m_out->event == LSM_WRITE_REQUEST)
    {
        m_out->event = LSM_WRITE_COMPLETION;
    }
    else
    {
        m_out->event = LSM_READ_COMPLETION;
    }

    m_out->prio = m_in->prio;

    tw_event_send(e);

    return;
}


/*
 * handle_rev_io_request
 *   - handle reversing the io request
 */
static void handle_rev_io_request(lsm_state_t *ns,
                                  tw_bf *b,
                                  lsm_message_data_t *data,
                                  lsm_message_t *m_in,
                                  tw_lp *lp)
{
    (void)b;
    (void)lp;
    lsm_stats_t *stat;

    stat = find_stats(data->category, ns);

    ns->next_idle = m_in->prev_idle;
    *stat = m_in->prev_stat;
    ns->current_object = m_in->prev_object;
    ns->current_offset = m_in->prev_offset;

    return;
}

/*
 * handle_io_completion
 *   - handle IO completion events
 *   - invoke the callers original completion event
 */
static void handle_io_completion (lsm_state_t *ns,
                                  tw_bf *b,
                                  lsm_message_t *m_in,
                                  tw_lp *lp)
{
    SANITY_CHECK_CB(&m_in->cb.info, lsm_return_t);

    tw_event * e = tw_event_new(m_in->cb.h.src, codes_local_latency(lp), lp);
    void * m = tw_event_data(e);

    GET_INIT_CB_PTRS(&m_in->cb, m, lp->gid, h, tag, rc, lsm_return_t);

    /* no failures to speak of yet */
    rc->rc = 0;

    tw_event_send(e);

    // continue the loop
    if (ns->use_sched)
        handle_io_sched_compl(ns, b, m_in, lp);

    return;
}

/*
 * handle_rev_io_completion
 *   - reverse io completion event
 *   - currently nothing to do in this case
 */
static void handle_rev_io_completion (lsm_state_t *ns,
                                      tw_bf *b,
                                      lsm_message_t *m_in,
                                      tw_lp *lp)
{
    if (ns->use_sched)
        handle_rev_io_sched_compl(ns, b, m_in, lp);

    codes_local_latency_reverse(lp);
    return;
}

static lsm_stats_t *find_stats(const char* category, lsm_state_t *ns)
{
    int i;
    int new_flag = 0;
    int found_flag = 0;

    for(i=0; i<CATEGORY_MAX; i++)
    {
        if(strlen(ns->lsm_stats_array[i].category) == 0)
        {
            found_flag = 1;
            new_flag = 1;
            break;
        }
        if(strcmp(category, ns->lsm_stats_array[i].category) == 0)
        {
            found_flag = 1;
            new_flag = 0;
            break;
        }
    }
    assert(found_flag);

    if(new_flag)
    {
        strcpy(ns->lsm_stats_array[i].category, category);
    }
    return(&ns->lsm_stats_array[i]);

}

static void write_stats(tw_lp* lp, lsm_stats_t* stat)
{
    int ret;
    char id[32];
    char data[1024];

    sprintf(id, "lsm-category-%s", stat->category);
    sprintf(data, "lp:%ld\twrite_count:%ld\twrite_bytes:%ld\twrite_seeks:%ld\twrite_time:%f\t"
        "read_count:%ld\tread_bytes:%ld\tread_seeks:%ld\tread_time:%f\n",
        (long)lp->gid,
        stat->write_count,
        stat->write_bytes,
        stat->write_seeks,
        stat->write_time,
        stat->read_count,
        stat->read_bytes,
        stat->read_seeks,
        stat->read_time);

    ret = lp_io_write(lp->gid, id, strlen(data), data);
    assert(ret == 0);

    return;

}

void lsm_register(void)
{
    uint32_t h1=0, h2=0;

    bj_hashlittle2("localstorage", strlen("localstorage"), &h1, &h2);
    lsm_magic = h1+h2;

    lp_type_register(LSM_NAME, &lsm_lp);
}

// read the configuration file for a given annotation
static void read_config(ConfigHandle *ch, char const * anno, disk_model_t *model)
{
    char       **values;
    size_t       length;
    int          rc;
    // request sizes
    rc = configuration_get_multivalue(ch, LSM_NAME, "request_sizes", anno,
            &values,&length);
    assert(rc == 1);
    model->request_sizes = (unsigned int*)malloc(sizeof(int)*length);
    assert(model->request_sizes);
    model->bins = length;
    for (size_t i = 0; i < length; i++)
    {
        model->request_sizes[i] = atoi(values[i]);
    }
    free(values);

    // write rates
    rc = configuration_get_multivalue(ch, LSM_NAME, "write_rates", anno,
            &values,&length);
    assert(rc == 1);
    model->write_rates = (double*)malloc(sizeof(double)*length);
    assert(model->write_rates);
    assert(length == model->bins);
    for (size_t i = 0; i < length; i++)
    {
        model->write_rates[i] = strtod(values[i], NULL);
    }
    free(values);

    // read rates
    rc = configuration_get_multivalue(ch, LSM_NAME, "read_rates", anno,
            &values,&length);
    assert(rc == 1);
    model->read_rates = (double*)malloc(sizeof(double)*length);
    assert(model->read_rates);
    assert(model->bins == length);
    for (size_t i = 0; i < length; i++)
    {
        model->read_rates[i] = strtod(values[i], NULL);
    }
    free(values);

    // write overheads
    rc = configuration_get_multivalue(ch, LSM_NAME, "write_overheads", anno,
            &values,&length);
    assert(rc == 1);
    model->write_overheads = (double*)malloc(sizeof(double)*length);
    assert(model->write_overheads);
    assert(model->bins == length);
    for (size_t i = 0; i < length; i++)
    {
        model->write_overheads[i] = strtod(values[i], NULL);
    }
    free(values);

    // read overheades
    rc = configuration_get_multivalue(ch, LSM_NAME, "read_overheads", anno,
            &values,&length);
    assert(rc == 1);
    model->read_overheads = (double*)malloc(sizeof(double)*length);
    assert(model->read_overheads);
    assert(model->bins == length);
    for (size_t i = 0; i < length; i++)
    {
        model->read_overheads[i] = strtod(values[i], NULL);
    }
    free(values);

    // write seek latency
    rc = configuration_get_multivalue(ch, LSM_NAME, "write_seeks", anno,
            &values,&length);
    assert(rc == 1);
    model->write_seeks = (double*)malloc(sizeof(double)*length);
    assert(model->write_seeks);
    assert(model->bins == length);
    for (size_t i = 0; i < length; i++)
    {
        model->write_seeks[i] = strtod(values[i], NULL);
    }
    free(values);

    // read seek latency
    rc = configuration_get_multivalue(ch, LSM_NAME, "read_seeks", anno,
            &values,&length);
    assert(rc == 1);
    model->read_seeks = (double*)malloc(sizeof(double)*length);
    assert(model->read_seeks);
    assert(model->bins == length);
    for (size_t i = 0; i < length; i++)
    {
        model->read_seeks[i] = strtod(values[i], NULL);
    }
    free(values);

    // scheduling parameters (this can fail)
    configuration_get_value_int(ch, LSM_NAME, "enable_scheduler", anno,
            &model->use_sched);
    assert(model->use_sched >= 0);
}

void lsm_configure(void)
{
    /* check and see if any lsm LPs are being used - otherwise,
     * skip the config */
    if (0 == codes_mapping_get_lp_count(NULL, 0, LSM_NAME, NULL, 1))
        return;

    anno_map = codes_mapping_get_lp_anno_map(LSM_NAME);
    assert(anno_map);
    models_anno = (disk_model_t*)malloc(anno_map->num_annos * sizeof(*models_anno));

    // read the configuration for unannotated entries
    if (anno_map->has_unanno_lp > 0){
        read_config(&config, NULL, &model_unanno);
    }

    for (int i = 0; i < anno_map->num_annos; i++){
        char const * anno = anno_map->annotations[i].ptr;
        read_config(&config, anno, &models_anno[i]);
    }
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
