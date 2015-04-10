/*
 * Copyright (C) 2014 University of Chicago.
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
#include "codes/net/simplep2p.h"

#define CATEGORY_NAME_MAX 16
#define CATEGORY_MAX 12

#define SIMPLEP2P_DEBUG 0

#define LP_CONFIG_NM (model_net_lp_config_names[SIMPLEP2P])
#define LP_METHOD_NM (model_net_method_names[SIMPLEP2P])

// parameters for simplep2p configuration
struct simplep2p_param
{
    double * net_latency_ns_table;
    double * net_bw_mbps_table;

    int mat_len;
    int num_lps;
};
typedef struct simplep2p_param simplep2p_param;

/*Define simplep2p data types and structs*/
typedef struct sp_state sp_state;

typedef struct category_idles_s category_idles;

struct category_idles_s{
    /* each simplep2p "NIC" actually has N connections, so we need to track
     * idle times across all of them to correctly do stats */
    tw_stime send_next_idle_all;
    tw_stime send_prev_idle_all;
    tw_stime recv_next_idle_all;
    tw_stime recv_prev_idle_all;
    char category[CATEGORY_NAME_MAX];
};

struct sp_state
{
    /* next idle times for network card, both inbound and outbound */
    tw_stime *send_next_idle;
    tw_stime *recv_next_idle;

    const char * anno;
    const simplep2p_param * params;

    int id; /* logical id for matrix lookups */

    /* Each simplep2p "NIC" actually has N connections, so we need to track
     * idle times across all of them to correctly do stats.
     * Additionally need to track different idle times across different 
     * categories */
    category_idles idle_times_cat[CATEGORY_MAX];

    struct mn_stats sp_stats_array[CATEGORY_MAX];
};

/* annotation-specific parameters (unannotated entry occurs at the 
 * last index) */
static uint64_t                  num_params = 0;
static simplep2p_param         * all_params = NULL;
static const config_anno_map_t * anno_map   = NULL;

static int sp_magic = 0;

/* returns a pointer to the lptype struct to use for simplep2p LPs */
static const tw_lptype* sp_get_lp_type(void);

/* set model parameters:
 * - latency_fname - path containing triangular matrix of net latencies, in ns
 * - bw_fname      - path containing triangular matrix of bandwidths in MB/s.
 * note that this merely stores the files, they will be parsed later 
 */
static void sp_set_params(
        const char      * latency_fname,
        const char      * bw_fname,
        simplep2p_param * params);

static void sp_configure();

/* retrieve the size of the portion of the event struct that is consumed by
 * the simplep2p module.  The caller should add this value to the size of
 * its own event structure to get the maximum total size of a message.
 */
static int sp_get_msg_sz(void);

/* Returns the simplep2p magic number */
static int sp_get_magic();

/* given two simplep2p logical ids, do matrix lookups to get the point-to-point
 * latency/bandwidth */
static double sp_get_table_ent(
        int      from_id, 
        int      to_id,
	int	 is_outgoing,
        int      num_lps,
        double * table);

/* category lookup */
static category_idles* sp_get_category_idles(
        char * category, category_idles *idles);

/* collective network calls */
static void simple_wan_collective();

/* collective network calls-- rc */
static void simple_wan_collective_rc();

/* Issues a simplep2p packet event call */
static tw_stime simplep2p_packet_event(
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
        int is_last_pckt);

static void simplep2p_packet_event_rc(tw_lp *sender);

static void sp_report_stats();

static tw_lpid sp_find_local_device(
        const char * annotation,
        int ignore_annotations,
        tw_lp *sender);

/* data structure for model-net statistics */
struct model_net_method simplep2p_method =
{
    .mn_configure = sp_configure,
    .mn_register = NULL,
    .model_net_method_packet_event = simplep2p_packet_event,
    .model_net_method_packet_event_rc = simplep2p_packet_event_rc,
    .model_net_method_recv_msg_event = NULL,
    .model_net_method_recv_msg_event_rc = NULL,
    .mn_get_lp_type = sp_get_lp_type,
    .mn_get_msg_sz = sp_get_msg_sz,
    .mn_report_stats = sp_report_stats,
    .model_net_method_find_local_device = NULL,
    .mn_collective_call = simple_wan_collective,
    .mn_collective_call_rc = simple_wan_collective_rc  
};

