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
int mn_sample_enabled = 0;

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

static tw_stime mn_sample_interval = 0.0;
static tw_stime mn_sample_end = 0.0;

typedef struct model_net_base_state {
    int net_id;
    // whether scheduler loop is running
    int in_sched_send_loop, in_sched_recv_loop;
    // unique message id counter. This doesn't get decremented on RC to prevent
    // optimistic orderings using "stale" ids
    uint64_t msg_id;
    // model-net schedulers
    model_net_sched *sched_send, *sched_recv;
    // parameters
    const model_net_base_params * params;
    // lp type and state of underlying model net method - cache here so we
    // don't have to constantly look up
    const tw_lptype *sub_type;
    const st_model_types *sub_model_type;
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
    (commit_f) NULL,
    (final_f)  model_net_base_finalize,
    (map_f) codes_mapping,
    sizeof(model_net_base_state),
};

/* setup for the ROSS event tracing
 * can have a different function for  rbev_trace_f and ev_trace_f
 * but right now it is set to the same function for both
 */
void mn_event_collect(model_net_wrap_msg *m, tw_lp *lp, char *buffer, int *collect_flag)
{
    // assigning large numbers to message types to make it easier to
    // determine which messages are model net base LP msgs
    int type;
    void * sub_msg;
    switch (m->h.event_type){
        case MN_BASE_NEW_MSG:
            type = 9000;
            memcpy(buffer, &type, sizeof(type));
            break;
        case MN_BASE_SCHED_NEXT:
            type = 9001;
            memcpy(buffer, &type, sizeof(type));
            break;
        case MN_BASE_SAMPLE: 
            type = 9002;
            memcpy(buffer, &type, sizeof(type));
            break;
        case MN_BASE_PASS:
            sub_msg = ((char*)m)+msg_offsets[((model_net_base_state*)lp->cur_state)->net_id];
            if (g_st_ev_trace == RB_TRACE || g_st_ev_trace == COMMIT_TRACE)
                (((model_net_base_state*)lp->cur_state)->sub_model_type->rbev_trace)(sub_msg, lp, buffer, collect_flag);
            else if (g_st_ev_trace == FULL_TRACE)
                (((model_net_base_state*)lp->cur_state)->sub_model_type->ev_trace)(sub_msg, lp, buffer, collect_flag);
            break;
        default:  // this shouldn't happen, but can help detect an issue
            type = 9004;
            break;
    }
}

void mn_model_stat_collect(model_net_base_state *s, tw_lp *lp, char *buffer)
{
    // need to call the model level stats collection fn
    (*s->sub_model_type->model_stat_fn)(s->sub_state, lp, buffer);
    return;
}

st_model_types mn_model_types[MAX_NETS];

st_model_types mn_model_base_type = {
    (rbev_trace_f) mn_event_collect,
     sizeof(int),
     (ev_trace_f) mn_event_collect,
     sizeof(int),
     (model_stat_f) mn_model_stat_collect,
     0
};

/**** END LP, EVENT PROCESSING FUNCTION DECLS ****/

/**** BEGIN IMPLEMENTATIONS ****/

void model_net_enable_sampling(tw_stime interval, tw_stime end)
{
    mn_sample_interval = interval;
    mn_sample_end = end;
    mn_sample_enabled = 1;
}

int model_net_sampling_enabled(void)
{
    return mn_sample_enabled;
}

