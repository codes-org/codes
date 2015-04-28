/*
 * Copyright (C) 2014 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#include <stddef.h>
#include <assert.h>
#include "codes/model-net.h"
#include "codes/model-net-method.h"
#include "codes/model-net-lp.h"
#include "codes/model-net-sched.h"
#include "codes/codes_mapping.h"
#include "codes/jenkins-hash.h"

#define MN_NAME "model_net_base"

/**** BEGIN SIMULATION DATA STRUCTURES ****/

int model_net_base_magic;

// message-type specific offsets - don't want to get bitten later by alignment
// issues...
static int msg_offsets[MAX_NETS];

typedef struct model_net_base_params_s {
    model_net_sched_cfg_params sched_params;
    uint64_t packet_size;
    int use_recv_queue;
} model_net_base_params;

/* annotation-specific parameters (unannotated entry occurs at the 
 * last index) */
static int                       num_params = 0;
static const char              * annos[CONFIGURATION_MAX_ANNOS];
static model_net_base_params     all_params[CONFIGURATION_MAX_ANNOS];

typedef struct model_net_base_state {
    int net_id;
    // whether scheduler loop is running
    int in_sched_send_loop, in_sched_recv_loop;
    // model-net schedulers
    model_net_sched *sched_send, *sched_recv;
    // parameters
    const model_net_base_params * params;
    // lp type and state of underlying model net method - cache here so we
    // don't have to constantly look up
    const tw_lptype *sub_type;
    void *sub_state;
} model_net_base_state;


/**** END SIMULATION DATA STRUCTURES ****/

/**** BEGIN LP, EVENT PROCESSING FUNCTION DECLS ****/

/* ROSS LP processing functions */  
static void model_net_base_lp_init(
        model_net_base_state * ns,
        tw_lp * lp);
static void model_net_base_event(
        model_net_base_state * ns,
        tw_bf * b,
        model_net_wrap_msg * m,
        tw_lp * lp);
static void model_net_base_event_rc(
        model_net_base_state * ns,
        tw_bf * b,
        model_net_wrap_msg * m,
        tw_lp * lp);
static void model_net_base_finalize(
        model_net_base_state * ns,
        tw_lp * lp);

/* event type handlers */
static void handle_new_msg(
        model_net_base_state * ns,
        tw_bf *b,
        model_net_wrap_msg * m,
        tw_lp * lp);
static void handle_sched_next(
        model_net_base_state * ns,
        tw_bf *b,
        model_net_wrap_msg * m,
        tw_lp * lp);
static void handle_new_msg_rc(
        model_net_base_state * ns,
        tw_bf *b,
        model_net_wrap_msg * m,
        tw_lp * lp);
static void handle_sched_next_rc(
        model_net_base_state * ns,
        tw_bf *b,
        model_net_wrap_msg * m,
        tw_lp * lp);

/* ROSS function pointer table for this LP */
tw_lptype model_net_base_lp = {
    (init_f) model_net_base_lp_init,
    (pre_run_f) NULL,
    (event_f) model_net_base_event,
    (revent_f) model_net_base_event_rc,
    (final_f)  model_net_base_finalize, 
    (map_f) codes_mapping,
    sizeof(model_net_base_state),
};

/**** END LP, EVENT PROCESSING FUNCTION DECLS ****/

/**** BEGIN IMPLEMENTATIONS ****/

void model_net_base_register(int *do_config_nets){
    // here, we initialize ALL lp types to use the base type
    for (int i = 0; i < MAX_NETS; i++){
        if (do_config_nets[i]){
            // some model-net lps need custom registration hooks (dragonfly).
            // Those that don't NULL out the reg. function
            if (method_array[i]->mn_register == NULL)
                lp_type_register(model_net_lp_config_names[i],
                        &model_net_base_lp);
            else
                method_array[i]->mn_register(&model_net_base_lp);
        }
    }
}