static void sp_init(
    sp_state * ns,
    tw_lp * lp);
static void sp_event(
    sp_state * ns,
    tw_bf * b,
    sp_message * m,
    tw_lp * lp);
static void sp_rev_event(
    sp_state * ns,
    tw_bf * b,
    sp_message * m,
    tw_lp * lp);
static void sp_finalize(
    sp_state * ns,
    tw_lp * lp);

tw_lptype sp_lp = {
    (init_f) sp_init,
    (pre_run_f) NULL,
    (event_f) sp_event,
    (revent_f) sp_rev_event,
    (final_f) sp_finalize,
    (map_f) codes_mapping,
    sizeof(sp_state),
};

static tw_stime rate_to_ns(uint64_t bytes, double MB_p_s);
static void handle_msg_ready_rev_event(
    sp_state * ns,
    tw_bf * b,
    sp_message * m,
    tw_lp * lp);
static void handle_msg_ready_event(
    sp_state * ns,
    tw_bf * b,
    sp_message * m,
    tw_lp * lp);
static void handle_msg_start_rev_event(
    sp_state * ns,
    tw_bf * b,
    sp_message * m,
    tw_lp * lp);
static void handle_msg_start_event(
    sp_state * ns,
    tw_bf * b,
    sp_message * m,
    tw_lp * lp);

/* collective network calls */
static void simple_wan_collective()
{
/* collectives not supported */
    return;
}

static void simple_wan_collective_rc()
{
/* collectives not supported */
   return;
}

/* returns pointer to LP information for simplep2p module */
static const tw_lptype* sp_get_lp_type()
{
    return(&sp_lp);
}

/* returns number of bytes that the simplep2p module will consume in event
 * messages
 */
static int sp_get_msg_sz(void)
{
    return(sizeof(sp_message));
}

static double * parse_mat(char * buf, int *nvals_first, int *nvals_total, int is_tri_mat){
    int bufn = 128;
    double *vals = malloc(bufn*sizeof(double));

    *nvals_first = 0;
    *nvals_total = 0;

    //printf("\n parsing the matrix ");
    /* parse the files by line */ 
    int line_ct, line_ct_prev = 0;
    char * line_save;
    char * line = strtok_r(buf, "\r\n", &line_save);
    while (line != NULL){
        line_ct = 0;
        char * tok_save;
        char * tok = strtok_r(line, " \t", &tok_save);
        while (tok != NULL){
	    char * val_save;
            if (line_ct + *nvals_total >= bufn){
                bufn<<=1;
                vals = realloc(vals, bufn*sizeof(double));
            }
	    char * val = strtok_r(tok, ",", &val_save);
	    while(val != NULL)
	    {
            	vals[line_ct+*nvals_total] = atof(val);
	        val = strtok_r(NULL, " \t", &val_save);
		line_ct++;
            }
	    tok = strtok_r(NULL, " \t", &tok_save);
        }
        /* first line check - number of tokens = the matrix dim */
        if (*nvals_first == 0) {
            *nvals_first = line_ct;
        }
        else if (is_tri_mat && line_ct != line_ct_prev-1){
            fprintf(stderr, "ERROR: tokens in line don't match triangular matrix format\n");
            exit(1);
        }
        else if (!is_tri_mat && line_ct != line_ct_prev){
            fprintf(stderr, "ERROR: tokens in line don't match square matrix format\n");
            exit(1);
        }
        *nvals_total += line_ct;
        line_ct_prev = line_ct;
        line = strtok_r(NULL, "\r\n", &line_save);
    }
    return vals;
}

