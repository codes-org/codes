/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#include <string.h>
#include <assert.h>
#include <ross.h>

#include "codes/lp-io.h"
#include "codes/jenkins-hash.h"
#include "codes/model-net-method.h"
#include "codes/model-net.h"
#include "codes/model-net-lp.h"
#include "codes/model-net-sched.h"
#include "codes/codes_mapping.h"
#include "codes/codes.h"
#include "codes/net/loggp.h"

#define CATEGORY_NAME_MAX 16
#define CATEGORY_MAX 12

#define LP_CONFIG_NM (model_net_lp_config_names[LOGGP])
#define LP_METHOD_NM (model_net_method_names[LOGGP])

// conditionally use the new recv-queue stuff. this is only here in the case
// that we want to collect results based on the old model
#define USE_RECV_QUEUE 1

#define LOGGP_MSG_TRACE 0

#if LOGGP_MSG_TRACE
#define dprintf(_fmt, ...) printf(_fmt, __VA_ARGS__);
#else
#define dprintf(_fmt, ...)
#endif


/*Define loggp data types and structs*/
typedef struct loggp_state loggp_state;

/* loggp parameters for a given msg size, as reported by netgauge */
struct param_table_entry
{
    uint64_t size;
    int n;
    double PRTT_10s;
    double PRTT_n0s;
    double PRTT_nPRTT_10ss;
    double L;
    double o_s;
    double o_r;
    double g;
    double G;
    double lsqu_gG;
};
typedef struct param_table_entry param_table_entry;

typedef struct loggp_param loggp_param;
// loggp parameters
struct loggp_param
{
    int table_size;
    param_table_entry table[100];
};


struct loggp_state
{
    /* next idle times for network card, both inbound and outbound */
    tw_stime net_send_next_idle;
    tw_stime net_recv_next_idle;
    const char * anno;
    const loggp_param *params;
    struct mn_stats loggp_stats_array[CATEGORY_MAX];
};

/* annotation-specific parameters (unannotated entry occurs at the 
 * last index) */
static uint64_t                  num_params = 0;
static loggp_param             * all_params = NULL;
static const config_anno_map_t * anno_map   = NULL;

static int loggp_magic = 0;

/* returns a pointer to the lptype struct to use for loggp LPs */
static const tw_lptype* loggp_get_lp_type(void);

/* retrieve the size of the portion of the event struct that is consumed by
 * the loggp module.  The caller should add this value to the size of
 * its own event structure to get the maximum total size of a message.
 */
static int loggp_get_msg_sz(void);

/* Returns the loggp magic number */
static int loggp_get_magic();

/* collective network calls */
static void loggp_collective();

/* collective network calls-- rc */
static void loggp_collective_rc();

/* Modelnet interface events */
/* sets up the loggp parameters through modelnet interface */
static void loggp_configure();

static void loggp_set_params(const char * config_file, loggp_param * params);

/* Issues a loggp packet event call */
static tw_stime loggp_packet_event(
     char* category, 
     tw_lpid final_dest_lp, 
     uint64_t packet_size, 
     int is_pull,
     uint64_t pull_size, /* only used when is_pull==1 */
     tw_stime offset,
     const mn_sched_params *sched_params,
     int remote_event_size, 
     const void* remote_event, 
     int self_event_size,
     const void* self_event,
     tw_lpid src_lp,
     tw_lp *sender,
     int is_last_pckt);
static void loggp_packet_event_rc(tw_lp *sender);

tw_stime loggp_recv_msg_event(
        const char * category,
        tw_lpid final_dest_lp,
        uint64_t msg_size,
        int is_pull,
        uint64_t pull_size,
        tw_stime offset,
        int remote_event_size,
        const void* remote_event,
        tw_lpid src_lp,
        tw_lp *sender);

void loggp_recv_msg_event_rc(tw_lp *sender);

static void loggp_report_stats();

static tw_lpid loggp_find_local_device(
        const char * annotation,
        int          ignore_annotations,
        tw_lp      * sender);

