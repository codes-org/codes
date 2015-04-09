/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#include <string.h>
#include <assert.h>

#include "codes/model-net.h"
#include "codes/model-net-method.h"
#include "codes/model-net-lp.h"
#include "codes/model-net-sched.h"
#include "codes/codes.h"
#include <codes/codes_mapping.h>

#define STR_SIZE 16
#define PROC_TIME 10.0

extern struct model_net_method simplenet_method;
extern struct model_net_method simplep2p_method;
extern struct model_net_method torus_method;
extern struct model_net_method dragonfly_method;
extern struct model_net_method loggp_method;

#define X(a,b,c,d) b,
char * model_net_lp_config_names[] = {
    NETWORK_DEF
};
#undef X

#define X(a,b,c,d) c,
char * model_net_method_names[] = {
    NETWORK_DEF
};
#undef X

/* Global array initialization, terminated with a NULL entry */
#define X(a,b,c,d) d,
struct model_net_method* method_array[] = { 
    NETWORK_DEF
};
#undef X

// counter and offset for the MN_START_SEQ / MN_END_SEQ macros
int mn_in_seqence = 0;
tw_stime mn_msg_offset = 0.0;

// message parameters for use via model_net_set_msg_param
static int is_msg_params_set[MAX_MN_MSG_PARAM_TYPES];
static mn_sched_params sched_params;

// global listing of lp types found by model_net_register
// - needs to be held between the register and configure calls
static int do_config_nets[MAX_NETS];

void model_net_register(){
    // first set up which networks need to be registered, then pass off to base
    // LP to do its thing
    memset(do_config_nets, 0, MAX_NETS * sizeof(*do_config_nets));
    for (int grp = 0; grp < lpconf.lpgroups_count; grp++){
        config_lpgroup_t *lpgroup = &lpconf.lpgroups[grp];
        for (int lpt = 0; lpt < lpgroup->lptypes_count; lpt++){
            char *nm = lpgroup->lptypes[lpt].name;
            for (int n = 0; n < MAX_NETS; n++){
                if (!do_config_nets[n] && 
                        strcmp(model_net_lp_config_names[n], nm) == 0){
                    do_config_nets[n] = 1;
                    break;
                }
            }
        }
    }
    model_net_base_register(do_config_nets);
}

int* model_net_configure(int *id_count){
    // first call the base LP configure, which sets up the general parameters
    model_net_base_configure();

    // do network-specific configures
    *id_count = 0;
    for (int i = 0; i < MAX_NETS; i++) {
        if (do_config_nets[i]){
            method_array[i]->mn_configure();
            (*id_count)++;
        }
    }

    // allocate the output
    int *ids = malloc(*id_count * sizeof(int));
    // read the ordering provided by modelnet_order
    char **values;
    size_t length;
    int ret = configuration_get_multivalue(&config, "PARAMS", "modelnet_order",
            NULL, &values, &length);
    if (ret != 1){
        tw_error(TW_LOC, "unable to read PARAMS:modelnet_order variable\n");
    }
    if (length != (size_t) *id_count){
        tw_error(TW_LOC, "number of networks in PARAMS:modelnet_order "
                "do not match number in LPGROUPS\n");
    }
    // set the index
    for (int i = 0; i < *id_count; i++){
        ids[i] = -1;
        for (int n = 0; n < MAX_NETS; n++){
            if (strcmp(values[i], model_net_method_names[n]) == 0){
                if (!do_config_nets[n]){
                    tw_error(TW_LOC, "network in PARAMS:modelnet_order not "
                            "present in LPGROUPS: %s\n", values[i]);
                }
                ids[i] = n;
                break;
            }
        }
        if (ids[i] == -1){
            tw_error(TW_LOC, "unknown network in PARAMS:modelnet_order: %s\n",
                    values[i]);
        }
        free(values[i]);
    }
    free(values);

    // init the per-msg params here
    memset(is_msg_params_set, 0,
            MAX_MN_MSG_PARAM_TYPES*sizeof(*is_msg_params_set));

    return ids;
}

int model_net_get_id(char *name){
    int i;
    for(i=0; method_array[i] != NULL; i++) {
        if(strcmp(model_net_method_names[i], name) == 0) {
            return i;
        }
    }
    return -1;
}

