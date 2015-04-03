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
#include "codes/codes_mapping.h"
#include "codes/codes.h"
#include "codes/net/simplenet-upd.h"

#define CATEGORY_NAME_MAX 16
#define CATEGORY_MAX 12

#define LP_CONFIG_NM (model_net_lp_config_names[SIMPLENET])
#define LP_METHOD_NM (model_net_method_names[SIMPLENET])

/* structs for initializing a network/ specifying network parameters */
struct simplenet_param
{
  double net_startup_ns; /*simplenet startup cost*/
  double net_bw_mbps; /*Link bandwidth per byte*/
};
typedef struct simplenet_param simplenet_param;

/*Define simplenet data types and structs*/
typedef struct sn_state sn_state;

struct sn_state
{
    /* next idle times for network card, both inbound and outbound */
    tw_stime net_send_next_idle;
    tw_stime net_recv_next_idle;
    const char * anno;
    simplenet_param params;
    struct mn_stats sn_stats_array[CATEGORY_MAX];
};

/* annotation-specific parameters (unannotated entry occurs at the 
 * last index) */
static uint64_t                  num_params = 0;
static simplenet_param         * all_params = NULL;
static const config_anno_map_t * anno_map   = NULL;

static int sn_magic = 0;

/* returns a pointer to the lptype struct to use for simplenet LPs */
static const tw_lptype* sn_get_lp_type(void);

/* retrieve the size of the portion of the event struct that is consumed by
 * the simplenet module.  The caller should add this value to the size of
 * its own event structure to get the maximum total size of a message.
 */
static int sn_get_msg_sz(void);

/* Returns the simplenet magic number */
static int sn_get_magic();

#if SIMPLENET_DEBUG
static void print_msg(sn_message *m);
#endif

/* collective network calls */
static void simple_net_collective();

/* collective network calls-- rc */
static void simple_net_collective_rc();

/* Modelnet interface events */
/* sets up the simplenet parameters through modelnet interface */
static void sn_configure();

/* allocate a new event that will pass through simplenet to arriave at its
 * destination:
 *
 * - category: category name to associate with this communication
 * - final_dest_gid: the LP that the message should be delivered to.
 * - event_size_bytes: size of event msg that will be delivered to
 * final_dest_gid.
 * - local_event_size_byte: size of event message that will delivered to
 *   local LP upon local send comletion (set to 0 if not used)
 * - net_msg_size_bytes: size of simulated network message in bytes.
 * - sender: LP calling this function.
 */