static void fill_tri_mat(int N, double *mat, double *tri){
    int i, j, p = 0;
    /* first fill in triangular mat entries */
    for (i = 0; i < N; i++){
        double *row = mat + i*N;
        row[i] = 0.0;
        for (j = i+1; j < N; j++){
            row[j] = tri[p++];
        }
    }
    /* now fill in remaining entries (basically a transpose) */
    for (i = 1; i < N; i++){
        for (j = 0; j < i; j++){
            mat[i*N+j] = mat[j*N+i];
        }
    }
}

/* lets caller specify model parameters to use */
static void sp_set_params(
        const char      * latency_fname,
        const char      * bw_fname,
        simplep2p_param * params){
    long int fsize_s, fsize_b;
    /* TODO: make this a run-time option */
    int is_tri_mat = 0;

    /* slurp the files */
    FILE *sf = fopen(latency_fname, "r");
    FILE *bf = fopen(bw_fname, "r");
    if (!sf)
        tw_error(TW_LOC, "simplep2p: unable to open %s", latency_fname);
    if (!bf)
        tw_error(TW_LOC, "simplep2p: unable to open %s", bw_fname);
    fseek(sf, 0, SEEK_END);
    fsize_s = ftell(sf);
    fseek(sf, 0, SEEK_SET);
    fseek(bf, 0, SEEK_END);
    fsize_b = ftell(bf);
    fseek(bf, 0, SEEK_SET);
    char *sbuf = malloc(fsize_s+1);
    sbuf[fsize_s] = '\0';
    char *bbuf = malloc(fsize_b+1);
    bbuf[fsize_b] = '\0';
    assert(fread(sbuf, 1, fsize_s, sf) == fsize_s);
    assert(fread(bbuf, 1, fsize_b, bf) == fsize_b);
    fclose(sf);
    fclose(bf);

    int nvals_first_s, nvals_first_b, nvals_total_s, nvals_total_b;

    double *latency_tmp = parse_mat(sbuf, &nvals_first_s, 
            &nvals_total_s, is_tri_mat);
    double *bw_tmp = parse_mat(bbuf, &nvals_first_b, &nvals_total_b, is_tri_mat);

    /* convert tri mat into a regular mat */
    assert(nvals_first_s == nvals_first_b);
    params->mat_len = nvals_first_s + ((is_tri_mat) ? 1 : 0);
    if (is_tri_mat){
        params->net_latency_ns_table = 
            malloc(2*params->mat_len*params->mat_len*sizeof(double));
	params->net_bw_mbps_table = 
            malloc(2*params->mat_len*params->mat_len*sizeof(double));

	fill_tri_mat(params->mat_len, params->net_latency_ns_table, latency_tmp);
        fill_tri_mat(params->mat_len, params->net_bw_mbps_table, bw_tmp);
        free(latency_tmp);
        free(bw_tmp);
    }
    else{
        params->net_latency_ns_table = latency_tmp;
        params->net_bw_mbps_table = bw_tmp;
    }
    /* done */
}

/* report network statistics */
static void sp_report_stats()
{
   /* TODO: Do we have some simplep2p statistics to report like we have for torus and dragonfly? */
   return;
}
static void sp_init(
    sp_state * ns,
    tw_lp * lp)
{
    uint32_t h1 = 0, h2 = 0;
    memset(ns, 0, sizeof(*ns));

    bj_hashlittle2(LP_METHOD_NM, strlen(LP_METHOD_NM), &h1, &h2);
    sp_magic = h1+h2;
    /* printf("\n sp_magic %d ", sp_magic); */

    ns->anno = codes_mapping_get_annotation_by_lpid(lp->gid);
    if (ns->anno == NULL)
        ns->params = &all_params[num_params-1];
    else{
        int id = configuration_get_annotation_index(ns->anno, anno_map);
        ns->params = &all_params[id];
    }

    /* inititalize global logical ID w.r.t. annotation */
    ns->id = codes_mapping_get_lp_relative_id(lp->gid, 0, 1);

    /* all devices are idle to begin with */
    ns->send_next_idle = malloc(ns->params->num_lps * 
            sizeof(ns->send_next_idle));
    ns->recv_next_idle = malloc(ns->params->num_lps * 
            sizeof(ns->recv_next_idle));
    tw_stime st = tw_now(lp);
    int i;
    for (i = 0; i < ns->params->num_lps; i++){
        ns->send_next_idle[i] = st;
        ns->recv_next_idle[i] = st;
    }

    for (i = 0; i < CATEGORY_MAX; i++){
        ns->idle_times_cat[i].send_next_idle_all = 0.0;
        ns->idle_times_cat[i].send_prev_idle_all = 0.0;
        ns->idle_times_cat[i].recv_next_idle_all = 0.0;
        ns->idle_times_cat[i].recv_prev_idle_all = 0.0;
        ns->idle_times_cat[i].category[0] = '\0';
    }

    return;
}