static void base_read_config(const char * anno, model_net_base_params *p){
    char sched[MAX_NAME_LENGTH];
    long int packet_size_l = 0;
    uint64_t packet_size;
    int ret;

    ret = configuration_get_value(&config, "PARAMS", "modelnet_scheduler",
            anno, sched, MAX_NAME_LENGTH);
    configuration_get_value_longint(&config, "PARAMS", "packet_size", anno,
            &packet_size_l);
    packet_size = packet_size_l;

    if (ret > 0){
        int i;
        for (i = 0; i < MAX_SCHEDS; i++){
            if (strcmp(sched_names[i], sched) == 0){
                p->sched_params.type = i;
                break;
            }
        }
        if (i == MAX_SCHEDS){
            tw_error(TW_LOC,"Unknown value for PARAMS:modelnet-scheduler : "
                    "%s", sched); 
        }
    }
    else{
        // default: FCFS
        p->sched_params.type = MN_SCHED_FCFS;
    }

    // get scheduler-specific parameters
    if (p->sched_params.type == MN_SCHED_PRIO){
        // prio scheduler uses default parameters 
        int             * num_prios = &p->sched_params.u.prio.num_prios;
        enum sched_type * sub_stype = &p->sched_params.u.prio.sub_stype;
        // number of priorities to allocate
        ret = configuration_get_value_int(&config, "PARAMS",
                "prio-sched-num-prios", anno, num_prios);
        if (ret != 0)
            *num_prios = 10;

        ret = configuration_get_value(&config, "PARAMS",
                "prio-sched-sub-sched", anno, sched, MAX_NAME_LENGTH);
        if (ret == 0)
            *sub_stype = MN_SCHED_FCFS;
        else{
            int i;
            for (i = 0; i < MAX_SCHEDS; i++){
                if (strcmp(sched_names[i], sched) == 0){
                    *sub_stype = i;
                    break;
                }
            }
            if (i == MAX_SCHEDS){
                tw_error(TW_LOC, "Unknown value for "
                        "PARAMS:prio-sched-sub-sched %s", sched);
            }
            else if (i == MN_SCHED_PRIO){
                tw_error(TW_LOC, "priority scheduler cannot be used as a "
                        "priority scheduler's sub sched "
                        "(PARAMS:prio-sched-sub-sched)");
            }
        }
    }

    if (p->sched_params.type == MN_SCHED_FCFS_FULL ||
            (p->sched_params.type == MN_SCHED_PRIO &&
             p->sched_params.u.prio.sub_stype == MN_SCHED_FCFS_FULL)){
        // override packet size to something huge (leave a bit in the unlikely
        // case that an op using packet size causes overflow)
        packet_size = 1ull << 62;
    }
    else if (!packet_size &&
            (p->sched_params.type != MN_SCHED_FCFS_FULL ||
             (p->sched_params.type == MN_SCHED_PRIO &&
              p->sched_params.u.prio.sub_stype != MN_SCHED_FCFS_FULL))){
        packet_size = 512;
        fprintf(stderr, "WARNING, no packet size specified, setting packet "
                "size to %llu\n", packet_size);
    }


    p->packet_size = packet_size;
}

void model_net_base_configure(){
    uint32_t h1=0, h2=0;

    bj_hashlittle2(MN_NAME, strlen(MN_NAME), &h1, &h2);
    model_net_base_magic = h1+h2;

    // set up offsets - doesn't matter if they are actually used or not
    msg_offsets[SIMPLENET] =
        offsetof(model_net_wrap_msg, msg.m_snet);
    msg_offsets[SIMPLEP2P] =
        offsetof(model_net_wrap_msg, msg.m_sp2p);
    msg_offsets[TORUS] =
        offsetof(model_net_wrap_msg, msg.m_torus);
    msg_offsets[DRAGONFLY] =
        offsetof(model_net_wrap_msg, msg.m_dfly);
    msg_offsets[LOGGP] =
        offsetof(model_net_wrap_msg, msg.m_loggp);

    // perform the configuration(s)
    // This part is tricky, as we basically have to look up all annotations that
    // have LP names of the form modelnet_*. For each of those, we need to read
    // the base parameters
    // - the init is a little easier as we can use the LP-id to look up the
    // annotation

    // first grab all of the annotations and store locally
    for (int c = 0; c < lpconf.lpannos_count; c++){
        const config_anno_map_t *amap = &lpconf.lpannos[c];
        if (strncmp("modelnet_", amap->lp_name, 9) == 0){
            for (int n = 0; n < amap->num_annos; n++){
                int a;
                for (a = 0; a < num_params; a++){
                    if (annos[a] != NULL &&
                            strcmp(amap->annotations[n], annos[a]) == 0){
                        break;
                    }
                }
                if (a == num_params){
                    // found a new annotation
                    annos[num_params++] = amap->annotations[n];
                }
            }
            if (amap->has_unanno_lp){
                int a;
                for (a = 0; a < num_params; a++){
                    if (annos[a] == NULL)
                        break;
                }
                if (a == num_params){
                    // found a new (empty) annotation
                    annos[num_params++] = NULL;
                }
            }
        }
    }

    // now that we have all of the annos for all of the networks, loop through
    // and read the configs
    for (int i = 0; i < num_params; i++){
        base_read_config(annos[i], &all_params[i]);
    }
}