static const struct param_table_entry* find_params(
        uint64_t msg_size,
        const loggp_param *params);

/* data structure for model-net statistics */
struct model_net_method loggp_method =
{
    .mn_configure = loggp_configure,
    .mn_register = NULL,
    .model_net_method_packet_event = loggp_packet_event,
    .model_net_method_packet_event_rc = loggp_packet_event_rc,
    .model_net_method_recv_msg_event = loggp_recv_msg_event,
    .model_net_method_recv_msg_event_rc = loggp_recv_msg_event_rc,
    .mn_get_lp_type = loggp_get_lp_type,
    .mn_get_msg_sz = loggp_get_msg_sz,
    .mn_report_stats = loggp_report_stats,
    .model_net_method_find_local_device = NULL,
    .mn_collective_call = loggp_collective,
    .mn_collective_call_rc = loggp_collective_rc
};

static void loggp_init(
    loggp_state * ns,
    tw_lp * lp);
static void loggp_event(
    loggp_state * ns,
    tw_bf * b,
    loggp_message * m,
    tw_lp * lp);
static void loggp_rev_event(
    loggp_state * ns,
    tw_bf * b,
    loggp_message * m,
    tw_lp * lp);
static void loggp_finalize(
    loggp_state * ns,
    tw_lp * lp);

tw_lptype loggp_lp = {
    (init_f) loggp_init,
    (pre_run_f) NULL,
    (event_f) loggp_event,
    (revent_f) loggp_rev_event,
    (final_f) loggp_finalize,
    (map_f) codes_mapping,
    sizeof(loggp_state),
};

static void handle_msg_ready_rev_event(
    loggp_state * ns,
    tw_bf * b,
    loggp_message * m,
    tw_lp * lp);
static void handle_msg_ready_event(
    loggp_state * ns,
    tw_bf * b,
    loggp_message * m,
    tw_lp * lp);
static void handle_msg_start_rev_event(
    loggp_state * ns,
    tw_bf * b,
    loggp_message * m,
    tw_lp * lp);
static void handle_msg_start_event(
    loggp_state * ns,
    tw_bf * b,
    loggp_message * m,
    tw_lp * lp);

/* returns pointer to LP information for loggp module */
static const tw_lptype* loggp_get_lp_type()
{
    return(&loggp_lp);
}

/* returns number of bytes that the loggp module will consume in event
 * messages
 */
static int loggp_get_msg_sz(void)
{
    return(sizeof(loggp_message));
}

/* report network statistics */
static void loggp_report_stats()
{
   /* TODO: Do we have some loggp statistics to report like we have for torus and dragonfly? */
   return;
}

/* collective network calls */
static void loggp_collective()
{
/* collectives not supported */
    return;
}

static void loggp_collective_rc()
{
/* collectives not supported */
   return;
}

static void loggp_init(
    loggp_state * ns,
    tw_lp * lp)
{
    uint32_t h1 = 0, h2 = 0;
    memset(ns, 0, sizeof(*ns));

    /* all devices are idle to begin with */
    ns->net_send_next_idle = tw_now(lp);
    ns->net_recv_next_idle = tw_now(lp);

    ns->anno = codes_mapping_get_annotation_by_lpid(lp->gid);
    if (ns->anno == NULL)
        ns->params = &all_params[num_params-1];
    else{
        int id = configuration_get_annotation_index(ns->anno, anno_map);
        ns->params = &all_params[id];
    }

    bj_hashlittle2(LP_METHOD_NM, strlen(LP_METHOD_NM), &h1, &h2);
    loggp_magic = h1+h2;
    /* printf("\n loggp_magic %d ", loggp_magic); */

    return;
}

static void loggp_event(
    loggp_state * ns,
    tw_bf * b,
    loggp_message * m,
    tw_lp * lp)
{
    assert(m->magic == loggp_magic);

    switch (m->event_type)
    {
        case LG_MSG_START:
            handle_msg_start_event(ns, b, m, lp);
            break;
        case LG_MSG_READY:
            handle_msg_ready_event(ns, b, m, lp);
            break;
        default:
            assert(0);
            break;
    }
}

