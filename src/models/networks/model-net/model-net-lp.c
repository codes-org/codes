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

typedef struct model_net_base_state {
    int net_id;
    // whether scheduler loop is running
    int in_sched_loop;
    // model-net scheduler
    model_net_sched *sched;
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
     (event_f) model_net_base_event,
     (revent_f) model_net_base_event_rc,
     (final_f)  model_net_base_finalize, 
     (map_f) codes_mapping,
     sizeof(model_net_base_state),
};

/**** END LP, EVENT PROCESSING FUNCTION DECLS ****/

/**** BEGIN IMPLEMENTATIONS ****/

void model_net_base_init(){
    uint32_t h1=0, h2=0;

    bj_hashlittle2(MN_NAME, strlen(MN_NAME), &h1, &h2);
    model_net_base_magic = h1+h2;

    // here, we initialize ALL lp types to use the base type
    // TODO: only initialize ones that are actually used
    for (int i = 0; i < MAX_NETS; i++){
        lp_type_register(model_net_lp_config_names[i], &model_net_base_lp);
    }

    // initialize the msg-specific offsets
    msg_offsets[SIMPLENET] = offsetof(model_net_wrap_msg, msg.m_snet);
    msg_offsets[SIMPLEWAN] = offsetof(model_net_wrap_msg, msg.m_swan);
    msg_offsets[TORUS] = offsetof(model_net_wrap_msg, msg.m_torus);
    msg_offsets[DRAGONFLY] = offsetof(model_net_wrap_msg, msg.m_dfly);
    msg_offsets[LOGGP] = offsetof(model_net_wrap_msg, msg.m_loggp);
}

void model_net_base_lp_init(
        model_net_base_state * ns,
        tw_lp * lp){
    // obtain the underlying lp type through codes-mapping
    char grp_name[MAX_NAME_LENGTH], lp_type_name[MAX_NAME_LENGTH];
    int grp_id, lp_type_id, grp_rep_id, offset;

    codes_mapping_get_lp_info(lp->gid, grp_name, &grp_id, &lp_type_id,
            lp_type_name, &grp_rep_id, &offset);

    // find the corresponding method name / index
    for (int i = 0; i < MAX_NETS; i++){
        if (strcmp(model_net_lp_config_names[i], lp_type_name) == 0){
            ns->net_id = i;
            break;
        }
    }

    // TODO: parameterize scheduler type
    ns->sched = malloc(sizeof(model_net_sched));
    model_net_sched_init(mn_sched_type, method_array[ns->net_id], ns->sched);

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
    assert(m->magic == model_net_base_magic);
    
    switch (m->event_type){
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
    assert(m->magic == model_net_base_magic);
    
    switch (m->event_type){
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
    model_net_request *r = &m->msg.m_base.u.req;
    void * m_data = m+1;
    void *remote = NULL, *local = NULL;
    if (r->remote_event_size > 0){
        remote = m_data;
        m_data = (char*)m_data + r->remote_event_size;
    }
    if (r->self_event_size > 0){
        local = m_data;
    }
    
    model_net_sched_add(r, r->remote_event_size, remote, r->self_event_size,
            local, ns->sched, &m->msg.m_base.rc, lp);
    
    if (ns->in_sched_loop == 0){
        b->c0 = 1;
        tw_event *e = codes_event_new(lp->gid, codes_local_latency(lp), lp);
        model_net_wrap_msg *m = tw_event_data(e);
        m->event_type = MN_BASE_SCHED_NEXT;
        m->magic = model_net_base_magic;
        // m_base not used in sched event
        tw_event_send(e);
        ns->in_sched_loop = 1;
    }
}

void handle_new_msg_rc(
        model_net_base_state *ns,
        tw_bf *b,
        model_net_wrap_msg *m,
        tw_lp *lp){
    model_net_sched_add_rc(ns->sched, &m->msg.m_base.rc, lp);
    if (b->c0){
        codes_local_latency_reverse(lp);
        ns->in_sched_loop = 0;
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
    int ret = model_net_sched_next(&poffset, ns->sched, m+1, 
            &m->msg.m_base.rc, lp);
    // we only need to know whether scheduling is finished or not - if not,
    // go to the 'next iteration' of the loop
    if (ret == -1){
        b->c0 = 1;
        ns->in_sched_loop = 0;
    }
    else {
        tw_event *e = codes_event_new(lp->gid, 
                poffset+codes_local_latency(lp), lp);
        model_net_wrap_msg *m = tw_event_data(e);
        m->event_type = MN_BASE_SCHED_NEXT;
        m->magic = model_net_base_magic;
        // no need to set m_base here
        tw_event_send(e);
    }
}

void handle_sched_next_rc(
        model_net_base_state * ns,
        tw_bf *b,
        model_net_wrap_msg * m,
        tw_lp * lp){
    model_net_sched_next_rc(ns->sched, m+1, &m->msg.m_base.rc, lp);

    if (b->c0){
        ns->in_sched_loop = 1;
    }
    else{
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
    m_wrap->event_type = MN_BASE_PASS;
    m_wrap->magic = model_net_base_magic;
    *msg_data = ((char*)m_wrap)+msg_offsets[net_id];
    // extra_data is optional
    if (extra_data != NULL){
        *extra_data = m_wrap + 1;
    }
    return e;
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
