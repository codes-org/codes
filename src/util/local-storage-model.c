/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#include <assert.h>
#include <ross.h>
#include "codes/lp-io.h"
#include "codes/jenkins-hash.h"
#include "codes/codes.h"
#include "codes/codes_mapping.h"
#include "codes/lp-type-lookup.h"
#include "codes/local-storage-model.h"

#define CATEGORY_NAME_MAX 16
#define CATEGORY_MAX 12

int lsm_in_sequence = 0;
tw_stime lsm_msg_offset = 0.0;

/*
 * wrapped_event_t
 *   - holds the callers event and data they want sent upon
 *     completion of a IO operation.
 */
typedef struct wrapped_event_s
{
    tw_lpid id;
    size_t  size;
    char    message[1];
} wrapped_event_t;

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
} disk_model_t;

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
} lsm_state_t;

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
} lsm_message_data_t;

/*
 * lsm_message_init_t
 *   - event data to initiale model
 *   - rate: peak rate of disk in MiB/s
 *   - seek: avg. seek time in microseconds
 */
typedef struct lsm_message_init_s
{
    char name[32];
} lsm_message_init_t;

/*
 * lsm_message_t
 *   - holds event data
 *   - event: event type
 *   - u.data: IO request data
 .init: model initialization data
 *   - wrap: wrapped event data of caller
 */
typedef struct lsm_message_s
{
    int magic; /* magic number */
    lsm_event_t event;
    tw_stime    prev_idle;
    lsm_stats_t prev_stat;
    int64_t     prev_offset;
    uint64_t    prev_object;
    union {
        lsm_message_data_t data;
        lsm_message_init_t init;
    } u;
    wrapped_event_t wrap;
} lsm_message_t;

/*
 * Prototypes
 */
static void lsm_lp_init (lsm_state_t *ns, tw_lp *lp);
static void lsm_event (lsm_state_t *ns, tw_bf *b, lsm_message_t *m, tw_lp *lp);
static void lsm_rev_event (lsm_state_t *ns, tw_bf *b, lsm_message_t *m, tw_lp *lp);
static void lsm_finalize (lsm_state_t *ns, tw_lp *lp);
static void handle_io_request(lsm_state_t *ns, tw_bf *b, lsm_message_t *m_in, tw_lp *lp);
static void handle_rev_io_request(lsm_state_t *ns, tw_bf *b, lsm_message_t *m_in, tw_lp *lp);
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
    int i;

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

void lsm_event_new_reverse(tw_lp *sender)
{
    codes_local_latency_reverse(sender);
    return;
}

static tw_lpid lsm_find_local_device_default(
        const char * annotation,
        int          ignore_annotations,
        tw_lpid      sender_gid) {
    char group_name[MAX_NAME_LENGTH];
    int dummy, mapping_rep, mapping_offset;
    int num_lsm_lps;
    tw_lpid rtn;

    codes_mapping_get_lp_info(sender_gid, group_name, &dummy, NULL, &dummy,
            NULL, &mapping_rep, &mapping_offset);
    num_lsm_lps = codes_mapping_get_lp_count(group_name, 1, LSM_NAME,
            annotation, ignore_annotations);
    codes_mapping_get_lp_id(group_name, LSM_NAME, annotation,
            ignore_annotations, mapping_rep, mapping_offset % num_lsm_lps,
            &rtn);
    return rtn;
}

tw_lpid lsm_find_local_device(
        const char * annotation,
        int ignore_annotations,
        tw_lpid sender_gid) {
    return lsm_find_local_device_default(annotation, ignore_annotations,
            sender_gid);
}

static tw_event* lsm_event_new_base(
        const char* category,
        tw_lpid  dest_gid,
        uint64_t io_object,
        int64_t  io_offset,
        uint64_t io_size_bytes,
        int      io_type,
        size_t   message_bytes,
        tw_lp   *sender,
        tw_stime delay,
        const char * annotation,
        int ignore_annotations)
{
    tw_event *e;
    lsm_message_t *m;
    tw_lpid lsm_gid; 
    tw_stime delta;

    assert(strlen(category) < CATEGORY_NAME_MAX-1);
    assert(strlen(category) > 0);

    /* Generate an event for the local storage model, and send the
     * event to an lsm LP. 
     */
    lsm_gid = lsm_find_local_device(annotation, ignore_annotations, sender->gid);

    delta = codes_local_latency(sender) + delay;
    if (lsm_in_sequence) {
        tw_stime tmp = lsm_msg_offset;
        lsm_msg_offset += delta;
        delta += tmp;
    }
    e = codes_event_new(lsm_gid, delta, sender);
    m = (lsm_message_t*)tw_event_data(e);
    m->magic = lsm_magic;
    m->event  = (lsm_event_t)io_type;
    m->u.data.object = io_object;
    m->u.data.offset = io_offset;
    m->u.data.size   = io_size_bytes;
    strcpy(m->u.data.category, category);

    /* save callers dest_gid and message size */
    m->wrap.id = dest_gid;
    m->wrap.size = message_bytes;

    return e;
}

