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

#define DEBUG 0
#define MODELNET_LP_DEBUG 1
/**** BEGIN SIMULATION DATA STRUCTURES ****/

int model_net_base_magic;
int mn_sample_enabled = 0;

static int is_freezing_on = false;

// message-type specific offsets - don't want to get bitten later by alignment
// issues...
static int msg_offsets[MAX_NETS];

typedef struct model_net_base_params_s {
    model_net_sched_cfg_params sched_params;
    uint64_t packet_size;
    int num_queues;
    int use_recv_queue;
    tw_stime nic_seq_delay;
    int node_copy_queues;
} model_net_base_params;

/* annotation-specific parameters (unannotated entry occurs at the
 * last index) */
static int                       num_params = 0;
static const char              * annos[CONFIGURATION_MAX_ANNOS];
static model_net_base_params     all_params[CONFIGURATION_MAX_ANNOS];

static tw_stime mn_sample_interval = 0.0;
static tw_stime mn_sample_end = 0.0;
static int servers_per_node_queue = -1;
extern tw_stime codes_cn_delay;

typedef struct model_net_base_state {
#if MODELNET_LP_DEBUG
	size_t num_events_processed;
#endif /* if MODELNET_LP_DEBUG */
    int net_id, nics_per_router;
    // whether scheduler loop is running
    int *in_sched_send_loop, in_sched_recv_loop;
    // unique message id counter. This doesn't get decremented on RC to prevent
    // optimistic orderings using "stale" ids
    uint64_t msg_id;
    // model-net schedulers
    model_net_sched **sched_send, *sched_recv;
    // parameters
    const model_net_base_params * params;
    // lp type and state of underlying model net method - cache here so we
    // don't have to constantly look up
    const tw_lptype *sub_type;
    const st_model_types *sub_model_type;
    void *sub_state;
    tw_stime next_available_time;
    tw_stime *node_copy_next_available_time;
    // Copy of in_sched_send_loop before switching to surrogate mode
    int * sched_loop_pre_surrogate, sched_recv_loop_pre_surrogate;
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
static void model_net_commit_event(
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
    (commit_f) model_net_commit_event,
    (final_f)  model_net_base_finalize,
    (map_f) codes_mapping,
    sizeof(model_net_base_state),
};

// Functionality to check for correct implementation of reverse event handler
static void save_state_net_state(model_net_base_state * into, model_net_base_state const * from);
static void clean_state_net_state(model_net_base_state * state);
static bool check_model_net_state(model_net_base_state * before, model_net_base_state * after);
static void print_model_net_state(FILE * out, model_net_base_state * state);
static void print_model_net_checkpoint(FILE * out, model_net_base_state * state);
static void print_event_state(FILE * out, model_net_wrap_msg * state);

// ROSS function pointer table to check reverse event handler
crv_checkpointer model_net_chkptr = {
    &model_net_base_lp,
    0,
    (save_checkpoint_state_f) save_state_net_state,
    (clean_checkpoint_state_f) clean_state_net_state,
    (check_states_f) check_model_net_state,
    (print_lpstate_f) print_model_net_state,
    (print_checkpoint_state_f) print_model_net_checkpoint,
    (print_event_f) print_event_state,
};

static void model_net_commit_event(model_net_base_state * ns, tw_bf *b,  model_net_wrap_msg * m, tw_lp * lp)
{
    if(m->h.event_type == MN_BASE_PASS)
    {
        void * sub_msg;
        sub_msg = ((char*)m)+msg_offsets[ns->net_id];

        if(ns->sub_type->commit != NULL)
            ns->sub_type->commit(ns->sub_state, b, sub_msg, lp);
    }

    if(m->h.event_type == MN_CONGESTION_EVENT)
    {
        void * sub_msg;
        sub_msg = ((char*)m)+msg_offsets[CONGESTION_CONTROLLER];
        commit_f con_ev_commit = method_array[ns->net_id]->cc_congestion_event_commit_fn;
        if(con_ev_commit != NULL)
            con_ev_commit(ns->sub_state, b, sub_msg, lp);
    }
}
/* setup for the ROSS event tracing
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
            if (((model_net_base_state*)lp->cur_state)->sub_model_type)
            {
                if (g_st_ev_trace)
                    (((model_net_base_state*)lp->cur_state)->sub_model_type->ev_trace)(sub_msg, lp, buffer, collect_flag);
            }
            break;
        default:  // this shouldn't happen, but can help detect an issue
            type = 9004;
            break;
    }
}

void mn_model_stat_collect(model_net_base_state *s, tw_lp *lp, char *buffer)
{
    // need to call the model level stats collection fn
    if (s->sub_model_type)
        (*s->sub_model_type->model_stat_fn)(s->sub_state, lp, buffer);
    return;
}

void mn_sample_event(model_net_base_state *s, tw_bf * bf, tw_lp * lp, void *sample)
{
    if (s->sub_model_type)
        (*s->sub_model_type->sample_event_fn)(s->sub_state, bf, lp, sample);
}

void mn_sample_rc_event(model_net_base_state *s, tw_bf * bf, tw_lp * lp, void *sample)
{
    if (s->sub_model_type)
        (*s->sub_model_type->sample_revent_fn)(s->sub_state, bf, lp, sample);
}

st_model_types mn_model_types[MAX_NETS];

st_model_types mn_model_base_type = {
     (ev_trace_f) mn_event_collect,
     sizeof(int),
     (model_stat_f) mn_model_stat_collect,
     0,
     (sample_event_f) mn_sample_event,
     (sample_revent_f) mn_sample_rc_event,
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
            if (g_st_ev_trace || g_st_model_stats || g_st_use_analysis_lps) // for ROSS event tracing
            {
                if (method_array[i]->mn_model_stat_register != NULL)
                //    st_model_type_register(model_net_lp_config_names[i], &mn_model_types[i]);
                //else
                {
                    memcpy(&mn_model_types[i], &mn_model_base_type, sizeof(st_model_types));
                    method_array[i]->mn_model_stat_register(&mn_model_types[i]);
                }
            }
        }
    }
    crv_add_custom_state_checkpoint(&model_net_chkptr);
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

    p->num_queues = 1;
    ret = configuration_get_value_int(&config, "PARAMS", "num_injection_queues", anno,
            &p->num_queues);
    if(ret && !g_tw_mynode) {
        fprintf(stdout, "NIC num injection port not specified, "
                "setting to %d\n", p->num_queues);
    }

    p->nic_seq_delay = 10;
    ret = configuration_get_value_double(&config, "PARAMS", "nic_seq_delay", anno,
            &p->nic_seq_delay);
    if(ret && !g_tw_mynode) {
        fprintf(stdout, "NIC seq delay not specified, "
                "setting to %lf\n", p->nic_seq_delay);
    }

    p->node_copy_queues = 1;
    ret = configuration_get_value_int(&config, "PARAMS", "node_copy_queues", anno,
            &p->node_copy_queues);
    if(ret && !g_tw_mynode) {
        fprintf(stdout, "NIC num copy queues not specified, "
                "setting to %d\n", p->node_copy_queues);
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
        if (ret <= 0)
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
    msg_offsets[DRAGONFLY_PLUS] =
        offsetof(model_net_wrap_msg, msg.m_dfly_plus);
    msg_offsets[DRAGONFLY_PLUS_ROUTER] =
        offsetof(model_net_wrap_msg, msg.m_dfly_plus);
    msg_offsets[DRAGONFLY_DALLY] =
        offsetof(model_net_wrap_msg, msg.m_dally_dfly);
    msg_offsets[DRAGONFLY_DALLY_ROUTER] =
        offsetof(model_net_wrap_msg, msg.m_dally_dfly);
    msg_offsets[SLIMFLY] =
        offsetof(model_net_wrap_msg, msg.m_slim);
    msg_offsets[SLIMFLY_ROUTER] =
        offsetof(model_net_wrap_msg, msg.m_slim);
    msg_offsets[FATTREE] =
	    offsetof(model_net_wrap_msg, msg.m_fat);
    msg_offsets[LOGGP] =
        offsetof(model_net_wrap_msg, msg.m_loggp);
    msg_offsets[EXPRESS_MESH] =
        offsetof(model_net_wrap_msg, msg.m_em);
    msg_offsets[EXPRESS_MESH_ROUTER] =
        offsetof(model_net_wrap_msg, msg.m_em);
    msg_offsets[CONGESTION_CONTROLLER] =
        offsetof(model_net_wrap_msg, msg.m_cc);


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
    char lp_type_name[MAX_NAME_LENGTH], anno[MAX_NAME_LENGTH], group[MAX_NAME_LENGTH];
    int dummy;

    codes_mapping_get_lp_info(lp->gid, group, &dummy,
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

    ns->nics_per_router = codes_mapping_get_lp_count(group, 1,
            lp_type_name, NULL, 1);

    ns->msg_id = 0;
    ns->next_available_time = 0;
    ns->node_copy_next_available_time = (tw_stime*)malloc(ns->params->node_copy_queues * sizeof(tw_stime));
    for(int i = 0; i < ns->params->node_copy_queues; i++) {
        ns->node_copy_next_available_time[i] = 0;
    }

    ns->in_sched_send_loop = (int *)malloc(ns->params->num_queues * sizeof(int));
    ns->sched_loop_pre_surrogate = (int *)malloc(ns->params->num_queues * sizeof(int));
    ns->sched_send = (model_net_sched**)malloc(ns->params->num_queues * sizeof(model_net_sched*));
    for(int i = 0; i < ns->params->num_queues; i++) {
        ns->sched_send[i] = (model_net_sched*)malloc(sizeof(model_net_sched));
        model_net_sched_init(&ns->params->sched_params, 0, method_array[ns->net_id],
                ns->sched_send[i]);
        ns->in_sched_send_loop[i] = 0;
    }
    ns->sched_recv = malloc(sizeof(model_net_sched));
    model_net_sched_init(&ns->params->sched_params, 1, method_array[ns->net_id],
            ns->sched_recv);

    ns->sub_type = model_net_get_lp_type(ns->net_id);

    /* some ROSS instrumentation setup */
    if (g_st_ev_trace || g_st_model_stats || g_st_use_analysis_lps)
    {
        ns->sub_model_type = model_net_get_model_stat_type(ns->net_id);
        if (ns->sub_model_type)
        {
            mn_model_types[ns->net_id].mstat_sz = ns->sub_model_type->mstat_sz;
            mn_model_types[ns->net_id].sample_struct_sz = ns->sub_model_type->sample_struct_sz;
        }
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
    memset(b, 0, sizeof(tw_bf));
#if MODELNET_LP_DEBUG
    ns->num_events_processed++;
#endif /* if MODELNET_LP_DEBUG */

    if(m->h.magic != model_net_base_magic)
        printf("\n LP ID mismatched %llu\n", LLU(lp->gid));

    assert(m->h.magic == model_net_base_magic);

    if(!is_freezing_on && m->h.event_type == MN_BASE_SCHED_NEXT && m->msg.m_base.created_in_surrogate) {
        return;
    }

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
        case MN_BASE_END_NOTIF: ;
            event_f end_ev = method_array[ns->net_id]->mn_end_notif_fn;
            end_ev(ns->sub_state, b, m, lp);
            break;
        case MN_CONGESTION_EVENT: ;
            event_f con_ev = method_array[ns->net_id]->cc_congestion_event_fn;
            assert(g_congestion_control_enabled && con_ev != NULL);
            sub_msg = ((char*)m)+msg_offsets[CONGESTION_CONTROLLER];
            con_ev(ns->sub_state, b, sub_msg, lp);
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
#if MODELNET_LP_DEBUG
    ns->num_events_processed--;
#endif /* if MODELNET_LP_DEBUG */

    if(!is_freezing_on && m->h.event_type == MN_BASE_SCHED_NEXT && m->msg.m_base.created_in_surrogate) {
        return;
    }

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
        case MN_BASE_END_NOTIF: ;
            event_f end_ev_rc = method_array[ns->net_id]->mn_end_notif_rc_fn;
            end_ev_rc(ns->sub_state, b, m, lp);
            break;
        case MN_CONGESTION_EVENT: ;
            revent_f con_ev_rc = method_array[ns->net_id]->cc_congestion_event_rc_fn;
            assert(g_congestion_control_enabled && con_ev_rc != NULL);
            sub_msg = ((char*)m)+msg_offsets[CONGESTION_CONTROLLER];
            con_ev_rc(ns->sub_state, b, sub_msg, lp);
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
#if DEBUG
    printf("%llu Entered handle_new_msg()\n",LLU(tw_now(lp)));
#endif
    static int num_servers = -1;
    static int servers_per_node = -1;
    if(num_servers == -1) {
        char const *sender_group;
        char const *sender_lpname;
        int rep_id, offset;
        model_net_request *r = &m->msg.m_base.req;
        codes_mapping_get_lp_info2(r->src_lp, &sender_group, &sender_lpname,
                NULL, &rep_id, &offset);
        num_servers = codes_mapping_get_lp_count(sender_group, 1,
                sender_lpname, NULL, 1);
        servers_per_node = num_servers/ns->params->num_queues; //this is for entire switch
        if(servers_per_node == 0) servers_per_node = 1;
        servers_per_node_queue = num_servers/ns->nics_per_router/ns->params->node_copy_queues;
        if(servers_per_node_queue == 0) servers_per_node_queue = 1;
        if(!g_tw_mynode) {
            fprintf(stdout, "Set num_servers per router %d, servers per "
                "injection queue per router %d, servers per node copy queue "
                "per node %d, num nics %d\n", num_servers, servers_per_node,
                servers_per_node_queue, ns->nics_per_router);
        }
    }

    if(lp->gid == m->msg.m_base.req.dest_mn_lp) {
        model_net_request *r = &m->msg.m_base.req;
        int rep_id, offset;
        codes_mapping_get_lp_info2(r->src_lp, NULL, NULL, NULL, &rep_id, &offset);
        int queue = offset/ns->nics_per_router/servers_per_node_queue;
        m->msg.m_base.save_ts = ns->node_copy_next_available_time[queue];
        tw_stime exp_time = ((ns->node_copy_next_available_time[queue]
                            > tw_now(lp)) ? ns->node_copy_next_available_time[queue] : tw_now(lp));
        exp_time += r->msg_size * codes_cn_delay;
        exp_time -= tw_now(lp);
        tw_stime delay = codes_local_latency(lp);
        ns->node_copy_next_available_time[queue] = tw_now(lp) + exp_time;
        // ns->node_copy_next_available_time[queue] = exp_time;
        int remote_event_size = r->remote_event_size;
        int self_event_size = r->self_event_size;
        void *e_msg = (m+1);
        if (remote_event_size > 0) {
            exp_time += delay;
            tw_event *e = tw_event_new(r->final_dest_lp, exp_time, lp);
            memcpy(tw_event_data(e), e_msg, remote_event_size);
            tw_event_send(e);
            e_msg = (char*)e_msg + remote_event_size;
        }
        if (self_event_size > 0) {
            exp_time += delay;
            tw_event *e = tw_event_new(r->src_lp, exp_time, lp);
            memcpy(tw_event_data(e), e_msg, self_event_size);
            tw_event_send(e);
        }
        return;
    }

    if(m->msg.m_base.isQueueReq) {
        m->msg.m_base.save_ts = ns->next_available_time;
        tw_stime exp_time = ((ns->next_available_time > tw_now(lp)) ? ns->next_available_time : tw_now(lp));
        exp_time += ns->params->nic_seq_delay + codes_local_latency(lp);
        ns->next_available_time = exp_time;
        tw_event *e = tw_event_new(lp->gid, exp_time - tw_now(lp), lp);
        model_net_wrap_msg *m_new = tw_event_data(e);
        memcpy(m_new, m, sizeof(model_net_wrap_msg));
        void *e_msg = (m+1);
        void *e_new_msg = (m_new+1);
        model_net_request *r = &m->msg.m_base.req;
        int remote_event_size = r->remote_event_size;
        int self_event_size = r->self_event_size;
        if (remote_event_size > 0){
            memcpy(e_new_msg, e_msg, remote_event_size);
            e_msg = (char*)e_msg + remote_event_size;
            e_new_msg = (char*)e_new_msg + remote_event_size;
        }
        if (self_event_size > 0){
            memcpy(e_new_msg, e_msg, self_event_size);
        }
        m_new->msg.m_base.isQueueReq = 0;
        tw_event_send(e);
#if DEBUG
        printf("%llu isQueueReq and dropping outof handle_new_msg(\n",LLU(tw_now(lp)));
#endif
        return;
    }
    // simply pass down to the scheduler
    model_net_request *r = &m->msg.m_base.req;
    // don't forget to set packet size, now that we're responsible for it!
    r->msg_new_mn_event = tw_now(lp);
    r->packet_size = ns->params->packet_size;
    b->c30 = 1;
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

    int queue_offset = 0;
    if(!m->msg.m_base.is_from_remote && ns->params->num_queues != 1) {
        int rep_id, offset;
        if(num_servers == -1) {
            char const *sender_group;
            char const *sender_lpname;
            codes_mapping_get_lp_info2(r->src_lp, &sender_group, &sender_lpname,
                    NULL, &rep_id, &offset);
            num_servers = codes_mapping_get_lp_count(sender_group, 1,
                    sender_lpname, NULL, 1);
            servers_per_node = num_servers/ns->params->num_queues;
            if(servers_per_node == 0) servers_per_node = 1;
        } else {
            codes_mapping_get_lp_info2(r->src_lp, NULL, NULL, NULL, &rep_id, &offset);
        }
#if DEBUG
        printf("r->src_lp:%llu, num_servers:%d num_queues:%d, offset:%d servers_per_node:%d\n",LLU(r->src_lp), num_servers, ns->params->num_queues, offset, servers_per_node);
#endif
        queue_offset = (offset/servers_per_node) % ns->params->num_queues;
    }
    r->queue_offset = queue_offset;
#if DEBUG
    printf("queue_offset:%d\n",queue_offset);
#endif
    //printf("num_queues:%d q0_loop:%d q1_loop:%d\n",ns->params->num_queues,ns->in_sched_send_loop[0], ns->in_sched_send_loop[1]);

    //for(int j=0; j<ns->params->num_queues; j++){
    //    queue_offset = j;
    //    r->queue_offset = j;
    // set message-specific params
    int is_from_remote = m->msg.m_base.is_from_remote;
    model_net_sched *ss = is_from_remote ? ns->sched_recv : ns->sched_send[queue_offset];
    int *in_sched_loop = is_from_remote  ?
        &ns->in_sched_recv_loop : &ns->in_sched_send_loop[queue_offset];
    model_net_sched_add(r, &m->msg.m_base.sched_params, r->remote_event_size,
            remote, r->self_event_size, local, ss, &m->msg.m_base.rc, lp);

    if (*in_sched_loop == 0){
        b->c31 = 1;
        /* No need to issue an extra sched-next event if we're currently idle */
        *in_sched_loop = 1;
        /* NOTE: we can do this because the sched rc struct in the event is
         * *very* lightly used (there's harmless overlap in usage for the
         * priority scheduler) */
#if DEBUG
        printf("%llu handle_shed_next() from handle_new_msg()\n",LLU(tw_now(lp)));
#endif
        handle_sched_next(ns, b, m, lp);
        assert(*in_sched_loop);
    }
}

void handle_new_msg_rc(
        model_net_base_state *ns,
        tw_bf *b,
        model_net_wrap_msg *m,
        tw_lp *lp){
    if(lp->gid == m->msg.m_base.req.dest_mn_lp) {
        codes_local_latency_reverse(lp);
        model_net_request *r = &m->msg.m_base.req;
        int rep_id, offset;
        codes_mapping_get_lp_info2(r->src_lp, NULL, NULL, NULL, &rep_id, &offset);
        int queue = offset/ns->nics_per_router/servers_per_node_queue;
        ns->node_copy_next_available_time[queue] = m->msg.m_base.save_ts;
        return;
    }
    if(m->msg.m_base.isQueueReq) {
        codes_local_latency_reverse(lp);
        ns->next_available_time = m->msg.m_base.save_ts;
        return;
    }
    model_net_request *r = &m->msg.m_base.req;
    int is_from_remote = m->msg.m_base.is_from_remote;
    model_net_sched *ss = is_from_remote ? ns->sched_recv : ns->sched_send[r->queue_offset];
    int *in_sched_loop = is_from_remote  ?
        &ns->in_sched_recv_loop : &ns->in_sched_send_loop[r->queue_offset];

    if (b->c31) {
        handle_sched_next_rc(ns, b, m, lp);
        *in_sched_loop = 0;
    }
    model_net_sched_add_rc(ss, &m->msg.m_base.rc, lp);

    if (b->c30) {
        ns->msg_id--;
    }
}

/// bitfields used
/// c0 - scheduler loop is finished
void handle_sched_next(
        model_net_base_state * ns,
        tw_bf *b,
        model_net_wrap_msg * m,
        tw_lp * lp){
#if DEBUG
    printf("%llu handle sched_next function\n",LLU(tw_now(lp)));
#endif
    tw_stime poffset;
    model_net_request *r = &m->msg.m_base.req;
    int is_from_remote = m->msg.m_base.is_from_remote;
    model_net_sched * ss = is_from_remote ? ns->sched_recv : ns->sched_send[r->queue_offset];
    int *in_sched_loop = is_from_remote ?
        &ns->in_sched_recv_loop : &ns->in_sched_send_loop[r->queue_offset];
    int ret = model_net_sched_next(&poffset, ss, m+1, &m->msg.m_base.rc, lp);
    // we only need to know whether scheduling is finished or not - if not,
    // go to the 'next iteration' of the loop
#if DEBUG
    printf("return value from model_net_sched_next(): %d in_sched_loop changing from %d to 0\n",ret,*in_sched_loop);
#endif
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
        model_net_request *r_wrap = &m_wrap->msg.m_base.req;
        msg_set_header(model_net_base_magic, MN_BASE_SCHED_NEXT, lp->gid,
                &m_wrap->h);
        m_wrap->msg.m_base.is_from_remote = is_from_remote;
        r_wrap->queue_offset = r->queue_offset;
        // no need to set m_base here
        tw_event_send(e);
    }
}