static void loggp_rev_event(
    loggp_state * ns,
    tw_bf * b,
    loggp_message * m,
    tw_lp * lp)
{
    assert(m->magic == loggp_magic);

    switch (m->event_type)
    {
        case LG_MSG_START:
            handle_msg_start_rev_event(ns, b, m, lp);
            break;
        case LG_MSG_READY:
            handle_msg_ready_rev_event(ns, b, m, lp);
            break;
        default:
            assert(0);
            break;
    }

    return;
}

static void loggp_finalize(
    loggp_state * ns,
    tw_lp * lp)
{
    model_net_print_stats(lp->gid, &ns->loggp_stats_array[0]);
    return;
}

int loggp_get_magic()
{
  return loggp_magic;
}

/* reverse computation for msg ready event */
static void handle_msg_ready_rev_event(
    loggp_state * ns,
    tw_bf * b,
    loggp_message * m,
    tw_lp * lp)
{

    dprintf("%lu (mn): ready msg rc %lu->%lu, size %lu (%3s last)\n"
            "          now:%0.3le, idle[now:%0.3le, prev:%0.3le]\n",
            lp->gid, m->src_gid, m->final_dest_gid, m->net_msg_size_bytes,
            m->event_size_bytes+m->local_event_size_bytes > 0 ? "is" : "not",
            tw_now(lp), ns->net_recv_next_idle, m->net_recv_next_idle_saved);

    struct mn_stats* stat;

    ns->net_recv_next_idle = m->net_recv_next_idle_saved;
    
    stat = model_net_find_stats(m->category, ns->loggp_stats_array);
    stat->recv_count--;
    stat->recv_bytes -= m->net_msg_size_bytes;
    stat->recv_time -= m->recv_time_saved;

#if USE_RECV_QUEUE
    codes_local_latency_reverse(lp);
#endif

    if (m->event_size_bytes && m->is_pull){
        int net_id = model_net_get_id(LP_METHOD_NM);
        model_net_event_rc(net_id, lp, m->pull_size);
    }

    return;
}

/* handler for msg ready event.  This indicates that a message is available
 * to recv, but we haven't checked to see if the recv queue is available yet
 */
static void handle_msg_ready_event(
    loggp_state * ns,
    tw_bf * b,
    loggp_message * m,
    tw_lp * lp)
{
    tw_stime recv_queue_time = 0;
    tw_event *e_new;
    loggp_message *m_new;
    struct mn_stats* stat;
    double recv_time;
    const struct param_table_entry *param;

    param = find_params(m->net_msg_size_bytes, ns->params);

    recv_time = ((double)(m->net_msg_size_bytes-1)*param->G);
    /* scale to nanoseconds */
    recv_time *= 1000.0;
    m->recv_time_saved = recv_time;

    //printf("handle_msg_ready_event(), lp %llu.\n", (unsigned long long)lp->gid);
    /* add statistics */
    stat = model_net_find_stats(m->category, ns->loggp_stats_array);
    stat->recv_count++;
    stat->recv_bytes += m->net_msg_size_bytes;
    stat->recv_time += recv_time;

    /* are we available to recv the msg? */
    /* were we available when the transmission was started? */
    if(ns->net_recv_next_idle > tw_now(lp))
        recv_queue_time += ns->net_recv_next_idle - tw_now(lp);

    /* calculate transfer time based on msg size and bandwidth */
    recv_queue_time += recv_time;

    /* bump up input queue idle time accordingly, include gap (g) parameter */
    m->net_recv_next_idle_saved = ns->net_recv_next_idle;
    ns->net_recv_next_idle = recv_queue_time + tw_now(lp) + param->g*1000.0;

    dprintf("%lu (mn): ready msg    %lu->%lu, size %lu (%3s last)\n"
            "          now:%0.3le, idle[prev:%0.3le, next:%0.3le], "
            "q-time:%0.3le\n",
            lp->gid, m->src_gid, m->final_dest_gid, m->net_msg_size_bytes,
            m->event_size_bytes+m->local_event_size_bytes > 0 ? "is" : "not",
            tw_now(lp), m->net_recv_next_idle_saved, ns->net_recv_next_idle,
            recv_queue_time);

    // if we're using a recv-side queue, then we need to tell the scheduler we
    // are idle
#if USE_RECV_QUEUE
    model_net_method_idle_event(codes_local_latency(lp) +
            ns->net_recv_next_idle - tw_now(lp), 1, lp);
#endif

    /* copy only the part of the message used by higher level */
    if(m->event_size_bytes)
    {
      /* schedule event to final destination for when the recv is complete */
//      printf("\n Remote message to LP %d ", m->final_dest_gid); 

        void *tmp_ptr = model_net_method_get_edata(LOGGP, m);
        //char* tmp_ptr = (char*)m;
        //tmp_ptr += loggp_get_msg_sz();
        if (m->is_pull){
            /* call the model-net event */
            int net_id = model_net_get_id(LP_METHOD_NM);
            model_net_event(net_id, m->category, m->src_gid, m->pull_size,
                    recv_queue_time, m->event_size_bytes, tmp_ptr, 0, NULL,
                    lp);
        }
        else{
            e_new = tw_event_new(m->final_dest_gid, recv_queue_time, lp);
            m_new = tw_event_data(e_new);
            memcpy(m_new, tmp_ptr, m->event_size_bytes);
            tw_event_send(e_new);
        }
    }

    return;
}