/* Issues a simplenet packet event call */
static tw_stime simplenet_packet_event(
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
static void simplenet_packet_event_rc(tw_lp *sender);

static void simplenet_packet_event_rc(tw_lp *sender);

static void sn_report_stats();

static tw_lpid sn_find_local_device(
        const char * annotation, 
        int          ignore_annotations,
        tw_lp      * sender);

/* data structure for model-net statistics */
struct model_net_method simplenet_method =
{
    .mn_configure = sn_configure,
    .mn_register = NULL,
    .model_net_method_packet_event = simplenet_packet_event,
    .model_net_method_packet_event_rc = simplenet_packet_event_rc,
    .model_net_method_recv_msg_event = NULL,
    .model_net_method_recv_msg_event_rc = NULL,
    .mn_get_lp_type = sn_get_lp_type,
    .mn_get_msg_sz = sn_get_msg_sz,
    .mn_report_stats = sn_report_stats,
    .model_net_method_find_local_device = NULL, /* use the default */
    .mn_collective_call = simple_net_collective,	
    .mn_collective_call_rc = simple_net_collective_rc
};

static void sn_init(
    sn_state * ns,
    tw_lp * lp);
static void sn_event(
    sn_state * ns,
    tw_bf * b,
    sn_message * m,
    tw_lp * lp);
static void sn_rev_event(
    sn_state * ns,
    tw_bf * b,
    sn_message * m,
    tw_lp * lp);
static void sn_finalize(
    sn_state * ns,
    tw_lp * lp);

tw_lptype sn_lp = {
    (init_f) sn_init,
    (pre_run_f) NULL,
    (event_f) sn_event,
    (revent_f) sn_rev_event,
    (final_f) sn_finalize,
    (map_f) codes_mapping,
    sizeof(sn_state),
};

static tw_stime rate_to_ns(uint64_t bytes, double MB_p_s);
static void handle_msg_ready_rev_event(
    sn_state * ns,
    tw_bf * b,
    sn_message * m,
    tw_lp * lp);
static void handle_msg_ready_event(
    sn_state * ns,
    tw_bf * b,
    sn_message * m,
    tw_lp * lp);
static void handle_msg_start_rev_event(
    sn_state * ns,
    tw_bf * b,
    sn_message * m,
    tw_lp * lp);
static void handle_msg_start_event(
    sn_state * ns,
    tw_bf * b,
    sn_message * m,
    tw_lp * lp);

/* returns pointer to LP information for simplenet module */
static const tw_lptype* sn_get_lp_type()
{
    return(&sn_lp);
}

/* returns number of bytes that the simplenet module will consume in event
 * messages
 */
static int sn_get_msg_sz(void)
{
    return(sizeof(sn_message));
}
/* collective network calls */
static void simple_net_collective()
{
/* collectives not supported */
    return;
}

/* collective network call -- rc*/
static void simple_net_collective_rc()
{
/* collectives not supported */
   return;
}

/* report network statistics */
static void sn_report_stats()
{
   /* TODO: Do we have some simplenet statistics to report like we have for torus and dragonfly? */
   return;
}
static void sn_init(
    sn_state * ns,
    tw_lp * lp)
{
    uint32_t h1 = 0, h2 = 0;
    memset(ns, 0, sizeof(*ns));

    /* all devices are idle to begin with */
    ns->net_send_next_idle = tw_now(lp);
    ns->net_recv_next_idle = tw_now(lp);

    ns->anno = codes_mapping_get_annotation_by_lpid(lp->gid);
    if (ns->anno == NULL)
        ns->params = all_params[num_params-1];
    else{
        int id = configuration_get_annotation_index(ns->anno, anno_map);
        ns->params = all_params[id];
    }

    bj_hashlittle2(LP_METHOD_NM, strlen(LP_METHOD_NM), &h1, &h2);
    sn_magic = h1+h2;
    /* printf("\n sn_magic %d ", sn_magic); */

    return;
}

static void sn_event(
    sn_state * ns,
    tw_bf * b,
    sn_message * m,
    tw_lp * lp)
{
    assert(m->magic == sn_magic);

    switch (m->event_type)
    {
        case SN_MSG_START:
            handle_msg_start_event(ns, b, m, lp);
            break;
        case SN_MSG_READY:
            handle_msg_ready_event(ns, b, m, lp);
            break;
        default:
            assert(0);
            break;
    }
}

static void sn_rev_event(
    sn_state * ns,
    tw_bf * b,
    sn_message * m,
    tw_lp * lp)
{
    assert(m->magic == sn_magic);

    switch (m->event_type)
    {
        case SN_MSG_START:
            handle_msg_start_rev_event(ns, b, m, lp);
            break;
        case SN_MSG_READY:
            handle_msg_ready_rev_event(ns, b, m, lp);
            break;
        default:
            assert(0);
            break;
    }

    return;
}

static void sn_finalize(
    sn_state * ns,
    tw_lp * lp)
{
    model_net_print_stats(lp->gid, &ns->sn_stats_array[0]);
    return;
}

int sn_get_magic()
{
  return sn_magic;
}

/* convert MiB/s and bytes to ns */
static tw_stime rate_to_ns(uint64_t bytes, double MB_p_s)
{
    tw_stime time;

    /* bytes to MB */
    time = ((double)bytes)/(1024.0*1024.0);
    /* MB to s */
    time = time / MB_p_s;
    /* s to ns */
    time = time * 1000.0 * 1000.0 * 1000.0;

    return(time);
}

/* reverse computation for msg ready event */
static void handle_msg_ready_rev_event(
    sn_state * ns,
    tw_bf * b,
    sn_message * m,
    tw_lp * lp)
{
    struct mn_stats* stat;

    ns->net_recv_next_idle = m->net_recv_next_idle_saved;
    
    stat = model_net_find_stats(m->category, ns->sn_stats_array);
    stat->recv_count--;
    stat->recv_bytes -= m->net_msg_size_bytes;
    stat->recv_time = m->recv_time_saved;

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
    sn_state * ns,
    tw_bf * b,
    sn_message * m,
    tw_lp * lp)
{
    tw_stime recv_queue_time = 0;
    struct mn_stats* stat;

    //printf("handle_msg_ready_event(), lp %llu.\n", (unsigned long long)lp->gid);
    /* add statistics */
    stat = model_net_find_stats(m->category, ns->sn_stats_array);
    stat->recv_count++;
    stat->recv_bytes += m->net_msg_size_bytes;
    m->recv_time_saved = stat->recv_time;
    stat->recv_time += rate_to_ns(m->net_msg_size_bytes,
            ns->params.net_bw_mbps);

    /* are we available to recv the msg? */
    /* were we available when the transmission was started? */
    if(ns->net_recv_next_idle > tw_now(lp))
        recv_queue_time += ns->net_recv_next_idle - tw_now(lp);

    /* calculate transfer time based on msg size and bandwidth */
    recv_queue_time += rate_to_ns(m->net_msg_size_bytes,
            ns->params.net_bw_mbps);

    /* bump up input queue idle time accordingly */
    m->net_recv_next_idle_saved = ns->net_recv_next_idle;
    ns->net_recv_next_idle = recv_queue_time + tw_now(lp);

    /* copy only the part of the message used by higher level */
    if(m->event_size_bytes)
    {
        //char *tmp_ptr = (char*)m;
        //tmp_ptr += sn_get_msg_sz();
        void *tmp_ptr = model_net_method_get_edata(SIMPLENET, m);
      /* schedule event to final destination for when the recv is complete */
//      printf("\n Remote message to LP %d ", m->final_dest_gid); 
        if (m->is_pull){
            /* call the model-net event */
            int net_id = model_net_get_id(LP_METHOD_NM);
            model_net_event(net_id, m->category, m->src_gid, m->pull_size,
                    recv_queue_time, m->event_size_bytes, tmp_ptr, 0, NULL,
                    lp);
        }
        else{
            tw_event * e_new = tw_event_new(m->final_dest_gid, recv_queue_time, lp);
            void * m_new = tw_event_data(e_new);
            memcpy(m_new, tmp_ptr, m->event_size_bytes);
            tw_event_send(e_new);
        }
    }

    return;
}

/* reverse computation for msg start event */
static void handle_msg_start_rev_event(
    sn_state * ns,
    tw_bf * b,
    sn_message * m,
    tw_lp * lp)
{
    ns->net_send_next_idle = m->net_send_next_idle_saved;

    codes_local_latency_reverse(lp);

    if(m->local_event_size_bytes > 0)
    {
        codes_local_latency_reverse(lp);
    }

    mn_stats* stat;
    stat = model_net_find_stats(m->category, ns->sn_stats_array);
    stat->send_count--;
    stat->send_bytes -= m->net_msg_size_bytes;
    stat->send_time = m->send_time_saved;

    return;
}

/* handler for msg start event; this indicates that the caller is trying to
 * transmit a message through this NIC
 */
static void handle_msg_start_event(
    sn_state * ns,
    tw_bf * b,
    sn_message * m,
    tw_lp * lp)
{
    tw_event *e_new;
    sn_message *m_new;
    tw_stime send_queue_time = 0;
    mn_stats* stat;
    int mapping_rep_id, mapping_offset, dummy;
    tw_lpid dest_id;
    char lp_group_name[MAX_NAME_LENGTH];
    int total_event_size;

    total_event_size = model_net_get_msg_sz(SIMPLENET) + m->event_size_bytes +
        m->local_event_size_bytes;

    //printf("handle_msg_start_event(), lp %llu.\n", (unsigned long long)lp->gid);
    /* add statistics */
    stat = model_net_find_stats(m->category, ns->sn_stats_array);
    stat->send_count++;
    stat->send_bytes += m->net_msg_size_bytes;
    m->send_time_saved = stat->send_time;
    stat->send_time += (ns->params.net_startup_ns + rate_to_ns(m->net_msg_size_bytes,
                ns->params.net_bw_mbps));
    if(stat->max_event_size < total_event_size)
        stat->max_event_size = total_event_size;

    /* calculate send time stamp */
    send_queue_time = ns->params.net_startup_ns; /* net msg startup cost */
    /* bump up time if the NIC send queue isn't idle right now */
    if(ns->net_send_next_idle > tw_now(lp))
        send_queue_time += ns->net_send_next_idle - tw_now(lp);

    /* move the next idle time ahead to after this transmission is
     * _complete_ from the sender's perspective 
     */ 
    m->net_send_next_idle_saved = ns->net_send_next_idle;
    ns->net_send_next_idle = send_queue_time + tw_now(lp) +
        rate_to_ns(m->net_msg_size_bytes, ns->params.net_bw_mbps);


    /* create new event to send msg to receiving NIC */
    dest_id = model_net_find_local_device(SIMPLENET, ns->anno, 0,
            m->final_dest_gid);

//    printf("\n msg start sending to %d ", dest_id);
    void *m_data;
    //e_new = tw_event_new(dest_id, send_queue_time, lp);
    //m_new = tw_event_data(e_new);
    e_new = model_net_method_event_new(dest_id, send_queue_time, lp,
            SIMPLENET, (void**)&m_new, &m_data);

    /* copy entire previous message over, including payload from user of
     * this module
     */
    //memcpy(m_new, m, m->event_size_bytes + model_net_get_msg_sz(SIMPLENET));
    memcpy(m_new, m, sizeof(sn_message));
    if (m->event_size_bytes){
        memcpy(m_data, model_net_method_get_edata(SIMPLENET, m),
                m->event_size_bytes);
    }
    
    m_new->event_type = SN_MSG_READY;
    
    //print_base_from(SIMPLENET, m_new);
    //print_msg(m_new);
    tw_event_send(e_new);

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

        void * m_loc = (char*) model_net_method_get_edata(SIMPLENET, m) +
            m->event_size_bytes;

         //local_event = (char*)m;
         //local_event += model_net_get_msg_sz(SIMPLENET) + m->event_size_bytes;         	 
        /* copy just the local event data over (which is past the remote event
         * in memory) */
        memcpy(m_new, m_loc, m->local_event_size_bytes);
        tw_event_send(e_new);
    }
    return;
}