void handle_sched_next_rc(
        model_net_base_state * ns,
        tw_bf *b,
        model_net_wrap_msg * m,
        tw_lp * lp){
    model_net_request *r = &m->msg.m_base.req;
    int is_from_remote = m->msg.m_base.is_from_remote;
    model_net_sched * ss = is_from_remote ? ns->sched_recv : ns->sched_send[r->queue_offset];
    int *in_sched_loop = is_from_remote ?
        &ns->in_sched_recv_loop : &ns->in_sched_send_loop[r->queue_offset];

    model_net_sched_next_rc(ss, m+1, &m->msg.m_base.rc, lp);
    if (b->c0){
        *in_sched_loop = 1;
    }
    else if (ns->net_id == SIMPLEP2P || ns->net_id == TORUS){
        codes_local_latency_reverse(lp);
    }
}

/**** END IMPLEMENTATIONS ****/

tw_event * model_net_method_event_new_user_prio(
        tw_lpid dest_gid,
        tw_stime offset_ts,
        tw_lp *sender,
        int net_id,
        void **msg_data,
        void **extra_data,
        tw_stime prio){
    tw_event *e = tw_event_new_user_prio(dest_gid, offset_ts, sender, prio);
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
    model_net_method_idle_event2(offset_ts, is_recv_queue, 0, lp);
}