/* reverse computation for msg start event */
static void handle_msg_start_rev_event(
    loggp_state * ns,
    tw_bf * b,
    loggp_message * m,
    tw_lp * lp)
{

    dprintf("%lu (mn): start msg rc %lu->%lu, size %lu (%3s last)\n"
            "          now:%0.3le, idle[now:%0.3le, prev:%0.3le]\n",
            lp->gid, m->src_gid, m->final_dest_gid, m->net_msg_size_bytes,
            m->event_size_bytes+m->local_event_size_bytes > 0 ? "is" : "not",
            tw_now(lp), ns->net_send_next_idle, m->net_send_next_idle_saved);

    ns->net_send_next_idle = m->net_send_next_idle_saved;

#if USE_RECV_QUEUE
    model_net_method_send_msg_recv_event_rc(lp);
#endif

    codes_local_latency_reverse(lp);

    if(m->local_event_size_bytes > 0)
    {
        codes_local_latency_reverse(lp);
    }

    mn_stats* stat;
    stat = model_net_find_stats(m->category, ns->loggp_stats_array);
    stat->send_count--;
    stat->send_bytes -= m->net_msg_size_bytes;
    stat->send_time -= m->xmit_time_saved;

    return;
}

/* handler for msg start event; this indicates that the caller is trying to
 * transmit a message through this NIC
 */