void model_net_base_lp_init(
        model_net_base_state * ns,
        tw_lp * lp){
    // obtain the underlying lp type through codes-mapping
    char lp_type_name[MAX_NAME_LENGTH], anno[MAX_NAME_LENGTH];
    int dummy;

    codes_mapping_get_lp_info(lp->gid, NULL, &dummy, 
            lp_type_name, &dummy, anno, &dummy, &dummy);

    // get annotation-specific parameters
    for (int i = 0; i < num_params; i++){
        if ((anno[0]=='\0' && annos[i] == NULL) ||
                strcmp(anno, annos[i]) == 0){
            ns->params = &all_params[i];
            break;
        }
    }

    // find the corresponding method name / index
    for (int i = 0; i < MAX_NETS; i++){
        if (strcmp(model_net_lp_config_names[i], lp_type_name) == 0){
            ns->net_id = i;
            break;
        }
    }

    ns->sched_send = malloc(sizeof(model_net_sched));
    ns->sched_recv = malloc(sizeof(model_net_sched));
    // init both the sender queue and the 'receiver' queue 
    model_net_sched_init(&ns->params->sched_params, 0, method_array[ns->net_id],
            ns->sched_send);
    model_net_sched_init(&ns->params->sched_params, 1, method_array[ns->net_id],
            ns->sched_recv);

    ns->sub_type = model_net_get_lp_type(ns->net_id);
    // NOTE: some models actually expect LP state to be 0 initialized...
    // *cough anything that uses mn_stats_array cough*
    ns->sub_state = calloc(1, ns->sub_type->state_sz);

    // initialize the model-net method
    ns->sub_type->init(ns->sub_state, lp);
}

void model_net_base_event(
        model_net_base_state * ns,
        tw_bf * b,
        model_net_wrap_msg * m,
        tw_lp * lp){
    assert(m->h.magic == model_net_base_magic);
    
    switch (m->h.event_type){
        case MN_BASE_NEW_MSG:
            handle_new_msg(ns, b, m, lp);
            break;
        case MN_BASE_SCHED_NEXT:
            handle_sched_next(ns, b, m, lp);
            break;
        case MN_BASE_PASS: ;
            void * sub_msg = ((char*)m)+msg_offsets[ns->net_id];
            ns->sub_type->event(ns->sub_state, b, sub_msg, lp);
            break;
        /* ... */
        default:
            assert(!"model_net_base event type not known");
            break;
    }
}

void model_net_base_event_rc(
        model_net_base_state * ns,
        tw_bf * b,
        model_net_wrap_msg * m,
        tw_lp * lp){
    assert(m->h.magic == model_net_base_magic);
    
    switch (m->h.event_type){
        case MN_BASE_NEW_MSG:
            handle_new_msg_rc(ns, b, m, lp);
            break;
        case MN_BASE_SCHED_NEXT:
            handle_sched_next_rc(ns, b, m, lp);
            break;
        case MN_BASE_PASS: ;
            void * sub_msg = ((char*)m)+msg_offsets[ns->net_id];
            ns->sub_type->revent(ns->sub_state, b, sub_msg, lp);
            break;
        /* ... */
        default:
            assert(!"model_net_base event type not known");
            break;
    }

    *(int*)b = 0;
}