void model_net_method_idle_event2(tw_stime offset_ts, int is_recv_queue,
        int queue_offset, tw_lp * lp){
    tw_event *e = tw_event_new(lp->gid, offset_ts, lp);
    model_net_wrap_msg *m_wrap = tw_event_data(e);
    model_net_request *r_wrap = &m_wrap->msg.m_base.req;
#if DEBUG
    printf("%llu handle_sched_next() from model_net_method_idle_event2()\n",LLU(tw_now(lp)));
#endif
    msg_set_header(model_net_base_magic, MN_BASE_SCHED_NEXT, lp->gid,
            &m_wrap->h);
    m_wrap->msg.m_base.is_from_remote = is_recv_queue;
    r_wrap->queue_offset = queue_offset;
    m_wrap->msg.m_base.created_in_surrogate = is_freezing_on;
    tw_event_send(e);
}

void * model_net_method_get_edata(int net_id, void *msg){
    return (char*)msg + sizeof(model_net_wrap_msg) - msg_offsets[net_id];
}

// This is utilized by a workoad file, when a single workload rank has determined that
// all of the workload ranks have finished their respective tasks, it uses this method
// to send a notification to all model net LPs that the workloads have completed.
// This enables network models to implement features that require a heartbeat message
// that's repeated ad infinitum. If those ranks aren't notified that the workload
// ranks are completed, then the simulation will 'never' end. (it will run until g_tw_ts_end is reached)
// note: network models must explicitly implement and initialize the necessary model-net-method callback
//return number of random calls utilized by this method
int model_net_method_end_sim_broadcast(
    tw_stime offset_ts,
    tw_lp *sender)
{
    //get lp names of active model-net LPs
    //send end sim notification to each LP that match an active model-net lp name
    for (int cid = 0; cid < lpconf.num_uniq_lptypes; cid++)
    {
        const char* lp_name = codes_mapping_get_lp_name_by_cid(cid);
        for (int n = 0; n < MAX_NETS; n++)
        {
            if (strcmp(lp_name, model_net_lp_config_names[n]) == 0)
            {
                // printf("ACTIVE TYPE FOUND: %s\n", lp_name);
                for (int lid = 0; lid < codes_mapping_get_lp_count("MODELNET_GRP", 0, lp_name, NULL, 1); lid++)
                {
                    tw_lpid lpgid = codes_mapping_get_lpid_from_relative(lid, NULL, lp_name, NULL, 0);
                    tw_event* e = model_net_method_end_sim_notification(lpgid, offset_ts, sender);
                    tw_event_send(e);
                }
            }
        }
    }
    return 0;
}