static void handle_msg_start_event(
    loggp_state * ns,
    tw_bf * b,
    loggp_message * m,
    tw_lp * lp)
{
    tw_event *e_new;
    loggp_message *m_new;
    tw_stime send_queue_time = 0;
    mn_stats* stat;
    int mapping_grp_id, mapping_type_id, mapping_rep_id, mapping_offset;
    tw_lpid dest_id;
    char lp_group_name[MAX_NAME_LENGTH];
    int total_event_size;
    double xmit_time;
    const struct param_table_entry *param;

    param = find_params(m->net_msg_size_bytes, ns->params);

    total_event_size = model_net_get_msg_sz(LOGGP) + m->event_size_bytes +
        m->local_event_size_bytes;

    /* NOTE: we do not use the o_s or o_r parameters here; as indicated in
     * the netgauge paper those are typically overlapping with L (and the
     * msg xfer as well) and therefore are more important for overlapping
     * computation rather than simulating communication time.
     */
    xmit_time = ((double)(m->net_msg_size_bytes-1)*param->G);
    /* scale to nanoseconds */
    xmit_time *= 1000.0;
    m->xmit_time_saved = xmit_time;

    //printf("handle_msg_start_event(), lp %llu.\n", (unsigned long long)lp->gid);
    /* add statistics */
    stat = model_net_find_stats(m->category, ns->loggp_stats_array);
    stat->send_count++;
    stat->send_bytes += m->net_msg_size_bytes;
    stat->send_time += xmit_time;
    if(stat->max_event_size < total_event_size)
        stat->max_event_size = total_event_size;

    /* calculate send time stamp */
    send_queue_time = (param->L)*1000.0; 
    /* bump up time if the NIC send queue isn't idle right now */
    if(ns->net_send_next_idle > tw_now(lp))
        send_queue_time += ns->net_send_next_idle - tw_now(lp);

    /* move the next idle time ahead to after this transmission is
     * _complete_ from the sender's perspective, include gap paramater (g)
     * at this point. 
     */ 
    m->net_send_next_idle_saved = ns->net_send_next_idle;
    if(ns->net_send_next_idle < tw_now(lp))
        ns->net_send_next_idle = tw_now(lp);
    ns->net_send_next_idle += xmit_time + param->g*1000.0;

    /* create new event to send msg to receiving NIC */
    dest_id = model_net_find_local_device(LOGGP, ns->anno, 0,
            m->final_dest_gid);

    dprintf("%lu (mn): start msg    %lu->%lu, size %lu (%3s last)\n"
            "          now:%0.3le, idle[prev:%0.3le, next:%0.3le], "
            "q-time:%0.3le\n",
            lp->gid, m->src_gid, m->final_dest_gid, m->net_msg_size_bytes,
            m->event_size_bytes+m->local_event_size_bytes > 0 ? "is" : "not",
            tw_now(lp), m->net_send_next_idle_saved, ns->net_send_next_idle,
            send_queue_time);

#if USE_RECV_QUEUE
    model_net_method_send_msg_recv_event(m->final_dest_gid, dest_id, m->src_gid,
            m->net_msg_size_bytes, m->is_pull, m->pull_size,
            m->event_size_bytes, &m->sched_params, m->category, LOGGP, m,
            send_queue_time, lp);
#else 
    void *m_data;
//    printf("\n msg start sending to %d ", dest_id);
    //e_new = tw_event_new(dest_id, send_queue_time, lp);
    //m_new = tw_event_data(e_new);
    e_new = model_net_method_event_new(dest_id, send_queue_time, lp, LOGGP,
            (void**)&m_new, &m_data);
    /* copy entire previous message over, including payload from user of
     * this module
     */
    memcpy(m_new, m, sizeof(loggp_message));
    if (m->event_size_bytes){
        memcpy(m_data, model_net_method_get_edata(LOGGP, m),
                m->event_size_bytes);
    }

    m_new->event_type = LG_MSG_READY;
    
    tw_event_send(e_new);
#endif

    // now that message is sent, issue an "idle" event to tell the scheduler
    // when I'm next available
    model_net_method_idle_event(codes_local_latency(lp) +
            ns->net_send_next_idle - tw_now(lp), 0, lp);

    /* if there is a local event to handle, then create an event for it as
     * well
     */
    if(m->local_event_size_bytes > 0)
    {
        //char* local_event;

        e_new = tw_event_new(m->src_gid, send_queue_time+codes_local_latency(lp), lp);
        m_new = tw_event_data(e_new);

        void * m_loc = (char*) model_net_method_get_edata(LOGGP, m) +
            m->event_size_bytes;
         //local_event = (char*)m;
         //local_event += loggp_get_msg_sz() + m->event_size_bytes;         	 
        /* copy just the local event data over */
        memcpy(m_new, m_loc, m->local_event_size_bytes);
        tw_event_send(e_new);
    }
    return;
}

/* Model-net function calls */

/*This method will serve as an intermediate layer between loggp and modelnet. 
 * It takes the packets from modelnet layer and calls underlying loggp methods*/