void model_net_write_stats(tw_lpid lpid, struct mn_stats* stat)
{
    int ret;
    char id[19+CATEGORY_NAME_MAX+1];
    char data[1024];

    sprintf(id, "model-net-category-%s", stat->category);
    sprintf(data, "lp:%ld\tsend_count:%ld\tsend_bytes:%ld\tsend_time:%f\t" 
        "recv_count:%ld\trecv_bytes:%ld\trecv_time:%f\tmax_event_size:%ld\n",
        (long)lpid,
        stat->send_count,
        stat->send_bytes,
        stat->send_time,
        stat->recv_count,
        stat->recv_bytes,
        stat->recv_time,
        stat->max_event_size);

    ret = lp_io_write(lpid, id, strlen(data), data);
    assert(ret == 0);

    return;
}

void model_net_print_stats(tw_lpid lpid, mn_stats mn_stats_array[])
{

    int i;
    struct mn_stats all;

    memset(&all, 0, sizeof(all));
    sprintf(all.category, "all");

    for(i=0; i<CATEGORY_MAX; i++)
    {
        if(strlen(mn_stats_array[i].category) > 0)
        {
            all.send_count += mn_stats_array[i].send_count;
            all.send_bytes += mn_stats_array[i].send_bytes;
            all.send_time += mn_stats_array[i].send_time;
            all.recv_count += mn_stats_array[i].recv_count;
            all.recv_bytes += mn_stats_array[i].recv_bytes;
            all.recv_time += mn_stats_array[i].recv_time;
            if(mn_stats_array[i].max_event_size > all.max_event_size)
                all.max_event_size = mn_stats_array[i].max_event_size;

            model_net_write_stats(lpid, &mn_stats_array[i]);
        }
    }
    model_net_write_stats(lpid, &all);
}

struct mn_stats* model_net_find_stats(const char* category, mn_stats mn_stats_array[])
{
    int i;
    int new_flag = 0;
    int found_flag = 0;

    for(i=0; i<CATEGORY_MAX; i++)
    {
        if(strlen(mn_stats_array[i].category) == 0)
        {
            found_flag = 1;
            new_flag = 1;
            break;
        }
        if(strcmp(category, mn_stats_array[i].category) == 0)
        {
            found_flag = 1;
            new_flag = 0;
            break;
        }
    }
    assert(found_flag);

    if(new_flag)
    {
        strcpy(mn_stats_array[i].category, category);
    }
    return(&mn_stats_array[i]);
}

static void model_net_event_impl_base(
        int net_id,
        const char * annotation,
        int ignore_annotations,
        char* category, 
        tw_lpid final_dest_lp, 
        uint64_t message_size, 
        int is_pull,
        tw_stime offset,
        int remote_event_size,
        const void* remote_event,
        int self_event_size,
        const void* self_event,
        tw_lp *sender) {

    if (remote_event_size + self_event_size + sizeof(model_net_wrap_msg) 
            > g_tw_msg_sz){
        tw_error(TW_LOC, "Error: model_net trying to transmit an event of size "
                         "%d but ROSS is configured for events of size %zd\n",
                         remote_event_size+self_event_size+sizeof(model_net_wrap_msg),
                         g_tw_msg_sz);
        return;
    }

    tw_lpid mn_lp = model_net_find_local_device(net_id, annotation, 
            ignore_annotations, sender->gid);
    tw_stime poffset = codes_local_latency(sender);
    if (mn_in_seqence){
        tw_stime tmp = mn_msg_offset;
        mn_msg_offset += poffset;
        poffset += tmp;
    }
    tw_event *e = codes_event_new(mn_lp, poffset+offset, sender);

    model_net_wrap_msg *m = tw_event_data(e);
    msg_set_header(model_net_base_magic, MN_BASE_NEW_MSG, sender->gid, &m->h);

    // set the request struct 
    model_net_request *r = &m->msg.m_base.req;
    r->net_id = net_id;
    r->final_dest_lp = final_dest_lp;
    r->src_lp = sender->gid;
    r->msg_size = message_size;
    r->remote_event_size = remote_event_size;
    r->self_event_size = self_event_size;
    r->is_pull = is_pull;
    strncpy(r->category, category, CATEGORY_NAME_MAX-1);
    r->category[CATEGORY_NAME_MAX-1]='\0';

    // this is an outgoing message
    m->msg.m_base.is_from_remote = 0;
    
    // set the msg-specific params
    if (is_msg_params_set[MN_SCHED_PARAM_PRIO])
        m->msg.m_base.sched_params = sched_params;
    else // set the default
        model_net_sched_set_default_params(&m->msg.m_base.sched_params);
    // once params are set, clear the flags 
    memset(is_msg_params_set, 0,
            MAX_MN_MSG_PARAM_TYPES*sizeof(*is_msg_params_set));

    void *e_msg = (m+1);
    if (remote_event_size > 0){
        memcpy(e_msg, remote_event, remote_event_size);
        e_msg = (char*)e_msg + remote_event_size; 
    }
    if (self_event_size > 0){
        memcpy(e_msg, self_event, self_event_size);
    }

    //print_base(m);
    tw_event_send(e);
}
static void model_net_event_impl_base_rc(tw_lp *sender){
    codes_local_latency_reverse(sender);
}