// see model_net_method_end_sim_broadcast for details on why this method is useful
tw_event* model_net_method_end_sim_notification(
    tw_lpid dest_gid,
    tw_stime offset_ts,
    tw_lp *sender)
{
    tw_event *e = tw_event_new(dest_gid, offset_ts, sender);
    model_net_wrap_msg *m_wrap = tw_event_data(e);
    msg_set_header(model_net_base_magic, MN_BASE_END_NOTIF, sender->gid,
            &m_wrap->h);
    return e;
}

tw_event* model_net_method_congestion_event(tw_lpid dest_gid,
    tw_stime offset_ts,
    tw_lp *sender,
    void **msg_data,
    void **extra_data)
{
    tw_event *e = tw_event_new(dest_gid, offset_ts, sender);
    model_net_wrap_msg *m_wrap = tw_event_data(e);
    msg_set_header(model_net_base_magic, MN_CONGESTION_EVENT, sender->gid,
            &m_wrap->h);
    *msg_data = ((char*)m_wrap)+msg_offsets[CONGESTION_CONTROLLER];
    // extra_data is optional
    if (extra_data != NULL){
        *extra_data = m_wrap + 1;
    }
    return e;

}

/* START Checking reverse handler functionality */
static void save_state_net_state(model_net_base_state * into, model_net_base_state const * from) {
    memcpy(into, from, sizeof(model_net_base_state));

    into->in_sched_send_loop = malloc(from->params->num_queues * sizeof(int));
    for (int i=0; i < from->params->num_queues; i++) {
        into->in_sched_send_loop[i] = from->in_sched_send_loop[i];
    }

    into->sched_send = malloc(from->params->num_queues * sizeof(model_net_sched*));
    if (from->params->num_queues > 0) {
        model_net_sched * sched_send_array = malloc(from->params->num_queues * sizeof(model_net_sched));
        for(int i = 0; i < from->params->num_queues; i++) {
            into->sched_send[i] = &sched_send_array[i];
            save_model_net_sched(into->sched_send[i], from->sched_send[i]);
        }
    }

    into->sched_recv = malloc(sizeof(model_net_sched));
    save_model_net_sched(into->sched_recv, from->sched_recv);

    into->sub_state = NULL;
    crv_checkpointer * chptr = method_array[from->net_id]->checkpointer;
    if (chptr && chptr->check_lps) {
        into->sub_state = calloc(1, from->sub_type->state_sz);
        chptr->save_lp(into->sub_state, from->sub_state);
    }

    into->node_copy_next_available_time = malloc(from->params->node_copy_queues * sizeof(tw_stime));
    for (int i=0; i < from->params->node_copy_queues; i++) {
        into->node_copy_next_available_time[i] = from->node_copy_next_available_time[i];
    }
}