static tw_stime loggp_packet_event(
		char* category,
		tw_lpid final_dest_lp,
		uint64_t packet_size,
                int is_pull,
                uint64_t pull_size, /* only used when is_pull==1 */
                tw_stime offset,
                const mn_sched_params *sched_params,
		int remote_event_size,
		const void* remote_event,
		int self_event_size,
		const void* self_event,
                tw_lpid src_lp,
		tw_lp *sender,
		int is_last_pckt)
{
     tw_event * e_new;
     tw_stime xfer_to_nic_time;
     loggp_message * msg;
     char* tmp_ptr;

     xfer_to_nic_time = codes_local_latency(sender);
     e_new = model_net_method_event_new(sender->gid, xfer_to_nic_time+offset,
             sender, LOGGP, (void**)&msg, (void**)&tmp_ptr);
     //e_new = tw_event_new(dest_id, xfer_to_nic_time+offset, sender);
     //msg = tw_event_data(e_new);
     strcpy(msg->category, category);
     msg->final_dest_gid = final_dest_lp;
     msg->src_gid = src_lp;
     msg->magic = loggp_get_magic();
     msg->net_msg_size_bytes = packet_size;
     msg->event_size_bytes = 0;
     msg->local_event_size_bytes = 0;
     msg->event_type = LG_MSG_START;
     msg->is_pull = is_pull;
     msg->pull_size = pull_size;
     msg->sched_params = *sched_params;

     //tmp_ptr = (char*)msg;
     //tmp_ptr += loggp_get_msg_sz();
      
    //printf("\n Sending to LP %d msg magic %d ", (int)dest_id, loggp_get_magic()); 
     /*Fill in loggp information*/     
     if(is_last_pckt) /* Its the last packet so pass in remote event information*/
      {
       if(remote_event_size)
	 {
           msg->event_size_bytes = remote_event_size;
           memcpy(tmp_ptr, remote_event, remote_event_size);
           tmp_ptr += remote_event_size;
	 }
       if(self_event_size)
       {
	   msg->local_event_size_bytes = self_event_size;
	   memcpy(tmp_ptr, self_event, self_event_size);
	   tmp_ptr += self_event_size;
       }
      // printf("\n Last packet size: %d ", loggp_get_msg_sz() + remote_event_size + self_event_size);
      }
     tw_event_send(e_new);
     return xfer_to_nic_time;
}

tw_stime loggp_recv_msg_event(
        const char * category,
        tw_lpid final_dest_lp,
        uint64_t msg_size,
        int is_pull,
        uint64_t pull_size,
        tw_stime offset,
        int remote_event_size,
        const void* remote_event,
        tw_lpid src_lp,
        tw_lp *sender){
    loggp_message *m;
    void *m_data;

    tw_stime moffset = offset + codes_local_latency(sender);

    // this message goes to myself
    tw_event *e = model_net_method_event_new(sender->gid, moffset, sender,
            LOGGP, (void**)&m, &m_data);

    m->magic = loggp_magic;
    m->event_type = LG_MSG_READY;
    m->src_gid = src_lp;
    m->final_dest_gid = final_dest_lp;
    m->net_msg_size_bytes = msg_size;
    m->event_size_bytes = remote_event_size;
    m->local_event_size_bytes = 0;
    strncpy(m->category, category, CATEGORY_NAME_MAX-1);
    m->category[CATEGORY_NAME_MAX-1]='\0';
    m->is_pull = is_pull;
    m->pull_size = pull_size;
    // default sched params for just calling the receiver (for now...)
    model_net_sched_set_default_params(&m->sched_params);

    // copy the remote event over if necessary
    if (remote_event_size > 0){
        memcpy(m_data, remote_event, remote_event_size);
    }

    tw_event_send(e);

    return moffset;
}

void loggp_recv_msg_event_rc(tw_lp *sender){
    codes_local_latency_reverse(sender);
}