// schedule sample event - want to be precise, so no noise here
static void issue_sample_event(tw_lp *lp)
{
    if (tw_now(lp) + mn_sample_interval < mn_sample_end + 0.0001) {
        tw_event *e = tw_event_new(lp->gid, mn_sample_interval, lp);
        model_net_wrap_msg *m = tw_event_data(e);
        msg_set_header(model_net_base_magic, MN_BASE_SAMPLE, lp->gid, &m->h);
        tw_event_send(e);
    }
}

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
            if (g_st_ev_trace || g_st_model_stats) // for ROSS event tracing
            {
                memcpy(&mn_model_types[i], &mn_model_base_type, sizeof(st_model_types));

                if (method_array[i]->mn_model_stat_register == NULL)
                    st_model_type_register(model_net_lp_config_names[i], &mn_model_types[i]);
                else
                    method_array[i]->mn_model_stat_register(&mn_model_types[i]);
            }
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
                "size to %llu\n", LLU(packet_size));
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
    // note: dragonfly router uses the same event struct
    msg_offsets[DRAGONFLY_ROUTER] =
        offsetof(model_net_wrap_msg, msg.m_dfly);
    msg_offsets[DRAGONFLY_CUSTOM] =
        offsetof(model_net_wrap_msg, msg.m_custom_dfly);
    msg_offsets[DRAGONFLY_CUSTOM_ROUTER] =
        offsetof(model_net_wrap_msg, msg.m_custom_dfly);
    msg_offsets[SLIMFLY] =
        offsetof(model_net_wrap_msg, msg.m_slim);
    msg_offsets[FATTREE] =
	offsetof(model_net_wrap_msg, msg.m_fat);
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
        if (strncmp("modelnet_", amap->lp_name.ptr, 9) == 0){
            for (int n = 0; n < amap->num_annos; n++){
                int a;
                for (a = 0; a < num_params; a++){
                    if (annos[a] != NULL && amap->annotations[n].ptr != NULL &&
                            strcmp(amap->annotations[n].ptr, annos[a]) == 0){
                        break;
                    }
                }
                if (a == num_params){
                    // found a new annotation
                    annos[num_params++] = amap->annotations[n].ptr;
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

    ns->msg_id = 0;

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

    /* some ROSS instrumentation setup */
    if (g_st_ev_trace || g_st_model_stats)
    {
        ns->sub_model_type = model_net_get_model_stat_type(ns->net_id);
        mn_model_types[ns->net_id].mstat_sz = ns->sub_model_type->mstat_sz;
    }

    // NOTE: some models actually expect LP state to be 0 initialized...
    // *cough anything that uses mn_stats_array cough*
    ns->sub_state = calloc(1, ns->sub_type->state_sz);

    // initialize the model-net method
    ns->sub_type->init(ns->sub_state, lp);

    // check validity of sampling function
    event_f  sample  = method_array[ns->net_id]->mn_sample_fn;
    revent_f rsample = method_array[ns->net_id]->mn_sample_rc_fn;
    if (model_net_sampling_enabled()) {
        if (sample == NULL) {
            /* MM: Commented out temporarily--- */
            //tw_error(TW_LOC,
            //        "Sampling requested for a model that doesn't provide it\n");
        }
        else if (rsample == NULL &&
                (g_tw_synchronization_protocol == OPTIMISTIC ||
                 g_tw_synchronization_protocol == OPTIMISTIC_DEBUG)) {
            /* MM: Commented out temporarily--- */
            //tw_error(TW_LOC,
            //        "Sampling requested for a model that doesn't provide it\n");
        }
        else {
            init_f sinit = method_array[ns->net_id]->mn_sample_init_fn;
            if (sinit != NULL)
                sinit(ns->sub_state, lp);
            issue_sample_event(lp);
        }
    }
}

void model_net_base_event(
        model_net_base_state * ns,
        tw_bf * b,
        model_net_wrap_msg * m,
        tw_lp * lp){

    if(m->h.magic != model_net_base_magic)
        printf("\n LP ID mismatched %llu ", lp->gid);

    assert(m->h.magic == model_net_base_magic);

    void * sub_msg;
    switch (m->h.event_type){
        case MN_BASE_NEW_MSG:
            handle_new_msg(ns, b, m, lp);
            break;
        case MN_BASE_SCHED_NEXT:
            handle_sched_next(ns, b, m, lp);
            break;
        case MN_BASE_SAMPLE: ;
            event_f sample = method_array[ns->net_id]->mn_sample_fn;
            assert(model_net_sampling_enabled() && sample != NULL);
            sub_msg = ((char*)m)+msg_offsets[ns->net_id];
            sample(ns->sub_state, b, sub_msg, lp);
            issue_sample_event(lp);
            break;
        case MN_BASE_PASS: ;
            sub_msg = ((char*)m)+msg_offsets[ns->net_id];
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

    void * sub_msg;
    switch (m->h.event_type){
        case MN_BASE_NEW_MSG:
            handle_new_msg_rc(ns, b, m, lp);
            break;
        case MN_BASE_SCHED_NEXT:
            handle_sched_next_rc(ns, b, m, lp);
            break;
        case MN_BASE_SAMPLE: ;
            revent_f sample_rc = method_array[ns->net_id]->mn_sample_rc_fn;
            assert(model_net_sampling_enabled() && sample_rc != NULL);
            sub_msg = ((char*)m)+msg_offsets[ns->net_id];
            sample_rc(ns->sub_state, b, sub_msg, lp);
            break;
        case MN_BASE_PASS: ;
            sub_msg = ((char*)m)+msg_offsets[ns->net_id];
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
    final_f sfini = method_array[ns->net_id]->mn_sample_fini_fn;
    if (sfini != NULL)
        sfini(ns->sub_state, lp);
    ns->sub_type->final(ns->sub_state, lp);
    free(ns->sub_state);
}

/// bitfields used:
/// c31 - we initiated a sched_next event
void handle_new_msg(
        model_net_base_state * ns,
        tw_bf *b,
        model_net_wrap_msg * m,
        tw_lp * lp){
    // simply pass down to the scheduler
    model_net_request *r = &m->msg.m_base.req;
    // don't forget to set packet size, now that we're responsible for it!
    r->packet_size = ns->params->packet_size;
    r->msg_id = ns->msg_id++;
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
        b->c31 = 1;
        /* No need to issue an extra sched-next event if we're currently idle */
        *in_sched_loop = 1;
        /* NOTE: we can do this because the sched rc struct in the event is
         * *very* lightly used (there's harmless overlap in usage for the
         * priority scheduler) */
        handle_sched_next(ns, b, m, lp);
        assert(*in_sched_loop); // we shouldn't have fallen out of the loop
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

    if (b->c31) {
        handle_sched_next_rc(ns, b, m, lp);
        *in_sched_loop = 0;
    }
    model_net_sched_add_rc(ss, &m->msg.m_base.rc, lp);
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
        tw_event *e = tw_event_new(lp->gid,
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

    model_net_request *r = &m->msg.m_base.req;
    r->final_dest_lp = final_dest_lp;
    r->src_lp = src_lp;
    // for "recv" events, set the "dest" to this LP in the case of a pull event
    r->dest_mn_lp = sender->gid;
    r->pull_size = pull_size;
    r->msg_size = msg_size;
    // TODO: document why we're setting packet_size this way
    r->packet_size = msg_size;
    r->net_id = net_id;
    r->is_pull = is_pull;
    r->remote_event_size = remote_event_size;
    r->self_event_size = 0;
    m->msg.m_base.is_from_remote = 1;

    strncpy(r->category, category, CATEGORY_NAME_MAX-1);
    r->category[CATEGORY_NAME_MAX-1] = '\0';

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