static void clean_state_net_state(model_net_base_state * state) {
    free(state->in_sched_send_loop);

    if (state->params->num_queues > 0) {
        for(int i = 0; i < state->params->num_queues; i++) {
            clean_model_net_sched(state->sched_send[i]);
        }
    }
    free(state->sched_send[0]);
    free(state->sched_send);
    clean_model_net_sched(state->sched_recv);
    free(state->sched_recv);

    if (state->sub_state != NULL) {
        free(state->sub_state);
    }
    free(state->node_copy_next_available_time);
}

static bool check_model_net_state(model_net_base_state * before, model_net_base_state * after) {
    bool is_same = true;
    is_same &= before->net_id == after->net_id;
    is_same &= before->nics_per_router == after->nics_per_router;
    for (int i=0; i < before->params->num_queues; i++) {
        is_same &= before->in_sched_send_loop[i] == after->in_sched_send_loop[i];
    }
    is_same &= before->in_sched_recv_loop == after->in_sched_recv_loop;
    is_same &= before->msg_id == after->msg_id;
    for(int i = 0; i < before->params->num_queues; i++) {
        is_same &= check_model_net_sched(before->sched_send[i], after->sched_send[i]);
    }
    is_same &= check_model_net_sched(before->sched_recv, after->sched_recv);
    crv_checkpointer * chptr = method_array[before->net_id]->checkpointer;
    if (chptr && before->sub_state != NULL && chptr->check_lps) {
        is_same &= chptr->check_lps(before->sub_state, after->sub_state);
    } else {
        tw_error(TW_LOC, "Network of type \"%s\" has not been configured to be checkpointed", model_net_method_names[before->net_id]);
    }
    is_same &= before->next_available_time == after->next_available_time;
    for (int i=0; i < before->params->node_copy_queues; i++) {
        is_same &= before->node_copy_next_available_time[i] == after->node_copy_next_available_time[i];
    }

    return is_same;
}