/*
 * lsm_event_new
 *   - creates a new event that is targeted for the corresponding
 *     LSM LP.
 *   - this event will allow wrapping the callers completion event
 *   - category: string name to identify the traffic category
 *   - dest_gid: the gid to send the callers event to
 *   - gid_offset: relative offset of the LSM LP to the originating LP
 *   - io_object: id of byte stream the caller will modify
 *   - io_offset: offset into byte stream
 *   - io_size_bytes: size in bytes of IO request
 *   - io_type: read or write request
 *   - message_bytes: size of the event message the caller will have
 *   - sender: id of the sender
 */
tw_event* lsm_event_new(
        const char* category,
        tw_lpid  dest_gid,
        uint64_t io_object,
        int64_t  io_offset,
        uint64_t io_size_bytes,
        int      io_type,
        size_t   message_bytes,
        tw_lp   *sender,
        tw_stime delay) {
    return lsm_event_new_base(category, dest_gid, io_object, io_offset,
            io_size_bytes, io_type, message_bytes, sender, delay, NULL, 1);
}
tw_event* lsm_event_new_annotated(
        const char* category,
        tw_lpid  dest_gid,
        uint64_t io_object,
        int64_t  io_offset,
        uint64_t io_size_bytes,
        int      io_type,
        size_t   message_bytes,
        tw_lp   *sender,
        tw_stime delay,
        const char * annotation,
        int ignore_annotations) {
    return lsm_event_new_base(category, dest_gid, io_object, io_offset,
            io_size_bytes, io_type, message_bytes, sender, delay, annotation,
            ignore_annotations);
}

/*
 * lsm_event_data
 *   - returns the pointer to the message data for the callers data
 *   - event: a lsm_event_t event
 */
void* lsm_event_data(tw_event *event)
{
    lsm_message_t *m;
    
    /* return a pointer to space for caller to store event message
     * space was allocated in lsm_event_new
     */
    m = (lsm_message_t *) tw_event_data(event);

    return m->wrap.message;
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
                    (unsigned long long)m->u.data.object,
                    (unsigned long long)m->u.data.offset,
                    (unsigned long long)m->u.data.size);
            assert(ns->model);
            handle_io_request(ns, b, m, lp);
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
                    (unsigned long long)m->u.data.object,
                    (unsigned long long)m->u.data.offset,
                    (unsigned long long)m->u.data.size);
            handle_rev_io_request(ns, b, m, lp);
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

/*
 * handle_io_request
 *   - handles the IO request events
 *   - computes the next_idle time
 *   - fires disk completion event at computed time
 */
static void handle_io_request(lsm_state_t *ns,
                              tw_bf *b,
                              lsm_message_t *m_in,
                              tw_lp *lp)
{
    tw_stime queue_time, t_time;
    tw_event *e;
    lsm_message_t *m_out;
    lsm_stats_t *stat;
    int rw = (m_in->event == LSM_READ_REQUEST) ? 1 : 0;

    tw_stime (*transfer_time) (lsm_state_t *, lsm_stats_t *, int, uint64_t, int64_t, uint64_t);

    transfer_time = transfer_time_table;

    stat = find_stats(m_in->u.data.category, ns);

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
                           m_in->u.data.object,
                           m_in->u.data.offset,
                           m_in->u.data.size);
    queue_time += t_time;
    ns->next_idle = queue_time + tw_now(lp); 
    ns->current_offset = m_in->u.data.offset + m_in->u.data.size;
    ns->current_object = m_in->u.data.object;

    e = codes_event_new(lp->gid, queue_time, lp);
    m_out = (lsm_message_t*)tw_event_data(e);

    memcpy(m_out, m_in, sizeof(*m_in)+m_in->wrap.size);
    if (m_out->event == LSM_WRITE_REQUEST)
    {
        m_out->event = LSM_WRITE_COMPLETION;
    }
    else
    {
        m_out->event = LSM_READ_COMPLETION;
    }

    tw_event_send(e);

    return;
}


/*
 * handle_rev_io_request
 *   - handle reversing the io request
 */
static void handle_rev_io_request(lsm_state_t *ns,
                                  tw_bf *b,
                                  lsm_message_t *m_in,
                                  tw_lp *lp)
{
    lsm_stats_t *stat;
    
    stat = find_stats(m_in->u.data.category, ns);

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
    tw_event *e;
    void     *m;

    e = codes_event_new(m_in->wrap.id, codes_local_latency(lp), lp);
    m = tw_event_data(e);

    memcpy(m, m_in->wrap.message, m_in->wrap.size);

    tw_event_send(e);

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
static void read_config(ConfigHandle *ch, char * anno, disk_model_t *model)
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
    for (int i = 0; i < length; i++)
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
    for (int i = 0; i < length; i++)
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
    for (int i = 0; i < length; i++)
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
    for (int i = 0; i < length; i++)
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
    for (int i = 0; i < length; i++)
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
    for (int i = 0; i < length; i++)
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
    for (int i = 0; i < length; i++)
    {
        model->read_seeks[i] = strtod(values[i], NULL);
    }
    free(values);
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

    for (uint64_t i = 0; i < anno_map->num_annos; i++){
        char * anno = anno_map->annotations[i];
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
