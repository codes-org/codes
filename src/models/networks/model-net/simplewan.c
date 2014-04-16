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
#include "codes/codes_mapping.h"
#include "codes/codes.h"

#define CATEGORY_NAME_MAX 16
#define CATEGORY_MAX 12

/*Define simplewan data types and structs*/
typedef struct sw_message sw_message;
typedef struct sw_state sw_state;

/* types of events that will constitute triton requests */
enum sw_event_type 
{
    MSG_READY = 1,  /* sender has transmitted msg to receiver */
    MSG_START,      /* initiate a transmission */
};

typedef struct category_idles_s category_idles;

struct category_idles_s{
    /* each simplewan "NIC" actually has N connections, so we need to track
     * idle times across all of them to correctly do stats */
    tw_stime send_next_idle_all;
    tw_stime send_prev_idle_all;
    tw_stime recv_next_idle_all;
    tw_stime recv_prev_idle_all;
    char category[CATEGORY_NAME_MAX];
};

struct sw_state
{
    /* next idle times for network card, both inbound and outbound */
    tw_stime *send_next_idle;
    tw_stime *recv_next_idle;

    int id; /* logical id for matrix lookups */

    /* Each simplewan "NIC" actually has N connections, so we need to track
     * idle times across all of them to correctly do stats.
     * Additionally need to track different idle times across different 
     * categories */
    category_idles idle_times_cat[CATEGORY_MAX];

    struct mn_stats sw_stats_array[CATEGORY_MAX];
};

struct sw_message
{
    int magic; /* magic number */
    enum sw_event_type event_type;
    tw_lpid src_gid; /* who transmitted this msg? */
    tw_lpid final_dest_gid; /* who is eventually targetted with this msg? */
    /* relative ID of the sending simplewan message (for latency/bandwidth lookup) */
    int src_mn_rel_id;
    int dest_mn_rel_id; /* included to make rc easier */
    int net_msg_size_bytes;     /* size of modeled network message */
    int event_size_bytes;     /* size of simulator event message that will be tunnelled to destination */
    int local_event_size_bytes;     /* size of simulator event message that delivered locally upon local completion */
    char category[CATEGORY_NAME_MAX]; /* category for communication */
    
    /* for reverse computation */
    tw_stime send_next_idle_saved;
    tw_stime recv_next_idle_saved;
    tw_stime send_time_saved;
    tw_stime recv_time_saved;
    tw_stime send_next_idle_all_saved;
    tw_stime send_prev_idle_all_saved;
    tw_stime recv_next_idle_all_saved;
    tw_stime recv_prev_idle_all_saved;
};
/* net startup cost, ns */
static double *global_net_startup_ns = NULL;
/* net bw, MB/s */
static double *global_net_bw_mbs = NULL;
/* number of simplewan lps, used for addressing the network parameters
 * set by the first LP to init per process */
static int num_lps = -1;

static int sw_magic = 0;

/* returns a pointer to the lptype struct to use for simplewan LPs */
static const tw_lptype* sw_get_lp_type(void);

/* set model parameters:
 * - startup_fname - path containing triangular matrix of net latencies, in ns
 * - bw_fname      - path containing triangular matrix of bandwidths in MB/s.
 * note that this merely stores the files, they will be parsed later 
 */
static void sw_set_params(char * startup_fname, char * bw_fname);

/* retrieve the size of the portion of the event struct that is consumed by
 * the simplewan module.  The caller should add this value to the size of
 * its own event structure to get the maximum total size of a message.
 */
static int sw_get_msg_sz(void);

/* Returns the simplewan magic number */
static int sw_get_magic();

/* given two simplewan logical ids, do matrix lookups to get the point-to-point
 * latency/bandwidth */
static double sw_get_bw(int from_id, int to_id);
static double sw_get_startup(int from_id, int to_id);

/* category lookup */
static category_idles* sw_get_category_idles(
        char * category, category_idles *idles);