static void __print_model_net(FILE * out, model_net_base_state * state, bool is_lp_state) {
    fprintf(out, "model_net_state ->\n");
#if MODELNET_LP_DEBUG
    fprintf(out, "  |num_events_processed = %zu\n", state->num_events_processed);
#endif /* if MODELNET_LP_DEBUG */
    fprintf(out, "  |              net_id = %d\n", state->net_id);
    fprintf(out, "  |     nics_per_router = %d\n", state->nics_per_router);
    fprintf(out, "  | *in_sched_send_loop[%d] = [", state->params->num_queues);  // (done) deep-all
    for (int i=0; i < state->params->num_queues; i++) {
        fprintf(out, "%d%s", state->in_sched_send_loop[i], i==state->params->num_queues-1 ? "" : ", ");
    }
    fprintf(out, "]\n");
    fprintf(out, "  |  in_sched_recv_loop = %d\n", state->in_sched_recv_loop);
    fprintf(out, "  |              msg_id = %lu\n", state->msg_id);
    fprintf(out, "  | **       sched_send = %p\n", state->sched_send);  // (done) deep-all
    fprintf(out, "  | *        sched_recv = %p\n", state->sched_recv);  // (done) deep-all
    fprintf(out, "  | *            params = %p\n", state->params);
    fprintf(out, "  | *          sub_type = %p\n", state->sub_type);
    fprintf(out, "  | *    sub_model_type = %p\n", state->sub_model_type);
    fprintf(out, "  | *         sub_state = %p\n", state->sub_state);  // deep-all
    fprintf(out, "  | next_available_time = %f\n", state->next_available_time);
    fprintf(out, "  | *node_copy_next_available_time[%d] = [", state->params->num_queues);  // (done) deep-all
    for (int i=0; i < state->params->node_copy_queues; i++) {
        fprintf(out, "%g%s", state->node_copy_next_available_time[i], i==state->params->node_copy_queues-1 ? "" : ", ");
    }
    fprintf(out, "]\n");
    fprintf(out, "  | *sched_loop_pre_surrogate = %p\n", state->sched_loop_pre_surrogate);  // no need to check
    fprintf(out, "  | sched_recv_loop_pre_surrogate = %d\n", state->sched_recv_loop_pre_surrogate);  // no need to check

    void (*print_modelnet) (FILE * out, model_net_sched *sched) = is_lp_state ? print_model_net_sched : print_model_net_sched_checkpoint;

    fprintf(out, "\n");
    for(int i = 0; i < state->params->num_queues; i++) {
        fprintf(out, "==== CONTENT for sched_send[%d]:\n", i);
        print_modelnet(stderr, state->sched_send[i]);
    }

    fprintf(out, "\n==== CONTENT for sched_recv:\n");
    print_modelnet(stderr, state->sched_recv);

    crv_checkpointer * chptr = method_array[state->net_id]->checkpointer;
    if (chptr && state->sub_state != NULL) {
        if (is_lp_state && chptr->print_lp) {
            fprintf(out, "\n==== CONTENT for sub_state:\n");
            chptr->print_lp(out, state->sub_state);
        }
        if (!is_lp_state && chptr->print_checkpoint) {
            fprintf(out, "\n==== CONTENT for sub_state:\n");
            chptr->print_checkpoint(out, state->sub_state);
        }
    }
}