/* Model-net function calls */

/*This method will serve as an intermediate layer between simplenet and modelnet. 
 * It takes the packets from modelnet layer and calls underlying simplenet methods*/
static tw_stime simplenet_packet_event(
		char* category,
		tw_lpid final_dest_lp,
		uint64_t packet_size,
                int is_pull,
                uint64_t pull_size, /* only used when is_pull == 1 */
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
     sn_message * msg;
     char* tmp_ptr;

     xfer_to_nic_time = codes_local_latency(sender);
     // this is a self message
     e_new = model_net_method_event_new(sender->gid, xfer_to_nic_time+offset,
             sender, SIMPLENET, (void**)&msg, (void**)&tmp_ptr);
     strcpy(msg->category, category);
     msg->final_dest_gid = final_dest_lp;
     msg->src_gid = src_lp;
     msg->magic = sn_get_magic();
     msg->net_msg_size_bytes = packet_size;
     msg->event_size_bytes = 0;
     msg->local_event_size_bytes = 0;
     msg->event_type = SN_MSG_START;
     msg->is_pull = is_pull;
     msg->pull_size = pull_size;

     /*Fill in simplenet information*/     
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
      }
     tw_event_send(e_new);
     return xfer_to_nic_time;
}

static void sn_configure()
{
    anno_map = codes_mapping_get_lp_anno_map(LP_CONFIG_NM);
    assert(anno_map);
    num_params = anno_map->num_annos + (anno_map->has_unanno_lp > 0);
    all_params = malloc(num_params * sizeof(*all_params));
    for (uint64_t i = 0; i < anno_map->num_annos; i++){
        const char * anno = anno_map->annotations[i];
        int rc;
        rc = configuration_get_value_double(&config, "PARAMS",
                "net_startup_ns", anno, &all_params[i].net_startup_ns);
        if (rc != 0){
            tw_error(TW_LOC,
                    "simplenet: unable to read PARAMS:net_startup_ns@%s",
                    anno);
        }
        rc = configuration_get_value_double(&config, "PARAMS", "net_bw_mbps",
                anno, &all_params[i].net_bw_mbps);
        if (rc != 0){
            tw_error(TW_LOC, "simplenet: unable to read PARAMS:net_bw_mbps@%s",
                    anno);
        }
    }
    if (anno_map->has_unanno_lp > 0){
        int rc;
        rc = configuration_get_value_double(&config, "PARAMS",
                "net_startup_ns", NULL,
                &all_params[num_params-1].net_startup_ns);
        if (rc != 0){
            tw_error(TW_LOC, "simplenet: unable to read PARAMS:net_startup_ns");
        }
        rc = configuration_get_value_double(&config, "PARAMS", "net_bw_mbps",
                NULL, &all_params[num_params-1].net_bw_mbps);
        if (rc != 0){
            tw_error(TW_LOC, "simplenet: unable to read PARAMS:net_bw_mbps");
        }
    }
}

static void simplenet_packet_event_rc(tw_lp *sender)
{
    codes_local_latency_reverse(sender);
    return;
}

static tw_lpid sn_find_local_device(
        const char * annotation, 
        int          ignore_annotations,
        tw_lp      * sender)
{
     char lp_group_name[MAX_NAME_LENGTH];
     int mapping_rep_id, mapping_offset, dummy;
     tw_lpid dest_id;

     // TODO: don't ignore annotations
     codes_mapping_get_lp_info(sender->gid, lp_group_name, &dummy, NULL,
             &dummy, NULL, &mapping_rep_id, &mapping_offset);
     codes_mapping_get_lp_id(lp_group_name, LP_CONFIG_NM, annotation,
             ignore_annotations, mapping_rep_id, mapping_offset, &dest_id);

    return(dest_id);
}

#if SIMPLENET_DEBUG
void print_msg(sn_message *m){
    printf(" sn:\n  type:%d, magic:%d, src:%lu, dest:%lu, esize:%d, lsize:%d\n",
            m->event_type, m->magic, m->src_gid, m->final_dest_gid,
            m->event_size_bytes, m->local_event_size_bytes);
}
#endif


/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