static void sp_event(
    sp_state * ns,
    tw_bf * b,
    sp_message * m,
    tw_lp * lp)
{
    assert(m->magic == sp_magic);

    switch (m->event_type)
    {
        case SP_MSG_START:
            handle_msg_start_event(ns, b, m, lp);
            break;
        case SP_MSG_READY:
            handle_msg_ready_event(ns, b, m, lp);
            break;
        default:
            assert(0);
            break;
    }
}

static void sp_rev_event(
    sp_state * ns,
    tw_bf * b,
    sp_message * m,
    tw_lp * lp)
{
    assert(m->magic == sp_magic);

    switch (m->event_type)
    {
        case SP_MSG_START:
            handle_msg_start_rev_event(ns, b, m, lp);
            break;
        case SP_MSG_READY:
            handle_msg_ready_rev_event(ns, b, m, lp);
            break;
        default:
            assert(0);
            break;
    }

    return;
}

static void sp_finalize(
    sp_state * ns,
    tw_lp * lp)
{
    /* first need to add last known active-range times (they aren't added 
     * until afterwards) */ 
    int i;
    for (i = 0; 
            i < CATEGORY_MAX && strlen(ns->idle_times_cat[i].category) > 0; 
            i++){
        category_idles *id = ns->idle_times_cat + i;
        mn_stats       *st = ns->sp_stats_array + i;
        st->send_time += id->send_next_idle_all - id->send_prev_idle_all;
        st->recv_time += id->recv_next_idle_all - id->recv_prev_idle_all;
    }

    model_net_print_stats(lp->gid, &ns->sp_stats_array[0]);
    return;
}