static void print_model_net_state(FILE * out, model_net_base_state * state) {
    __print_model_net(out, state, true);
}
static void print_model_net_checkpoint(FILE * out, model_net_base_state * state) {
    __print_model_net(out, state, false);
}

static void print_type(FILE * out, enum model_net_base_event_type type) {
    switch (type) {
        case MN_BASE_NEW_MSG:
            fprintf(out, "MN_BASE_NEW_MSG");
            break;
        case MN_BASE_SCHED_NEXT:
            fprintf(out, "MN_BASE_SCHED_NEXT");
            break;
        case MN_BASE_SAMPLE:
            fprintf(out, "MN_BASE_SAMPLE");
            break;
        case MN_BASE_PASS:
            fprintf(out, "MN_BASE_PASS");
            break;
        case MN_BASE_END_NOTIF:
            fprintf(out, "MN_BASE_END_NOTIF");
            break;
        case MN_CONGESTION_EVENT:
            fprintf(out, "MN_CONGESTION_EVENT");
            break;
    }
}

// Used Claude for an initial draft of this function
bool check_model_net_request(model_net_request const * before, model_net_request const * after) {
    bool is_same = true;

    is_same &= (before->final_dest_lp == after->final_dest_lp);
    is_same &= (before->dest_mn_lp == after->dest_mn_lp);
    is_same &= (before->src_lp == after->src_lp);
    is_same &= (before->msg_start_time == after->msg_start_time);
    is_same &= (before->msg_new_mn_event == after->msg_new_mn_event);
    is_same &= (before->msg_size == after->msg_size);
    is_same &= (before->pull_size == after->pull_size);
    is_same &= (before->packet_size == after->packet_size);
    is_same &= (before->msg_id == after->msg_id);
    is_same &= (before->net_id == after->net_id);
    is_same &= (before->is_pull == after->is_pull);
    is_same &= (before->queue_offset == after->queue_offset);
    is_same &= (before->remote_event_size == after->remote_event_size);
    is_same &= (before->self_event_size == after->self_event_size);
    is_same &= (before->app_id == after->app_id);
    is_same &= (strncmp(before->category, after->category, CATEGORY_NAME_MAX) == 0);

    return is_same;
}

void print_model_net_request(FILE * out, char const * starts_with, model_net_request * req) {
    fprintf(out, "%sfinal_dest_lp = %ld\n", starts_with, req->final_dest_lp);
    fprintf(out, "%sdest_mn_lp = %ld\n", starts_with, req->dest_mn_lp);
    fprintf(out, "%ssrc_lp = %ld\n", starts_with, req->src_lp);
    fprintf(out, "%smsg_start_time = %f\n", starts_with, req->msg_start_time);
    fprintf(out, "%smsg_new_mn_event = %f\n", starts_with, req->msg_new_mn_event);
    fprintf(out, "%smsg_size = %ld\n", starts_with, req->msg_size);
    fprintf(out, "%spull_size = %ld\n", starts_with, req->pull_size);
    fprintf(out, "%spacket_size = %ld\n", starts_with, req->packet_size);
    fprintf(out, "%smsg_id = %ld\n", starts_with, req->msg_id);
    fprintf(out, "%snet_id = %d\n", starts_with, req->net_id);
    fprintf(out, "%sis_pull = %d\n", starts_with, req->is_pull);
    fprintf(out, "%squeue_offset = %d\n", starts_with, req->queue_offset);
    fprintf(out, "%sremote_event_size = %d\n", starts_with, req->remote_event_size);
    fprintf(out, "%sself_event_size = %d\n", starts_with, req->self_event_size);
    fprintf(out, "%scategory = '%s'\n", starts_with, req->category);
    fprintf(out, "%sapp_id = %d\n", starts_with, req->app_id);
}