/* allocate a new event that will pass through simplewan to arriave at its
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
/* sets up the simplewan parameters through modelnet interface */
static void sw_setup(const void* net_params);

/* Issues a simplewan packet event call */
static tw_stime simplewan_packet_event(
     char* category, 
     tw_lpid final_dest_lp, 
     int packet_size, 
     tw_stime offset,
     int remote_event_size, 
     const void* remote_event, 
     int self_event_size,
     const void* self_event,
     tw_lp *sender,
     int is_last_pckt);
static void simplewan_packet_event_rc(tw_lp *sender);

static void simplewan_packet_event_rc(tw_lp *sender);

static void sw_report_stats();

static tw_lpid sw_find_local_device(tw_lp *sender);

/* data structure for model-net statistics */
struct model_net_method simplewan_method =
{
    .method_name = "simplewan",
    .mn_setup = sw_setup,
    .model_net_method_packet_event = simplewan_packet_event,
    .model_net_method_packet_event_rc = simplewan_packet_event_rc,
    .mn_get_lp_type = sw_get_lp_type,
    .mn_get_msg_sz = sw_get_msg_sz,
    .mn_report_stats = sw_report_stats,
    .model_net_method_find_local_device = sw_find_local_device,
};

static void sw_init(
    sw_state * ns,
    tw_lp * lp);
static void sw_event(
    sw_state * ns,
    tw_bf * b,
    sw_message * m,
    tw_lp * lp);
static void sw_rev_event(
    sw_state * ns,
    tw_bf * b,
    sw_message * m,
    tw_lp * lp);
static void sw_finalize(
    sw_state * ns,
    tw_lp * lp);

tw_lptype sw_lp = {
     (init_f) sw_init,
     (event_f) sw_event,
     (revent_f) sw_rev_event,
     (final_f) sw_finalize,
     (map_f) codes_mapping,
     sizeof(sw_state),
};

static tw_stime rate_to_ns(unsigned int bytes, double MB_p_s);
static void handle_msg_ready_rev_event(
    sw_state * ns,
    tw_bf * b,
    sw_message * m,
    tw_lp * lp);
static void handle_msg_ready_event(
    sw_state * ns,
    tw_bf * b,
    sw_message * m,
    tw_lp * lp);
static void handle_msg_start_rev_event(
    sw_state * ns,
    tw_bf * b,
    sw_message * m,
    tw_lp * lp);
static void handle_msg_start_event(
    sw_state * ns,
    tw_bf * b,
    sw_message * m,
    tw_lp * lp);

/* returns pointer to LP information for simplewan module */
static const tw_lptype* sw_get_lp_type()
{
    return(&sw_lp);
}

/* returns number of bytes that the simplewan module will consume in event
 * messages
 */
static int sw_get_msg_sz(void)
{
    return(sizeof(sw_message));
}