int sp_get_magic()
{
  return sp_magic;
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
    sp_state * ns,
    tw_bf * b,
    sp_message * m,
    tw_lp * lp)
{
    struct mn_stats* stat;
    category_idles * idles;

    stat = model_net_find_stats(m->category, ns->sp_stats_array);
    stat->recv_count--;
    stat->recv_bytes -= m->net_msg_size_bytes;
    stat->recv_time = m->recv_time_saved;

    ns->recv_next_idle[m->src_mn_rel_id] = m->recv_next_idle_saved;
    idles = sp_get_category_idles(m->category, ns->idle_times_cat);
    idles->recv_next_idle_all = m->recv_next_idle_all_saved;
    idles->recv_prev_idle_all = m->recv_prev_idle_all_saved;

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
    sp_state * ns,
    tw_bf * b,
    sp_message * m,
    tw_lp * lp)
{
    tw_stime recv_queue_time = 0;
    tw_event *e_new;
    struct mn_stats* stat;

    /* get source->me network stats */
    double bw = sp_get_table_ent(m->src_mn_rel_id, ns->id,
            1, ns->params->num_lps, ns->params->net_bw_mbps_table);
    double latency = sp_get_table_ent(m->src_mn_rel_id, ns->id,
            1, ns->params->num_lps, ns->params->net_latency_ns_table);
    
   // printf("\n LP %d outgoing bandwidth with LP %d is %f ", ns->id, m->src_mn_rel_id, bw);
    if (bw <= 0.0 || latency < 0.0){
        fprintf(stderr, 
                "Invalid link from Rel. id %d to LP %lu (rel. id %d)\n", 
                m->src_mn_rel_id, lp->gid, ns->id);
        abort();
    }

    /* are we available to recv the msg? */
    /* were we available when the transmission was started? */
    if(ns->recv_next_idle[m->src_mn_rel_id] > tw_now(lp))
        recv_queue_time += 
            ns->recv_next_idle[m->src_mn_rel_id] - tw_now(lp);

    /* calculate transfer time based on msg size and bandwidth */
    recv_queue_time += rate_to_ns(m->net_msg_size_bytes, bw);

    /* bump up input queue idle time accordingly */
    m->recv_next_idle_saved = ns->recv_next_idle[m->src_mn_rel_id];
    ns->recv_next_idle[m->src_mn_rel_id] = recv_queue_time + tw_now(lp);

    /* get stats, save state (TODO: smarter save state than param dump?)  */
    stat = model_net_find_stats(m->category, ns->sp_stats_array);
    category_idles *idles = 
        sp_get_category_idles(m->category, ns->idle_times_cat);
    stat->recv_count++;
    stat->recv_bytes += m->net_msg_size_bytes;
    m->recv_time_saved = stat->recv_time;
    m->recv_next_idle_all_saved = idles->recv_next_idle_all;
    m->recv_prev_idle_all_saved = idles->recv_prev_idle_all;

#if SIMPLEP2P_DEBUG
    printf("%d: from_id:%d now: %8.3lf next_idle_recv: %8.3lf\n",
            ns->id, m->src_mn_rel_id,
            tw_now(lp), ns->recv_next_idle[m->src_mn_rel_id]);
    printf("%d: BEFORE all_idles_recv %8.3lf %8.3lf\n",
            ns->id,
            idles->recv_prev_idle_all, idles->recv_next_idle_all);
#endif

    /* update global idles, recv time */
    if (tw_now(lp) > idles->recv_next_idle_all){
        /* there was an idle period between last idle and now */
        stat->recv_time += 
            idles->recv_next_idle_all - idles->recv_prev_idle_all;
        idles->recv_prev_idle_all = tw_now(lp); 
    }
    if (ns->recv_next_idle[m->src_mn_rel_id] > idles->recv_next_idle_all){
        /* extend the active period (active until at least this request) */
        idles->recv_next_idle_all = ns->recv_next_idle[m->src_mn_rel_id];
    }

#if SIMPLEP2P_DEBUG
    printf("%d: AFTER  all_idles_recv %8.3lf %8.3lf",
            ns->id, idles->recv_prev_idle_all, idles->recv_next_idle_all);
    if (m->event_size_bytes>0){
        printf(" - with event\n");
    }
    else{
        printf(" - without event\n");
    }
#endif

    /* copy only the part of the message used by higher level */
    if(m->event_size_bytes)
    {
        //char* tmp_ptr = (char*)m;
        //tmp_ptr += sp_get_msg_sz();
        void *tmp_ptr = model_net_method_get_edata(SIMPLEP2P, m);
        if (m->is_pull){
            int net_id = model_net_get_id(LP_METHOD_NM);
            model_net_event(net_id, m->category, m->src_gid, m->pull_size,
                    recv_queue_time, m->event_size_bytes, tmp_ptr, 0, NULL, lp);
        }
        else{
            /* schedule event to final destination for when the recv is complete */
            e_new = tw_event_new(m->final_dest_gid, recv_queue_time, lp);
            void *m_new = tw_event_data(e_new);
            memcpy(m_new, tmp_ptr, m->event_size_bytes);
            tw_event_send(e_new);
        }
    }

    return;
}