static void print_event_state(FILE * out, model_net_wrap_msg * msg) {
    fprintf(out, "h\n");
    fprintf(out, "|.src = %lu\n", msg->h.src);
    fprintf(out, "|.event_type = %d (", msg->h.event_type);
    print_type(out, msg->h.event_type);
    fprintf(out, ")\n");
    fprintf(out, "|.magic = %d\n", msg->h.magic);
    switch (msg->h.event_type) {
        case MN_BASE_NEW_MSG:
        case MN_BASE_SCHED_NEXT:
            // We can check m_base values
            fprintf(out, "m_base\n");
            fprintf(out, "     |.req\n");
            print_model_net_request(out, "     |   |.", &msg->msg.m_base.req);
            fprintf(out, "     |.is_from_remote = %d\n", msg->msg.m_base.is_from_remote);
            fprintf(out, "     |.isQueueReq = %d\n", msg->msg.m_base.isQueueReq);
            fprintf(out, "     |.save_ts = %f\n", msg->msg.m_base.save_ts);
            fprintf(out, "     |.sched_params.prio = %d\n", msg->msg.m_base.sched_params.prio);
            fprintf(out, "     |.rc\n");
            fprintf(out, "     |  |.req\n");
            print_model_net_request(out, "     |  |   |.", &msg->msg.m_base.rc.req);
            fprintf(out, "     |  |.sched_params.prio = %d\n", msg->msg.m_base.rc.sched_params.prio);
            fprintf(out, "     |  |.rtn = %d\n", msg->msg.m_base.rc.rtn);
            fprintf(out, "     |  |.prio = %d\n", msg->msg.m_base.rc.prio);
            fprintf(out, "     |.created_in_surrogate = %d\n", msg->msg.m_base.created_in_surrogate);
            break;
        default:
            fprintf(out, "The content of this message cannot be deciphered yet with the information given\n");
    }
    // TODO: print internal state of message
    // void * sub_msg = ((char*)msg) + msg_offsets[state->net_id];
}

/* END checking reverse handler functionality */

void model_net_method_switch_to_surrogate(void) {
    is_freezing_on = true;
}

void model_net_method_switch_to_highdef(void) {
    is_freezing_on = false;
}

void model_net_method_switch_to_surrogate_lp(tw_lp * lp) {
    model_net_base_state * const ns = (model_net_base_state*) lp->cur_state;

    //printf("PID %d in_sched_send_loop = [", lp->gid);
    for (int i = 0; i < ns->params->num_queues; i++) {
        //printf("%d ", ns->in_sched_send_loop[i]);
        ns->sched_loop_pre_surrogate[i] = ns->in_sched_send_loop[i];
        // scheduling an idle event to prevent getting stuck in the middle of a scheduling loop
        if (ns->sched_loop_pre_surrogate[i]) { // <- this can be more finely tuned
        // TODO: change zero-offset event for something a bit more sensible
            model_net_method_idle_event2(0.0, 0, i, lp);
        }
        //ns->in_sched_send_loop[i] = 0;
    }
    //printf("]\n");

    ns->sched_recv_loop_pre_surrogate = ns->in_sched_recv_loop;
    if (ns->in_sched_recv_loop) {
        model_net_method_idle_event(0.0, 1, lp);
    }
    //ns->in_sched_recv_loop = 0;
}

void model_net_method_switch_to_highdef_lp(tw_lp * lp) {
    model_net_base_state * const ns = (model_net_base_state*) lp->cur_state;

    //printf("PID %d in_sched_send_loop = [", lp->gid);
    for (int i = 0; i < ns->params->num_queues; i++) {
        //printf("%d ", ns->in_sched_send_loop[i]);
        // We have to duplicate an idle event that was produced in surrogate-mode, but not yet processed by the time we switch to high-def again, if that event was in the middle of the loop (asking for the next packet to inject) and in no other case
        // TODO: Not all LPs need an event like this!
        if (ns->sched_loop_pre_surrogate[i] == 0 && ns->in_sched_send_loop[i] == 1) {
            model_net_method_idle_event2(0.0, 0, i, lp);
        }
        ns->in_sched_send_loop[i] |= ns->sched_loop_pre_surrogate[i];
    }

    if (ns->sched_recv_loop_pre_surrogate == 0 && ns->in_sched_recv_loop == 1) {
        model_net_method_idle_event(0.0, 1, lp);
    }
    ns->in_sched_recv_loop |= ns->sched_recv_loop_pre_surrogate;
}

void model_net_method_call_inner(tw_lp * lp, void (*fun) (void * inner, tw_lp * lp, tw_event **), tw_event ** lp_events) {
    model_net_base_state * const ns = (model_net_base_state*) lp->cur_state;

    fun(ns->sub_state, lp, lp_events);
}

int model_net_get_event_type_lp(model_net_wrap_msg * msg) {
    return msg->h.event_type;
}

void * model_net_method_msg_from_tw_event(tw_lp * lp, model_net_wrap_msg * msg) {
    model_net_base_state * const ns = (model_net_base_state*) lp->cur_state;

    if (msg->h.event_type & MN_BASE_PASS) { // grab sub message
        void * const sub_msg = ((char*)msg)+msg_offsets[ns->net_id];
        return sub_msg;
    }
    return NULL;
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