void model_net_base_finalize(
        model_net_base_state * ns,
        tw_lp * lp){
    ns->sub_type->final(ns->sub_state, lp);
    free(ns->sub_state);
}

/// bitfields used:
/// c0 - we initiated a sched_next event
void handle_new_msg(
        model_net_base_state * ns,
        tw_bf *b,
        model_net_wrap_msg * m,
        tw_lp * lp){
    // simply pass down to the scheduler
    model_net_request *r = &m->msg.m_base.req;
    // don't forget to set packet size, now that we're responsible for it!
    r->packet_size = ns->params->packet_size;
    void * m_data = m+1;
    void *remote = NULL, *local = NULL;
    if (r->remote_event_size > 0){
        remote = m_data;
        m_data = (char*)m_data + r->remote_event_size;
    }
    if (r->self_event_size > 0){
        local = m_data;
    }
    
    // set message-specific params
    int is_from_remote = m->msg.m_base.is_from_remote;
    model_net_sched *ss = is_from_remote ? ns->sched_recv : ns->sched_send;
    int *in_sched_loop = is_from_remote  ? 
        &ns->in_sched_recv_loop : &ns->in_sched_send_loop;
    model_net_sched_add(r, &m->msg.m_base.sched_params, r->remote_event_size,
            remote, r->self_event_size, local, ss, &m->msg.m_base.rc, lp);
    
    if (*in_sched_loop == 0){
        b->c0 = 1;
        tw_event *e = codes_event_new(lp->gid, codes_local_latency(lp), lp);
        model_net_wrap_msg *m = tw_event_data(e);
        msg_set_header(model_net_base_magic, MN_BASE_SCHED_NEXT, lp->gid,
                &m->h);
        m->msg.m_base.is_from_remote = is_from_remote;
        // m_base not used in sched event
        tw_event_send(e);
        *in_sched_loop = 1;
    }
}

void handle_new_msg_rc(
        model_net_base_state *ns,
        tw_bf *b,
        model_net_wrap_msg *m,
        tw_lp *lp){
    int is_from_remote = m->msg.m_base.is_from_remote;
    model_net_sched *ss = is_from_remote ? ns->sched_recv : ns->sched_send;
    int *in_sched_loop = is_from_remote  ? 
        &ns->in_sched_recv_loop : &ns->in_sched_send_loop;

    model_net_sched_add_rc(ss, &m->msg.m_base.rc, lp);
    if (b->c0){
        codes_local_latency_reverse(lp);
        *in_sched_loop = 0;
    }
}

/// bitfields used
/// c0 - scheduler loop is finished
void handle_sched_next(
        model_net_base_state * ns,
        tw_bf *b,
        model_net_wrap_msg * m,
        tw_lp * lp){
    tw_stime poffset;
    int is_from_remote = m->msg.m_base.is_from_remote;
    model_net_sched * ss = is_from_remote ? ns->sched_recv : ns->sched_send;
    int *in_sched_loop = is_from_remote ?
        &ns->in_sched_recv_loop : &ns->in_sched_send_loop;
    int ret = model_net_sched_next(&poffset, ss, m+1, &m->msg.m_base.rc, lp);
    // we only need to know whether scheduling is finished or not - if not,
    // go to the 'next iteration' of the loop
    if (ret == -1){
        b->c0 = 1;
        *in_sched_loop = 0;
    }
    // Currently, only a subset of the network implementations use the
    // callback-based scheduling loop (model_net_method_idle_event).
    // For all others, we need to schedule the next packet
    // immediately
    else if (ns->net_id == SIMPLEP2P || ns->net_id == TORUS){
        tw_event *e = codes_event_new(lp->gid, 
                poffset+codes_local_latency(lp), lp);
        model_net_wrap_msg *m_wrap = tw_event_data(e);
        msg_set_header(model_net_base_magic, MN_BASE_SCHED_NEXT, lp->gid,
                &m_wrap->h);
        m_wrap->msg.m_base.is_from_remote = is_from_remote;
        // no need to set m_base here
        tw_event_send(e);
    }
}