void model_net_event(
    int net_id,
    char* category, 
    tw_lpid final_dest_lp, 
    uint64_t message_size, 
    tw_stime offset,
    int remote_event_size,
    const void* remote_event,
    int self_event_size,
    const void* self_event,
    tw_lp *sender)
{
    model_net_event_impl_base(net_id, NULL, 1, category, final_dest_lp,
            message_size, 0, offset, remote_event_size, remote_event,
            self_event_size, self_event, sender);
}

void model_net_event_annotated(
        int net_id,
        const char * annotation,
        char* category, 
        tw_lpid final_dest_lp, 
        uint64_t message_size, 
        tw_stime offset,
        int remote_event_size,
        const void* remote_event,
        int self_event_size,
        const void* self_event,
        tw_lp *sender){
    model_net_event_impl_base(net_id, annotation, 0, category, final_dest_lp,
            message_size, 0, offset, remote_event_size, remote_event,
            self_event_size, self_event, sender);
}

void model_net_pull_event(
        int net_id,
        char *category,
        tw_lpid final_dest_lp,
        uint64_t message_size,
        tw_stime offset,
        int self_event_size,
        const void *self_event,
        tw_lp *sender){
    /* NOTE: for a pull, we are filling the *remote* event - it will be remote
     * from the destination's POV */
    model_net_event_impl_base(net_id, NULL, 0, category, final_dest_lp,
            message_size, 1, offset, self_event_size, self_event, 0, NULL,
            sender);
}

void model_net_pull_event_annotated(
        int net_id,
        const char * annotation,
        char *category,
        tw_lpid final_dest_lp,
        uint64_t message_size,
        tw_stime offset,
        int self_event_size,
        const void *self_event,
        tw_lp *sender){
    /* NOTE: for a pull, we are filling the *remote* event - it will be remote
     * from the destination's POV */
    model_net_event_impl_base(net_id, annotation, 1, category, final_dest_lp,
            message_size, 1, offset, self_event_size, self_event, 0, NULL,
            sender);
}

void model_net_event_rc(
        int net_id,
        tw_lp *sender,
        uint64_t message_size){
    model_net_event_impl_base_rc(sender);
}

void model_net_pull_event_rc(
        int net_id,
        tw_lp *sender) {
    model_net_event_impl_base_rc(sender);
}

void model_net_set_msg_param(
        enum msg_param_type type,
        int sub_type,
        const void * params){
    switch(type){
        case MN_MSG_PARAM_SCHED:
            is_msg_params_set[MN_MSG_PARAM_SCHED] = 1;
            switch(sub_type){
                case MN_SCHED_PARAM_PRIO:
                    sched_params.prio = *(int*)params;
                    break;
                default:
                    tw_error(TW_LOC, "unknown or unsupported "
                            "MN_MSG_PARAM_SCHED parameter type");
            }
            break;
        default:
            tw_error(TW_LOC, "unknown or unsupported msg_param_type");
    }
}

