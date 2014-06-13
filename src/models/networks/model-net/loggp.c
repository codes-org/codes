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
#include "codes/net/loggp.h"

#define CATEGORY_NAME_MAX 16
#define CATEGORY_MAX 12

#define LP_CONFIG_NM (model_net_lp_config_names[LOGGP])
#define LP_METHOD_NM (model_net_method_names[LOGGP])

/*Define loggp data types and structs*/
typedef struct loggp_state loggp_state;


struct loggp_state
{
    /* next idle times for network card, both inbound and outbound */
    tw_stime net_send_next_idle;
    tw_stime net_recv_next_idle;
    struct mn_stats loggp_stats_array[CATEGORY_MAX];
};

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
struct param_table_entry param_table[100];
static int param_table_size = 0;

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

/* allocate a new event that will pass through loggp to arriave at its
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
/* Modelnet interface events */
/* sets up the loggp parameters through modelnet interface */
static void loggp_setup(const void* net_params);

/* Issues a loggp packet event call */
static tw_stime loggp_packet_event(
     char* category, 
     tw_lpid final_dest_lp, 
     uint64_t packet_size, 
     int is_pull,
     uint64_t pull_size, /* only used when is_pull==1 */
     tw_stime offset,
     int remote_event_size, 
     const void* remote_event, 
     int self_event_size,
     const void* self_event,
     tw_lpid src_lp,
     tw_lp *sender,
     int is_last_pckt);
static void loggp_packet_event_rc(tw_lp *sender);

static void loggp_packet_event_rc(tw_lp *sender);

static void loggp_report_stats();

static tw_lpid loggp_find_local_device(tw_lp *sender);

static struct param_table_entry* find_params(uint64_t msg_size);

/* data structure for model-net statistics */
struct model_net_method loggp_method =
{
    .mn_setup = loggp_setup,
    .model_net_method_packet_event = loggp_packet_event,
    .model_net_method_packet_event_rc = loggp_packet_event_rc,
    .mn_get_lp_type = loggp_get_lp_type,
    .mn_get_msg_sz = loggp_get_msg_sz,
    .mn_report_stats = loggp_report_stats,
    .model_net_method_find_local_device = loggp_find_local_device,
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
    struct mn_stats* stat;

    ns->net_recv_next_idle = m->net_recv_next_idle_saved;
    
    stat = model_net_find_stats(m->category, ns->loggp_stats_array);
    stat->recv_count--;
    stat->recv_bytes -= m->net_msg_size_bytes;
    stat->recv_time -= m->recv_time_saved;

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
    struct param_table_entry *param;

    param = find_params(m->net_msg_size_bytes);

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

    /* copy only the part of the message used by higher level */
    if(m->event_size_bytes)
    {
      /* schedule event to final destination for when the recv is complete */
//      printf("\n Remote message to LP %d ", m->final_dest_gid); 

        void *tmp_ptr = model_net_method_get_edata(SIMPLENET, m);
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
    ns->net_send_next_idle = m->net_send_next_idle_saved;

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
    char lp_type_name[MAX_NAME_LENGTH], lp_group_name[MAX_NAME_LENGTH];
    int total_event_size;
    double xmit_time;
    struct param_table_entry *param;

    param = find_params(m->net_msg_size_bytes);

    total_event_size = model_net_get_msg_sz(LOGGP) + m->event_size_bytes +
        m->local_event_size_bytes;

    /* NOTE: we do not use the o_s or o_r parameters here; as indicated in
     * the netgauge paper those are typically overlapping with L (and the
     * msg xfer as well) and therefore are more important for overlapping
     * computation rather than simulating communication time.
     */
    xmit_time = param->L + ((double)(m->net_msg_size_bytes-1)*param->G);
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
    codes_mapping_get_lp_info(m->final_dest_gid, lp_group_name, &mapping_grp_id, &mapping_type_id, lp_type_name, &mapping_rep_id, &mapping_offset);
    codes_mapping_get_lp_id(lp_group_name, LP_CONFIG_NM, mapping_rep_id , mapping_offset, &dest_id); 

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
#if 0
     char lp_type_name[MAX_NAME_LENGTH], lp_group_name[MAX_NAME_LENGTH];

     int mapping_grp_id, mapping_rep_id, mapping_type_id, mapping_offset;
     codes_mapping_get_lp_info(sender->gid, lp_group_name, &mapping_grp_id, &mapping_type_id, lp_type_name, &mapping_rep_id, &mapping_offset);
     codes_mapping_get_lp_id(lp_group_name, LP_CONFIG_NM, mapping_rep_id, mapping_offset, &dest_id);
#endif

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

static void loggp_setup(const void* net_params)
{
    loggp_param* loggp_params = (loggp_param*)net_params;
    FILE *conf;
    int ret;
    char buffer[512];
    int line_nr = 0;

    /* TODO: update this to read on one proc and then broadcast */

    printf("Loggp configured to use parameters from file %s\n", loggp_params->net_config_file);

    conf = fopen(loggp_params->net_config_file, "r");
    if(!conf)
    {
        perror("fopen");
        assert(0);
    }

    while(fgets(buffer, 512, conf))
    {
        line_nr++;
        if(buffer[0] == '#')
            continue;
        ret = sscanf(buffer, "%llu %d %lf %lf %lf %lf %lf %lf %lf %lf %lf", 
            &param_table[param_table_size].size,
            &param_table[param_table_size].n,
            &param_table[param_table_size].PRTT_10s,
            &param_table[param_table_size].PRTT_n0s,
            &param_table[param_table_size].PRTT_nPRTT_10ss,
            &param_table[param_table_size].L,
            &param_table[param_table_size].o_s,
            &param_table[param_table_size].o_r,
            &param_table[param_table_size].g,
            &param_table[param_table_size].G,
            &param_table[param_table_size].lsqu_gG);
        if(ret != 11)
        {
            fprintf(stderr, "Error: malformed line %d in %s\n", line_nr, 
                loggp_params->net_config_file);
            assert(0);
        }
        param_table_size++;
    }

    printf("Parsed %d loggp table entries.\n", param_table_size);

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
static struct param_table_entry* find_params(uint64_t msg_size)
{
    int i;

    /* pick parameters based on the next smallest size in the table, but
     * default to beginning or end of table if we are out of range
     */

    for(i=0; i<param_table_size; i++)
    {
        if(param_table[i].size > msg_size)
        {
            break;
        }
    }
    if(i>=param_table_size)
        i = param_table_size-1;

    return(&param_table[i]);
}

static tw_lpid loggp_find_local_device(tw_lp *sender)
{
     char lp_type_name[MAX_NAME_LENGTH], lp_group_name[MAX_NAME_LENGTH];
     int mapping_grp_id, mapping_rep_id, mapping_type_id, mapping_offset;
     tw_lpid dest_id;

     codes_mapping_get_lp_info(sender->gid, lp_group_name, &mapping_grp_id, &mapping_type_id, lp_type_name, &mapping_rep_id, &mapping_offset);
     codes_mapping_get_lp_id(lp_group_name, LP_CONFIG_NM, mapping_rep_id, mapping_offset, &dest_id);

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