void handle_sched_next_rc(
        model_net_base_state * ns,
        tw_bf *b,
        model_net_wrap_msg * m,
        tw_lp * lp){
    int is_from_remote = m->msg.m_base.is_from_remote;
    model_net_sched * ss = is_from_remote ? ns->sched_recv : ns->sched_send;
    int *in_sched_loop = is_from_remote ?
        &ns->in_sched_recv_loop : &ns->in_sched_send_loop;

    model_net_sched_next_rc(ss, m+1, &m->msg.m_base.rc, lp);
    if (b->c0){
        *in_sched_loop = 1;
    }
    else if (ns->net_id == SIMPLEP2P || ns->net_id == TORUS){
        codes_local_latency_reverse(lp);
    }
}

/**** END IMPLEMENTATIONS ****/

tw_event * model_net_method_event_new(
        tw_lpid dest_gid,
        tw_stime offset_ts,
        tw_lp *sender,
        int net_id,
        void **msg_data,
        void **extra_data){
    tw_event *e = tw_event_new(dest_gid, offset_ts, sender);
    model_net_wrap_msg *m_wrap = tw_event_data(e);
    msg_set_header(model_net_base_magic, MN_BASE_PASS, sender->gid,
            &m_wrap->h);
    *msg_data = ((char*)m_wrap)+msg_offsets[net_id];
    // extra_data is optional
    if (extra_data != NULL){
        *extra_data = m_wrap + 1;
    }
    return e;
}

void model_net_method_send_msg_recv_event(
        tw_lpid final_dest_lp,
        tw_lpid dest_mn_lp,
        tw_lpid src_lp, // the "actual" source (as opposed to the model net lp)
        uint64_t msg_size,
        int is_pull,
        uint64_t pull_size,
        int remote_event_size,
        const mn_sched_params *sched_params,
        const char * category,
        int net_id,
        void * msg,
        tw_stime offset,
        tw_lp *sender){
    tw_event *e = 
        tw_event_new(dest_mn_lp, offset+codes_local_latency(sender), sender);
    model_net_wrap_msg *m = tw_event_data(e);
    msg_set_header(model_net_base_magic, MN_BASE_NEW_MSG, sender->gid, &m->h);

    if (sched_params != NULL)
        m->msg.m_base.sched_params = *sched_params;
    else
        model_net_sched_set_default_params(&m->msg.m_base.sched_params);

    m->msg.m_base.req.final_dest_lp = final_dest_lp;
    m->msg.m_base.req.src_lp = src_lp;
    m->msg.m_base.req.msg_size    = is_pull ? pull_size : msg_size;
    m->msg.m_base.req.packet_size = m->msg.m_base.req.msg_size;
    m->msg.m_base.req.net_id = net_id;
    m->msg.m_base.req.is_pull = is_pull;
    m->msg.m_base.req.remote_event_size = remote_event_size;
    m->msg.m_base.req.self_event_size = 0;
    m->msg.m_base.is_from_remote = 1;

    strncpy(m->msg.m_base.req.category, category, CATEGORY_NAME_MAX-1);
    m->msg.m_base.req.category[CATEGORY_NAME_MAX-1] = '\0';

    if (remote_event_size > 0){
        void * m_dat = model_net_method_get_edata(net_id, msg);
        memcpy(m+1, m_dat, remote_event_size);
    }

    tw_event_send(e);
}

void model_net_method_send_msg_recv_event_rc(tw_lp *sender){
    codes_local_latency_reverse(sender);
}


void model_net_method_idle_event(tw_stime offset_ts, int is_recv_queue,
        tw_lp * lp){
    tw_event *e = tw_event_new(lp->gid, offset_ts, lp);
    model_net_wrap_msg *m_wrap = tw_event_data(e);
    msg_set_header(model_net_base_magic, MN_BASE_SCHED_NEXT, lp->gid,
            &m_wrap->h);
    m_wrap->msg.m_base.is_from_remote = is_recv_queue;
    tw_event_send(e);
}

void * model_net_method_get_edata(int net_id, void *msg){
    return (char*)msg + sizeof(model_net_wrap_msg) - msg_offsets[net_id];
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