static void loggp_configure(){
    char config_file[MAX_NAME_LENGTH];

    anno_map = codes_mapping_get_lp_anno_map(LP_CONFIG_NM);
    assert(anno_map);
    num_params = anno_map->num_annos + (anno_map->has_unanno_lp > 0);
    all_params = malloc(num_params * sizeof(*all_params));

    for (uint64_t i = 0; i < anno_map->num_annos; i++){
        const char * anno = anno_map->annotations[i];
        int rc = configuration_get_value_relpath(&config, "PARAMS",
                "net_config_file", anno, config_file, MAX_NAME_LENGTH);
        if (rc <= 0){
            tw_error(TW_LOC, "unable to read PARAMS:net_config_file@%s",
                    anno);
        }
        loggp_set_params(config_file, &all_params[i]);
    }
    if (anno_map->has_unanno_lp > 0){
        int rc = configuration_get_value_relpath(&config, "PARAMS",
                "net_config_file", NULL, config_file, MAX_NAME_LENGTH);
        if (rc <= 0){
            tw_error(TW_LOC, "unable to read PARAMS:net_config_file");
        }
        loggp_set_params(config_file, &all_params[anno_map->num_annos]);
    }
}

void loggp_set_params(const char * config_file, loggp_param * params){
    FILE *conf;
    int ret;
    char buffer[512];
    int line_nr = 0;
    printf("Loggp configured to use parameters from file %s\n", config_file);

    conf = fopen(config_file, "r");
    if(!conf)
    {
        perror("fopen");
        assert(0);
    }

    params->table_size = 0;
    while(fgets(buffer, 512, conf))
    {
        line_nr++;
        if(buffer[0] == '#')
            continue;
        ret = sscanf(buffer, "%llu %d %lf %lf %lf %lf %lf %lf %lf %lf %lf", 
            &params->table[params->table_size].size,
            &params->table[params->table_size].n,
            &params->table[params->table_size].PRTT_10s,
            &params->table[params->table_size].PRTT_n0s,
            &params->table[params->table_size].PRTT_nPRTT_10ss,
            &params->table[params->table_size].L,
            &params->table[params->table_size].o_s,
            &params->table[params->table_size].o_r,
            &params->table[params->table_size].g,
            &params->table[params->table_size].G,
            &params->table[params->table_size].lsqu_gG);
        if(ret != 11)
        {
            fprintf(stderr, "Error: malformed line %d in %s\n", line_nr, 
                config_file);
            assert(0);
        }
        params->table_size++;
    }

    printf("Parsed %d loggp table entries.\n", params->table_size);

    fclose(conf);
 
    return;
}

static void loggp_packet_event_rc(tw_lp *sender)
{
    codes_local_latency_reverse(sender);
    return;
}

/* find the parameters corresponding to the message size we are transmitting
 */
static const struct param_table_entry* find_params(
        uint64_t msg_size,
        const loggp_param *params) {
    int i;

    /* pick parameters based on the next smallest size in the table, but
     * default to beginning or end of table if we are out of range
     */

    for(i=0; i < params->table_size; i++)
    {
        if(params->table[i].size > msg_size)
        {
            break;
        }
    }
    if(i>=params->table_size)
        i = params->table_size-1;

    return(&params->table[i]);
}

static tw_lpid loggp_find_local_device(
        const char * annotation,
        int          ignore_annotations,
        tw_lp      * sender)
{
     char lp_group_name[MAX_NAME_LENGTH];
     int mapping_grp_id, mapping_rep_id, mapping_type_id, mapping_offset;
     tw_lpid dest_id;

     //TODO: be annotation-aware
     codes_mapping_get_lp_info(sender->gid, lp_group_name, &mapping_grp_id,
             NULL, &mapping_type_id, NULL, &mapping_rep_id, &mapping_offset);
     codes_mapping_get_lp_id(lp_group_name, LP_CONFIG_NM, annotation,
             ignore_annotations, mapping_rep_id, mapping_offset, &dest_id);

    return(dest_id);
}


/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