/* reverse computation for msg start event */
static void handle_msg_start_rev_event(
    sp_state * ns,
    tw_bf * b,
    sp_message * m,
    tw_lp * lp)
{
    if(m->local_event_size_bytes > 0)
    {
        codes_local_latency_reverse(lp);
    }

    mn_stats* stat;
    stat = model_net_find_stats(m->category, ns->sp_stats_array);
    stat->send_count--;
    stat->send_bytes -= m->net_msg_size_bytes;
    stat->send_time = m->send_time_saved;

    category_idles *idles = 
        sp_get_category_idles(m->category, ns->idle_times_cat);
    ns->send_next_idle[m->dest_mn_rel_id] = m->send_next_idle_saved;
    idles->send_next_idle_all = m->send_next_idle_all_saved;
    idles->send_prev_idle_all = m->send_prev_idle_all_saved;

    return;
}

/* handler for msg start event; this indicates that the caller is trying to
 * transmit a message through this NIC
 */
static void handle_msg_start_event(
    sp_state * ns,
    tw_bf * b,
    sp_message * m,
    tw_lp * lp)
{
    tw_event *e_new;
    sp_message *m_new;
    tw_stime send_queue_time = 0;
    mn_stats* stat;
    int mapping_rep_id, mapping_offset, dummy;
    tw_lpid dest_id;
    char lp_group_name[MAX_NAME_LENGTH];
    int total_event_size;
    int dest_rel_id;
    double bw, latency;

    total_event_size = model_net_get_msg_sz(SIMPLEP2P) + m->event_size_bytes +
        m->local_event_size_bytes;

    dest_id = model_net_find_local_device(SIMPLEP2P, ns->anno, 0,
            m->final_dest_gid);
    dest_rel_id = codes_mapping_get_lp_relative_id(dest_id, 0, 0);
    m->dest_mn_rel_id = dest_rel_id;

    /* grab the link params */
    bw = sp_get_table_ent(ns->id, dest_rel_id,
            0, ns->params->num_lps, ns->params->net_bw_mbps_table);
    latency = sp_get_table_ent(ns->id, dest_rel_id,
            0, ns->params->num_lps, ns->params->net_latency_ns_table);
    
    //printf("\n LP %d incoming bandwidth with LP %d is %f ", ns->id, dest_rel_id, bw);
    if (bw <= 0.0 || latency < 0.0){
        fprintf(stderr, 
                "Invalid link from LP %lu (rel. id %d) to LP %lu (rel. id %d)\n", 
                lp->gid, ns->id, dest_id, dest_rel_id);
        abort();
    }

    /* calculate send time stamp */
    send_queue_time = 0.0; /* net msg latency cost (negligible for this model) */
    /* bump up time if the NIC send queue isn't idle right now */
    if(ns->send_next_idle[dest_rel_id] > tw_now(lp))
        send_queue_time += ns->send_next_idle[dest_rel_id] - tw_now(lp);

    /* move the next idle time ahead to after this transmission is
     * _complete_ from the sender's perspective 
     */ 
    m->send_next_idle_saved = ns->send_next_idle[dest_rel_id];
    ns->send_next_idle[dest_rel_id] = send_queue_time + tw_now(lp) +
        rate_to_ns(m->net_msg_size_bytes, bw);

    /* get stats, save state (TODO: smarter save state than param dump?)  */
    stat = model_net_find_stats(m->category, ns->sp_stats_array);
    category_idles *idles = 
        sp_get_category_idles(m->category, ns->idle_times_cat);
    stat->send_count++;
    stat->send_bytes += m->net_msg_size_bytes;
    m->send_time_saved = stat->send_time;
    m->send_next_idle_all_saved = idles->send_next_idle_all;
    m->send_prev_idle_all_saved = idles->send_prev_idle_all;
    if(stat->max_event_size < total_event_size)
        stat->max_event_size = total_event_size;

#if SIMPLEP2P_DEBUG
    printf("%d: to_id:%d now: %8.3lf next_idle_send: %8.3lf\n",
            ns->id, dest_rel_id,
            tw_now(lp), ns->send_next_idle[dest_rel_id]);
    printf("%d: BEFORE all_idles_send %8.3lf %8.3lf\n",
            ns->id, idles->send_prev_idle_all, idles->send_next_idle_all);
#endif

    /* update global idles, send time */
    if (tw_now(lp) > idles->send_next_idle_all){
        /* there was an idle period between last idle and now */
        stat->send_time += idles->send_next_idle_all - idles->send_prev_idle_all;
        idles->send_prev_idle_all = tw_now(lp); 
    }
    if (ns->send_next_idle[dest_rel_id] > idles->send_next_idle_all){
        /* extend the active period (active until at least this request) */
        idles->send_next_idle_all = ns->send_next_idle[dest_rel_id];
    }

#if SIMPLEP2P_DEBUG
    printf("%d: AFTER  all_idles_send %8.3lf %8.3lf",
            ns->id, idles->send_prev_idle_all, idles->send_next_idle_all);
    if (m->local_event_size_bytes>0){
        printf(" - with local event\n");
    }
    else{
        printf(" - without local event\n");
    }
#endif

    /* create new event to send msg to receiving NIC */
//    printf("\n msg start sending to %d ", dest_id);
    void *m_data;
    //e_new = tw_event_new(dest_id, send_queue_time, lp);
    //m_new = tw_event_data(e_new);
    e_new = model_net_method_event_new(dest_id, send_queue_time+latency, lp,
            SIMPLEP2P, (void**)&m_new, &m_data);

    /* copy entire previous message over, including payload from user of
     * this module
     */
    //memcpy(m_new, m, m->event_size_bytes + sp_get_msg_sz());
    memcpy(m_new, m, sizeof(sp_message));
    if (m->event_size_bytes){
        memcpy(m_data, model_net_method_get_edata(SIMPLEP2P, m),
                m->event_size_bytes);
    }
    m_new->event_type = SP_MSG_READY;
    m_new->src_mn_rel_id = ns->id;
    
    tw_event_send(e_new);

    /* if there is a local event to handle, then create an event for it as
     * well
     */
    if(m->local_event_size_bytes > 0)
    {
        //char* local_event;

        e_new = tw_event_new(m->src_gid, send_queue_time+codes_local_latency(lp), lp);
        m_new = tw_event_data(e_new);

        void * m_loc = (char*) model_net_method_get_edata(SIMPLEP2P, m) +
            m->event_size_bytes;
         //local_event = (char*)m;
         //local_event += sp_get_msg_sz() + m->event_size_bytes;         	 
        /* copy just the local event data over */
        memcpy(m_new, m_loc, m->local_event_size_bytes);
        tw_event_send(e_new);
    }
    return;
}