/* returns the message size, can be either simplenet, dragonfly or torus message size*/
int model_net_get_msg_sz(int net_id)
{
   // TODO: Add checks on network name
   // TODO: Add dragonfly and torus network models
   return sizeof(model_net_wrap_msg);
#if 0
   if(net_id < 0 || net_id >= MAX_NETS)
     {
      printf("%s Error: Uninitializied modelnet network, call modelnet_init first\n", __FUNCTION__);
      exit(-1);
     }

       return method_array[net_id]->mn_get_msg_sz();
#endif
}

/* returns the packet size in the modelnet struct */
uint64_t model_net_get_packet_size(int net_id)
{
  if(net_id < 0 || net_id >= MAX_NETS)
     {
       fprintf(stderr, "%s Error: Uninitializied modelnet network, call modelnet_init first\n", __FUNCTION__);
       exit(-1);
     }
  return method_array[net_id]->packet_size; // TODO: where to set the packet size?
}

/* This event does a collective operation call for model-net */
void model_net_event_collective(int net_id, char* category, int message_size, int remote_event_size, const void* remote_event, tw_lp* sender)
{
  if(net_id < 0 || net_id > MAX_NETS)
     {
       fprintf(stderr, "%s Error: Uninitializied modelnet network, call modelnet_init first\n", __FUNCTION__);
       exit(-1);
     }
  return method_array[net_id]->mn_collective_call(category, message_size, remote_event_size, remote_event, sender);
}

/* reverse event of the collective operation call */
void model_net_event_collective_rc(int net_id, int message_size, tw_lp* sender)
{
  if(net_id < 0 || net_id > MAX_NETS)
     {
       fprintf(stderr, "%s Error: Uninitializied modelnet network, call modelnet_init first\n", __FUNCTION__);
       exit(-1);
     }
  return method_array[net_id]->mn_collective_call_rc(message_size, sender);
}

/* returns lp type for modelnet */
const tw_lptype* model_net_get_lp_type(int net_id)
{
    if(net_id < 0 || net_id >= MAX_NETS)
     {
       fprintf(stderr, "%s Error: Uninitializied modelnet network, call modelnet_init first\n", __FUNCTION__);
       exit(-1);
     }

   // TODO: ADd checks by network names
   // Add dragonfly and torus network models
   return method_array[net_id]->mn_get_lp_type();
}

void model_net_report_stats(int net_id)
{
  if(net_id < 0 || net_id >= MAX_NETS)
  {
    fprintf(stderr, "%s Error: Uninitializied modelnet network, call modelnet_init first\n", __FUNCTION__);
    exit(-1);
   }

     // TODO: ADd checks by network names
     //    // Add dragonfly and torus network models
   method_array[net_id]->mn_report_stats();
   return;
}

static tw_lpid model_net_find_local_device_default(
        int          net_id,
        const char * annotation,
        int          ignore_annotations,
        tw_lpid      sender_gid) {
    char group_name[MAX_NAME_LENGTH];
    int dummy, mapping_rep, mapping_offset;
    int num_mn_lps;
    tw_lpid rtn;

    codes_mapping_get_lp_info(sender_gid, group_name, &dummy, NULL, &dummy,
            NULL, &mapping_rep, &mapping_offset);
    num_mn_lps = codes_mapping_get_lp_count(group_name, 1,
            model_net_lp_config_names[net_id], annotation, ignore_annotations);
    if (num_mn_lps <= 0) {
        tw_error(TW_LOC,
                "ERROR: Found no modelnet lps in group %s "
                "(source lpid %lu) with network type %s, annotation %s\n",
                group_name, sender_gid, model_net_lp_config_names[net_id],
                (ignore_annotations) ? "<ignored>" : annotation);
    }
    codes_mapping_get_lp_id(group_name, model_net_lp_config_names[net_id],
            annotation, ignore_annotations, mapping_rep,
            mapping_offset % num_mn_lps, &rtn);
    return rtn;
}

tw_lpid model_net_find_local_device(
        int          net_id,
        const char * annotation,
        int          ignore_annotations,
        tw_lpid      sender_gid)
{
    if (method_array[net_id]->model_net_method_find_local_device == NULL)
        return model_net_find_local_device_default(net_id, annotation,
                ignore_annotations, sender_gid);
    else
        return(method_array[net_id]->model_net_method_find_local_device(
                    annotation, ignore_annotations, sender_gid));
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