static double * parse_tri_mat(char * buf, int *nvals_first, int *nvals_total){
    int bufn = 128;
    double *vals = malloc(bufn*sizeof(double));

    *nvals_first = 0;
    *nvals_total = 0;

    /* parse the files by line */ 
    int line_ct, line_ct_prev;
    char * line_save;
    char * line = strtok_r(buf, "\r\n", &line_save);
    while (line != NULL){
        line_ct = 0;
        char * tok_save;
        char * tok = strtok_r(line, " \t", &tok_save);
        while (tok != NULL){
            if (line_ct + *nvals_total >= bufn){
                bufn<<=1;
                vals = realloc(vals, bufn*sizeof(double));
            }
            vals[line_ct+*nvals_total] = atof(tok);
            line_ct++;
            tok = strtok_r(NULL, " \t", &tok_save);
        }
        /* first line check - number of tokens = the matrix dim */
        if (*nvals_first == 0) {
            *nvals_first = line_ct;
        }
        else if (line_ct != line_ct_prev-1){
            fprintf(stderr, "ERROR: tokens in line don't match triangular matrix format\n");
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
static void sw_set_params(char * startup_fname, char * bw_fname){
    long int fsize_s, fsize_b;

    /* slurp the files */
    FILE *sf = fopen(startup_fname, "r");
    FILE *bf = fopen(bw_fname, "r");
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
    fread(sbuf, 1, fsize_s, sf);
    fread(bbuf, 1, fsize_b, bf);
    fclose(sf);
    fclose(bf);

    int nvals_first_s, nvals_first_b, nvals_total_s, nvals_total_b;

    double *startup_tmp = parse_tri_mat(sbuf, &nvals_first_s, 
            &nvals_total_s);
    double *bw_tmp = parse_tri_mat(bbuf, &nvals_first_b, &nvals_total_b);

    /* convert tri mat into a regular mat */
    assert(nvals_first_s == nvals_first_b);
    int N = nvals_first_s + 1;
    global_net_startup_ns = malloc(N*N*sizeof(double));
    global_net_bw_mbs = malloc(N*N*sizeof(double));
    /* first fill in mat, then fill opposites */
    fill_tri_mat(N, global_net_startup_ns, startup_tmp);
    fill_tri_mat(N, global_net_bw_mbs, bw_tmp);
    free(startup_tmp);
    free(bw_tmp);

    /* done */
}

/* report network statistics */
static void sw_report_stats()
{
   /* TODO: Do we have some simplewan statistics to report like we have for torus and dragonfly? */
   return;
}
static void sw_init(
    sw_state * ns,
    tw_lp * lp)
{
    uint32_t h1 = 0, h2 = 0;
    memset(ns, 0, sizeof(*ns));

    bj_hashlittle2("simplewan", strlen("simplewan"), &h1, &h2);
    sw_magic = h1+h2;
    /* printf("\n sw_magic %d ", sw_magic); */

    /* inititalize logical ID */
    ns->id = codes_mapping_get_lp_global_rel_id(lp->gid);

    /* get the total number of simplewans */
    if (num_lps == -1){
        num_lps = codes_mapping_get_global_lp_count("modelnet_simplewan");
        assert(num_lps > 0);
    }

    /* all devices are idle to begin with */
    ns->send_next_idle = malloc(num_lps * sizeof(ns->send_next_idle));
    ns->recv_next_idle = malloc(num_lps * sizeof(ns->recv_next_idle));
    tw_stime st = tw_now(lp);
    int i;
    for (i = 0; i < num_lps; i++){
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

static void sw_event(
    sw_state * ns,
    tw_bf * b,
    sw_message * m,
    tw_lp * lp)
{
    assert(m->magic == sw_magic);

    switch (m->event_type)
    {
        case MSG_START:
            handle_msg_start_event(ns, b, m, lp);
            break;
        case MSG_READY:
            handle_msg_ready_event(ns, b, m, lp);
            break;
        default:
            assert(0);
            break;
    }
}

static void sw_rev_event(
    sw_state * ns,
    tw_bf * b,
    sw_message * m,
    tw_lp * lp)
{
    assert(m->magic == sw_magic);

    switch (m->event_type)
    {
        case MSG_START:
            handle_msg_start_rev_event(ns, b, m, lp);
            break;
        case MSG_READY:
            handle_msg_ready_rev_event(ns, b, m, lp);
            break;
        default:
            assert(0);
            break;
    }

    return;
}

static void sw_finalize(
    sw_state * ns,
    tw_lp * lp)
{
    /* first need to add last known active-range times (they aren't added 
     * until afterwards) */ 
    int i;
    for (i = 0; 
            i < CATEGORY_MAX && strlen(ns->idle_times_cat[i].category) > 0; 
            i++){
        category_idles *id = ns->idle_times_cat + i;
        mn_stats       *st = ns->sw_stats_array + i;
        st->send_time += id->send_next_idle_all - id->send_prev_idle_all;
        st->recv_time += id->recv_next_idle_all - id->recv_prev_idle_all;
    }

    model_net_print_stats(lp->gid, &ns->sw_stats_array[0]);
    return;
}

int sw_get_magic()
{
  return sw_magic;
}

/* convert MiB/s and bytes to ns */
static tw_stime rate_to_ns(unsigned int bytes, double MB_p_s)
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
    sw_state * ns,
    tw_bf * b,
    sw_message * m,
    tw_lp * lp)
{
    struct mn_stats* stat;
    category_idles * idles;

    stat = model_net_find_stats(m->category, ns->sw_stats_array);
    stat->recv_count--;
    stat->recv_bytes -= m->net_msg_size_bytes;
    stat->recv_time = m->recv_time_saved;

    ns->recv_next_idle[m->src_mn_rel_id] = m->recv_next_idle_saved;
    idles = sw_get_category_idles(m->category, ns->idle_times_cat);
    idles->recv_next_idle_all = m->recv_next_idle_all_saved;
    idles->recv_prev_idle_all = m->recv_prev_idle_all_saved;

    return;
}

/* handler for msg ready event.  This indicates that a message is available
 * to recv, but we haven't checked to see if the recv queue is available yet
 */
static void handle_msg_ready_event(
    sw_state * ns,
    tw_bf * b,
    sw_message * m,
    tw_lp * lp)
{
    tw_stime recv_queue_time = 0;
    tw_event *e_new;
    sw_message *m_new;
    struct mn_stats* stat;

    /* get source->me network stats */
    double bw = sw_get_bw(m->src_mn_rel_id, ns->id);
    double startup = sw_get_startup(m->src_mn_rel_id, ns->id);
    if (bw <= 0.0 || startup < 0.0){
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
    stat = model_net_find_stats(m->category, ns->sw_stats_array);
    category_idles *idles = 
        sw_get_category_idles(m->category, ns->idle_times_cat);
    stat->recv_count++;
    stat->recv_bytes += m->net_msg_size_bytes;
    m->recv_time_saved = stat->recv_time;
    m->recv_next_idle_all_saved = idles->recv_next_idle_all;
    m->recv_prev_idle_all_saved = idles->recv_prev_idle_all;

    printf("%d: from_id:%d now: %8.3lf next_idle_recv: %8.3lf\n",
            ns->id, m->src_mn_rel_id,
            tw_now(lp), ns->recv_next_idle[m->src_mn_rel_id]);
    printf("%d: BEFORE all_idles_recv %8.3lf %8.3lf\n",
            ns->id,
            idles->recv_prev_idle_all, idles->recv_next_idle_all);

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

    printf("%d: AFTER  all_idles_recv %8.3lf %8.3lf",
            ns->id, idles->recv_prev_idle_all, idles->recv_next_idle_all);
    if (m->event_size_bytes>0){
        printf(" - with event\n");
    }
    else{
        printf(" - without event\n");
    }

    /* copy only the part of the message used by higher level */
    if(m->event_size_bytes)
    {
      /* schedule event to final destination for when the recv is complete */
      e_new = tw_event_new(m->final_dest_gid, recv_queue_time, lp);
      m_new = tw_event_data(e_new);
      char* tmp_ptr = (char*)m;
      tmp_ptr += sw_get_msg_sz();
      memcpy(m_new, tmp_ptr, m->event_size_bytes);
      tw_event_send(e_new);
    }

    return;
}

/* reverse computation for msg start event */
static void handle_msg_start_rev_event(
    sw_state * ns,
    tw_bf * b,
    sw_message * m,
    tw_lp * lp)
{
    if(m->local_event_size_bytes > 0)
    {
        codes_local_latency_reverse(lp);
    }

    mn_stats* stat;
    stat = model_net_find_stats(m->category, ns->sw_stats_array);
    stat->send_count--;
    stat->send_bytes -= m->net_msg_size_bytes;
    stat->send_time = m->send_time_saved;

    category_idles *idles = 
        sw_get_category_idles(m->category, ns->idle_times_cat);
    ns->send_next_idle[m->dest_mn_rel_id] = m->send_next_idle_saved;
    idles->send_next_idle_all = m->send_next_idle_all_saved;
    idles->send_prev_idle_all = m->send_prev_idle_all_saved;

    return;
}

/* handler for msg start event; this indicates that the caller is trying to
 * transmit a message through this NIC
 */
static void handle_msg_start_event(
    sw_state * ns,
    tw_bf * b,
    sw_message * m,
    tw_lp * lp)
{
    tw_event *e_new;
    sw_message *m_new;
    tw_stime send_queue_time = 0;
    mn_stats* stat;
    int mapping_grp_id, mapping_type_id, mapping_rep_id, mapping_offset;
    tw_lpid dest_id;
    char lp_type_name[MAX_NAME_LENGTH], lp_group_name[MAX_NAME_LENGTH];
    int total_event_size;
    int dest_rel_id;
    double bw, startup;

    total_event_size = sw_get_msg_sz() + m->event_size_bytes + m->local_event_size_bytes;

    codes_mapping_get_lp_info(m->final_dest_gid, lp_group_name, &mapping_grp_id, &mapping_type_id, lp_type_name, &mapping_rep_id, &mapping_offset);
    codes_mapping_get_lp_id(lp_group_name, "modelnet_simplewan", mapping_rep_id , mapping_offset, &dest_id); 
    dest_rel_id = codes_mapping_get_lp_global_rel_id(dest_id);
    m->dest_mn_rel_id = dest_rel_id;

    /* grab the link params */
    bw      = sw_get_bw(ns->id, dest_rel_id);
    startup = sw_get_startup(ns->id, dest_rel_id);
    if (bw <= 0.0 || startup < 0.0){
        fprintf(stderr, 
                "Invalid link from LP %lu (rel. id %d) to LP %lu (rel. id %d)\n", 
                lp->gid, ns->id, dest_id, dest_rel_id);
        abort();
    }

    /* calculate send time stamp */
    send_queue_time = startup; /* net msg startup cost */
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
    stat = model_net_find_stats(m->category, ns->sw_stats_array);
    category_idles *idles = 
        sw_get_category_idles(m->category, ns->idle_times_cat);
    stat->send_count++;
    stat->send_bytes += m->net_msg_size_bytes;
    m->send_time_saved = stat->send_time;
    m->send_next_idle_all_saved = idles->send_next_idle_all;
    m->send_prev_idle_all_saved = idles->send_prev_idle_all;
    if(stat->max_event_size < total_event_size)
        stat->max_event_size = total_event_size;

    printf("%d: to_id:%d now: %8.3lf next_idle_send: %8.3lf\n",
            ns->id, dest_rel_id,
            tw_now(lp), ns->send_next_idle[dest_rel_id]);
    printf("%d: BEFORE all_idles_send %8.3lf %8.3lf\n",
            ns->id, idles->send_prev_idle_all, idles->send_next_idle_all);

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

    printf("%d: AFTER  all_idles_send %8.3lf %8.3lf",
            ns->id, idles->send_prev_idle_all, idles->send_next_idle_all);
    if (m->local_event_size_bytes>0){
        printf(" - with local event\n");
    }
    else{
        printf(" - without local event\n");
    }

    /* create new event to send msg to receiving NIC */
//    printf("\n msg start sending to %d ", dest_id);
    e_new = tw_event_new(dest_id, send_queue_time, lp);
    m_new = tw_event_data(e_new);

    /* copy entire previous message over, including payload from user of
     * this module
     */
    memcpy(m_new, m, m->event_size_bytes + sw_get_msg_sz());
    m_new->event_type = MSG_READY;
    m_new->src_mn_rel_id = ns->id;
    
    tw_event_send(e_new);

    /* if there is a local event to handle, then create an event for it as
     * well
     */
    if(m->local_event_size_bytes > 0)
    {
        char* local_event;

        e_new = tw_event_new(m->src_gid, send_queue_time+codes_local_latency(lp), lp);
        m_new = tw_event_data(e_new);

         local_event = (char*)m;
         local_event += sw_get_msg_sz() + m->event_size_bytes;         	 
        /* copy just the local event data over */
        memcpy(m_new, local_event, m->local_event_size_bytes);
        tw_event_send(e_new);
    }
    return;
}

/* Model-net function calls */

/*This method will serve as an intermediate layer between simplewan and modelnet. 
 * It takes the packets from modelnet layer and calls underlying simplewan methods*/
static tw_stime simplewan_packet_event(
		char* category,
		tw_lpid final_dest_lp,
		int packet_size,
                tw_stime offset,
		int remote_event_size,
		const void* remote_event,
		int self_event_size,
		const void* self_event,
		tw_lp *sender,
		int is_last_pckt)
{
     tw_event * e_new;
     tw_stime xfer_to_nic_time;
     sw_message * msg;
     tw_lpid dest_id;
     char* tmp_ptr;
     char lp_type_name[MAX_NAME_LENGTH], lp_group_name[MAX_NAME_LENGTH];

     int mapping_grp_id, mapping_rep_id, mapping_type_id, mapping_offset;
     codes_mapping_get_lp_info(sender->gid, lp_group_name, &mapping_grp_id, &mapping_type_id, lp_type_name, &mapping_rep_id, &mapping_offset);
     codes_mapping_get_lp_id(lp_group_name, "modelnet_simplewan", mapping_rep_id, mapping_offset, &dest_id);

     xfer_to_nic_time = codes_local_latency(sender);

    printf("%lu: final %lu packet sz %d remote sz %d self sz %d is_last_pckt %d latency %lf\n",
            (dest_id - 1) / 2, final_dest_lp, packet_size, 
            remote_event_size, self_event_size, is_last_pckt,
            xfer_to_nic_time+offset);

     e_new = tw_event_new(dest_id, xfer_to_nic_time+offset, sender);
     msg = tw_event_data(e_new);
     strcpy(msg->category, category);
     msg->final_dest_gid = final_dest_lp;
     msg->src_gid = sender->gid;
     msg->magic = sw_get_magic();
     msg->net_msg_size_bytes = packet_size;
     msg->event_size_bytes = 0;
     msg->local_event_size_bytes = 0;
     msg->event_type = MSG_START;

     tmp_ptr = (char*)msg;
     tmp_ptr += sw_get_msg_sz();
      
    //printf("\n Sending to LP %d msg magic %d ", (int)dest_id, sw_get_magic()); 
     /*Fill in simplewan information*/     
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
      // printf("\n Last packet size: %d ", sw_get_msg_sz() + remote_event_size + self_event_size);
      }
     tw_event_send(e_new);
     return xfer_to_nic_time;
}

static void sw_setup(const void* net_params)
{
  simplewan_param* sw_param = (simplewan_param*)net_params;
  sw_set_params(sw_param->startup_filename, sw_param->bw_filename); 
  //printf("\n Bandwidth setup %lf %lf ", sw_param->net_startup_ns, sw_param->net_bw_mbps);
}

static void simplewan_packet_event_rc(tw_lp *sender)
{
    codes_local_latency_reverse(sender);
    return;
}

static tw_lpid sw_find_local_device(tw_lp *sender)
{
     char lp_type_name[MAX_NAME_LENGTH], lp_group_name[MAX_NAME_LENGTH];
     int mapping_grp_id, mapping_rep_id, mapping_type_id, mapping_offset;
     tw_lpid dest_id;

     codes_mapping_get_lp_info(sender->gid, lp_group_name, &mapping_grp_id, &mapping_type_id, lp_type_name, &mapping_rep_id, &mapping_offset);
     codes_mapping_get_lp_id(lp_group_name, "modelnet_simplewan", mapping_rep_id, mapping_offset, &dest_id);

    return(dest_id);
}

static double sw_get_bw(int from_id, int to_id){
    return global_net_bw_mbs[from_id * num_lps + to_id];
}
static double sw_get_startup(int from_id, int to_id){
    return global_net_startup_ns[from_id * num_lps + to_id];
}

/* category lookup (more or less copied from model_net_find_stats) */
static category_idles* sw_get_category_idles(
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