/* Model-net function calls */

/*This method will serve as an intermediate layer between simplep2p and modelnet. 
 * It takes the packets from modelnet layer and calls underlying simplep2p methods*/
static tw_stime simplep2p_packet_event(
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
     sp_message * msg;
     char* tmp_ptr;

     xfer_to_nic_time = codes_local_latency(sender);

#if SIMPLEP2P_DEBUG
    printf("%lu: final %lu packet sz %d remote sz %d self sz %d is_last_pckt %d latency %lf\n",
            (src_lp - 1) / 2, final_dest_lp, packet_size, 
            remote_event_size, self_event_size, is_last_pckt,
            xfer_to_nic_time+offset);
#endif

     e_new = model_net_method_event_new(sender->gid, xfer_to_nic_time+offset,
             sender, SIMPLEP2P, (void**)&msg, (void**)&tmp_ptr);
     strcpy(msg->category, category);
     msg->final_dest_gid = final_dest_lp;
     msg->src_gid = src_lp;
     msg->magic = sp_get_magic();
     msg->net_msg_size_bytes = packet_size;
     msg->event_size_bytes = 0;
     msg->local_event_size_bytes = 0;
     msg->event_type = SP_MSG_START;
     msg->is_pull = is_pull;
     msg->pull_size = pull_size;

    //printf("\n Sending to LP %d msg magic %d ", (int)dest_id, sp_get_magic()); 
     /*Fill in simplep2p information*/     
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
      // printf("\n Last packet size: %d ", sp_get_msg_sz() + remote_event_size + self_event_size);
      }
     tw_event_send(e_new);
     return xfer_to_nic_time;
}

static void sp_read_config(const char * anno, simplep2p_param *p){
    char latency_file[MAX_NAME_LENGTH];
    char bw_file[MAX_NAME_LENGTH];
    int rc;
    rc = configuration_get_value_relpath(&config, "PARAMS", 
            "net_latency_ns_file", anno, latency_file, MAX_NAME_LENGTH);
    if (rc <= 0){
        if (anno == NULL)
            tw_error(TW_LOC,
                    "simplep2p: unable to read PARAMS:net_latency_ns_file");
        else
            tw_error(TW_LOC,
                    "simplep2p: unable to read PARAMS:net_latency_ns_file@%s",
                    anno);
    }
    rc = configuration_get_value_relpath(&config, "PARAMS", "net_bw_mbps_file",
            anno, bw_file, MAX_NAME_LENGTH);
    if (rc <= 0){
        if (anno == NULL)
            tw_error(TW_LOC,
                    "simplep2p: unable to read PARAMS:net_bw_mbps_file");
        else
            tw_error(TW_LOC,
                    "simplep2p: unable to read PARAMS:net_bw_mbps_file@%s",
                    anno);
    }
    p->num_lps = codes_mapping_get_lp_count(NULL, 0,
            LP_CONFIG_NM, anno, 0);
    sp_set_params(latency_file, bw_file, p);
    if (p->mat_len != (2 * p->num_lps)){
        tw_error(TW_LOC, "simplep2p config matrix doesn't match the "
                "number of simplep2p LPs (%d vs. %d)\n",
                p->mat_len, p->num_lps);
    }
}

static void sp_configure(){
    anno_map = codes_mapping_get_lp_anno_map(LP_CONFIG_NM);
    assert(anno_map);
    num_params = anno_map->num_annos + (anno_map->has_unanno_lp > 0);
    all_params = malloc(num_params * sizeof(*all_params));
    for (uint64_t i = 0; i < anno_map->num_annos; i++){
        sp_read_config(anno_map->annotations[i], &all_params[i]);
    }
    if (anno_map->has_unanno_lp > 0){
        sp_read_config(NULL, &all_params[anno_map->num_annos]);
    }
}

static void simplep2p_packet_event_rc(tw_lp *sender)
{
    codes_local_latency_reverse(sender);
    return;
}

static tw_lpid sp_find_local_device(
        const char * annotation,
        int ignore_annotations,
        tw_lp *sender)
{
     char lp_group_name[MAX_NAME_LENGTH];
     int mapping_rep_id, mapping_offset, dummy;
     tw_lpid dest_id;

     // TODO: don't ignore annotation
     codes_mapping_get_lp_info(sender->gid, lp_group_name, &dummy, NULL, &dummy, NULL, &mapping_rep_id, &mapping_offset);
     codes_mapping_get_lp_id(lp_group_name, LP_CONFIG_NM, annotation,
             ignore_annotations, mapping_rep_id, mapping_offset, &dest_id);

    return(dest_id);
}

static double sp_get_table_ent(
        int      from_id, 
        int      to_id,
	int 	 is_incoming, /* chooses between incoming and outgoing bandwidths */
        int      num_lps,
        double * table){
    // TODO: if a tri-matrix, then change the addressing
    return table[2 * from_id * num_lps + 2 * to_id + is_incoming]; 
}

/* category lookup (more or less copied from model_net_find_stats) */
static category_idles* sp_get_category_idles(
        char * category, category_idles *idles){
    int i;
    int new_flag = 0;
    int found_flag = 0;

    for(i=0; i<CATEGORY_MAX; i++) {
        if(strlen(idles[i].category) == 0) {
            found_flag = 1;
            new_flag = 1;
            break;
        }
        if(strcmp(category, idles[i].category) == 0) {
            found_flag = 1;
            new_flag = 0;
            break;
        }
    }
    assert(found_flag);

    if(new_flag) {
        strcpy(idles[i].category, category);
    }
    return &idles[i];
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
