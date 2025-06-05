/*
* Copyright (C) 2014 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */
#include <ross.h>
#include <inttypes.h>
#include <stddef.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include "codes/codes-workload.h"
#include "codes/codes.h"
#include "codes/configuration.h"
#include "codes/codes_mapping.h"
#include "codes/model-net.h"
#include "codes/model-net-lp.h"
#include "codes/rc-stack.h"
#include "codes/quicklist.h"
#include "codes/quickhash.h"
#include "codes/codes-jobmap.h"
#include "codes/congestion-controller-core.h"
#include "codes/surrogate/init.h"
#include "surrogate/app-iteration-predictor/common.h"

/* turning on track lp will generate a lot of output messages */
#define DBG_COMM 1
#define MN_LP_NM "modelnet_dragonfly_custom"
#define CONTROL_MSG_SZ 64
#define TRACE -1
#define MAX_WAIT_REQS 2048
#define CS_LP_DBG 1
#define RANK_HASH_TABLE_SZ 8193
#define NW_LP_NM "nw-lp"
#define lprintf(_fmt, ...) \
        do {if (CS_LP_DBG) printf(_fmt, __VA_ARGS__);} while (0)
#define MAX_STATS 65536
#define COL_TAG 1235
#define BAR_TAG 1234
#define PRINT_SYNTH_TRAFFIC 1
#define MAX_JOBS 64
#define MAX_PERIODS_PER_APP 512
#define NEAR_ZERO .0001 //timestamp for use to be 'close to zero' but still allow progress, zero offset events are hard on the PDES engine
#define OUTPUT_MARKS 0
#define LP_DEBUG 0
#define HARD_CODED_AVG_ITER_PREDICTOR 0

static int msg_size_hash_compare(
            void *key, struct qhash_head *link);

static unsigned long perm_switch_thresh = 8388608;

/* NOTE: Message tracking works in sequential mode only! */
static int debug_cols = 0;
static int synthetic_pattern = 1;
/* Turning on this option slows down optimistic mode substantially. Only turn
 * on if you get issues with wait-all completion with traces. */
static int preserve_wait_ordering = 0;
static int enable_msg_tracking = 0;
static int is_synthetic = 0;
static unsigned long long max_gen_data = 0;
static int num_qos_levels;
static double compute_time_speedup;
tw_lpid TRACK_LP = -1;
int nprocs = 0;
static double total_syn_data = 0;
static int unmatched = 0;
char workload_type[128];
char workload_name[128];
char workload_file[8192];
char offset_file[8192];
static int num_net_traces = 0;
static int prioritize_collectives = 0;
static int num_dumpi_traces = 0;
static int num_nonsyn_jobs = 0;
static int num_total_jobs = 0;
static int64_t EAGER_THRESHOLD = 8192;

// static int upper_threshold = 1048576;
static int alloc_spec = 0;
static tw_stime self_overhead = 10.0;
static tw_stime mean_interval = 100000;
static int payload_sz = 1024;

/* Doing LP IO*/
static char * params = NULL;
static char lp_io_dir[256] = {'\0'};
static char sampling_dir[32] = {'\0'};
static char mpi_msg_dir[32] = {'\0'};
static lp_io_handle io_handle;
static unsigned int lp_io_use_suffix = 0;
static int do_lp_io = 0;

/* variables for loading multiple applications */
char workloads_conf_file[8192];
char workloads_timer_file[8192];
char workloads_period_file[8192];
char alloc_file[8192];
int num_traces_of_job[MAX_JOBS];
int is_job_synthetic[MAX_JOBS]; //0 if job is not synthetic 1 if job is
int qos_level_of_job[MAX_JOBS];
float mean_interval_of_job[MAX_JOBS];
long job_timer1[MAX_JOBS];
long job_timer2[MAX_JOBS];
int period_count[MAX_JOBS];
double period_time[MAX_JOBS][MAX_PERIODS_PER_APP];
float period_interval[MAX_JOBS][MAX_PERIODS_PER_APP];
char file_name_of_job[MAX_JOBS][8192];
char skipping_iterations_file[8192];

tw_stime max_elapsed_time_per_job[MAX_JOBS] = {0};

/* On-node delay variables */
tw_stime soft_delay_mpi = 2500;
tw_stime nic_delay = 1000;
tw_stime copy_per_byte_eager = 0.55;

static struct codes_jobmap_ctx *jobmap_ctx;
static struct codes_jobmap_params_list jobmap_p;
static struct codes_jobmap_params_identity jobmap_ident_p; // for if an alloc file isn't supplied.


/* Variables for Cortex Support */
/* Matthieu's additions start */
#ifdef ENABLE_CORTEX_PYTHON
static char cortex_file[512] = "\0";
static char cortex_class[512] = "\0";
static char cortex_gen[512] = "\0";
#endif
/* Matthieu's additions end */

typedef struct nw_state nw_state;
typedef struct nw_message nw_message;
typedef unsigned int dumpi_req_id;

static int net_id = 0;
// static float noise = 1.0;
static int num_nw_lps = 0, num_mpi_lps = 0;

static int num_syn_clients;
static int syn_type = 0;

FILE * workload_log = NULL;
FILE * msg_size_log = NULL;
FILE * iteration_log = NULL;
FILE * workload_agg_log = NULL;
FILE * workload_meta_log = NULL;

static uint64_t sample_bytes_written = 0;

unsigned long long num_bytes_sent=0;
unsigned long long num_bytes_recvd=0;

unsigned long long num_syn_bytes_sent = 0;
unsigned long long num_syn_bytes_recvd = 0;

double max_time = 0,  max_comm_time = 0, max_wait_time = 0, max_send_time = 0, max_recv_time = 0;
double avg_time = 0, avg_comm_time = 0, avg_wait_time = 0, avg_send_time = 0, avg_recv_time = 0;


/* runtime option for disabling computation time simulation */
static int disable_delay = 0;
static int enable_sampling = 0;
static double sampling_interval = 5000000;
static double sampling_end_time = 3000000000;
static int enable_debug = 0;

// Surrogate variables
struct app_iteration_predictor *iter_predictor = NULL;
static int nw_id_counter = 0;
// We can skip multiple iterations using an average as our predicted iteration time. This will skip ahead to a future step in the simulation
static struct AvgSurrogateSwitchingTimesForApp *skip_iter_config;
static size_t skip_iter_config_size = 0;

/* set group context */
struct codes_mctx mapping_context;
enum MAPPING_CONTEXTS
{
    GROUP_RATIO=1,
    GROUP_RATIO_REVERSE,
    GROUP_DIRECT,
    GROUP_MODULO,
    GROUP_MODULO_REVERSE,
    UNKNOWN
};
static int map_ctxt = GROUP_MODULO;

/* MPI_OP_GET_NEXT is for getting next MPI operation when the previous operation completes.
* MPI_SEND_ARRIVED is issued when a MPI message arrives at its destination (the message is transported by model-net and an event is invoked when it arrives.
* MPI_SEND_POSTED is issued when a MPI message has left the source LP (message is transported via model-net). */
enum MPI_NW_EVENTS
{
	MPI_OP_GET_NEXT=1,
	MPI_SEND_ARRIVED,
    MPI_SEND_ARRIVED_CB, // for tracking message times on sender
	MPI_SEND_POSTED,
    MPI_REND_ARRIVED,
    MPI_REND_ACK_ARRIVED,
    CLI_BCKGND_FIN,
    CLI_BCKGND_ARRIVE,
    CLI_BCKGND_GEN,
    CLI_BCKGND_CHANGE,
    CLI_NBR_FINISH,
    CLI_OTHER_FINISH, //received when another workload has finished
    // Surrogate events
    SURR_SKIP_ITERATION, // skips one (several) iteration(s) of simulation
};

/* type of synthetic traffic */
enum TRAFFIC
{
    UNIFORM = 1, /* sends message to a randomly selected node */
    NEAREST_NEIGHBOR = 2, /* sends message to the next node (potentially connected to the same router) */
    ALLTOALL = 3, /* sends message to all other nodes */
    STENCIL = 4, /* sends message to 4 nearby neighbors */
    PERMUTATION = 5,
    BISECTION = 6
};
struct mpi_workload_sample
{
    /* Sampling data */
    int nw_id;
    int app_id;
    unsigned long num_sends_sample;
    unsigned long num_bytes_sample;
    unsigned long num_waits_sample;
    double sample_end_time;
};
/* stores pointers of pending MPI operations to be matched with their respective sends/receives. */
struct mpi_msgs_queue
{
    int op_type;
    int tag;
    int source_rank;
    int dest_rank;
    int64_t num_bytes;
    tw_stime req_init_time;
	dumpi_req_id req_id;
    struct qlist_head ql;
};

/* stores request IDs of completed MPI operations (Isends or Irecvs) */
struct completed_requests
{
	unsigned int req_id;
    int index; // for rollbacking
    struct qlist_head ql;
};

/* for wait operations, store the pending operation and number of completed waits so far. */
struct pending_waits
{
    int op_type;
    unsigned int req_ids[MAX_WAIT_REQS];
	int num_completed;
	int count;
    tw_stime start_time;
};

struct msg_size_info
{
    int64_t msg_size;
    int num_msgs;
    tw_stime agg_latency;
    tw_stime avg_latency;
    struct qhash_head  hash_link;
    struct qlist_head ql; 
};

struct ross_model_sample
{
    tw_lpid nw_id;
    int app_id;
    int local_rank;
    unsigned long num_sends;
    unsigned long num_recvs;
    unsigned long long num_bytes_sent;
    unsigned long long num_bytes_recvd;
    double send_time;
    double recv_time;
    double wait_time;
    double compute_time;
    double comm_time;
    double max_time;
    double avg_msg_time;
};

typedef struct mpi_msgs_queue mpi_msgs_queue;
typedef struct completed_requests completed_requests;
typedef struct pending_waits pending_waits;

/*
 * state of the network LP. It contains the pointers to send/receive lists
 *
 * nw-lp's can only run one job! Which all start at time 0
 *
 * Three possible states for nw-lp:
 * - run application (non-synthetic workload)
 * - run background noise pattern (synthetic workload)
 * - do nothing
 **/
struct nw_state
{
#if LP_DEBUG
	size_t num_events_processed;
#endif /* if LP_DEBUG */

    tw_lpid nw_id;  // compute node id, as labeled by the network
    tw_lpid nw_id_in_pe;  // compute node id for this PE
    int local_rank; // id local to the application or synthetic workload, this is the number that the application sees, their phony "MPI rank"

    // Parameters used for non-synthetic workloads
    short wrkld_id; // workload machinery in charge, e.g, swm
    int app_id;     // application id, position on the queue for this app to run
    int * known_completed_jobs; //array of whether this rank knows other jobs are completed.
    struct rc_stack * processed_ops;
    struct rc_stack * processed_wait_op;
    struct rc_stack * matched_reqs;
    struct pending_waits * wait_op; // Pending wait operation

    // Parameters used for synthetic workload parameters
    int synthetic_pattern;
    int is_finished;
    int num_own_job_ranks_completed; //counted by the root rank 0 of a job

    int qos_level;

    /* count of sends, receives, collectives and delays */
	unsigned long num_sends;
	unsigned long num_recvs;
	unsigned long num_cols;
	unsigned long num_delays;
	unsigned long num_wait;
	unsigned long num_waitall;
	unsigned long num_waitsome;


	/* time spent by the LP in executing the app trace*/
	double start_time;

    double col_time;

    double reduce_time;
    int num_reduce;

    double all_reduce_time;
    int num_all_reduce;

	double elapsed_time;
	/* time spent in compute operations */
	double compute_time;
	/* time spent in message send/isend */
	double send_time;
    /* max time for synthetic traffic message */
    double max_time;
	/* time spent in message receive */
	double recv_time;
	/* time spent in wait operation */
	double wait_time;
	/* FIFO for isend messages arrived on destination */
	struct qlist_head arrival_queue;
	/* FIFO for irecv messages posted but not yet matched with send operations */
	struct qlist_head pending_recvs_queue;
	/* List of completed send/receive requests */
	struct qlist_head completed_reqs;

    tw_stime cur_interval_end;

    /* Message size latency information */
    struct qhash_table * msg_sz_table;
    struct qlist_head msg_sz_list;

    /* quick hash for maintaining message latencies */

    unsigned long long num_bytes_sent;
    unsigned long long num_bytes_recvd;

    unsigned long long syn_data;
    unsigned long long gen_data;
  
    unsigned long prev_switch;
    int saved_perm_dest;
    unsigned long rc_perm;

    /* For sampling data */
    int sampling_indx;
    int max_arr_size;
    struct mpi_workload_sample * mpi_wkld_samples;
    char output_buf[512];
    char col_stats[64];
    struct ross_model_sample ross_sample;

    // Configuration to tell the node when to skip some iterations
    struct AvgSurrogateSwitchingTimesForApp *switch_config;
    size_t switch_config_size;
};

/* data for handling reverse computation.
* saved_matched_req holds the request ID of matched receives/sends for wait operations.
* ptr_match_op holds the matched MPI operation which are removed from the queues when a send is matched with the receive in forward event handler.
* network event being sent. op is the MPI operation issued by the network workloads API. rv_data holds the data for reverse computation (TODO: Fill this data structure only when the simulation runs in optimistic mode). */
struct nw_message
{
   // forward message handler
   enum MPI_NW_EVENTS msg_type;
   int op_type;
   int num_rngs;
   model_net_event_return event_rc;
   struct codes_workload_op * mpi_op;

   struct
   {
       tw_lpid src_rank;
       int dest_rank;
       int64_t num_bytes;
       int num_matched;
       double sim_start_time;
       // for callbacks - time message was received
       double msg_send_time;
       unsigned int req_id;
       int matched_req;
       int tag;
       int app_id;
       int found_match;
       short wait_completed;
       short rend_send;
       int resume_at_iter;
   } fwd;

   // A different struct for each type of MPI_NW_EVENTS
   union {
       // For CLI_BCKGND_GEN
       struct {
           int saved_syn_length;
           int saved_perm;  // Used by PERMUTATION
           unsigned long saved_prev_switch;  // Used by PERMUTATION
       } gen;

       // For CLI_BCKGND_ARRIVE and MPI_SEND_ARRIVED_CB
       struct {
           double saved_prev_max_time;
           double saved_send_time;
           double saved_send_time_sample;
       } arrive;

       // For CLI_BCKGND_CHANGE
       struct {
           double saved_send_time;
           double saved_marker_time;
       } change;

       // For MPI_OP_GET_NEXT there are also different types
       struct {
	       double saved_elapsed_time;
           union {
               // CODES_WK_ALLREDUCE
               struct {
                   double saved_send_time;
                   double saved_delay;
               } all_reduce;
               // CODES_WK_RECV and CODES_WK_IRECV
               struct {
                   double saved_recv_time;
                   double saved_recv_time_sample;
               } recv;
               // CODES_WK_DELAY
               struct {
                   double saved_delay;
                   double saved_delay_sample;
               } delay;
               // CODES_WK_END and CODES_WK_MARK
               struct {
                   double saved_marker_time;
               } mark;
           };
       } mpi_next;

       // For MPI_SEND_ARRIVED and MPI_REND_ARRIVED and MPI_SEND_POSTED
       struct {
           double saved_wait_time;
           double saved_wait_time_sample;
           double saved_recv_time;
           double saved_recv_time_sample;
           int64_t saved_num_bytes;
       } mpi_send;

       // For MPI_REND_ACK_ARRIVED
       struct {
           int64_t saved_num_bytes;
       } mpi_ack;

        // For SURR_SKIP_ITERATION
       struct {
           struct AvgSurrogateSwitchingTimesForApp * config_used;
       } surr;
   } rc;
};

static void send_ack_back(nw_state* s, tw_bf * bf, nw_message * m, tw_lp * lp, mpi_msgs_queue * mpi_op, int matched_req);

static void send_ack_back_rc(nw_state* s, tw_bf * bf, nw_message * m, tw_lp * lp);
/* executes MPI isend and send operations */
static void codes_exec_mpi_send(
        nw_state* s, tw_bf * bf, nw_message * m, tw_lp* lp, struct codes_workload_op * mpi_op, int is_rend);
/* execute MPI irecv operation */
static void codes_exec_mpi_recv(
        nw_state* s, tw_bf * bf, nw_message * m, tw_lp * lp, struct codes_workload_op * mpi_op);
/* reverse of mpi recv function. */
static void codes_exec_mpi_recv_rc(
        nw_state* s, tw_bf * bf, nw_message* m, tw_lp* lp);
/* execute the computational delay */
static void codes_exec_comp_delay(
        nw_state* s, tw_bf *bf, nw_message * m, tw_lp* lp, struct codes_workload_op * mpi_op);
/* gets the next MPI operation from the network-workloads API. */
static void get_next_mpi_operation(
        nw_state* s, tw_bf * bf, nw_message * m, tw_lp * lp);
/* reverse handler of get next mpi operation. */
static void get_next_mpi_operation_rc(
        nw_state* s, tw_bf * bf, nw_message * m, tw_lp * lp);
/* Makes a call to get_next_mpi_operation. */
static void codes_issue_next_event(tw_lp* lp);
/* reverse handler of next operation */
static void codes_issue_next_event_rc(tw_lp* lp);


///////////////////// HELPER FUNCTIONS FOR MPI MESSAGE QUEUE HANDLING ///////////////
/* upon arrival of local completion message, inserts operation in completed send queue */
/* upon arrival of an isend operation, updates the arrival queue of the network */
static void update_completed_queue(
        nw_state * s, tw_bf * bf, nw_message * m, tw_lp * lp, dumpi_req_id req_id);
/* reverse of the above function */
static void update_completed_queue_rc(
        nw_state*s,
        tw_bf * bf,
        nw_message * m,
        tw_lp * lp);
static void update_arrival_queue(
        nw_state*s, tw_bf* bf, nw_message* m, tw_lp * lp);
/* reverse of the above function */
static void update_arrival_queue_rc(
        nw_state*s, tw_bf* bf, nw_message* m, tw_lp * lp);
/* callback to a message sender for computing message time */
static void update_message_time(
        nw_state*s, tw_bf* bf, nw_message* m, tw_lp * lp);
/* reverse for computing message time */
static void update_message_time_rc(
        nw_state*s, tw_bf* bf, nw_message* m, tw_lp * lp);

/* conversion from seconds to eanaoseconds */
static tw_stime s_to_ns(tw_stime ns);

/*static void update_message_size_rc(
        struct nw_state * ns,
        tw_lp * lp,
        tw_bf * bf,
        struct nw_message * m)
{*/
/*TODO: Complete reverse handler */
/*    (void)ns;
    (void)lp;
    (void)bf;
    (void)m;
}*/
/* update the message size */
static void update_message_size(
        struct nw_state * ns,
        tw_lp * lp,
        tw_bf * bf,
        struct nw_message * m,
        mpi_msgs_queue * qitem,
        int is_eager,
        int is_send)
{
            (void)bf;
            (void)is_eager;

            struct qhash_head * hash_link = NULL;
            tw_stime msg_init_time = qitem->req_init_time;
        
            if(ns->msg_sz_table == NULL)
                ns->msg_sz_table = qhash_init(msg_size_hash_compare, quickhash_64bit_hash, RANK_HASH_TABLE_SZ); 
            
            hash_link = qhash_search(ns->msg_sz_table, &(qitem->num_bytes));

            if(is_send)
                msg_init_time = m->fwd.sim_start_time;
            
            /* update hash table */
            if(!hash_link)
            {
                struct msg_size_info * msg_info = (struct msg_size_info*)malloc(sizeof(struct msg_size_info));
                msg_info->msg_size = qitem->num_bytes;
                msg_info->num_msgs = 1;
                msg_info->agg_latency = tw_now(lp) - msg_init_time;
                msg_info->avg_latency = msg_info->agg_latency;
                assert(ns->msg_sz_table);
                qhash_add(ns->msg_sz_table, &(msg_info->msg_size), &(msg_info->hash_link));
                qlist_add(&msg_info->ql, &ns->msg_sz_list);
                //printf("\n Msg size %d aggregate latency %f num messages %d ", m->fwd.num_bytes, msg_info->agg_latency, msg_info->num_msgs);
            }
            else
            {
                struct msg_size_info * tmp = qhash_entry(hash_link, struct msg_size_info, hash_link);
                tmp->num_msgs++;
                tmp->agg_latency += tw_now(lp) - msg_init_time;  
                tmp->avg_latency = (tmp->agg_latency / tmp->num_msgs);
//                printf("\n Msg size %lld aggregate latency %f num messages %d ", qitem->num_bytes, tmp->agg_latency, tmp->num_msgs);
            }
}
static void notify_background_traffic_rc(
	    struct nw_state * ns,
        tw_lp * lp,
        tw_bf * bf,
        struct nw_message * m)
{
    (void)ns;
    (void)bf;
    (void)m;
        
    int num_jobs = codes_jobmap_get_num_jobs(jobmap_ctx); 
    
    for(int i = 0; i < num_jobs - 1; i++)
    {
        if(is_job_synthetic[i] == 0)
            continue;
    }
}

static void notify_background_traffic(
	    struct nw_state * ns,
        tw_lp * lp,
        tw_bf * bf,
        struct nw_message * m)
{
        (void)bf;
        (void)m;

        struct codes_jobmap_id jid; 
        jid = codes_jobmap_to_local_id(ns->nw_id, jobmap_ctx);
        
        int num_jobs = codes_jobmap_get_num_jobs(jobmap_ctx); 
        
        for(int other_id = 0; other_id < num_jobs; other_id++)
        {
            if(is_job_synthetic[other_id] == 0) //we only want to notify the synthetic (background) ranks
                continue;
            assert(other_id != jid.job);

            struct codes_jobmap_id other_jid;
            other_jid.job = other_id;

            int num_other_ranks = codes_jobmap_get_num_ranks(other_id, jobmap_ctx);

            lprintf("\n Other ranks %d ", num_other_ranks);
            tw_stime ts = NEAR_ZERO;
            tw_lpid global_dest_id;
     
            for(int k = 0; k < num_other_ranks; k++)    
            {
                other_jid.rank = k;
                int intm_dest_id = codes_jobmap_to_global_id(other_jid, jobmap_ctx); 
                global_dest_id = codes_mapping_get_lpid_from_relative(intm_dest_id, NULL, NW_LP_NM, NULL, 0);

                tw_event * e;
                struct nw_message * m_new;  
                e = tw_event_new(global_dest_id, ts, lp);
                m_new = (struct nw_message*)tw_event_data(e);
                m_new->msg_type = CLI_BCKGND_FIN;
                tw_event_send(e);   
            }
        }
        return;
}

static void notify_root_workload_rc(
        struct nw_state * ns,
        tw_lp * lp,
        tw_bf * bf,
        struct nw_message * m)
{
    (void)ns;
    (void)bf;
    (void)m;
}

//notifies the lowest nonsynthetic job that this one has completed
//this is important for synthetic ranks that continue generating until all non-synthetic have completed
static void notify_root_workload(
        struct nw_state * ns,
        tw_lp * lp,
        tw_bf * bf,
        struct nw_message * m)
{
    (void)bf;
    (void)m;

    struct codes_jobmap_id jid;
    jid = codes_jobmap_to_local_id(ns->nw_id, jobmap_ctx);

    int num_jobs = codes_jobmap_get_num_jobs(jobmap_ctx);

    int lowest_non_synth_job_id;
    for (int other_id = 0; other_id < num_jobs; other_id++)
    {
        if (!is_job_synthetic[other_id]) {
            lowest_non_synth_job_id = other_id;
            break;
        }
    }
    struct codes_jobmap_id other_jid;
    other_jid.job = 0;
    other_jid.rank = 0; //root rank

    int intm_dest_id = codes_jobmap_to_global_id(other_jid, jobmap_ctx);
    tw_lpid global_dest_id = codes_mapping_get_lpid_from_relative(intm_dest_id, NULL, NW_LP_NM, NULL, 0);

    tw_event *e;
    struct nw_message *m_new;
    tw_stime ts = NEAR_ZERO;
    e = tw_event_new(global_dest_id, ts, lp);
    m_new = (struct nw_message*)tw_event_data(e);
    m_new->msg_type = CLI_OTHER_FINISH;
    m_new->fwd.app_id = ns->app_id; //just borrowing the fwd struct even though this isn't an injected message
    tw_event_send(e);
}

void handle_other_finish_rc(
    struct nw_state * ns,
    tw_lp * lp,
    tw_bf * bf,
    struct nw_message *m)
{
    ns->known_completed_jobs[m->fwd.app_id] = 0;
    if(bf->c2)
        notify_background_traffic_rc(ns, lp, bf, m);

}

//for nonsynthetic jobs to determine if they have all completed
//the highest ordered non synthetic job then notifies all synthetic ranks to stop generating traffic
void handle_other_finish(
    struct nw_state * ns,
    tw_lp * lp,
    tw_bf * bf,
    struct nw_message *m)
{
    // printf("APP %d RANK %d RECEIVED COMPLETION NOTICE\n",ns->app_id, ns->local_rank);
    assert(ns->app_id == 0); //make sure that only the root workload is getting this notification
    assert(ns->local_rank == 0); //make sure that only the root rank is getting this notification

    printf("App %d: Received finished workload notification",ns->app_id);
    // if(is_job_synthetic[ns->app_id])
        // return; //nothing for synthetic (background) ranks to do here
    // printf(" And I am not synthetic\n");
    int num_jobs = codes_jobmap_get_num_jobs(jobmap_ctx);

    ns->known_completed_jobs[m->fwd.app_id] = 1;
    int total_completed_jobs = 0;
    int total_non_syn_completed_jobs = 0;

    //Find number of completed non synthetic jobs
    for(int i = 0; i < num_jobs; i++)
    {
        // if (is_job_synthetic[i] == 0) {
            total_non_syn_completed_jobs += ns->known_completed_jobs[i];
        // }
    }
    
    if (total_non_syn_completed_jobs == num_nonsyn_jobs) //then all nonsynthetic jobs have completed, the background synthetic workloads must be notified
    {
        printf("App %d: All non-synthetic workloads have completed\n", ns->app_id);
        //Determine which job should be the one to notify all the background ranks
        //Let's say: highest numbered non-synthetic job (found above)
        // also make sure that there actually are synthetic jobs
        if (total_non_syn_completed_jobs < num_jobs)
        {
            if(max_gen_data <= 0) {
                printf("App %d: Notifying background traffic\n", ns->app_id);
                bf->c2 = 1;
                //If I am the last rank in the highest ordered non-synthetic job, then it is my job to notify
                notify_background_traffic(ns, lp, bf, m);
            }
            else {
                printf("App %d: Not notifying background traffic as max_gen_data > 0. Will let synthetic workloads finish",ns->app_id);
            }
        }

        //TODO because we're treating synthetic workloads differently based on whether or not there's max_gen_data set, creating this notification here at this exact spot in logic
        //will result in LPs receiving a "wrap up" notification potentially before synthetic workloads are done. To fix this, then we need to start treating synthetic workloads
        //with a max_gen_data set differently, so that they communicate with other workloads when they're done in the same way we do with regular workloads
        //this notification would then be sent by a single workload LP that knows for a fact that all other workloads have completed.
        //also this, to be more accurate, should only be sent when all of the workload LPs have received all messages that have been sent so that we know that no packets
        //are currently in transit. This currently isn't measured for model_net_mpi_replay.

        //send to all non nw-lp LPs (all model net, is there a function taht does this?)
        model_net_method_end_sim_broadcast(NEAR_ZERO, lp);
    }
    else
    {
        printf("There is still a nonsynethic workload left. %d != %d\n",total_non_syn_completed_jobs, num_nonsyn_jobs);
    }
}

static void handle_neighbor_finish_rc(
        struct nw_state * ns,
        tw_lp * lp,
        tw_bf * bf,
        struct nw_message *m)
{
    ns->num_own_job_ranks_completed--;

    if (bf->c1) {
        notify_root_workload_rc(ns, lp, bf, m);
    }
}

//Called by the root rank of the application when it receives a rank completion notification from one of its own job's ranks
static void handle_neighbor_finish(
        struct nw_state * ns,
        tw_lp * lp,
        tw_bf * bf,
        struct nw_message *m)
{
    ns->num_own_job_ranks_completed++;

    //have all of the ranks from our job completed?
    if (ns->num_own_job_ranks_completed == codes_jobmap_get_num_ranks(ns->app_id, jobmap_ctx)) {
        bf->c1 = 1;
        notify_root_workload(ns, lp, bf, m);
    }
}

static void notify_root_rank_rc(
        struct nw_state * ns,
        tw_lp * lp,
        tw_bf * bf,
        struct nw_message *m)
{

}

static void notify_root_rank(
        struct nw_state * ns,
        tw_lp * lp,
        tw_bf * bf,
        struct nw_message *m)
{
    assert(ns->is_finished);

    tw_stime ts = NEAR_ZERO;
    
    struct codes_jobmap_id root_jid;
    root_jid.job = ns->app_id;
    root_jid.rank = 0;
    tw_lpid global_dest_id;

    /* Send a notification to the root neighbor about completion */
    int intm_dest_id = codes_jobmap_to_global_id(root_jid, jobmap_ctx); 
    global_dest_id = codes_mapping_get_lpid_from_relative(intm_dest_id, NULL, NW_LP_NM, NULL, 0);
    
    tw_event * e;
    struct nw_message * m_new;
    e = tw_event_new(global_dest_id, ts, lp);
    m_new = (struct nw_message*)tw_event_data(e);
    m_new->msg_type = CLI_NBR_FINISH;
    tw_event_send(e);
}

void finish_bckgnd_traffic_rc(
    struct nw_state * ns,
    tw_bf * b,
    struct nw_message * msg,
    tw_lp * lp)
{
        (void)b;
        (void)msg;
        (void)lp;

        ns->is_finished = 0;
        ns->elapsed_time = msg->rc.mpi_next.saved_elapsed_time;
        return;
}
void finish_bckgnd_traffic(
    struct nw_state * ns,
    tw_bf * b,
    struct nw_message * msg,
    tw_lp * lp)
{
        (void)b;
        (void)msg;
        ns->is_finished = 1;
        msg->rc.mpi_next.saved_elapsed_time = ns->elapsed_time;
        ns->elapsed_time = tw_now(lp) - ns->start_time;

        printf("\n LP %llu App %d completed sending data %llu completed at time %lf ", LLU(lp->gid),ns->app_id, ns->gen_data, tw_now(lp));
        
        return;
}

static void gen_synthetic_tr_rc(nw_state * s, tw_bf * bf, nw_message * m, tw_lp * lp)
{
    if(bf->c0)
        return;

    if(bf->c1)
    {
        tw_rand_reverse_unif(lp->rng);
    }
    if(bf->c2)
    {
        s->prev_switch = m->rc.gen.saved_prev_switch;
        s->saved_perm_dest = m->rc.gen.saved_perm;
        tw_rand_reverse_unif(lp->rng);
    }
    int i;
    for (i=0; i < m->rc.gen.saved_syn_length; i++){
        model_net_event_rc2(lp, &m->event_rc);
        s->gen_data -= payload_sz;
        num_syn_bytes_sent -= payload_sz;
        s->num_bytes_sent -= payload_sz;
        s->ross_sample.num_bytes_sent -= payload_sz;
    }
        // tw_rand_reverse_unif(lp->rng);
        s->num_sends--;
        s->ross_sample.num_sends--;

     if(bf->c5)
        finish_bckgnd_traffic_rc(s, bf, m, lp);
    if(bf->c7) {
        s->saved_perm_dest = m->rc.gen.saved_perm;
        tw_rand_reverse_unif(lp->rng);
    }
}

/* generate synthetic traffic */
static void gen_synthetic_tr(nw_state * s, tw_bf * bf, nw_message * m, tw_lp * lp)
{
    if(s->is_finished == 1)
    {
        bf->c0 = 1;
        return;
    }

    /* Get job information */
    tw_lpid global_dest_id;
    int intm_dest_id;
    nw_message remote_m;

    struct codes_jobmap_id jid;
    jid = codes_jobmap_to_local_id(s->nw_id, jobmap_ctx); 

    int num_clients = codes_jobmap_get_num_ranks(jid.job, jobmap_ctx);

    /* Find destination */
    int* dest_svr = NULL; 
    int i, length=0;
    switch(s->synthetic_pattern)
    {
        case UNIFORM:
        {
            bf->c1 = 1;
            length = 1;
            dest_svr = (int*) calloc(1, sizeof(int));
            dest_svr[0] = tw_rand_integer(lp->rng, 0, num_clients - 1);
            if(dest_svr[0] == s->local_rank)
                dest_svr[0] = (s->local_rank + 1) % num_clients;
        }
        break;

        case PERMUTATION:
        {
            m->rc.gen.saved_prev_switch = s->prev_switch; //for reverse computation
            m->rc.gen.saved_perm = s->saved_perm_dest;

            length = 1;
            dest_svr = (int*) calloc(1, sizeof(int));
            if(s->gen_data == 0)
            {
                /*initialize the perm destination to something that is nonzero and thus preventing 
                  possible attempt at self message*/
                bf->c7 = 1;
                s->saved_perm_dest = tw_rand_integer(lp->rng, 0, num_clients - 1);
                if (s->saved_perm_dest == s->local_rank)
                    s->saved_perm_dest = (s->local_rank + num_clients/2) % num_clients;
            }

            if(s->gen_data - s->prev_switch >= perm_switch_thresh)
            {
                // printf("%d - %d >= %d\n",s->gen_data,s->prev_switch,perm_switch_thresh);
                bf->c2 = 1;
                s->prev_switch = s->gen_data; //Amount of data pushed at time when switch initiated
                dest_svr[0] = tw_rand_integer(lp->rng, 0, num_clients - 1);
                if(dest_svr[0] == s->local_rank)
                    dest_svr[0] = (s->local_rank + num_clients/2) % num_clients;
                s->saved_perm_dest = dest_svr[0];
                assert(s->saved_perm_dest != s->local_rank);
            }
            else
                dest_svr[0] = s->saved_perm_dest;

            assert(dest_svr[0] != s->local_rank);
        }
        break;
        case NEAREST_NEIGHBOR:
        {
            length = 1;
            dest_svr = (int*) calloc(1, sizeof(int));
            dest_svr[0] = (s->local_rank + 1) % num_clients;
        }
        break;
        case BISECTION:
        {
            length = 1;
            dest_svr = (int*) calloc(1, sizeof(int));
            dest_svr[0] = (s->local_rank + (num_clients/2)) % num_clients;
        }
        break;
        case ALLTOALL:
        {
            dest_svr = (int*) calloc(num_clients-1, sizeof(int));
            int index = 0;
            for (i=0;i<num_clients;i++)
            {
                if(i!=s->local_rank) 
                {
                    dest_svr[index] = i;
                    index++;
                    length++;
                }
            }
        }
        break;
        case STENCIL:  //2D 4-point stencil
        {
            /* I think this code snippet is coming from the LLNL stencil patterns. */
            int digits, x=1, y=1, row, col, temp=num_clients;
            length = 4;
            dest_svr = (int*) calloc(4, sizeof(int));
            for (digits = 0; temp > 0; temp >>= 1)
                digits++;
            digits = digits/2;
            for (i = 0; i < digits; i++)
                x = x * 2;
            y = num_clients / x;
            //printf("\nStencil Syn: x=%d, y=%d", x, y);
            row = s->local_rank / y;
            col = s->local_rank % y;

            dest_svr[0] = row * y + ((col-1+y)%y);   /* left neighbor */
            dest_svr[1] = row * y + ((col+1+y)%y);   /* right neighbor */
            dest_svr[2] = ((row-1+x)%x) * y + col;   /* bottom neighbor */
            dest_svr[3] = ((row+1+x)%x) * y + col;   /* up neighbor */
        }
        break;
        default:
            tw_error(TW_LOC, "Undefined traffic pattern");
    }   
    /* Record length for reverse handler*/
    m->rc.gen.saved_syn_length = length;

    char prio[12];
	switch(s->qos_level){
		case 0:
			strcpy(prio, "high"); break;
		case 1:
			strcpy(prio, "medium"); break;
		case 2:
			strcpy(prio, "low"); break;
		case 3:
			strcpy(prio, "class3"); break;
		case 4:
			strcpy(prio, "class4"); break;
		case 5:
			strcpy(prio, "class5"); break;
		default:
			tw_error(TW_LOC, "\n Invalid QoS level: %d", s->qos_level);
			break;
	}
	
	// TODO: Check if I can combine these two if statements. -Kevin
	if(tw_now(lp) < job_timer2[s->app_id] && job_timer1[s->app_id] > 0){
		if(tw_now(lp) > job_timer1[s->app_id]){
			length = 0;
		}
	}
    if(length > 0)
    {
        // m->event_array_rc = (model_net_event_return) malloc(length * sizeof(model_net_event_return));
        //printf("\nRANK %d Dests %d", s->local_rank, length);
        for (i = 0; i < length; i++)
        {
            /* Generate synthetic traffic */
            jid.rank = dest_svr[i];
            intm_dest_id = codes_jobmap_to_global_id(jid, jobmap_ctx); 
            global_dest_id = codes_mapping_get_lpid_from_relative(intm_dest_id, NULL, NW_LP_NM, NULL, 0);

            remote_m.fwd.sim_start_time = tw_now(lp);
            remote_m.fwd.dest_rank = dest_svr[i];
            remote_m.msg_type = CLI_BCKGND_ARRIVE;
            remote_m.fwd.num_bytes = payload_sz;
            remote_m.fwd.app_id = s->app_id;
            remote_m.fwd.src_rank = s->local_rank;

            // printf("\nAPP %d SRC %d Dest %d (twid %llu)", jid.job, s->local_rank, dest_svr[i], global_dest_id);
            m->event_rc = model_net_event(net_id, prio, global_dest_id, payload_sz, 0.0,
                    sizeof(nw_message), (const void*)&remote_m, 
                    0, NULL, lp);
            
            s->gen_data += payload_sz;
            s->num_bytes_sent += payload_sz;
            s->ross_sample.num_bytes_sent += payload_sz;
            num_syn_bytes_sent += payload_sz; 
        }
    }
    s->num_sends++;
    s->ross_sample.num_sends++;

    /* New event after MEAN_INTERVAL */  
    tw_stime ts = mean_interval_of_job[s->app_id];
    tw_event * e;
    nw_message * m_new;
    e = tw_event_new(lp->gid, ts, lp);
    m_new = (struct nw_message*)tw_event_data(e);
    m_new->msg_type = CLI_BCKGND_GEN;
    tw_event_send(e);
    
    if (max_gen_data != 0) { //max_gen_data is by default 0 (off). If it's on, then we use it to determine when to finish synth ranks
        if(s->gen_data >= max_gen_data) {
            bf->c5 = 1;
            finish_bckgnd_traffic(s, bf, m, lp);
        }
    }

    free(dest_svr);
}

void arrive_syn_tr_rc(nw_state * s, tw_bf * bf, nw_message * m, tw_lp * lp)
{
    (void)bf;
    (void)m;
    (void)lp;
//    printf("\n Data arrived %d total data %ld ", m->fwd.num_bytes, s->syn_data);
    s->num_recvs--;
    s->ross_sample.num_recvs--;
    int data = m->fwd.num_bytes;
    s->syn_data -= data;
    num_syn_bytes_recvd -= data;
    s->num_bytes_recvd -= data;
    s->ross_sample.num_bytes_recvd -= data;
    s->send_time = m->rc.arrive.saved_send_time;
    s->ross_sample.send_time = m->rc.arrive.saved_send_time_sample;
    if(bf->c0)
    {
        s->max_time = m->rc.arrive.saved_prev_max_time;
        s->ross_sample.max_time = m->rc.arrive.saved_prev_max_time;
    }
}
void arrive_syn_tr(nw_state * s, tw_bf * bf, nw_message * m, tw_lp * lp)
{
    (void)bf;
    (void)lp;
    m->rc.arrive.saved_send_time = s->send_time;
    m->rc.arrive.saved_send_time_sample = s->ross_sample.send_time;
    if((tw_now(lp) - m->fwd.sim_start_time) > s->max_time)
    {
        bf->c0 = 1;
        m->rc.arrive.saved_prev_max_time = s->max_time;
        s->max_time = tw_now(lp) - m->fwd.sim_start_time;
        s->ross_sample.max_time = tw_now(lp) - m->fwd.sim_start_time;
    }

    s->send_time += (tw_now(lp) - m->fwd.sim_start_time);
    s->ross_sample.send_time += (tw_now(lp) - m->fwd.sim_start_time);
    s->num_recvs++;
    s->ross_sample.num_recvs++;
    int data = m->fwd.num_bytes;
    s->syn_data += data;
    s->num_bytes_recvd += data;
    s->ross_sample.num_bytes_recvd += data;
    num_syn_bytes_recvd += data;

    if(PRINT_SYNTH_TRAFFIC) {
        if(s->local_rank == 0)
        {
            printf("\n Data arrived %llu rank %llu total data %llu ", LLU(m->fwd.num_bytes), LLU(s->nw_id), s->syn_data);
    /*	if(s->syn_data > upper_threshold)
        if(s->local_rank == 0)
        {
            printf("\n Data arrived %lld rank %llu total data %ld ", m->fwd.num_bytes, s->nw_id, s->syn_data);
        if(s->syn_data > upper_threshold)
        { 
                struct rusage mem_usage;
            int who = RUSAGE_SELF;
            int err = getrusage(who, &mem_usage);
            printf("\n Memory usage %lf gigabytes", ((double)mem_usage.ru_maxrss / (1024.0 * 1024.0)));
            upper_threshold += 1048576;
        }*/
        }
    }
}

// Surrogate switiching structure
struct AvgSurrogateSwitchingTimesForApp {
    int app_id;
    int skip_at_iter;
    int resume_at_iter;
    double time_per_iter;
    bool done; // This is a flag to indicate whethe we already completed this skipping stage
};

static int comp_AvgSurrogateSwitchingTimesForApp(
    struct AvgSurrogateSwitchingTimesForApp *left,
    struct AvgSurrogateSwitchingTimesForApp *right
) {
    if (left->app_id < right->app_id) {
        return -1;
    }
    if (left->app_id > right->app_id) {
        return 1;
    }
    // else: left->app_id == right->app_id

    if (left->skip_at_iter < right->skip_at_iter) {
        return -1;
    }
    if (left->skip_at_iter > right->skip_at_iter) {
        return 1;
    }

    return 0;
}

static int iters_skipped(struct AvgSurrogateSwitchingTimesForApp * avgSur) {
    return avgSur->resume_at_iter - avgSur->skip_at_iter;
}

static struct AvgSurrogateSwitchingTimesForApp * get_switch_config(struct nw_state * s) {
    if (s->switch_config == NULL) {
        return NULL;
    }
    for (int i=0; i < s->switch_config_size; i++) {
        struct AvgSurrogateSwitchingTimesForApp * jump = &s->switch_config[i];
        assert(jump->app_id == s->app_id);
        if (!jump->done) {
            return jump;
        }
    }
    return NULL;
}

static void skip_iteration_rc(nw_state * s, tw_lp * lp, tw_bf * bf, nw_message * m) {
    if (HARD_CODED_AVG_ITER_PREDICTOR) {
        m->rc.surr.config_used->done = false;
    }
}

static void skip_to_iteration(nw_state * s, tw_lp * lp, tw_bf * bf, nw_message * m)
{
    struct codes_workload_op mpi_op;
    int resume_at_iter;

    if (HARD_CODED_AVG_ITER_PREDICTOR) {
        struct AvgSurrogateSwitchingTimesForApp * switch_config = get_switch_config(s);
        assert(switch_config != NULL);
        resume_at_iter = switch_config->resume_at_iter;
        m->rc.surr.config_used = switch_config;
        switch_config->done = true;
    } else {
        resume_at_iter = m->fwd.resume_at_iter;
    }

    // consuming all events until indicated iteration is reached
    bool reached_end = false;
    while (!reached_end) {
        codes_workload_get_next(s->wrkld_id, s->app_id, s->local_rank, &mpi_op);

        switch (mpi_op.op_type) {
            case CODES_WK_MARK:
                if (mpi_op.u.send.tag == resume_at_iter) {
                    reached_end = true;
                    codes_workload_get_next_rc(s->wrkld_id, s->app_id, s->local_rank, &mpi_op);
                }
                break;
            // If we reach the end of simulation, rollback once to allow the operation to be processed normally
            case CODES_WK_END:
                codes_workload_get_next_rc(s->wrkld_id, s->app_id, s->local_rank, &mpi_op);
                reached_end = true;
                break;
            default:
                break;
        }
    }

    tw_event *e = tw_event_new(lp->gid, 0.0, lp);
    nw_message* msg = (nw_message*) tw_event_data(e);
    msg->msg_type = MPI_OP_GET_NEXT;
    tw_event_send(e);
}

static bool have_we_hit_surrogate_switch(struct nw_state* s, struct codes_workload_op * mpi_op) {
    struct AvgSurrogateSwitchingTimesForApp * switch_config = get_switch_config(s);
    if (switch_config != NULL) {
        return mpi_op->u.send.tag == switch_config->skip_at_iter;
    }
    return false;
}

static double time_to_skip_iterations(struct nw_state* s) {
    struct AvgSurrogateSwitchingTimesForApp * switch_config = get_switch_config(s);
    assert(switch_config != NULL);
    return switch_config->time_per_iter * iters_skipped(switch_config);
}

/* Debugging functions, may generate unused function warning */
/*static void print_waiting_reqs(uint32_t * reqs, int count)
{
    lprintf("\n Waiting reqs: %d count", count);
    int i;
    for(i = 0; i < count; i++ )
        lprintf(" %d ", reqs[i]);
}*/
static void print_msgs_queue(struct qlist_head * head, int is_send)
{
    if(is_send)
        printf("\n Send msgs queue: ");
    else
        printf("\n Recv msgs queue: ");

    struct qlist_head * ent = NULL;
    mpi_msgs_queue * current = NULL;
    qlist_for_each(ent, head)
       {
            current = qlist_entry(ent, mpi_msgs_queue, ql);
            //printf(" \n Source %d Dest %d bytes %"PRId64" tag %d ", current->source_rank, current->dest_rank, current->num_bytes, current->tag);
       }
}
static void print_completed_queue(tw_lp * lp, struct qlist_head * head)
{
//    printf("\n Completed queue: ");
      struct qlist_head * ent = NULL;
      struct completed_requests* current = NULL;
      tw_output(lp, "\n");
      qlist_for_each(ent, head)
       {
            current = qlist_entry(ent, completed_requests, ql);
            tw_output(lp, " %llu ", current->req_id);
       }
}
static int clear_completed_reqs(nw_state * s,
        tw_lp * lp,
        unsigned int * reqs, int count)
{
    (void)s;
    (void)lp;

    int matched = 0;

    struct qlist_head * ent, * _;
    struct completed_requests * current = NULL;

    int index = 0;
    qlist_for_each_safe(ent, _, &s->completed_reqs) {
        current = qlist_entry(ent, completed_requests, ql);
        for(int i = 0; i < count; i++) {
            if(current->req_id == reqs[i]) {
                current->index = index;
                ++matched;
                qlist_del(ent);
                rc_stack_push(lp, current, free, s->matched_reqs);
                break;
            }
        }
        index++;
    }
    return matched;
}
static void add_completed_reqs(nw_state * s,
        tw_lp * lp,
        int count)
{
    (void)lp;
    for(int i = 0; i < count; i++)
    {
       struct completed_requests * req = (struct completed_requests*)rc_stack_pop(s->matched_reqs);
       // turn on only if wait-all unmatched error arises in optimistic mode.
       qlist_add_at_index(&req->ql, &s->completed_reqs, req->index - count + i + 1);
    }//end for
}

/* helper function - maps an MPI rank to an LP id */
static tw_lpid rank_to_lpid(int rank)
{
    return codes_mapping_get_lpid_from_relative(rank, NULL, "nw-lp", NULL, 0);
}

static int notify_posted_wait(nw_state* s,
        tw_bf * bf, nw_message * m, tw_lp * lp,
        unsigned int completed_req)
{
    (void)bf;

    struct pending_waits* wait_elem = s->wait_op;
    int wait_completed = 0;

    m->fwd.wait_completed = 0;

    if(!wait_elem)
        return 0;

    int op_type = wait_elem->op_type;

    if(op_type == CODES_WK_WAIT &&
            (wait_elem->req_ids[0] == completed_req))
    {
            wait_completed = 1;
    }
    else if(op_type == CODES_WK_WAITALL
            || op_type == CODES_WK_WAITANY
            || op_type == CODES_WK_WAITSOME)
    {
        int i;
        for(i = 0; i < wait_elem->count; i++)
        {
            if(wait_elem->req_ids[i] == completed_req)
            {
                wait_elem->num_completed++;
                m->fwd.wait_completed++; //This is just the individual request handle - not the entire wait.
                if(wait_elem->num_completed > wait_elem->count)
                    printf("\n Num completed %d count %d LP %llu ",
                            wait_elem->num_completed,
                            wait_elem->count,
                            LLU(lp->gid));
//                if(wait_elem->num_completed > wait_elem->count)
//                    tw_lp_suspend(lp, 1, 0);

                if(wait_elem->num_completed >= wait_elem->count)
                {
                    if(enable_debug)
                    {
                        // fprintf(workload_log, "\n(%lf) APP ID %d MPI WAITALL COMPLETED AT %llu ", tw_now(lp), s->app_id, LLU(s->nw_id));
                        fprintf(workload_log, "\n (%lf) APP ID %d MPI WAITALL SOURCE %d COMPLETED", 
                          tw_now(lp), s->app_id, s->local_rank);
                    }
                    wait_completed = 1;
                }
            }
        }
    }
    return wait_completed;
}

/* reverse handler of MPI wait operation */
static void codes_exec_mpi_wait_rc(nw_state* s, tw_bf * bf, tw_lp* lp, nw_message * m)
{
   if(bf->c1)
    {
        completed_requests * qi = (completed_requests*)rc_stack_pop(s->processed_ops);
        if(m->fwd.found_match == 0)
        {
            qlist_add(&qi->ql, &s->completed_reqs);
        }
        else
        {
           int index = 1;
           struct qlist_head * ent = NULL;
           qlist_for_each(ent, &s->completed_reqs)
           {
                if(index == m->fwd.found_match)
                {
                    qlist_add(&qi->ql, ent);
                    break;
                }
                index++;
           }
        }
        codes_issue_next_event_rc(lp);
        return;
    }
         struct pending_waits * wait_op = s->wait_op;
         free(wait_op);
         s->wait_op = NULL;
}

/* execute MPI wait operation */
static void codes_exec_mpi_wait(nw_state* s, tw_bf * bf, nw_message * m, tw_lp* lp, struct codes_workload_op * mpi_op)
{
    /* check in the completed receives queue if the request ID has already been completed.*/
                
    if(enable_debug)
    {
      fprintf(workload_log, "\n (%lf) APP ID %d MPI WAIT POSTED SOURCE %d", 
            tw_now(lp), s->app_id, s->local_rank);
    }

    assert(!s->wait_op);
    unsigned int req_id = mpi_op->u.wait.req_id;

    struct completed_requests* current = NULL;

    struct qlist_head * ent = NULL;
    int index = 0;
    qlist_for_each(ent, &s->completed_reqs)
    {
        current = qlist_entry(ent, completed_requests, ql);
        if(current->req_id == req_id)
        {
            bf->c1=1;
            qlist_del(&current->ql);
            rc_stack_push(lp, current, free, s->processed_ops);
            codes_issue_next_event(lp);
            m->fwd.found_match = index;
            if(s->nw_id == (tw_lpid)TRACK_LP)
            {
                tw_output(lp, "\n wait matched at post %d ", req_id);
                print_completed_queue(lp, &s->completed_reqs);
            }
            return;
        }
        ++index;
    }

    /*if(s->nw_id == (tw_lpid)TRACK_LP)
    {
        tw_output(lp, "\n wait posted %llu ", req_id);
        print_completed_queue(lp, &s->completed_reqs);
    }*/
    /* If not, add the wait operation in the pending 'waits' list. */
    struct pending_waits* wait_op = (struct pending_waits*)malloc(sizeof(struct pending_waits));
    wait_op->op_type = mpi_op->op_type;
    wait_op->req_ids[0] = req_id;
    wait_op->count = 1;
    wait_op->num_completed = 0;
    wait_op->start_time = tw_now(lp);
    s->wait_op = wait_op;

    return;
}

static void codes_exec_mpi_wait_all_rc(
        nw_state* s,
        tw_bf * bf,
        nw_message * m,
        tw_lp* lp)
{
  if(bf->c1)
  {
    int sampling_indx = s->sampling_indx;
    s->mpi_wkld_samples[sampling_indx].num_waits_sample--;

    if(bf->c2)
    {
        s->cur_interval_end -= sampling_interval;
        s->sampling_indx--;
    }
  }
  if(s->wait_op)
  {
      struct pending_waits * wait_op = s->wait_op;
      free(wait_op);
      s->wait_op = NULL;
  }
  else
  {
      add_completed_reqs(s, lp, m->fwd.num_matched);
      codes_issue_next_event_rc(lp);
  }
  return;
}

static void codes_exec_mpi_wait_all(
        nw_state* s,
        tw_bf * bf,
        nw_message * m,
        tw_lp* lp,
        struct codes_workload_op * mpi_op)
{
  if(enable_debug)
  {
    // fprintf(workload_log, "\n MPI WAITALL POSTED AT %llu ", LLU(s->nw_id));
    fprintf(workload_log, "\n (%lf) APP ID %d MPI WAITALL POSTED SOURCE %d", 
          tw_now(lp), s->app_id, s->local_rank);
  }

  if(enable_sampling)
  {
    bf->c1 = 1;
    if(tw_now(lp) >= s->cur_interval_end)
    {
        bf->c2 = 1;
        int indx = s->sampling_indx;
        s->mpi_wkld_samples[indx].nw_id = s->nw_id;
        s->mpi_wkld_samples[indx].app_id = s->app_id;
        s->mpi_wkld_samples[indx].sample_end_time = s->cur_interval_end;
        s->cur_interval_end += sampling_interval;
        s->sampling_indx++;
    }
    if(s->sampling_indx >= MAX_STATS)
    {
        struct mpi_workload_sample * tmp = (struct mpi_workload_sample*)calloc((MAX_STATS + s->max_arr_size), sizeof(struct mpi_workload_sample));
        memcpy(tmp, s->mpi_wkld_samples, s->sampling_indx);
        free(s->mpi_wkld_samples);
        s->mpi_wkld_samples = tmp;
        s->max_arr_size += MAX_STATS;
    }
    int indx = s->sampling_indx;
    s->mpi_wkld_samples[indx].num_waits_sample++;
  }
  int count = mpi_op->u.waits.count;
  /* If the count is not less than max wait reqs then stop */
  assert(count < MAX_WAIT_REQS);

  int i = 0, num_matched = 0;
  m->fwd.num_matched = 0;

  /*if(lp->gid == TRACK_LP)
  {
      printf("\n MPI Wait all posted ");
      print_waiting_reqs(mpi_op->u.waits.req_ids, count);
      print_completed_queue(lp, &s->completed_reqs);
  }*/
      /* check number of completed irecvs in the completion queue */
  for(i = 0; i < count; i++)
  {
      unsigned int req_id = mpi_op->u.waits.req_ids[i];
      struct qlist_head * ent = NULL;
      struct completed_requests* current = NULL;
      qlist_for_each(ent, &s->completed_reqs)
       {
            current = qlist_entry(ent, struct completed_requests, ql);
            if(current->req_id == req_id)
                num_matched++;
       }
  }

  m->fwd.found_match = num_matched;
  if(num_matched == count)
  {
    /* No need to post a MPI Wait all then, issue next event */
      /* Remove all completed requests from the list */
      m->fwd.num_matched = clear_completed_reqs(s, lp, mpi_op->u.waits.req_ids, count);
      struct pending_waits* wait_op = s->wait_op;
      free(wait_op);
      s->wait_op = NULL;
      codes_issue_next_event(lp);
  }
  else
  {
    /* If not, add the wait operation in the pending 'waits' list. */
    struct pending_waits* wait_op = (struct pending_waits*)malloc(sizeof(struct pending_waits));
    wait_op->count = count;
    wait_op->op_type = mpi_op->op_type;
    assert(count < MAX_WAIT_REQS);

    for(i = 0; i < count; i++)
    wait_op->req_ids[i] =  mpi_op->u.waits.req_ids[i];

    wait_op->num_completed = num_matched;
    wait_op->start_time = tw_now(lp);
    s->wait_op = wait_op;
  }
  return;
}

/* search for a matching mpi operation and remove it from the list.
 * Record the index in the list from where the element got deleted.
 * Index is used for inserting the element once again in the queue for reverse computation. */
static int rm_matching_rcv(nw_state * ns,
        tw_bf * bf,
        nw_message * m,
        tw_lp * lp,
        mpi_msgs_queue * qitem)
{
    int matched = 0;
    int index = 0;
    int is_rend = 0;
    struct qlist_head *ent = NULL;
    mpi_msgs_queue * qi = NULL;

    qlist_for_each(ent, &ns->pending_recvs_queue){
        qi = qlist_entry(ent, mpi_msgs_queue, ql);
        if(//(qi->num_bytes == qitem->num_bytes)
                //&& 
               ((qi->tag == qitem->tag) || qi->tag == -1)
                && ((qi->source_rank == qitem->source_rank) || qi->source_rank == -1))
        {
            matched = 1;
            m->rc.mpi_send.saved_num_bytes = qi->num_bytes;
            qi->num_bytes = qitem->num_bytes;
            break;
        }
        ++index;
    }

    if(matched)
    {
        if(enable_msg_tracking && qitem->num_bytes < EAGER_THRESHOLD)
        {
            update_message_size(ns, lp, bf, m, qitem, 1, 1);
        }
        if(qitem->num_bytes >= EAGER_THRESHOLD)
        {
            /* Matching receive found, need to notify the sender to transmit
             * the data * (only works in sequential mode)*/
            bf->c10 = 1;
            is_rend = 1;
            send_ack_back(ns, bf, m, lp, qitem, qi->req_id);
        }
        else
        {
            bf->c12 = 1;
            m->rc.mpi_send.saved_recv_time = ns->recv_time;
            m->rc.mpi_send.saved_recv_time_sample = ns->ross_sample.recv_time;
            ns->recv_time += (tw_now(lp) - m->fwd.sim_start_time);
            ns->ross_sample.recv_time += (tw_now(lp) - m->fwd.sim_start_time);
        }
        if(qi->op_type == CODES_WK_IRECV && !is_rend)
        {
            bf->c9 = 1;
            /*if(ns->nw_id == (tw_lpid)TRACK_LP)
            {
                printf("\n Completed irecv req id %d ", qi->req_id);
            }*/
            update_completed_queue(ns, bf, m, lp, qi->req_id);
        }
        else if(qi->op_type == CODES_WK_RECV && !is_rend)
        {
            bf->c8 = 1;
            codes_issue_next_event(lp);
        }

        qlist_del(&qi->ql);

        rc_stack_push(lp, qi, free, ns->processed_ops);
        return index;
    }
    return -1;
}

static int rm_matching_send(nw_state * ns,
        tw_bf * bf,
        nw_message * m,
        tw_lp * lp, mpi_msgs_queue * qitem)
{
    int matched = 0;
    struct qlist_head *ent = NULL;
    mpi_msgs_queue * qi = NULL;

    int index = 0;
    qlist_for_each(ent, &ns->arrival_queue){
        qi = qlist_entry(ent, mpi_msgs_queue, ql);
        if(//(qi->num_bytes == qitem->num_bytes) // it is not a requirement in MPI that the send and receive sizes match
                // && 
		(qi->tag == qitem->tag || qitem->tag == -1)
                && ((qi->source_rank == qitem->source_rank) || qitem->source_rank == -1))
        {
            qitem->num_bytes = qi->num_bytes;
            matched = 1;
            break;
        }
        ++index;
    }

    if(matched)
    {
        if(enable_msg_tracking && (qi->num_bytes < EAGER_THRESHOLD))
            update_message_size(ns, lp, bf, m, qi, 1, 0);
        
        m->fwd.matched_req = qitem->req_id;
        int is_rend = 0;
        if(qitem->num_bytes >= EAGER_THRESHOLD)
        {
            /* Matching receive found, need to notify the sender to transmit
             * the data */
            bf->c10 = 1;
            is_rend = 1;
            send_ack_back(ns, bf, m, lp, qi, qitem->req_id);
        }

        m->rc.mpi_next.recv.saved_recv_time = ns->recv_time;
        m->rc.mpi_next.recv.saved_recv_time_sample = ns->ross_sample.recv_time;
        ns->recv_time += (tw_now(lp) - qitem->req_init_time);
        ns->ross_sample.recv_time += (tw_now(lp) - qitem->req_init_time);

        /*if(ns->nw_id == (tw_lpid)TRACK_LP && qitem->op_type == CODES_WK_IRECV)
        {
            tw_output(lp, "\n Completed recv req id %d ", qitem->req_id);
            print_completed_queue(lp, &ns->completed_reqs);
        }*/
        
        if(qitem->op_type == CODES_WK_IRECV && !is_rend)
        {
            bf->c29 = 1;
            update_completed_queue(ns, bf, m, lp, qitem->req_id);
        }
        else
         if(qitem->op_type == CODES_WK_RECV && !is_rend)
         {
            bf->c6 = 1;
            codes_issue_next_event(lp);
         }


        qlist_del(&qi->ql);

	    rc_stack_push(lp, qi, free, ns->processed_ops);
        return index;
    }
    return -1;
}
static void codes_issue_next_event_rc(tw_lp * lp)
{
	    // tw_rand_reverse_unif(lp->rng);
}

/* Trigger getting next event at LP */
static void codes_issue_next_event(tw_lp* lp)
{
   tw_event *e;
   nw_message* msg;

   tw_stime ts;

   ts = NEAR_ZERO;
//    ts = g_tw_lookahead + 0.1 + tw_rand_exponential(lp->rng, noise);
//    assert(ts > 0);
   e = tw_event_new( lp->gid, ts, lp );
   msg = (nw_message*)tw_event_data(e);

   msg->msg_type = MPI_OP_GET_NEXT;
   tw_event_send(e);
}

/* Simulate delays between MPI operations */
static void codes_exec_comp_delay(
        nw_state* s, tw_bf *bf, nw_message * m, tw_lp* lp, struct codes_workload_op * mpi_op)
{
    bf->c28 = 0;
	tw_event* e;
	tw_stime ts;
	nw_message* msg;

    m->rc.mpi_next.delay.saved_delay = s->compute_time;
    m->rc.mpi_next.delay.saved_delay_sample = s->ross_sample.compute_time;
    s->compute_time += (mpi_op->u.delay.nsecs/compute_time_speedup);
    s->ross_sample.compute_time += (mpi_op->u.delay.nsecs/compute_time_speedup);
    ts = (mpi_op->u.delay.nsecs/compute_time_speedup);
    if (ts < 0)
        ts = 0;
    // if(ts <= g_tw_lookahead)
    // {
    //     bf->c28 = 1;
    //     // ts = g_tw_lookahead + 0.1 + tw_rand_exponential(lp->rng, noise);
    //     ts = g_tw_lookahead;
    // }

	//ts += g_tw_lookahead + 0.1 + tw_rand_exponential(lp->rng, noise);
    // assert(ts > 0);

  if(enable_debug)
  {
    fprintf(workload_log, "\n (%lf) APP %d MPI DELAY SOURCE %d DURATION %lf",
              tw_now(lp), s->app_id, s->local_rank, ts);
  }

	e = tw_event_new( lp->gid, ts , lp );
	msg = (nw_message*)tw_event_data(e);
	msg->msg_type = MPI_OP_GET_NEXT;
	tw_event_send(e);

}

/* reverse computation operation for MPI irecv */
static void codes_exec_mpi_recv_rc(
        nw_state* ns,
        tw_bf * bf,
        nw_message* m,
        tw_lp* lp)
{
	ns->recv_time = m->rc.mpi_next.recv.saved_recv_time;
	ns->ross_sample.recv_time = m->rc.mpi_next.recv.saved_recv_time_sample;

    if(bf->c11)
        codes_issue_next_event_rc(lp);

    if(bf->c6)
        codes_issue_next_event_rc(lp);

    if(m->fwd.found_match >= 0)
	{
        //int queue_count = qlist_count(&ns->arrival_queue);

        mpi_msgs_queue * qi = (mpi_msgs_queue*)rc_stack_pop(ns->processed_ops);

        if(bf->c10)
            send_ack_back_rc(ns, bf, m, lp);
        if(m->fwd.found_match == 0)
        {
            qlist_add(&qi->ql, &ns->arrival_queue);
        }
        else 
        {
            int index = 1;
            struct qlist_head * ent = NULL;
            qlist_for_each(ent, &ns->arrival_queue)
            {
               if(index == m->fwd.found_match)
               {
                 qlist_add(&qi->ql, ent);
                 break;
               }
               index++;
            }
        }
        if(bf->c29)
        {
            update_completed_queue_rc(ns, bf, m, lp);
        }
    }
	else if(m->fwd.found_match < 0)
	    {
            struct qlist_head * ent = qlist_pop_back(&ns->pending_recvs_queue);
            mpi_msgs_queue * qi = qlist_entry(ent, mpi_msgs_queue, ql);
            free(qi);
	    }
}

/* Execute MPI Irecv operation (non-blocking receive) */
static void codes_exec_mpi_recv(
        nw_state* s,
        tw_bf * bf,
        nw_message * m,
        tw_lp* lp,
        struct codes_workload_op * mpi_op)
{
/* Once an irecv is posted, list of completed sends is checked to find a matching isend.
   If no matching isend is found, the receive operation is queued in the pending queue of
   receive operations. */

    m->rc.mpi_next.recv.saved_recv_time = s->recv_time;
    m->rc.mpi_next.recv.saved_recv_time_sample = s->ross_sample.recv_time;

    mpi_msgs_queue * recv_op = (mpi_msgs_queue*) malloc(sizeof(mpi_msgs_queue));
    recv_op->req_init_time = tw_now(lp);
    recv_op->op_type = mpi_op->op_type;
    recv_op->source_rank = mpi_op->u.recv.source_rank;
    recv_op->dest_rank = mpi_op->u.recv.dest_rank;
    recv_op->num_bytes = mpi_op->u.recv.num_bytes;
    recv_op->tag = mpi_op->u.recv.tag;
    recv_op->req_id = mpi_op->u.recv.req_id;


    //printf("\n Req id %d bytes %d source %d tag %d ", recv_op->req_id, recv_op->num_bytes, recv_op->source_rank, recv_op->tag);
//    if(s->nw_id == (tw_lpid)TRACK_LP)
//        printf("\n Receive op posted num bytes %llu source %d ", recv_op->num_bytes,
//                recv_op->source_rank);

  if(enable_debug)
  {
      if(mpi_op->op_type == CODES_WK_RECV)
      {
        fprintf(workload_log, "\n (%lf) APP %d MPI RECV SOURCE %d DEST %d BYTES %"PRId64,
                  tw_now(lp), s->app_id, recv_op->source_rank, recv_op->dest_rank, recv_op->num_bytes);
      }
      else
      {
        fprintf(workload_log, "\n (%lf) APP ID %d MPI IRECV SOURCE %d DEST %d BYTES %"PRId64,
                  tw_now(lp), s->app_id, recv_op->source_rank, recv_op->dest_rank, recv_op->num_bytes);
      }
  }

	int found_matching_sends = rm_matching_send(s, bf, m, lp, recv_op);

	       /* for mpi irecvs, this is a non-blocking receive so just post it and move on with the trace read. */
	if(mpi_op->op_type == CODES_WK_IRECV)
    {
        bf->c6 = 1;
	    codes_issue_next_event(lp);
    }


	/* save the req id inserted in the completed queue for reverse computation. */
	if(found_matching_sends < 0)
	  {
	   	  m->fwd.found_match = -1;
          qlist_add_tail(&recv_op->ql, &s->pending_recvs_queue);

      }
	else
	  {
        //bf->c6 = 1;
        m->fwd.found_match = found_matching_sends;
      }
}

int get_global_id_of_job_rank(tw_lpid job_rank, int app_id)
{
    struct codes_jobmap_id lid;
    lid.job = app_id;
    lid.rank = job_rank;
    int global_rank = codes_jobmap_to_global_id(lid, jobmap_ctx);
    return global_rank;
}
static void codes_exec_mpi_send_rc(nw_state * s, tw_bf * bf, nw_message * m, tw_lp * lp)
{
        if(enable_sampling)
        {
           int indx = s->sampling_indx;

           s->mpi_wkld_samples[indx].num_sends_sample--;
           s->mpi_wkld_samples[indx].num_bytes_sample -= m->rc.mpi_ack.saved_num_bytes;

           if(bf->c1)
           {
               s->sampling_indx--;
               s->cur_interval_end -= sampling_interval;
           }
        }
        if(bf->c15 || bf->c16)
        {
            s->num_sends--;
            s->ross_sample.num_sends--;
        }

        if (bf->c15)
            model_net_event_rc2(lp, &m->event_rc);
        if (bf->c16)
            model_net_event_rc2(lp, &m->event_rc);
        if (bf->c17)
            model_net_event_rc2(lp, &m->event_rc);

        if(bf->c4)
            codes_issue_next_event_rc(lp);

        if(bf->c3)
        {
            s->num_bytes_sent -= m->rc.mpi_ack.saved_num_bytes;
            s->ross_sample.num_bytes_sent -= m->rc.mpi_ack.saved_num_bytes;
            num_bytes_sent -= m->rc.mpi_ack.saved_num_bytes;
        }
}
/* executes MPI send and isend operations */
static void codes_exec_mpi_send(nw_state* s,
        tw_bf * bf,
        nw_message * m,
        tw_lp* lp,
        struct codes_workload_op * mpi_op,
        int is_rend)
{
    bf->c3 = 0;
    bf->c1 = 0;
    bf->c4 = 0;
   
    /* Class names in the CODES dragonfly-dally (as at 2020/09/21 - KB):
     * 	"high"		<- highest priority
     * 	"medium"
     * 	"low"
     * 	"class3"
     * 	"class4"
     * 	"class5"
     *
     * The name of the first three classes are kept for backwards compatibility. TODO: Rename classes
     */
    char prio[12];
	switch(s->qos_level){
		case 0:
			strcpy(prio, "high"); break;
		case 1:
			strcpy(prio, "medium"); break;
		case 2:
			strcpy(prio, "low"); break;
		case 3:
			strcpy(prio, "class3"); break;
		case 4:
			strcpy(prio, "class4"); break;
		case 5:
			strcpy(prio, "class5"); break;
		default:
			tw_error(TW_LOC, "\n Invalid QoS level: %d", s->qos_level);
			break;
	}

    if(prioritize_collectives == 1)
    {
        if(mpi_op->u.send.tag == COL_TAG || mpi_op->u.send.tag == BAR_TAG)
        {
            strcpy(prio, "high");
        }
    }

    int is_eager = 0;
	/* model-net event */
    int global_dest_rank = mpi_op->u.send.dest_rank;

    if(alloc_spec)
    {
        global_dest_rank = get_global_id_of_job_rank(mpi_op->u.send.dest_rank, s->app_id);
    }

    if(lp->gid == TRACK_LP)
        printf("\n Sender rank %llu global dest rank %d dest-rank %d bytes %"PRIu64" Tag %d", LLU(s->nw_id), global_dest_rank, mpi_op->u.send.dest_rank, mpi_op->u.send.num_bytes, mpi_op->u.send.tag);
    m->rc.mpi_ack.saved_num_bytes = mpi_op->u.send.num_bytes;
	/* model-net event */
	tw_lpid dest_rank = codes_mapping_get_lpid_from_relative(global_dest_rank, NULL, "nw-lp", NULL, 0);

    if(enable_sampling)
    {
        if(tw_now(lp) >= s->cur_interval_end)
        {
            bf->c1 = 1;
            int indx = s->sampling_indx;
            s->mpi_wkld_samples[indx].nw_id = s->nw_id;
            s->mpi_wkld_samples[indx].app_id = s->app_id;
            s->mpi_wkld_samples[indx].sample_end_time = s->cur_interval_end;
            s->sampling_indx++;
            s->cur_interval_end += sampling_interval;
        }
        if(s->sampling_indx >= MAX_STATS)
        {
            struct mpi_workload_sample * tmp = (struct mpi_workload_sample*)calloc((MAX_STATS + s->max_arr_size), sizeof(struct mpi_workload_sample));
            memcpy(tmp, s->mpi_wkld_samples, s->sampling_indx);
            free(s->mpi_wkld_samples);
            s->mpi_wkld_samples = tmp;
            s->max_arr_size += MAX_STATS;
        }
        int indx = s->sampling_indx;
        s->mpi_wkld_samples[indx].num_sends_sample++;
        s->mpi_wkld_samples[indx].num_bytes_sample += mpi_op->u.send.num_bytes;
    }
	nw_message local_m;
	nw_message remote_m;

    local_m.fwd.dest_rank = mpi_op->u.send.dest_rank;
    local_m.fwd.src_rank = mpi_op->u.send.source_rank;
    local_m.op_type = mpi_op->op_type;
    local_m.msg_type = MPI_SEND_POSTED;
    local_m.fwd.tag = mpi_op->u.send.tag;
    local_m.fwd.rend_send = 0;
    local_m.fwd.num_bytes = mpi_op->u.send.num_bytes;
    local_m.fwd.req_id = mpi_op->u.send.req_id;
    local_m.fwd.app_id = s->app_id;
    local_m.fwd.matched_req = m->fwd.matched_req;        
   
    if(mpi_op->u.send.num_bytes < EAGER_THRESHOLD)
    {
        /* directly issue a model-net send */
           
        bf->c15 = 1;
        is_eager = 1;
        s->num_sends++;
        s->ross_sample.num_sends++;
        tw_stime copy_overhead = copy_per_byte_eager * mpi_op->u.send.num_bytes;
        local_m.fwd.sim_start_time = tw_now(lp);

        remote_m = local_m;
        remote_m.msg_type = MPI_SEND_ARRIVED;
        remote_m.fwd.app_id = s->app_id;
    	m->event_rc = model_net_event_mctx(net_id, &mapping_context, &mapping_context, 
            prio, dest_rank, mpi_op->u.send.num_bytes, (self_overhead + copy_overhead + soft_delay_mpi + nic_delay),
	    sizeof(nw_message), (const void*)&remote_m, sizeof(nw_message), (const void*)&local_m, lp);
    }
    else if (is_rend == 0)
    {
        /* Initiate the handshake. Issue a control message to the destination first. No local message,
         * only remote message sent. */
        bf->c16 = 1;
        s->num_sends++;
        s->ross_sample.num_sends++;
        remote_m.fwd.sim_start_time = tw_now(lp);
        remote_m.fwd.dest_rank = mpi_op->u.send.dest_rank;   
        remote_m.fwd.src_rank = mpi_op->u.send.source_rank;
        remote_m.msg_type = MPI_SEND_ARRIVED;
        remote_m.op_type = mpi_op->op_type;
        remote_m.fwd.tag = mpi_op->u.send.tag; 
        remote_m.fwd.num_bytes = mpi_op->u.send.num_bytes;
        remote_m.fwd.req_id = mpi_op->u.send.req_id;  
        remote_m.fwd.app_id = s->app_id;

    	m->event_rc = model_net_event_mctx(net_id, &mapping_context, &mapping_context, 
            prio, dest_rank, CONTROL_MSG_SZ, (self_overhead + soft_delay_mpi + nic_delay),
	    sizeof(nw_message), (const void*)&remote_m, 0, NULL, lp);
    }
    else if(is_rend == 1)
    {
        bf->c17 = 1;
        /* initiate the actual data transfer, local completion message is sent
         * for any blocking sends. */
       local_m.fwd.sim_start_time = mpi_op->sim_start_time;
       local_m.fwd.rend_send = 1;
       remote_m = local_m; 
       remote_m.msg_type = MPI_REND_ARRIVED;


       m->event_rc = model_net_event_mctx(net_id, &mapping_context, &mapping_context, 
            prio, dest_rank, mpi_op->u.send.num_bytes, (self_overhead + soft_delay_mpi + nic_delay),
	    sizeof(nw_message), (const void*)&remote_m, sizeof(nw_message), (const void*)&local_m, lp);
    }
    if(enable_debug && !is_rend)
    {
        if(mpi_op->op_type == CODES_WK_ISEND)
        {
            // fprintf(workload_log, "\n (%lf) APP %d MPI ISEND SOURCE %llu DEST %d TAG %d BYTES %"PRId64,
            //         tw_now(lp), s->app_id, LLU(s->nw_id), global_dest_rank, mpi_op->u.send.tag, mpi_op->u.send.num_bytes);
          fprintf(workload_log, "\n (%lf) APP %d MPI ISEND SOURCE %llu DEST %d TAG %d BYTES %"PRId64,
                    tw_now(lp), s->app_id, LLU(remote_m.fwd.src_rank), remote_m.fwd.dest_rank, mpi_op->u.send.tag, mpi_op->u.send.num_bytes);
        }
        else
        {
            // fprintf(workload_log, "\n (%lf) APP ID %d MPI SEND SOURCE %llu DEST %d TAG %d BYTES %"PRId64,
            //         tw_now(lp), s->app_id, LLU(s->nw_id), global_dest_rank, mpi_op->u.send.tag, mpi_op->u.send.num_bytes);
          fprintf(workload_log, "\n (%lf) APP ID %d MPI SEND SOURCE %llu DEST %d TAG %d BYTES %"PRId64,
                    tw_now(lp), s->app_id, LLU(remote_m.fwd.src_rank), remote_m.fwd.dest_rank, mpi_op->u.send.tag, mpi_op->u.send.num_bytes);
        }
    }
    if(is_rend || is_eager)    
    {
       bf->c3 = 1;
       s->num_bytes_sent += mpi_op->u.send.num_bytes;
       s->ross_sample.num_bytes_sent += mpi_op->u.send.num_bytes;
       num_bytes_sent += mpi_op->u.send.num_bytes;
    }
	/* isend executed, now get next MPI operation from the queue */
	if(mpi_op->op_type == CODES_WK_ISEND && (!is_rend || is_eager))
    {
       bf->c4 = 1;
	   codes_issue_next_event(lp);
    }
}

/* convert seconds to ns */
static tw_stime s_to_ns(tw_stime s)
{
    return(s * (1000.0 * 1000.0 * 1000.0));
}
/* convert seconds to ns */
static tw_stime ns_to_s(tw_stime ns)
{
    return(ns / (1000.0 * 1000.0 * 1000.0));
}

static void update_completed_queue_rc(nw_state * s, tw_bf * bf, nw_message * m, tw_lp * lp)
{

    if(bf->c30)
    {
       struct qlist_head * ent = qlist_pop(&s->completed_reqs);

        completed_requests * req = qlist_entry(ent, completed_requests, ql);
       free(req);
    }
    else if(bf->c31)
    {
       struct pending_waits* wait_elem = (struct pending_waits*)rc_stack_pop(s->processed_wait_op);
       s->wait_op = wait_elem;
       s->wait_time = m->rc.mpi_send.saved_wait_time;
       s->ross_sample.wait_time = m->rc.mpi_send.saved_wait_time_sample;
       add_completed_reqs(s, lp, m->fwd.num_matched);
       codes_issue_next_event_rc(lp);
    }
    if(m->fwd.wait_completed > 0) {
       s->wait_op->num_completed -= m->fwd.wait_completed;
    }
}

static void update_completed_queue(nw_state* s,
        tw_bf * bf,
        nw_message * m,
        tw_lp * lp,
        dumpi_req_id req_id)
{
    bf->c30 = 0;
    bf->c31 = 0;
    m->fwd.num_matched = 0;

     //done waiting means that this was either the only wait in the op or the last wait in the collective op.
    int done_waiting = 0;
    done_waiting = notify_posted_wait(s, bf, m, lp, req_id);

    if(!done_waiting)
    {
        bf->c30 = 1;
        completed_requests * req = (completed_requests*)malloc(sizeof(completed_requests));
        req->req_id = req_id;
        qlist_add(&req->ql, &s->completed_reqs);

        /*if(s->nw_id == (tw_lpid)TRACK_LP)
        {
            tw_output(lp, "\n Forward mode adding %d ", req_id);
            print_completed_queue(lp, &s->completed_reqs);
        }*/
    }
    else
     {
            bf->c31 = 1;
            m->fwd.num_matched = clear_completed_reqs(s, lp, s->wait_op->req_ids, s->wait_op->count);
    
            m->rc.mpi_send.saved_wait_time = s->wait_time;
            m->rc.mpi_send.saved_wait_time_sample = s->ross_sample.wait_time;
            s->wait_time += (tw_now(lp) - s->wait_op->start_time);
            s->ross_sample.wait_time += (tw_now(lp) - s->wait_op->start_time);

            struct pending_waits* wait_elem = s->wait_op;
            rc_stack_push(lp, wait_elem, free, s->processed_wait_op);
            s->wait_op = NULL;

            codes_issue_next_event(lp);
     }
}

static void send_ack_back_rc(nw_state* s, tw_bf * bf, nw_message * m, tw_lp * lp)
{
    (void)s;
    (void)bf;
    /* Send an ack back to the sender */
    model_net_event_rc2(lp, &m->event_rc);
}
static void send_ack_back(nw_state* s, tw_bf * bf, nw_message * m, tw_lp * lp, mpi_msgs_queue * mpi_op, int matched_req)
{
    (void)bf;

    int global_dest_rank = mpi_op->source_rank;

    if(alloc_spec)
    {
        global_dest_rank =  get_global_id_of_job_rank(mpi_op->source_rank, s->app_id);
    }

    tw_lpid dest_rank = codes_mapping_get_lpid_from_relative(global_dest_rank, NULL, "nw-lp", NULL, 0);
    /* Send a message back to sender indicating availability.*/
	nw_message remote_m;
    remote_m.fwd.sim_start_time = mpi_op->req_init_time;
    remote_m.fwd.dest_rank = mpi_op->dest_rank;   
    remote_m.fwd.src_rank = mpi_op->source_rank;
    remote_m.op_type = mpi_op->op_type;
    remote_m.msg_type = MPI_REND_ACK_ARRIVED;
    remote_m.fwd.tag = mpi_op->tag; 
    remote_m.fwd.num_bytes = mpi_op->num_bytes;
    remote_m.fwd.req_id = mpi_op->req_id;  
    remote_m.fwd.matched_req = matched_req;

    // TODO: If we want ack messages to be in the low-latency class, change this function. -Kevin Brown 2021.02.18
    char prio[12];
	switch(s->qos_level){
		case 0:
			strcpy(prio, "high"); break;
		case 1:
			strcpy(prio, "medium"); break;
		case 2:
			strcpy(prio, "low"); break;
		case 3:
			strcpy(prio, "class3"); break;
		case 4:
			strcpy(prio, "class4"); break;
		case 5:
			strcpy(prio, "class5"); break;
		default:
			tw_error(TW_LOC, "\n Invalid QoS level: %d", s->qos_level);
			break;
	}

    if(prioritize_collectives == 1)
    {
        if(mpi_op->tag == COL_TAG || mpi_op->tag == BAR_TAG)
        {
            strcpy(prio, "high");
        }
    }
    
    m->event_rc = model_net_event_mctx(net_id, &mapping_context, &mapping_context,
        prio, dest_rank, CONTROL_MSG_SZ, (self_overhead + soft_delay_mpi + nic_delay),
    sizeof(nw_message), (const void*)&remote_m, 0, NULL, lp);

}
/* reverse handler for updating arrival queue function */
static void update_arrival_queue_rc(nw_state* s,
        tw_bf * bf,
        nw_message * m, tw_lp * lp)
{
    s->num_bytes_recvd -= m->fwd.num_bytes;
    s->ross_sample.num_bytes_recvd -= m->fwd.num_bytes;
    num_bytes_recvd -= m->fwd.num_bytes;

    // if(bf->c1)
    //     codes_local_latency_reverse(lp);

    if(bf->c10)
        send_ack_back_rc(s, bf, m, lp);

    if(m->fwd.found_match >= 0)
	{
        mpi_msgs_queue * qi = (mpi_msgs_queue*)rc_stack_pop(s->processed_ops);
        qi->num_bytes = m->rc.mpi_send.saved_num_bytes;
//        int queue_count = qlist_count(&s->pending_recvs_queue);

        if(m->fwd.found_match == 0)
        {
            qlist_add(&qi->ql, &s->pending_recvs_queue);
        }
        else
        {
            int index = 1;
            struct qlist_head * ent = NULL;
            qlist_for_each(ent, &s->pending_recvs_queue)
            {
               if(index == m->fwd.found_match)
               {
                 qlist_add(&qi->ql, ent);
                 break;
               }
               index++;
            }
        }
        if(bf->c12)
        {
            s->recv_time = m->rc.mpi_send.saved_recv_time;
            s->ross_sample.recv_time = m->rc.mpi_send.saved_recv_time_sample;
        }
        
        //if(bf->c10)
        //    send_ack_back_rc(s, bf, m, lp);
        if(bf->c9)
            update_completed_queue_rc(s, bf, m, lp);
        if(bf->c8)
            codes_issue_next_event_rc(lp);
    }
	else if(m->fwd.found_match < 0)
	{
	    struct qlist_head * ent = qlist_pop_back(&s->arrival_queue);
        mpi_msgs_queue * qi = qlist_entry(ent, mpi_msgs_queue, ql);
        free(qi);
    }
}

/* once an isend operation arrives, the pending receives queue is checked to find out if there is a irecv that has already been posted. If no isend has been posted, */
static void update_arrival_queue(nw_state* s, tw_bf * bf, nw_message * m, tw_lp * lp)
{
    if(s->app_id != m->fwd.app_id)
        printf("\n Received message for app %d my id %d my rank %llu ",
                m->fwd.app_id, s->app_id, LLU(s->nw_id));
    assert(s->app_id == m->fwd.app_id);

    //if(s->local_rank != m->fwd.dest_rank)
    //    printf("\n Dest rank %d local rank %d ", m->fwd.dest_rank, s->local_rank);
    m->rc.mpi_send.saved_recv_time = s->recv_time;
    m->rc.mpi_send.saved_recv_time_sample = s->ross_sample.recv_time;
    s->num_bytes_recvd += m->fwd.num_bytes;
    s->ross_sample.num_bytes_recvd += m->fwd.num_bytes;
    num_bytes_recvd += m->fwd.num_bytes;

    // send a callback to the sender to increment times
    // find the global id of the source
    int global_src_id = m->fwd.src_rank;
    if(alloc_spec)
    {
        global_src_id = get_global_id_of_job_rank(m->fwd.src_rank, s->app_id);
    }

    if(m->fwd.num_bytes < EAGER_THRESHOLD)
    {
        // tw_stime ts = codes_local_latency(lp);
        tw_stime ts = 0;
        // assert(ts > 0);
        bf->c1 = 1;
        tw_event *e_callback =
        tw_event_new(rank_to_lpid(global_src_id),
                ts, lp);
        nw_message *m_callback = (nw_message*)tw_event_data(e_callback);
        m_callback->msg_type = MPI_SEND_ARRIVED_CB;
        m_callback->fwd.msg_send_time = tw_now(lp) - m->fwd.sim_start_time;
        tw_event_send(e_callback);
    }
    /* Now reconstruct the queue item */
    mpi_msgs_queue * arrived_op = (mpi_msgs_queue *) malloc(sizeof(mpi_msgs_queue));
    arrived_op->req_init_time = m->fwd.sim_start_time;
    arrived_op->op_type = m->op_type;
    arrived_op->source_rank = m->fwd.src_rank;
    arrived_op->tag = m->fwd.tag;
    arrived_op->req_id = m->fwd.req_id;
    arrived_op->num_bytes = m->fwd.num_bytes;
    arrived_op->dest_rank = m->fwd.dest_rank;

//    if(s->nw_id == (tw_lpid)TRACK_LP)
//        printf("\n Send op arrived source rank %d num bytes %llu", arrived_op->source_rank,
//                arrived_op->num_bytes);

    int found_matching_recv = rm_matching_rcv(s, bf, m, lp, arrived_op);

    if(found_matching_recv < 0)
    {
        m->fwd.found_match = -1;
        qlist_add_tail(&arrived_op->ql, &s->arrival_queue);
    }
    else
    {
        m->fwd.found_match = found_matching_recv;
        free(arrived_op);
    }
}
static void update_message_time(
        nw_state * s,
        tw_bf * bf,
        nw_message * m,
        tw_lp * lp)
{
    (void)bf;
    (void)lp;

    m->rc.arrive.saved_send_time = s->send_time;
    m->rc.arrive.saved_send_time_sample = s->ross_sample.send_time;
    s->send_time += m->fwd.msg_send_time;
    s->ross_sample.send_time += m->fwd.msg_send_time;
}

static void update_message_time_rc(
        nw_state * s,
        tw_bf * bf,
        nw_message * m,
        tw_lp * lp)
{
    (void)bf;
    (void)lp;
    s->send_time = m->rc.arrive.saved_send_time;
    s->ross_sample.send_time = m->rc.arrive.saved_send_time_sample;
}

/* initializes the network node LP, loads the trace file in the structs, calls the first MPI operation to be executed */
void nw_test_init(nw_state* s, tw_lp* lp)
{
   /* initialize the LP's and load the data */

   memset(s, 0, sizeof(*s));
   s->nw_id = codes_mapping_get_lp_relative_id(lp->gid, 0, 0);
   s->nw_id_in_pe = nw_id_counter++;
   s->mpi_wkld_samples = (struct mpi_workload_sample*)calloc(MAX_STATS, sizeof(struct mpi_workload_sample));
   s->sampling_indx = 0;
   s->is_finished = 0;
   s->cur_interval_end = 0;
   s->col_time = 0;
   s->num_reduce = 0;
   s->reduce_time = 0;
   s->all_reduce_time = 0;
   s->prev_switch = 0;
   s->max_time = 0;
   s->qos_level = 0; //TODO:  We need a more elegant solution for determining if qos is enabled or not.
                     //       This had been -1 but if qos is not configured (single job no workload conf file)
                     //       then this will error out
   s->wrkld_id = -1;

   char type_name[512];

   if(!num_net_traces)
	num_net_traces = num_mpi_lps;

   assert(num_net_traces <= num_mpi_lps);

   struct codes_jobmap_id lid;
   online_comm_params oc_params;
   dumpi_trace_params params_d;

   if(alloc_spec)
   {
        lid = codes_jobmap_to_local_id(s->nw_id, jobmap_ctx);

        if(lid.job == -1)
        {
            s->app_id = -1;
            s->local_rank = -1;
            return;
        }
   }
   else
   {
       /* Only one job running with contiguous mapping */
       lid.job = 0;
       lid.rank = s->nw_id;
       s->app_id = 0;

       if((int)s->nw_id >= num_net_traces)
	        return;
   }

   s->num_own_job_ranks_completed = 0;

   int num_jobs = codes_jobmap_get_num_jobs(jobmap_ctx);
   s->known_completed_jobs = calloc(num_jobs, sizeof(int));

   if (strcmp(workload_type, "dumpi") == 0){
       strcpy(params_d.file_name, file_name_of_job[lid.job]);
       params_d.num_net_traces = num_traces_of_job[lid.job];
       params_d.nprocs = nprocs;
       params = (void*)&params_d;
       strcpy(type_name, "dumpi-trace-workload");

       if(strlen(workloads_conf_file) > 0)
       {
	        s->qos_level = qos_level_of_job[lid.job];
       }
//       printf("network LP nw id %d app id %d local rank %d generating events, lp gid is %ld \n", s->nw_id, 
//               s->app_id, s->local_rank, lp->gid);
#ifdef ENABLE_CORTEX_PYTHON
	strcpy(params_d.cortex_script, cortex_file);
	strcpy(params_d.cortex_class, cortex_class);
	strcpy(params_d.cortex_gen, cortex_gen);
#endif
   }
   else if(strcmp(workload_type, "swm-online") == 0){

       if(strlen(workload_name) > 0)
       {
           strcpy(oc_params.workload_name, workload_name); 
       }
       else if(strlen(workloads_conf_file) > 0)
       {
            short filename_supplied = 0;
            int len = strlen(file_name_of_job[lid.job]);
            if (len > 4) {
                char *last_five = &file_name_of_job[lid.job][len-5];
                if (strcmp(".json", last_five) == 0)
                {
                    filename_supplied = 1;
                }
            }
            if (filename_supplied) { //then we were supplied a filepath
                strcpy(oc_params.file_path, file_name_of_job[lid.job]);
                oc_params.workload_name[0] = '\0';
                if(lid.rank == 0)
                    printf("Workload Filepath provided: %s\n", oc_params.file_path);
            }
            else { //then we were supplied just the name of the workload and will use on of the defaults
                strcpy(oc_params.workload_name, file_name_of_job[lid.job]);
                oc_params.file_path[0] = '\0';
                if(lid.rank == 0)
                    printf("Workload name provided: %s\n",oc_params.workload_name);
            }

	        s->qos_level = qos_level_of_job[lid.job];
       }

       //assert(strcmp(oc_params.workload_name, "lammps") == 0 || strcmp(oc_params.workload_name, "nekbone") == 0);
       /*TODO: nprocs is different for dumpi and online workload. for
        * online, it is the number of ranks to be simulated. */
       oc_params.nprocs = num_traces_of_job[lid.job]; 
       params = (void*)&oc_params;
       strcpy(type_name, "swm_online_comm_workload");
   }
   //Xin: add conceputual online workload
   else if(strcmp(workload_type, "conc-online") == 0){

       if(strlen(workload_name) > 0)
       {
           strcpy(oc_params.workload_name, workload_name); 
       }
       else if(strlen(workloads_conf_file) > 0)
       {
            strcpy(oc_params.workload_name, file_name_of_job[lid.job]);      
       }
       /*TODO: nprocs is different for dumpi and online workload. for
        * online, it is the number of ranks to be simulated. */
       // printf("conc-online num_traces_of_job %d\n", num_traces_of_job[lid.job]);
       oc_params.nprocs = num_traces_of_job[lid.job]; 
       params = (void*)&oc_params;
       strcpy(type_name, "conc_online_comm_workload");
   }

   int rc = configuration_get_value_int(&config, "PARAMS", "num_qos_levels", NULL, &num_qos_levels);
   if(rc)
       num_qos_levels = 1; 
       
   s->app_id = lid.job;
   s->local_rank = lid.rank;

   double overhead;
   rc = configuration_get_value_double(&config, "PARAMS", "self_msg_overhead", NULL, &overhead);

   if(rc == 0)
       self_overhead = overhead;

   INIT_QLIST_HEAD(&s->arrival_queue);
   INIT_QLIST_HEAD(&s->pending_recvs_queue);
   INIT_QLIST_HEAD(&s->completed_reqs);
   INIT_QLIST_HEAD(&s->msg_sz_list);

   s->msg_sz_table = NULL;
   /* Initialize the RC stack */
   rc_stack_create(&s->processed_ops);
   rc_stack_create(&s->processed_wait_op);
   rc_stack_create(&s->matched_reqs);
    
   assert(s->processed_ops != NULL);
   assert(s->processed_wait_op != NULL);
   assert(s->matched_reqs != NULL);

   /* clock starts ticking when the first event is processed */
   s->start_time = tw_now(lp);
   s->num_bytes_sent = 0;
   s->num_bytes_recvd = 0;
   s->compute_time = 0;
   s->elapsed_time = 0;
        
   s->app_id = lid.job;
   s->local_rank = lid.rank;

   bool am_i_synthetic = false;
   if(strncmp(file_name_of_job[lid.job], "synthetic", 9) == 0)
   {
        am_i_synthetic = true;
        sscanf(file_name_of_job[lid.job], "synthetic%d", &synthetic_pattern);
        if(synthetic_pattern <=0 || synthetic_pattern > 6)
        {
            printf("\n Undefined synthetic pattern: setting to uniform random ");
            s->synthetic_pattern = 1;
        }
        else
        {
            s->synthetic_pattern = synthetic_pattern;
        }
	s->qos_level = qos_level_of_job[lid.job];

        tw_event * e;
        nw_message * m_new;
        // tw_stime ts = tw_rand_exponential(lp->rng, noise);
        tw_stime ts = 0;
        e = tw_event_new(lp->gid, ts, lp);
        m_new = (nw_message*)tw_event_data(e);
        m_new->msg_type = CLI_BCKGND_GEN;
        printf("\naddress difference = %ld\n", (&m_new->fwd.app_id - (int *)m_new));
        tw_event_send(e);
        is_synthetic = 1;

	if(lid.rank == 0){
		for(int k = 0; k < period_count[lid.job]; k++){
			tw_event * e2;
			nw_message * m_new2;
			tw_stime ts2 = period_time[lid.job][k];
			e2 = tw_event_new(lp->gid, ts2, lp);
			m_new2 = (nw_message*)tw_event_data(e2);
			m_new2->msg_type = CLI_BCKGND_CHANGE;
			m_new2->fwd.msg_send_time = period_interval[lid.job][k];  // Warning: this is overwriting a variable meant for message type MPI_SEND_ARRIVED_CB
			tw_event_send(e2);
		}
	}

   }
   else 
   {
   s->wrkld_id = codes_workload_load(type_name, params, s->app_id, s->local_rank);
   codes_issue_next_event(lp);
   }
   if(enable_sampling && sampling_interval > 0)
   {
       s->max_arr_size = MAX_STATS;
       s->cur_interval_end = sampling_interval;
       if(!g_tw_mynode && !s->nw_id)
       {
           fprintf(workload_meta_log, "\n mpi_proc_id app_id num_waits "
                   " num_sends num_bytes_sent sample_end_time");
       }
   }

   if (skip_iter_config_size > 0) {
       size_t size = 0;
       // Finding number of times to skip for this job
       for (size_t i = 0; i < skip_iter_config_size; i++) {
           if (lid.job == skip_iter_config[i].app_id) {
               size++;
           }
       }
       // Constructing switch_config
       s->switch_config_size = size;
       if (size > 0) {
          s->switch_config = malloc(size * sizeof(struct AvgSurrogateSwitchingTimesForApp));
          size_t j = 0;
          for (size_t i = 0; i < skip_iter_config_size; i++) {
              if (lid.job == skip_iter_config[i].app_id) {
                  s->switch_config[j] = skip_iter_config[i];
                  j++;
              }
          }
       }
   } else {
       s->switch_config = NULL;
       s->switch_config_size = 0;
   }
   if (iter_predictor && !am_i_synthetic) {
        struct app_iter_node_config conf = {
            .app_id = s->app_id,
            .app_ending_iter = s->app_id ? 19 : 20,
        };
        iter_predictor->model.init(lp, s->nw_id_in_pe, &conf);
   }

   return;
}

void nw_test_event_handler(nw_state* s, tw_bf * bf, nw_message * m, tw_lp * lp)
{
    assert(s->app_id >= 0 && s->local_rank >= 0);
#if LP_DEBUG
    s->num_events_processed++;
#endif /* if LP_DEBUG */

    memset(bf, 0, sizeof(tw_bf));
    rc_stack_gc(lp, s->matched_reqs);
    rc_stack_gc(lp, s->processed_ops);
    rc_stack_gc(lp, s->processed_wait_op);

    switch((enum MPI_NW_EVENTS) m->msg_type)
	{
		case MPI_SEND_ARRIVED:
			update_arrival_queue(s, bf, m, lp);
		break;

        case MPI_REND_ARRIVED:
        {
           /* update time of messages */
            mpi_msgs_queue mpi_op;
            mpi_op.op_type = m->op_type;
            mpi_op.tag = m->fwd.tag;
            mpi_op.num_bytes = m->fwd.num_bytes;
            mpi_op.source_rank = m->fwd.src_rank;
            mpi_op.dest_rank = m->fwd.dest_rank;
            mpi_op.req_init_time = m->fwd.sim_start_time;
           
            if(enable_msg_tracking)
                update_message_size(s, lp, bf, m, &mpi_op, 0, 1);
        
            int global_src_id = m->fwd.src_rank;
            
            if(alloc_spec)
                global_src_id = get_global_id_of_job_rank(m->fwd.src_rank, s->app_id);
            
            tw_event *e_callback =
            tw_event_new(rank_to_lpid(global_src_id),
                0, lp);
                // codes_local_latency(lp), lp);
            nw_message *m_callback = (nw_message*)tw_event_data(e_callback);
            m_callback->msg_type = MPI_SEND_ARRIVED_CB;
            m_callback->fwd.msg_send_time = tw_now(lp) - m->fwd.sim_start_time;
            tw_event_send(e_callback);
           
            /* request id pending completion */
            if(m->fwd.matched_req >= 0)
            {
                bf->c8 = 1;
                update_completed_queue(s, bf, m, lp, m->fwd.matched_req);
            }
            else /* blocking receive pending completion*/
            {
                bf->c10 = 1;
                codes_issue_next_event(lp);
            }
            
            m->rc.mpi_send.saved_recv_time = s->recv_time;
            m->rc.mpi_send.saved_recv_time_sample = s->ross_sample.recv_time;
            s->recv_time += (tw_now(lp) - m->fwd.sim_start_time);
            s->ross_sample.recv_time += (tw_now(lp) - m->fwd.sim_start_time);

        }
        break;
        case MPI_REND_ACK_ARRIVED:
        {
         /* reconstruct the op and pass it on for actual data transfer */ 
            int is_rend = 1;

            struct codes_workload_op mpi_op;
            mpi_op.op_type = m->op_type;
            mpi_op.u.send.tag = m->fwd.tag;
            mpi_op.u.send.num_bytes = m->fwd.num_bytes;
            mpi_op.u.send.source_rank = m->fwd.src_rank;
            mpi_op.u.send.dest_rank = m->fwd.dest_rank;
            mpi_op.sim_start_time = m->fwd.sim_start_time;
            mpi_op.u.send.req_id = m->fwd.req_id;

            codes_exec_mpi_send(s, bf, m, lp, &mpi_op, is_rend);
        }
        break;

		case MPI_SEND_ARRIVED_CB:
			update_message_time(s, bf, m, lp);
		break;

        case MPI_SEND_POSTED:
        {
           int is_eager = 0;

           if(m->fwd.num_bytes < EAGER_THRESHOLD)
               is_eager = 1;

           if(m->op_type == CODES_WK_SEND && (is_eager == 1 || m->fwd.rend_send == 1))
           {
               bf->c29 = 1;
               codes_issue_next_event(lp);
           }
           else
            if(m->op_type == CODES_WK_ISEND && (is_eager == 1 || m->fwd.rend_send == 1))
            {
//              tw_output(lp, "\n isend req id %llu ", m->fwd.req_id);
                bf->c28 = 1;
                update_completed_queue(s, bf, m, lp, m->fwd.req_id);
            }
        }
        break;

        case MPI_OP_GET_NEXT:
			get_next_mpi_operation(s, bf, m, lp);
		break;
        
        case CLI_BCKGND_GEN:
            gen_synthetic_tr(s, bf, m, lp);
        break;

        case CLI_BCKGND_CHANGE:
            m->rc.change.saved_send_time = mean_interval_of_job[s->app_id];  // Warning: this is overwriting a variable meant for message type MPI_OP_GET_NEXT (specifically CODES_WK_ALLREDUCE) and CLI_BCKGND_ARRIVE
            mean_interval_of_job[s->app_id] = m->fwd.msg_send_time;
            m->rc.change.saved_marker_time = tw_now(lp);
            break;

        case CLI_BCKGND_ARRIVE:
            arrive_syn_tr(s, bf, m, lp);
            break;
        
        case CLI_NBR_FINISH:
            handle_neighbor_finish(s, lp, bf, m);
            break;
        
        case CLI_BCKGND_FIN:
            finish_bckgnd_traffic(s, bf, m, lp);
            break;

        case CLI_OTHER_FINISH:
            handle_other_finish(s, lp, bf, m);
            break;

        case SURR_SKIP_ITERATION:
            skip_to_iteration(s, lp, bf, m);
            break;
	}
}

static void get_next_mpi_operation_rc(nw_state* s, tw_bf * bf, nw_message * m, tw_lp * lp)
{
    codes_workload_get_next_rc(s->wrkld_id, s->app_id, s->local_rank, m->mpi_op);

	if(m->op_type == CODES_WK_END)
    {
        s->is_finished = 0;
        s->elapsed_time = m->rc.mpi_next.saved_elapsed_time;

        if(bf->c9)
            return;

        if(bf->c19)
            return;

        notify_root_rank_rc(s, lp, bf, m);
		return;
    }
	switch(m->op_type)
	{
		case CODES_WK_SEND:
		case CODES_WK_ISEND:
		{
            codes_exec_mpi_send_rc(s, bf, m, lp);
		}
		break;

		case CODES_WK_IRECV:
		case CODES_WK_RECV:
		{
			codes_exec_mpi_recv_rc(s, bf, m, lp);
			s->num_recvs--;
                        s->ross_sample.num_recvs--;
		}
		break;


        case CODES_WK_DELAY:
		{
			s->num_delays--;
            if(disable_delay)
                codes_issue_next_event_rc(lp);
            else
            {
                // if (bf->c28)
                //     tw_rand_reverse_unif(lp->rng);
                s->compute_time = m->rc.mpi_next.delay.saved_delay;
                s->ross_sample.compute_time = m->rc.mpi_next.delay.saved_delay_sample;
            }
		}
		break;
		case CODES_WK_ALLREDUCE:
        {
            if(bf->c27)
            {
                s->num_all_reduce--;
                s->col_time = m->rc.mpi_next.all_reduce.saved_send_time;
                s->all_reduce_time = m->rc.mpi_next.all_reduce.saved_delay;
            }
            else
            {
               s->col_time = 0; 
            }
            codes_issue_next_event_rc(lp);
        }
        break;
		case CODES_WK_BCAST:
		case CODES_WK_ALLGATHER:
		case CODES_WK_ALLGATHERV:
		case CODES_WK_ALLTOALL:
		case CODES_WK_ALLTOALLV:
		case CODES_WK_REDUCE:
		case CODES_WK_COL:
		{
			s->num_cols--;
		    codes_issue_next_event_rc(lp);
        }
		break;

		case CODES_WK_WAITSOME:
		case CODES_WK_WAITANY:
        {
           s->num_waitsome--;
           codes_issue_next_event_rc(lp);
        }
        break;

		case CODES_WK_WAIT:
		{
			s->num_wait--;
			codes_exec_mpi_wait_rc(s, bf, lp, m);
		}
		break;
		case CODES_WK_WAITALL:
		{
			s->num_waitall--;
            codes_exec_mpi_wait_all_rc(s, bf, m, lp);
		}
		break;
	case CODES_WK_MARK:
		codes_issue_next_event_rc(lp);
        if (bf->c13) {
            iter_predictor->model.predict_rc(lp, s->nw_id_in_pe);
        }
		break;

		default:
			printf("\n Invalid op type %d ", m->op_type);
	}
    free(m->mpi_op);
}

static void get_next_mpi_operation(nw_state* s, tw_bf * bf, nw_message * m, tw_lp * lp)
{
		//struct codes_workload_op * mpi_op = malloc(sizeof(struct codes_workload_op));
//        printf("\n App id %d local rank %d ", s->app_id, s->local_rank);
	    struct codes_workload_op * mpi_op = (struct codes_workload_op*)malloc(sizeof(struct codes_workload_op));
        codes_workload_get_next(s->wrkld_id, s->app_id, s->local_rank, mpi_op);
        m->mpi_op = mpi_op; 
        m->op_type = mpi_op->op_type;
	
        if(mpi_op->op_type == CODES_WK_END)
        {
            m->rc.mpi_next.saved_elapsed_time = s->elapsed_time;
            s->elapsed_time = tw_now(lp) - s->start_time;
            s->is_finished = 1;

            if(!alloc_spec)
            {
                bf->c9 = 1;
                return;
            }

            /* Notify ranks from other job that checkpoint traffic has
             * completed */
            //int num_jobs = codes_jobmap_get_num_jobs(jobmap_ctx);
            m->rc.mpi_next.mark.saved_marker_time = tw_now(lp);
            notify_root_rank(s, lp, bf, m);
            // printf("Client rank %llu completed workload, local rank %d .\n", s->nw_id, s->local_rank);

             return;
        }
		switch(mpi_op->op_type)
		{
			case CODES_WK_SEND:
			case CODES_WK_ISEND:
			 {
                //printf("\n MPI SEND ");
				codes_exec_mpi_send(s, bf, m, lp, mpi_op, 0);
			 }
			break;

			case CODES_WK_RECV:
			case CODES_WK_IRECV:
			  {
				s->num_recvs++;
                                s->ross_sample.num_recvs++;
                //printf("\n MPI RECV ");
				codes_exec_mpi_recv(s, bf, m, lp, mpi_op);
			  }
			break;

			case CODES_WK_DELAY:
			  {
                //printf("\n MPI DELAY ");
				s->num_delays++;
                if(disable_delay)
                    codes_issue_next_event(lp);
                else
				    codes_exec_comp_delay(s, bf, m, lp, mpi_op);
			  }
			break;

            case CODES_WK_WAITSOME:
            case CODES_WK_WAITANY:
            {
                //printf("\n MPI WAITANY WAITSOME ");
                s->num_waitsome++;
                codes_issue_next_event(lp);
            }
            break;

			case CODES_WK_WAITALL:
			  {
                //printf("\n MPI WAITALL ");
				s->num_waitall++;
			    codes_exec_mpi_wait_all(s, bf, m, lp, mpi_op);
                //codes_issue_next_event(lp);
              }
			break;
			case CODES_WK_WAIT:
			{
                //printf("\n MPI WAIT ");
				s->num_wait++;
                //TODO: Uncomment:
                codes_exec_mpi_wait(s, bf, m, lp, mpi_op);
			}
			break;
			case CODES_WK_ALLREDUCE:
            {
				s->num_cols++;
                if(s->col_time > 0)
                {
                    bf->c27 = 1;
                    m->rc.mpi_next.all_reduce.saved_delay = s->all_reduce_time;
                    s->all_reduce_time += tw_now(lp) - s->col_time;
                    m->rc.mpi_next.all_reduce.saved_send_time = s->col_time;
                    s->col_time = 0;
                    s->num_all_reduce++;
                }
                else
                {
                   s->col_time = tw_now(lp); 
                }
			    codes_issue_next_event(lp);
            }
            break;
			
            case CODES_WK_REDUCE:
			case CODES_WK_BCAST:
			case CODES_WK_ALLGATHER:
			case CODES_WK_ALLGATHERV:
			case CODES_WK_ALLTOALL:
			case CODES_WK_ALLTOALLV:
			case CODES_WK_COL:
			{
				s->num_cols++;
			    codes_issue_next_event(lp);
            }
			break;

		case CODES_WK_MARK:
			{
                m->rc.mpi_next.mark.saved_marker_time = tw_now(lp);
                int iteration_i = mpi_op->u.send.tag;

                if (HARD_CODED_AVG_ITER_PREDICTOR) {
                    // If we have reached the surrogate switch time, skip next iteration(s)
                    if (have_we_hit_surrogate_switch(s, mpi_op)) {
                        tw_event *e = tw_event_new(lp->gid, time_to_skip_iterations(s), lp);
                        nw_message* msg = (nw_message*) tw_event_data(e);
                        msg->msg_type = SURR_SKIP_ITERATION;
                        tw_event_send(e);
                    } else {
                        codes_issue_next_event(lp);
                    }
                } else {
                    if (iter_predictor && iter_predictor->model.have_we_hit_switch(lp, s->nw_id_in_pe, iteration_i)) {
                        bf->c13 = 1;
                        struct iteration_pred iter_pred = iter_predictor->model.predict(lp, s->nw_id_in_pe);
                        tw_event *e = tw_event_new(lp->gid, iter_pred.restart_at - tw_now(lp), lp);
                        nw_message* msg = (nw_message*) tw_event_data(e);
                        msg->msg_type = SURR_SKIP_ITERATION;
                        msg->fwd.resume_at_iter = iter_pred.resume_at_iter;
                        tw_event_send(e);
                    } else {
                        codes_issue_next_event(lp);
                    }
                }
			}
			break;


			default:
				printf("\n Invalid op type %d ", mpi_op->op_type);
		}
        return;
}

void nw_test_finalize(nw_state* s, tw_lp* lp)
{
    total_syn_data += s->syn_data;

    int written = 0;
    double avg_msg_time = 0;
    /*if(s->wait_op)
    {
        lprintf("\n Incomplete wait operation Rank %llu ", s->nw_id);
        print_waiting_reqs(s->wait_op->req_ids, s->wait_op->count);
        print_completed_queue(&s->completed_reqs);
    }*/
    if(alloc_spec == 1)
    {
        struct codes_jobmap_id lid;
        lid = codes_jobmap_to_local_id(s->nw_id, jobmap_ctx);

        if(lid.job < 0)
            return;
        if(strncmp(file_name_of_job[lid.job], "synthetic", 9) == 0)
            avg_msg_time = (s->send_time / s->num_recvs);
        else if(strcmp(workload_type, "swm-online") == 0) 
            codes_workload_finalize("swm_online_comm_workload", params, s->app_id, s->local_rank);
        //Xin: for conceptual online workload
        else if(strcmp(workload_type, "conc-online") == 0)
            codes_workload_finalize("conc_online_comm_workload", params, s->app_id, s->local_rank);
    }
    else
    {
        if(s->nw_id >= (tw_lpid)num_net_traces)
            return;
        
        if(strcmp(workload_type, "swm-online") == 0) 
            codes_workload_finalize("swm_online_comm_workload", params, s->app_id, s->local_rank);
        //Xin: for conceptual online workload
        if(strcmp(workload_type, "conc-online") == 0)
            codes_workload_finalize("conc_online_comm_workload", params, s->app_id, s->local_rank); 
    }

        struct msg_size_info * tmp_msg = NULL; 
        struct qlist_head * ent = NULL;

        if(s->local_rank == 0 && enable_msg_tracking)
            fprintf(msg_size_log, "\n rank_id message_size num_messages avg_latency");
        
        if(enable_msg_tracking)
        {
            qlist_for_each(ent, &s->msg_sz_list)
            {
                tmp_msg = qlist_entry(ent, struct msg_size_info, ql);
                printf("\n Rank %d Msg size %"PRId64" num_msgs %d agg_latency %f avg_latency %f",
                        s->local_rank, tmp_msg->msg_size, tmp_msg->num_msgs, tmp_msg->agg_latency, tmp_msg->avg_latency);
                //fprintf(msg_size_log, "\n Rank %d Msg size %d num_msgs %d agg_latency %f avg_latency %f",
                //        s->local_rank, tmp_msg->msg_size, tmp_msg->num_msgs, tmp_msg->agg_latency, tmp_msg->avg_latency);
                if(s->local_rank == 0)
                {
                    fprintf(msg_size_log, "\n %llu %"PRId64" %d %f",
                        LLU(s->nw_id), tmp_msg->msg_size, tmp_msg->num_msgs, tmp_msg->avg_latency);
                }
            }
        }
		int count_irecv = 0, count_isend = 0;
        count_irecv = qlist_count(&s->pending_recvs_queue);
        count_isend = qlist_count(&s->arrival_queue);
		if(count_irecv > 0 || count_isend > 0)
        {
            unmatched = 1;
            printf("\n nw-id %llu unmatched irecvs %d unmatched sends %d Total sends %ld receives %ld collectives %ld delays %ld wait alls %ld waits %ld send time %lf wait %lf",
			    LLU(s->nw_id) , count_irecv, count_isend, s->num_sends, s->num_recvs, s->num_cols, s->num_delays, s->num_waitall, s->num_wait, s->send_time, s->wait_time);
        }
        written = 0;
    
        if(!s->nw_id)
            written = sprintf(s->output_buf, "# Format <LP ID> <Terminal ID> <Job ID> <Local Rank> <Total sends> <Total Recvs> <Bytes sent> <Bytes recvd> <Send time> <Comm. time> <Compute time> <Avg msg time> <Max Msg Time>");

        written += sprintf(s->output_buf + written, "\n %llu %llu %d %d %ld %ld %llu %llu %lf %lf %lf %lf %lf", LLU(lp->gid), LLU(s->nw_id), s->app_id, s->local_rank, s->num_sends, s->num_recvs, s->num_bytes_sent,
                s->num_bytes_recvd, s->send_time, s->elapsed_time - s->compute_time, s->compute_time, avg_msg_time, s->max_time);
        lp_io_write(lp->gid, (char*)"mpi-replay-stats", written, s->output_buf);

        tw_stime my_comm_time = s->elapsed_time - s->compute_time;

		if(my_comm_time > max_comm_time)
			max_comm_time = my_comm_time;

        if(s->elapsed_time > max_elapsed_time_per_job[s->app_id])
            max_elapsed_time_per_job[s->app_id] = s->elapsed_time;

		if(s->elapsed_time > max_time )
			max_time = s->elapsed_time;

        if(count_irecv || count_isend)
        {
            print_msgs_queue(&s->pending_recvs_queue, 0);
            print_msgs_queue(&s->arrival_queue, 1);
        }
        if(enable_sampling)
        {
            fseek(workload_agg_log, sample_bytes_written, SEEK_SET);
            fwrite(s->mpi_wkld_samples, sizeof(struct mpi_workload_sample), s->sampling_indx + 1, workload_agg_log);
        }
        sample_bytes_written += (s->sampling_indx * sizeof(struct mpi_workload_sample));
		if(s->wait_time > max_wait_time)
			max_wait_time = s->wait_time;
        
		if(s->send_time > max_send_time)
			max_send_time = s->send_time;

		if(s->recv_time > max_recv_time)
			max_recv_time = s->recv_time;

        written = 0;

        if(debug_cols)
            written += sprintf(s->col_stats + written, "%llu \t %lf \n", LLU(s->nw_id), ns_to_s(s->all_reduce_time / s->num_all_reduce));
		
        lp_io_write(lp->gid, (char*)"avg-all-reduce-time", written, s->col_stats);

        avg_time += s->elapsed_time;
		avg_comm_time += (s->elapsed_time - s->compute_time);
		avg_wait_time += s->wait_time;
		avg_send_time += s->send_time;
		avg_recv_time += s->recv_time;

		//printf("\n LP %ld Time spent in communication %llu ", lp->gid, total_time - s->compute_time);
	    rc_stack_destroy(s->matched_reqs);
	    rc_stack_destroy(s->processed_ops);
	    rc_stack_destroy(s->processed_wait_op);

    if (s->switch_config != NULL) {
        free(s->switch_config);
    }
}

void nw_test_event_handler_rc(nw_state* s, tw_bf * bf, nw_message * m, tw_lp * lp)
{
#if LP_DEBUG
    s->num_events_processed--;
#endif /* if LP_DEBUG */

	switch(m->msg_type)
	{
		case MPI_SEND_ARRIVED:
			update_arrival_queue_rc(s, bf, m, lp);
		break;

		case MPI_SEND_ARRIVED_CB:
			update_message_time_rc(s, bf, m, lp);
		break;

        case MPI_SEND_POSTED:
        {
         if(bf->c29)
             codes_issue_next_event_rc(lp);
         if(bf->c28)
            update_completed_queue_rc(s, bf, m, lp);
        }
        break;

        case MPI_REND_ACK_ARRIVED:
        {
            codes_exec_mpi_send_rc(s, bf, m, lp);
        }
        break;

        case MPI_REND_ARRIVED:
        {
            if(bf->c10)
                codes_issue_next_event_rc(lp);

            if(bf->c8)
                update_completed_queue_rc(s, bf, m, lp);
            
            s->recv_time = m->rc.mpi_send.saved_recv_time;
            s->ross_sample.recv_time = m->rc.mpi_send.saved_recv_time_sample;
        }
        break;

        case MPI_OP_GET_NEXT:
			get_next_mpi_operation_rc(s, bf, m, lp);
		break;
        
        case CLI_BCKGND_GEN:
            gen_synthetic_tr_rc(s, bf, m, lp);
            break;

        case CLI_BCKGND_CHANGE:
	    mean_interval_of_job[s->app_id] = m->rc.change.saved_send_time;
	    break;

        case CLI_BCKGND_ARRIVE:
            arrive_syn_tr_rc(s, bf, m, lp);
            break;
        
        case CLI_NBR_FINISH:
            handle_neighbor_finish_rc(s, lp, bf, m);
            break;
        
        case CLI_BCKGND_FIN:
            finish_bckgnd_traffic_rc(s, bf, m, lp);
            break;

        case CLI_OTHER_FINISH:
            handle_other_finish_rc(s, lp, bf, m);
            break;

        case SURR_SKIP_ITERATION:
            skip_iteration_rc(s, lp, bf, m);
            break;
	}
}

void nw_test_event_handler_commit(nw_state* s, tw_bf * bf, nw_message * m, tw_lp * lp)
{
    switch(m->msg_type)
    {
        case MPI_OP_GET_NEXT:
            switch (m->mpi_op->op_type) {
                case CODES_WK_END:
                    printf("Network node %d Rank %llu App %d finished at %lf \n", s->local_rank, LLU(s->nw_id), s->app_id, m->rc.mpi_next.mark.saved_marker_time);
                    if (iter_predictor) {
                        iter_predictor->model.ended(lp, s->nw_id_in_pe, m->rc.mpi_next.mark.saved_marker_time);
                    }
                    break;

                case CODES_WK_MARK:
                    fprintf(iteration_log, "ITERATION %d node %llu job %d rank %d time %lf\n", m->mpi_op->u.send.tag, LLU(s->nw_id), s->app_id, s->local_rank, m->rc.mpi_next.mark.saved_marker_time);
                    if (iter_predictor) {
                        iter_predictor->model.feed(lp, s->nw_id_in_pe, m->mpi_op->u.send.tag, m->rc.mpi_next.mark.saved_marker_time);
                    }

                    if (OUTPUT_MARKS)
                    {
                        int written1;
                        char marker_filename[128];
                        written1 = sprintf(marker_filename, "mpi-replay-marker-tag-times");
                        marker_filename[written1] = '\0';

                        char tag_line[32];
                        int written;
                        written = sprintf(tag_line, "%llu %d %.5f\n",s->nw_id, m->mpi_op->u.send.tag, m->rc.mpi_next.mark.saved_marker_time);
                        lp_io_write(lp->gid, marker_filename, written, tag_line);
                    }
                    break;

                default:
                    break;
            }



            free(m->mpi_op);
        break;
        case SURR_SKIP_ITERATION:
            break;

        case CLI_BCKGND_CHANGE:
            printf("======== CHANGE [now: %lf] App|Job:%d | Period: %f\n", m->rc.change.saved_marker_time, s->app_id, m->fwd.msg_send_time);
            break;
    }
}

static void make_qlist_cpy(struct qlist_head * into, struct qlist_head const * from, unsigned int sizeof_elem, unsigned int offset_ql) {
    assert(sizeof_elem > offset_ql);

    int const num_elems = qlist_count(from);
    INIT_QLIST_HEAD(into);
    if (num_elems) {
        char * pending_recvs = malloc(num_elems * sizeof_elem);
        if (pending_recvs == NULL) {
            tw_error(TW_LOC, "Malloc failed!");
        }

        char * new_entry = pending_recvs;
        int i = 0;
        struct qlist_head * ent;
        qlist_for_each(ent, from) {
            char * entry = ((char*)ent) - offset_ql;

            mempcpy(new_entry, entry, sizeof_elem);
            struct qlist_head * new_entry_ql = (void*) (new_entry + offset_ql);
            new_entry_ql->prev = (void*)(new_entry - sizeof_elem + offset_ql);
            new_entry_ql->next = (void*)(new_entry + sizeof_elem + offset_ql);
            i++;
            new_entry += sizeof_elem;
        }
        assert(i == num_elems);

        struct qlist_head * first_ql = (void*)(pending_recvs + offset_ql);
        struct qlist_head * last_ql = (void*)(pending_recvs + (num_elems - 1) * sizeof_elem + offset_ql);
        into->next = first_ql;
        into->prev = last_ql;
        first_ql->prev = into;
        last_ql->next = into;
    }
}

static void free_qlist_cpy(struct qlist_head * into, unsigned int offset_ql) {
    if (! qlist_empty(into)) {
        void * entry = (char *)(into->next) - offset_ql;
        free(entry);
    }
}

bool compare_pending_waits(struct pending_waits const * before, struct pending_waits const * after) {
    // if one is null and the other isn't, then they're not equal
    if ((before == NULL) != (after == NULL)) {
        return false;
    }
    // only check values if they are not nul
    if (before == NULL) {
        return true;
    }

    bool is_same = true;

    is_same &= before->op_type == after->op_type;
    is_same &= before->num_completed == after->num_completed;
    is_same &= before->count == after->count;
    is_same &= before->start_time == after->start_time;

    for (int i=0; i<before->count; i++) {
        is_same &= before->req_ids[i] == after->req_ids[i];
    }

    return is_same;
}

static bool compare_mpi_msg_queues(mpi_msgs_queue * left, mpi_msgs_queue * right) {
    bool is_same = true;
    is_same &= left->op_type == right->op_type;
    is_same &= left->tag == right->tag;
    is_same &= left->source_rank == right->source_rank;
    is_same &= left->dest_rank == right->dest_rank;
    is_same &= left->num_bytes == right->num_bytes;
    is_same &= left->req_init_time == right->req_init_time;
    is_same &= left->req_id == right->req_id;
    return is_same;
}

static bool compare_completed_requests(completed_requests * left, completed_requests * right) {
    bool is_same = true;
    is_same &= left->req_id == right->req_id;
    return is_same;
}

static bool compare_msg_size_info(struct msg_size_info * left, struct msg_size_info * right) {
    bool is_same = true;
    is_same &= left->msg_size == right->msg_size;
    is_same &= left->num_msgs == right->num_msgs;
    is_same &= left->agg_latency == right->agg_latency;
    is_same &= left->avg_latency == right->avg_latency;
    is_same &= left->hash_link.next == right->hash_link.next; // This is not correct, we have to do deep copy this and chek that it is the same
    is_same &= left->hash_link.prev == right->hash_link.prev;
    return is_same;
}

// Deep-copy of nw_state!!
// Functionality to check for correct implementation of reverse event handler
static void save_nw_lp_state(nw_state * into, nw_state const * from) {
    memcpy(into, from, sizeof(nw_state));

    make_qlist_cpy(&into->arrival_queue, &from->arrival_queue,sizeof(mpi_msgs_queue), QLIST_OFFSET(mpi_msgs_queue, ql));
    make_qlist_cpy(&into->pending_recvs_queue, &from->pending_recvs_queue, sizeof(mpi_msgs_queue), QLIST_OFFSET(mpi_msgs_queue, ql));
    make_qlist_cpy(&into->completed_reqs, &from->completed_reqs, sizeof(completed_requests), QLIST_OFFSET(completed_requests, ql));
    make_qlist_cpy(&into->msg_sz_list, &from->msg_sz_list, sizeof(struct msg_size_info), QLIST_OFFSET(struct msg_size_info, ql));
    // No need to copy msg_sz_table because all data is also in msg_sz_list

    int num_jobs = codes_jobmap_get_num_jobs(jobmap_ctx);
    into->known_completed_jobs = malloc(num_jobs * sizeof(int));
    memcpy(into->known_completed_jobs, from->known_completed_jobs, num_jobs * sizeof(int));
    if (from->wait_op != NULL) {
        into->wait_op = malloc(sizeof(pending_waits));
        memcpy(into->wait_op, from->wait_op, sizeof(pending_waits));
    }

    // Don't forget to make deep copies of any new complex data types that nw_state points to
}

static void print_mpi_msgs_queue(FILE * out, char const * prefix, struct qlist_head * head) {
    mpi_msgs_queue * current = NULL;
    qlist_for_each_entry(current, head, ql) {
         fprintf(out, "%sMsg: OpType: %d Tag %d Source %d Dest %d bytes %"PRId64" req_init_time %g req_id %u\n", prefix, current->op_type, current->tag, current->source_rank, current->dest_rank, current->num_bytes, current->req_init_time, current->req_id);
    }
}

// Cleaning up deep-copy
static void clean_nw_lp_state(nw_state * into) {
    free_qlist_cpy(&into->arrival_queue, QLIST_OFFSET(mpi_msgs_queue, ql));
    free_qlist_cpy(&into->pending_recvs_queue, QLIST_OFFSET(mpi_msgs_queue, ql));
    free_qlist_cpy(&into->completed_reqs, QLIST_OFFSET(completed_requests, ql));
    free_qlist_cpy(&into->msg_sz_list, QLIST_OFFSET(struct msg_size_info, ql));
    free(into->known_completed_jobs);
    if (into->wait_op != NULL) {
        free(into->wait_op);
    }
}

// Checking that deep-copy is the same as original!!
// Originally filled with a prompt on Claude
static bool check_nw_lp_state(nw_state * before, nw_state const * after) {
    bool is_same = true;

    // Basic fields
    is_same &= (before->nw_id == after->nw_id);
    is_same &= (before->wrkld_id == after->wrkld_id);
    is_same &= (before->app_id == after->app_id);
    is_same &= (before->local_rank == after->local_rank);
    is_same &= (before->qos_level == after->qos_level);

    // Pattern and completion flags
    is_same &= (before->synthetic_pattern == after->synthetic_pattern);
    is_same &= (before->is_finished == after->is_finished);
    is_same &= (before->num_own_job_ranks_completed == after->num_own_job_ranks_completed);

    // Operation counts
    is_same &= (before->num_sends == after->num_sends);
    is_same &= (before->num_recvs == after->num_recvs);
    is_same &= (before->num_cols == after->num_cols);
    is_same &= (before->num_delays == after->num_delays);
    is_same &= (before->num_wait == after->num_wait);
    is_same &= (before->num_waitall == after->num_waitall);
    is_same &= (before->num_waitsome == after->num_waitsome);

    // Timing information
    is_same &= (before->start_time == after->start_time);
    is_same &= (before->col_time == after->col_time);
    is_same &= (before->reduce_time == after->reduce_time);
    is_same &= (before->num_reduce == after->num_reduce);
    is_same &= (before->all_reduce_time == after->all_reduce_time);
    is_same &= (before->num_all_reduce == after->num_all_reduce);
    is_same &= (before->elapsed_time == after->elapsed_time);
    is_same &= (before->compute_time == after->compute_time);
    is_same &= (before->send_time == after->send_time);
    is_same &= (before->max_time == after->max_time);
    is_same &= (before->recv_time == after->recv_time);
    is_same &= (before->wait_time == after->wait_time);

    // Interval and current state
    is_same &= (before->cur_interval_end == after->cur_interval_end);

    // Data statistics
    is_same &= (before->num_bytes_sent == after->num_bytes_sent);
    is_same &= (before->num_bytes_recvd == after->num_bytes_recvd);
    is_same &= (before->syn_data == after->syn_data);
    is_same &= (before->gen_data == after->gen_data);

    // Switch and routing information
    is_same &= (before->prev_switch == after->prev_switch);
    is_same &= (before->saved_perm_dest == after->saved_perm_dest);
    is_same &= (before->rc_perm == after->rc_perm);

    // Sampling information
    is_same &= (before->sampling_indx == after->sampling_indx);
    //is_same &= (before->max_arr_size == after->max_arr_size);

    // Compare string buffers
    is_same &= (strcmp(before->output_buf, after->output_buf) == 0);
    is_same &= (strcmp(before->col_stats, after->col_stats) == 0);

    // Compare switch configuration size
    is_same &= (before->switch_config_size == after->switch_config_size);

    // Complex elements
    is_same &= are_qlist_equal(&before->arrival_queue, &after->arrival_queue, QLIST_OFFSET(mpi_msgs_queue, ql), (bool (*) (void *, void *)) compare_mpi_msg_queues);
    is_same &= are_qlist_equal(&before->pending_recvs_queue, &after->pending_recvs_queue, QLIST_OFFSET(mpi_msgs_queue, ql), (bool (*) (void *, void *)) compare_mpi_msg_queues);
    is_same &= are_qlist_equal(&before->completed_reqs, &after->completed_reqs, QLIST_OFFSET(completed_requests, ql), (bool (*) (void *, void *)) compare_completed_requests);
    is_same &= are_qlist_equal(&before->msg_sz_list, &after->msg_sz_list, QLIST_OFFSET(struct msg_size_info, ql), (bool (*) (void *, void *)) compare_msg_size_info);

    is_same &= !memcmp(&before->ross_sample, &after->ross_sample, sizeof(struct ross_model_sample));

    int num_jobs = codes_jobmap_get_num_jobs(jobmap_ctx);
    is_same &= !memcmp(before->known_completed_jobs, after->known_completed_jobs, num_jobs * sizeof(int));
    is_same &= compare_pending_waits(before->wait_op, after->wait_op);

    // Skipped pointer comparisons (used in reverse computation):
    // - processed_ops
    // - processed_wait_op
    // - matched_reqs
    // - msg_sz_table
    // Pointers used in some data collection (IO) or outside of PDES loop
    // - mpi_wkld_samples
    // - switch_config

    // There is no need to implement msg_sz_table as all values are already
    // accounted for in msg_sz_list. We can safely ignore all values in msg_sz_list

    return is_same;
}

// Originally implemneted with a prompt on Claude.ai (tedious code, easy to check and produce)
static void print_nw_lp_state(FILE * out, char const * prefix, nw_state * state) {
    int num_jobs = codes_jobmap_get_num_jobs(jobmap_ctx);

    fprintf(out, "%snw-lp state ->\n", prefix);
#if LP_DEBUG
    fprintf(out, "%s |  num_events_processed = %zu\n", prefix, state->num_events_processed);
#endif /* if LP_DE%sBUG */
    fprintf(out, "%s |                 nw_id = %lu\n", prefix, state->nw_id);
    fprintf(out, "%s |             wrkld_end = %d\n", prefix, state->wrkld_id);
    fprintf(out, "%s |                app_id = %d\n", prefix, state->app_id);
    fprintf(out, "%s |            local_rank = %d\n", prefix, state->local_rank);
    fprintf(out, "%s |             qos_level = %d\n", prefix, state->qos_level);
    fprintf(out, "%s |     synthetic_pattern = %d\n", prefix, state->synthetic_pattern);
    fprintf(out, "%s |           is_finished = %d\n", prefix, state->is_finished);
    fprintf(out, "%s |num_own_job_ranks_completed = %d\n", prefix, state->num_own_job_ranks_completed);
    fprintf(out, "%s |  known_completed_jobs[%d] = [", prefix, num_jobs);
    for(int i=0; i<num_jobs; i++) {
        fprintf(out, "%s%d%s", prefix, state->known_completed_jobs[i], i+1==num_jobs ? "" : ", ");
    }
    fprintf(out, "]\n");
    fprintf(out, "%s |        *processed_ops = %p\n", prefix, state->processed_ops);
    fprintf(out, "%s |    *processed_wait_op = %p\n", prefix, state->processed_wait_op);
    fprintf(out, "%s |         *matched_reqs = %p\n", prefix, state->matched_reqs);

    // Operation counts
    fprintf(out, "%s |             num_sends = %lu\n", prefix, state->num_sends);
    fprintf(out, "%s |             num_recvs = %lu\n", prefix, state->num_recvs);
    fprintf(out, "%s |              num_cols = %lu\n", prefix, state->num_cols);
    fprintf(out, "%s |            num_delays = %lu\n", prefix, state->num_delays);
    fprintf(out, "%s |              num_wait = %lu\n", prefix, state->num_wait);
    fprintf(out, "%s |           num_waitall = %lu\n", prefix, state->num_waitall);
    fprintf(out, "%s |          num_waitsome = %lu\n", prefix, state->num_waitsome);

    // Timing information
    fprintf(out, "%s |            start_time = %g\n", prefix, state->start_time);
    fprintf(out, "%s |              col_time = %g\n", prefix, state->col_time);
    fprintf(out, "%s |           reduce_time = %g\n", prefix, state->reduce_time);
    fprintf(out, "%s |            num_reduce = %d\n", prefix, state->num_reduce);
    fprintf(out, "%s |       all_reduce_time = %g\n", prefix, state->all_reduce_time);
    fprintf(out, "%s |        num_all_reduce = %d\n", prefix, state->num_all_reduce);
    fprintf(out, "%s |          elapsed_time = %g\n", prefix, state->elapsed_time);
    fprintf(out, "%s |          compute_time = %g\n", prefix, state->compute_time);
    fprintf(out, "%s |             send_time = %g\n", prefix, state->send_time);
    fprintf(out, "%s |              max_time = %g\n", prefix, state->max_time);
    fprintf(out, "%s |             recv_time = %g\n", prefix, state->recv_time);
    fprintf(out, "%s |             wait_time = %g\n", prefix, state->wait_time);

    // Queue heads
    char addprefix[] = " |  | ";
    int len_subprefix = snprintf(NULL, 0, "%s%s", prefix, addprefix) + 1;
    char subprefix[len_subprefix];
    snprintf(subprefix, len_subprefix, "%s%s", prefix, addprefix);

    fprintf(out, "%s |         arrival_queue[%d] = [\n", prefix, qlist_count(&state->arrival_queue));
    print_mpi_msgs_queue(out, subprefix, &state->arrival_queue);
    fprintf(out, "%s | ]\n", prefix);
    fprintf(out, "%s |   pending_recvs_queue[%d] = [\n", prefix, qlist_count(&state->pending_recvs_queue));
    print_mpi_msgs_queue(out, subprefix, &state->pending_recvs_queue);
    fprintf(out, "%s | ]\n", prefix);

    fprintf(out, "%s |        completed_reqs[%d] = [\n", prefix, qlist_count(&state->completed_reqs));
    completed_requests * current = NULL;
    qlist_for_each_entry(current, &state->completed_reqs, ql) {
         fprintf(out, "%s |  | Req: req_id: %u\n", prefix, current->req_id);
    }
    fprintf(out, "%s | ]\n", prefix);

    fprintf(out, "%s |      cur_interval_end = %g\n", prefix, state->cur_interval_end);
    fprintf(out, "%s |              *wait_op = %p\n", prefix, state->wait_op);
    if (state->wait_op != NULL) {
        fprintf(out, "%s |                  |.op_type = %d\n", prefix, state->wait_op->op_type);
        fprintf(out, "%s |                  |.req_ids = [", prefix);
        for(int i = 0; i < state->wait_op->count; i++) {
            fprintf(out, "%d%s", state->wait_op->req_ids[i], i+1==state->wait_op->count ? "" : ", ");
        }
        fprintf(out, "]\n");
        fprintf(out, "%s |                  |.num_completed = %d\n", prefix, state->wait_op->num_completed);
        fprintf(out, "%s |                  |.count   = %d\n", prefix, state->wait_op->count);
        fprintf(out, "%s |                  |.start_time = %g\n", prefix, state->wait_op->start_time);
    }
    fprintf(out, "%s |           msg_sz_list[%d] = [\n", prefix, qlist_count(&state->completed_reqs));
    struct msg_size_info * ms_info = NULL;
    qlist_for_each_entry(ms_info, &state->msg_sz_list, ql) {
         fprintf(out, "%s |  | MsSizeInfo: msg_size: %lu num_msgs: %d agg_latency: %g avg_latency: %g hash_link.next: %p  hash_link.prev: %p\n", prefix, ms_info->msg_size, ms_info->num_msgs, ms_info->agg_latency, ms_info->avg_latency, ms_info->hash_link.next, ms_info->hash_link.prev);
    }
    fprintf(out, "%s | ]\n", prefix);

    // Data statistics
    fprintf(out, "%s |        num_bytes_sent = %llu\n", prefix, state->num_bytes_sent);
    fprintf(out, "%s |       num_bytes_recvd = %llu\n", prefix, state->num_bytes_recvd);
    fprintf(out, "%s |              syn_data = %llu\n", prefix, state->syn_data);
    fprintf(out, "%s |              gen_data = %llu\n", prefix, state->gen_data);

    fprintf(out, "%s |           prev_switch = %lu\n", prefix, state->prev_switch);
    fprintf(out, "%s |       saved_perm_dest = %d\n", prefix, state->saved_perm_dest);
    fprintf(out, "%s |               rc_perm = %lu\n", prefix, state->rc_perm);

    // Sampling information
    fprintf(out, "%s |         sampling_indx = %d\n", prefix, state->sampling_indx);
    fprintf(out, "%s |          max_arr_size = %d\n", prefix, state->max_arr_size);
    fprintf(out, "%s |*     mpi_wkld_samples = %p\n", prefix, state->mpi_wkld_samples);
    fprintf(out, "%s |            output_buf = %.512s...\n", prefix, state->output_buf);
    fprintf(out, "%s |             col_stats = %.64s...\n", prefix, state->col_stats);

    fprintf(out, "%s |ross_sample.\n", prefix);
    fprintf(out, "%s |    |           nw_id = %lu\n", prefix, state->ross_sample.nw_id);
    fprintf(out, "%s |    |          app_id = %d\n", prefix, state->ross_sample.app_id);
    fprintf(out, "%s |    |      local_rank = %d\n", prefix, state->ross_sample.local_rank);
    fprintf(out, "%s |    |       num_sends = %lu\n", prefix, state->ross_sample.num_sends);
    fprintf(out, "%s |    |       num_recvs = %lu\n", prefix, state->ross_sample.num_recvs);
    fprintf(out, "%s |    |  num_bytes_sent = %llu\n", prefix, state->ross_sample.num_bytes_sent);
    fprintf(out, "%s |    | num_bytes_recvd = %llu\n", prefix, state->ross_sample.num_bytes_recvd);
    fprintf(out, "%s |    |       send_time = %g\n", prefix, state->ross_sample.send_time);
    fprintf(out, "%s |    |       recv_time = %g\n", prefix, state->ross_sample.recv_time);
    fprintf(out, "%s |    |       wait_time = %g\n", prefix, state->ross_sample.wait_time);
    fprintf(out, "%s |    |    compute_time = %g\n", prefix, state->ross_sample.compute_time);
    fprintf(out, "%s |    |       comm_time = %g\n", prefix, state->ross_sample.comm_time);
    fprintf(out, "%s |    |        max_time = %g\n", prefix, state->ross_sample.max_time);
    fprintf(out, "%s |    |    avg_msg_time = %g\n", prefix, state->ross_sample.avg_msg_time);

    // Configuration
    fprintf(out, "%s |*        switch_config = %p\n", prefix, state->switch_config);
    fprintf(out, "%s |    switch_config_size = %zu\n", prefix, state->switch_config_size);
}

static char const * const MPI_NW_EVENTS_to_string(enum MPI_NW_EVENTS event_type) {

    switch (event_type) {
        case MPI_OP_GET_NEXT:      return "MPI_OP_GET_NEXT";
        case MPI_SEND_ARRIVED:     return "MPI_SEND_ARRIVED";
        case MPI_SEND_ARRIVED_CB:  return "MPI_SEND_ARRIVED_CB";
        case MPI_SEND_POSTED:      return "MPI_SEND_POSTED";
        case MPI_REND_ARRIVED:     return "MPI_REND_ARRIVED";
        case MPI_REND_ACK_ARRIVED: return "MPI_REND_ACK_ARRIVED";
        case CLI_BCKGND_FIN:       return "CLI_BCKGND_FIN";
        case CLI_BCKGND_ARRIVE:    return "CLI_BCKGND_ARRIVE";
        case CLI_BCKGND_GEN:       return "CLI_BCKGND_GEN";
        case CLI_BCKGND_CHANGE:    return "CLI_BCKGND_CHANGE";
        case CLI_NBR_FINISH:       return "CLI_NBR_FINISH";
        case CLI_OTHER_FINISH:     return "CLI_OTHER_FINISH";
        case SURR_SKIP_ITERATION:  return "SURR_SKIP_ITERATION";
        default: return "UNKNOWN!!";
    }

}

// Original printing function from Claude.ai
static void print_nw_message(FILE * out, char const * prefix, nw_state* s, struct nw_message * msg) {
    fprintf(out, "%snw_message ->\n", prefix);
    fprintf(out, "%s | msg_type = %s\n", prefix, MPI_NW_EVENTS_to_string(msg->msg_type));
    fprintf(out, "%s |  op_type = %s\n", prefix, op_type_string(msg->op_type));
    fprintf(out, "%s | num_rngs = %d\n", prefix, msg->num_rngs);
    fprintf(out, "%s | event_rc = %d\n", prefix, msg->event_rc);
    fprintf(out, "%s |   mpi_op = %p\n", prefix, msg->mpi_op);

    char addprefix[] = " |   | ";
    int len_subprefix = snprintf(NULL, 0, "%s%s", prefix, addprefix) + 1;
    char subprefix[len_subprefix];
    snprintf(subprefix, len_subprefix, "%s%s", prefix, addprefix);
    fprint_codes_workload_op(out, subprefix, msg->mpi_op);

    fprintf(out, "%s | fwd\n", prefix);
    fprintf(out, "%s |   |       src_rank = %lu\n", prefix, msg->fwd.src_rank);
    fprintf(out, "%s |   |      dest_rank = %d\n", prefix, msg->fwd.dest_rank);
    fprintf(out, "%s |   |      num_bytes = %ld\n", prefix, msg->fwd.num_bytes);
    fprintf(out, "%s |   |    num_matched = %d\n", prefix, msg->fwd.num_matched);
    fprintf(out, "%s |   | sim_start_time = %g\n", prefix, msg->fwd.sim_start_time);
    fprintf(out, "%s |   |  msg_send_time = %g\n", prefix, msg->fwd.msg_send_time);
    fprintf(out, "%s |   |         req_id = %u\n", prefix, msg->fwd.req_id);
    fprintf(out, "%s |   |    matched_req = %d\n", prefix, msg->fwd.matched_req);
    fprintf(out, "%s |   |            tag = %d\n", prefix, msg->fwd.tag);
    fprintf(out, "%s |   |         app_id = %d\n", prefix, msg->fwd.app_id);
    fprintf(out, "%s |   |    found_match = %d\n", prefix, msg->fwd.found_match);
    fprintf(out, "%s |   | wait_completed = %d\n", prefix, msg->fwd.wait_completed);
    fprintf(out, "%s |   |      rend_send = %d\n", prefix, msg->fwd.rend_send);

    fprintf(out, "%s | rc\n", prefix);
    switch(msg->msg_type) {
        case CLI_BCKGND_GEN:
            fprintf(out, "%s |   | gen\n", prefix);
            fprintf(out, "%s |       |  saved_syn_length = %d\n", prefix, msg->rc.gen.saved_syn_length);
            fprintf(out, "%s |       |        saved_perm = %d\n", prefix, msg->rc.gen.saved_perm);
            fprintf(out, "%s |       | saved_prev_switch = %lu\n", prefix, msg->rc.gen.saved_prev_switch);
            break;

        case CLI_BCKGND_ARRIVE:
        case MPI_SEND_ARRIVED_CB:
            fprintf(out, "%s |   |arrive.saved_prev_max_time = %g\n", prefix, msg->rc.arrive.saved_prev_max_time);
            fprintf(out, "%s |   |    arrive.saved_send_time = %g\n", prefix, msg->rc.arrive.saved_send_time);
            fprintf(out, "%s |   |arrive.saved_send_time_sample = %g\n", prefix, msg->rc.arrive.saved_send_time_sample);
            break;

        case CLI_BCKGND_CHANGE:
            fprintf(out, "%s |   |   change.saved_send_time = %g\n", prefix, msg->rc.change.saved_send_time);
            fprintf(out, "%s |   | change.saved_marker_time = %g\n", prefix, msg->rc.change.saved_marker_time);
            break;

        case MPI_OP_GET_NEXT:
            fprintf(out, "%s |     mpi_next\n", prefix);
            fprintf(out, "%s |            | saved_elapsed_time = %g\n", prefix, msg->rc.mpi_next.saved_elapsed_time);
            fprintf(out, "%s |            | all_reduce.saved_send_time = %g\n", prefix, msg->rc.mpi_next.all_reduce.saved_send_time);
            fprintf(out, "%s |            | all_reduce.saved_delay = %g\n", prefix, msg->rc.mpi_next.all_reduce.saved_delay);

            fprintf(out, "%s |            | recv.saved_recv_time = %g\n", prefix, msg->rc.mpi_next.recv.saved_recv_time);
            fprintf(out, "%s |            | recv.saved_recv_time_sample = %g\n", prefix, msg->rc.mpi_next.recv.saved_recv_time_sample);

            fprintf(out, "%s |            | delay.saved_delay = %g\n", prefix, msg->rc.mpi_next.delay.saved_delay);
            fprintf(out, "%s |            | delay.saved_delay_sample = %g\n", prefix, msg->rc.mpi_next.delay.saved_delay_sample);

            fprintf(out, "%s |            | mark.saved_marker_time = %g\n", prefix, msg->rc.mpi_next.mark.saved_marker_time);
            break;

        case MPI_SEND_ARRIVED:
        case MPI_REND_ARRIVED:
        case MPI_SEND_POSTED:
            fprintf(out, "%s |   | mpi_send\n", prefix);
            fprintf(out, "%s |            |        saved_wait_time = %g\n", prefix, msg->rc.mpi_send.saved_wait_time);
            fprintf(out, "%s |            | saved_wait_time_sample = %g\n", prefix, msg->rc.mpi_send.saved_wait_time_sample);
            fprintf(out, "%s |            |        saved_recv_time = %g\n", prefix, msg->rc.mpi_send.saved_recv_time);
            fprintf(out, "%s |            | saved_recv_time_sample = %g\n", prefix, msg->rc.mpi_send.saved_recv_time_sample);
            fprintf(out, "%s |            |        saved_num_bytes = %lu\n", prefix, msg->rc.mpi_send.saved_num_bytes);
            break;

        case MPI_REND_ACK_ARRIVED:
            fprintf(out, "%s |   |  mpi_ack.saved_num_bytes = %ld\n", prefix, msg->rc.mpi_ack.saved_num_bytes);
            break;

        case SURR_SKIP_ITERATION:
            fprintf(out, "%s |   |        surr.config_used = %p\n", prefix, msg->rc.surr.config_used);
            break;

        default:
            break;
    }
}

const tw_optdef app_opt [] =
{
	TWOPT_GROUP("Network workload test"),
    TWOPT_CHAR("workload_type", workload_type, "dumpi"),
    TWOPT_CHAR("workload_name", workload_name, "lammps or nekbone (for online workload generation)"),
	TWOPT_CHAR("workload_file", workload_file, "workload file name"),
	TWOPT_CHAR("alloc_file", alloc_file, "allocation file name"),
	TWOPT_CHAR("workload_conf_file", workloads_conf_file, "workload config file name"),
    TWOPT_CHAR("link_failure_file", g_nm_link_failure_filepath, "filepath for override of link failure file from configuration for supporting models"),
	TWOPT_CHAR("workload_timer_file", workloads_timer_file, "workload timer file name (for starting/pausing/stopping synthetic traffic)"),
	TWOPT_CHAR("workload_period_file", workloads_period_file, "workload periods file name (for changing the per-job synthetic traffic load at specified periods/times)"),
	TWOPT_UINT("num_net_traces", num_net_traces, "number of network traces"),
	TWOPT_UINT("priority_type", prioritize_collectives, "Priority type (zero): Normal, (one): high priority to collective operations "),
	TWOPT_UINT("payload_sz", payload_sz, "size of payload for synthetic traffic "),
	TWOPT_ULONGLONG("max_gen_data", max_gen_data, "maximum data to be generated for synthetic traffic (Default 0 (OFF))"),
	TWOPT_UINT("eager_threshold", EAGER_THRESHOLD, "the transition point for eager/rendezvous protocols (Default 8192)"),
    TWOPT_UINT("disable_compute", disable_delay, "disable compute simulation"),
    TWOPT_UINT("payload_sz", payload_sz, "size of the payload for synthetic traffic"),
    TWOPT_UINT("syn_type", syn_type, "type of synthetic traffic"),
    TWOPT_UINT("preserve_wait_ordering", preserve_wait_ordering, "only enable when getting unmatched send/recv errors in optimistic mode (turning on slows down simulation)"),
    TWOPT_UINT("debug_cols", debug_cols, "completion time of collective operations (currently MPI_AllReduce)"),
    TWOPT_UINT("enable_mpi_debug", enable_debug, "enable debugging of MPI sim layer (works with sync=1 only)"),
    TWOPT_UINT("sampling_interval", sampling_interval, "sampling interval for MPI operations"),
    TWOPT_UINT("perm-thresh", perm_switch_thresh, "threshold for random permutation operations"),
	TWOPT_UINT("enable_sampling", enable_sampling, "enable sampling (only works in sequential mode)"),
    TWOPT_STIME("mean_interval", mean_interval, "mean interval for generating background traffic"),
    TWOPT_STIME("sampling_end_time", sampling_end_time, "sampling_end_time"),
    TWOPT_CHAR("lp-io-dir", lp_io_dir, "Where to place io output (unspecified -> no output"),
    TWOPT_UINT("lp-io-use-suffix", lp_io_use_suffix, "Whether to append uniq suffix to lp-io directory (default 0)"),
	TWOPT_CHAR("offset_file", offset_file, "offset file name"),
#ifdef ENABLE_CORTEX_PYTHON
	TWOPT_CHAR("cortex-file", cortex_file, "Python file (without .py) containing the CoRtEx translation class"),
	TWOPT_CHAR("cortex-class", cortex_class, "Python class implementing the CoRtEx translator"),
	TWOPT_CHAR("cortex-gen", cortex_gen, "Python function to pre-generate MPI events"),
#endif
	TWOPT_CHAR("skipping-iterations-file", skipping_iterations_file, "Configuration file name for which steps to skip"),
	TWOPT_END()
};

tw_lptype nw_lp = {
    (init_f) nw_test_init,
    (pre_run_f) NULL,
    (event_f) nw_test_event_handler,
    (revent_f) nw_test_event_handler_rc,
    (commit_f) nw_test_event_handler_commit,
    (final_f) nw_test_finalize,
    (map_f) codes_mapping,
    sizeof(nw_state)
};

const tw_lptype* nw_get_lp_type()
{
            return(&nw_lp);
}

// ROSS function pointer table to check reverse event handler
crv_checkpointer nw_lp_chkptr = {
    &nw_lp,
    0,
    (save_checkpoint_state_f) save_nw_lp_state,
    (clean_checkpoint_state_f) clean_nw_lp_state,
    (check_states_f) check_nw_lp_state,
    (print_lpstate_f) print_nw_lp_state,
    (print_checkpoint_state_f) print_nw_lp_state,
    (print_event_f) print_nw_message,
};

static void nw_add_lp_type()
{
  lp_type_register("nw-lp", nw_get_lp_type());
  // registering custom print for nw_lp LPs
  crv_add_custom_state_checkpoint(&nw_lp_chkptr);
}

/* setup for the ROSS event tracing
 */
void nw_lp_event_collect(nw_message *m, tw_lp *lp, char *buffer, int *collect_flag)
{
    (void)lp;
    (void)collect_flag;

    int type = m->msg_type;
    memcpy(buffer, &type, sizeof(type));
}

/* can add in any model level data to be collected along with simulation engine data
 * in the ROSS instrumentation.  Will need to update the last field in 
 * nw_lp_model_types[0] for the size of the data to save in each function call
 */
void nw_lp_model_stat_collect(nw_state *s, tw_lp *lp, char *buffer)
{
    (void)s;
    (void)lp;
    (void)buffer;

    return;
}

void ross_nw_lp_sample_fn(nw_state * s, tw_bf * bf, tw_lp * lp, struct ross_model_sample *sample)
{
    (void)bf;
    (void)lp;
    memcpy(sample, &s->ross_sample, sizeof(s->ross_sample));
    sample->nw_id = s->nw_id;
    sample->app_id = s->app_id;
    sample->local_rank = s->local_rank;
    sample->comm_time = s->elapsed_time - s->compute_time;
    if (alloc_spec == 1)
    {
        struct codes_jobmap_id lid;
        lid = codes_jobmap_to_local_id(s->nw_id, jobmap_ctx);

        if(strncmp(file_name_of_job[lid.job], "synthetic", 9) == 0)
            sample->avg_msg_time = (s->send_time / s->num_recvs);
    }
    memset(&s->ross_sample, 0, sizeof(s->ross_sample));
}

void ross_nw_lp_sample_rc_fn(nw_state * s, tw_bf * bf, tw_lp * lp, struct ross_model_sample *sample)
{
    (void)bf;
    (void)lp;
    memcpy(&s->ross_sample, sample, sizeof(*sample));
}

st_model_types nw_lp_model_types[] = {
    {(ev_trace_f) nw_lp_event_collect,
     sizeof(int),
     (model_stat_f) nw_lp_model_stat_collect,
     0,
     (sample_event_f) ross_nw_lp_sample_fn,
     (sample_revent_f) ross_nw_lp_sample_rc_fn,
     sizeof(struct ross_model_sample)},
    {NULL, 0, NULL, 0, NULL, NULL, 0}
};

static const st_model_types  *nw_lp_get_model_stat_types(void)
{
    return(&nw_lp_model_types[0]);
}

void nw_lp_register_model()
{
    st_model_type_register("nw-lp", nw_lp_get_model_stat_types());
}
/* end of ROSS event tracing setup */

static int msg_size_hash_compare(
            void *key, struct qhash_head *link)
{
   int64_t *in_size = (int64_t *)key;
   struct msg_size_info *tmp;

    tmp = qhash_entry(link, struct msg_size_info, hash_link);
    if (tmp->msg_size == *in_size)
      return 1;

    return 0;
}

/* Method to organize all mpi_replay specific configuration parameters
to be specified in the loaded .conf file*/
void modelnet_mpi_replay_read_config()
{
    // Load the factor by which the compute time is sped up by. e.g. If compute_time_speedup = 2, all compute time delay is halved.
    int rc = configuration_get_value_double(&config, "PARAMS", "compute_time_speedup", NULL, &compute_time_speedup);
    if (rc) {
        compute_time_speedup = 1;
    }
}


int modelnet_mpi_replay(MPI_Comm comm, int* argc, char*** argv )
{
  int rank;
  int num_nets;
  int* net_ids;

  MPI_COMM_CODES = comm;
 
  tw_comm_set(MPI_COMM_CODES);

  g_tw_ts_end = s_to_ns(60*60); /* one hour, in nsecs */

  workload_type[0]='\0';
  tw_opt_add(app_opt);
  tw_opt_add(cc_app_opt);
  tw_init(argc, argv);

#ifdef USE_RDAMARIS
    if(g_st_ross_rank)
    { // keep damaris ranks from running code between here up until tw_end()
#endif
  codes_comm_update();
  //Xin: add conceptual online workload
  if(strcmp(workload_type, "dumpi") != 0 && strcmp(workload_type, "swm-online") != 0 && strcmp(workload_type, "conc-online") != 0)
    {
	if(tw_ismaster())
		printf("Usage: mpirun -np n ./modelnet-mpi-replay --sync=1/3"
                " --workload_type=dumpi/swm-online/conc-online"
		" --workload_conf_file=prefix-workload-file-name"
		" --workload_timer_file=timer-file"
		" --workload_period_file=period-file"
                " --alloc_file=alloc-file-name"
#ifdef ENABLE_CORTEX_PYTHON
		" --cortex-file=cortex-file-name"
		" --cortex-class=cortex-class-name"
		" --cortex-gen=cortex-function-name"
#endif
		" -- config-file-name\n"
                "See model-net/doc/README.dragonfly.txt and model-net/doc/README.torus.txt"
                " for instructions on how to run the models with network traces ");
	tw_end();
	return -1;
    }

    /* Xin: Currently rendezvous protocol cannot work with Conceptual online workloads */
    if(strcmp(workload_type, "conc-online") == 0) {
        EAGER_THRESHOLD = INT64_MAX;
    }

	jobmap_ctx = NULL; // make sure it's NULL if it's not used



    if(enable_msg_tracking) {
        sprintf(sampling_dir, "sampling-dir");
        mkdir(sampling_dir, S_IRUSR | S_IWUSR | S_IXUSR);

        sprintf(mpi_msg_dir, "synthetic%d", syn_type);
        mkdir(mpi_msg_dir, S_IRUSR | S_IWUSR | S_IXUSR);
    }
    if(strlen(workloads_conf_file) > 0)
    {
        FILE *name_file = fopen(workloads_conf_file, "r");
        if(!name_file)
            tw_error(TW_LOC, "\n Could not open file %s ", workloads_conf_file);

        int i = 0;
        char ref = '\n';
        while(!feof(name_file))
        {
            //TODO: can we allow for a 2 item line but with defaults for the last two?
            ref = fscanf(name_file, "%d %s %d %f", &num_traces_of_job[i], file_name_of_job[i], &qos_level_of_job[i], &mean_interval_of_job[i]);
            
            if(ref != EOF && strncmp(file_name_of_job[i], "synthetic", 9) == 0)
            {
              num_syn_clients = num_traces_of_job[i];
              num_net_traces += num_traces_of_job[i];
              is_job_synthetic[i] = 1;
              is_synthetic = 1;
              num_total_jobs += 1;
	      // Make sure BISECTION job has even number of clients
	      if (strcmp(file_name_of_job[i], "synthetic6") == 0 && num_traces_of_job[i] % 2 != 0)
		tw_error(TW_LOC, "BISECTION requires and even number of nodes.");

            }
            else if(ref!=EOF)
            {
                if(enable_debug)
                    printf("\n%d traces of app %s (default qos class: %d)\n", num_traces_of_job[i], file_name_of_job[i], qos_level_of_job[i]);

                num_net_traces += num_traces_of_job[i];
                num_dumpi_traces += num_traces_of_job[i];
                is_job_synthetic[i] = 0;
                num_nonsyn_jobs += 1;
                num_total_jobs += 1;
            }
                i++;
        }
        printf("\n num_net_traces %d; num_dumpi_traces %d", num_net_traces, num_dumpi_traces);
        fclose(name_file);
        assert(strlen(alloc_file) != 0);
        alloc_spec = 1;
        jobmap_p.alloc_file = alloc_file;
        jobmap_ctx = codes_jobmap_configure(CODES_JOBMAP_LIST, &jobmap_p);

        if(strlen(workloads_timer_file) > 0){
            FILE *timer_file = fopen(workloads_timer_file, "r");
            if(!timer_file)
                tw_error(TW_LOC, "\n Could not open file %s ", workloads_timer_file);

            int j = 0;
            char ref2 = '\n';
            while(!feof(timer_file))
            {
                ref2 = fscanf(timer_file, "%ld %ld", &job_timer1[j], &job_timer2[j]);
                j++;
            }
            fclose(timer_file);
        }

        if(strlen(workloads_period_file) > 0){
            FILE *period_file = fopen(workloads_period_file, "r");
            if(!period_file)
                tw_error(TW_LOC, "\n Could not open file %s ", workloads_period_file);

            int j = 0;
            char ref2 = '\n';
            while(!feof(period_file))
            {
                if (j >= MAX_JOBS && !g_tw_mynode) {
                    tw_error(TW_LOC, "Exceeded number of max workloads in workloads period file. Max: %d", MAX_JOBS);
                }
                ref2 = fscanf(period_file, "%d", &period_count[j]);
                if (period_count[j] > MAX_PERIODS_PER_APP && !g_tw_mynode) {
                    tw_error(TW_LOC, "Too many periods for workload app %d", period_count[j]);
                }
                if(ref2 != EOF){
                    if (!g_tw_mynode) {
                        printf("======== [ID: %d] Period count: %d\n", j, period_count[j]);
                    }
                    for(int k = 0; k < period_count[j]; k++){
                        fscanf(period_file, "%lf:%f", &period_time[j][k], &period_interval[j][k]);
                        if (!g_tw_mynode) {
                            printf("======== [ID: %d] Period time and interval: %lf and %f\n", j, period_time[j][k], period_interval[j][k]);
                        }
                    }
                }
                j++;
            }
            fclose(period_file);
        }
    }
    else
    {
        assert(num_net_traces);
        num_traces_of_job[0] = num_net_traces;
        if(strcmp(workload_type, "dumpi") == 0)
        {
            assert(strlen(workload_file) > 0);
            strcpy(file_name_of_job[0], workload_file);
        }
        alloc_spec = 0;
		if(strlen(alloc_file) > 0) {
			alloc_spec = 1;
			jobmap_p.alloc_file = alloc_file;
			jobmap_ctx = codes_jobmap_configure(CODES_JOBMAP_LIST, &jobmap_p);
		}
    }

    if (jobmap_ctx == NULL) //then we need to set up an identity jobmap for the ranks that do exist
    {
        jobmap_ident_p.num_ranks = num_net_traces;
        jobmap_ctx = codes_jobmap_configure(CODES_JOBMAP_IDENTITY, &jobmap_ident_p);
    }


    // Loading skipping iterations configuration
    if(strlen(skipping_iterations_file) > 0) {
        FILE *file = fopen(skipping_iterations_file, "r");
        if(!file) {
            tw_error(TW_LOC, "\n Could not open file %s ", workloads_conf_file);
        }

        // Finding number of skipping iteration rows
        int i = 0;
        for(; !feof(file); i++) {
            struct AvgSurrogateSwitchingTimesForApp skip_row;

            int ref = fscanf(file, "%d %d %d %lf", &skip_row.app_id, &skip_row.skip_at_iter, &skip_row.resume_at_iter, &skip_row.time_per_iter);

            if (ref != 4) { // We couldn't read all four values
                fprintf(stderr, "Warning: Couldn't read a row of 'skipping-iterations-file'. Stopping after reading %d rows.\n", i);
                break;
            }
        }

        skip_iter_config_size = i;

        skip_iter_config = malloc(skip_iter_config_size * sizeof(struct AvgSurrogateSwitchingTimesForApp));

        // Loading in memory all times to skip iterations
        fseek(file, 0, SEEK_SET);
        for(i = 0; i < skip_iter_config_size; i++) {
            struct AvgSurrogateSwitchingTimesForApp *skip_row = &skip_iter_config[i];

            fscanf(file, "%d %d %d %lf", &skip_row->app_id, &skip_row->skip_at_iter, &skip_row->resume_at_iter, &skip_row->time_per_iter);
            skip_row->done = false;
        }
        fclose(file);

        // Sorting. To skip iterations we asume that all skips for a specific job appear in increasing order
        qsort(
            skip_iter_config,
            skip_iter_config_size,
            sizeof(struct AvgSurrogateSwitchingTimesForApp),
            (int (*)(const void *, const void *)) comp_AvgSurrogateSwitchingTimesForApp);

        // Printing configuration
        if(!g_tw_mynode && skip_iter_config_size) {
            printf("\n\nConfiguration for skipping selected iterations of one or more jobs has been loaded.\n");
            printf("| job_id skip_at_iter resume_at_iter time_per_iter\n");
            for (size_t i=0; i<skip_iter_config_size; i++) {
                struct AvgSurrogateSwitchingTimesForApp *skip_row = &skip_iter_config[i];
                printf("| %d %d %d %lf\n", skip_row->app_id, skip_row->skip_at_iter, skip_row->resume_at_iter, skip_row->time_per_iter);
            }
            printf("\n");
        }
    }

    MPI_Comm_rank(MPI_COMM_CODES, &rank);
    MPI_Comm_size(MPI_COMM_CODES, &nprocs);

   configuration_load((*argv)[2], MPI_COMM_CODES, &config);

   nw_add_lp_type();
   model_net_register();

    if (g_st_ev_trace || g_st_model_stats || g_st_use_analysis_lps)
        nw_lp_register_model();

   net_ids = model_net_configure(&num_nets);
//   assert(num_nets == 1);
   net_id = *net_ids;
   free(net_ids);

   modelnet_mpi_replay_read_config();

   //Xin: output iteration time into log file

   char const iteration_dir[] = "iteration-logs";
   if (!g_tw_mynode) {
        int ret = mkdir("iteration-logs", 0775);
        if(ret != 0)
        {
            tw_error(TW_LOC, "mkdir(\"%s/\")", iteration_dir);
        }
   }
   MPI_Barrier(MPI_COMM_CODES);
   int buffer_size = snprintf(NULL, 0, "%s/pe=%d.txt", iteration_dir, g_tw_mynode) + 1;
   char *iteration_log_path = malloc(buffer_size);
   snprintf(iteration_log_path, buffer_size, "%s/pe=%d.txt", iteration_dir, g_tw_mynode);
   iteration_log = fopen(iteration_log_path, "w+");
   free(iteration_log_path);
   if(!iteration_log)
   {
       printf("\n Error logging iteration times... quitting ");
       MPI_Finalize();
       return -1;
   }

   if(enable_debug)
   {
       workload_log = fopen("mpi-op-logs", "w+");

       if(!workload_log)
       {
           printf("\n Error logging MPI operations... quitting ");
           MPI_Finalize();
           return -1;
       }
   }
   if(enable_msg_tracking)
   {
        char log_name[512];
        sprintf(log_name, "%s/mpi-msg-sz-logs-%s-syn-sz-%d-mean-%f-%d",
            mpi_msg_dir,
            file_name_of_job[0],
            payload_sz,
            mean_interval,
            rand());

        msg_size_log = fopen(log_name, "w+");

        if(!msg_size_log)
        {
            printf("\n Error logging MPI operations... quitting ");
            MPI_Finalize();
            return -1;
        }
        char agg_log_name[512];
        sprintf(agg_log_name, "%s/mpi-aggregate-logs-%d.bin", sampling_dir, rank);
        workload_agg_log = fopen(agg_log_name, "w+");
        workload_meta_log = fopen("mpi-workload-meta-log", "w+");
   

        if(!workload_agg_log || !workload_meta_log)
        {
            printf("\n Error logging MPI operations... quitting ");
            MPI_Finalize();
            return -1;
        }
    }

    switch(map_ctxt)
    {
        case GROUP_RATIO:
           mapping_context = codes_mctx_set_group_ratio(NULL, true);
           break;
        case GROUP_RATIO_REVERSE:
           mapping_context = codes_mctx_set_group_ratio_reverse(NULL, true);
           break;
        case GROUP_DIRECT:
           mapping_context = codes_mctx_set_group_direct(1,NULL, true);
           break;
        case GROUP_MODULO:
           mapping_context = codes_mctx_set_group_modulo(NULL, true);
           break;
        case GROUP_MODULO_REVERSE:
           mapping_context = codes_mctx_set_group_modulo_reverse(NULL, true);
           break;
    }

   if(enable_sampling)
       model_net_enable_sampling(sampling_interval, sampling_end_time);

   codes_mapping_setup();
   congestion_control_set_jobmap(jobmap_ctx, net_id); //must be placed after codes_mapping_setup - where g_congestion_control_enabled is set

   num_mpi_lps = codes_mapping_get_lp_count("MODELNET_GRP", 0, "nw-lp", NULL, 0);
   
   num_nw_lps = codes_mapping_get_lp_count("MODELNET_GRP", 1, 
			"nw-lp", NULL, 1);	
  
   if (lp_io_dir[0]){
        do_lp_io = 1;
        /* initialize lp io */
        int flags = lp_io_use_suffix ? LP_IO_UNIQ_SUFFIX : 0;
        int ret = lp_io_prepare(lp_io_dir, flags, &io_handle, MPI_COMM_CODES);
        assert(ret == 0 || !"lp_io_prepare failure");
    }

   // TODO: read from config whether to load iterator predictor
   application_surrogate_configure(24, 2, &iter_predictor);

   tw_run();

    fclose(iteration_log); //Xin
    
    if(enable_debug)
        fclose(workload_log);

    if(enable_msg_tracking) {
        fclose(msg_size_log);
        fclose(workload_agg_log);
        fclose(workload_meta_log);
    }

    long long total_bytes_sent, total_bytes_recvd;
    double max_run_time, avg_run_time;
   double max_comm_run_time, avg_comm_run_time;
    double total_avg_send_time, total_max_send_time;
     double total_avg_wait_time, total_max_wait_time;
     double total_avg_recv_time, total_max_recv_time;
     double g_total_syn_data = 0;

    MPI_Reduce(&num_bytes_sent, &total_bytes_sent, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_CODES);
    MPI_Reduce(&num_bytes_recvd, &total_bytes_recvd, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_CODES);
   MPI_Reduce(&max_comm_time, &max_comm_run_time, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_CODES);
   MPI_Reduce(&max_time, &max_run_time, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_CODES);
   MPI_Reduce(&avg_time, &avg_run_time, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_CODES);

   MPI_Reduce(&avg_recv_time, &total_avg_recv_time, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_CODES);
   MPI_Reduce(&avg_comm_time, &avg_comm_run_time, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_CODES);
   MPI_Reduce(&max_wait_time, &total_max_wait_time, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_CODES);
   MPI_Reduce(&max_send_time, &total_max_send_time, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_CODES);
   MPI_Reduce(&max_recv_time, &total_max_recv_time, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_CODES);
   MPI_Reduce(&avg_wait_time, &total_avg_wait_time, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_CODES);
   MPI_Reduce(&avg_send_time, &total_avg_send_time, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_CODES);
   MPI_Reduce(&total_syn_data, &g_total_syn_data, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_CODES);  

   assert(num_net_traces);

   if(!g_tw_mynode)
   {
	printf("\n Total bytes sent %lld recvd %lld \n max runtime %lf ns avg runtime %lf \n max comm time %lf avg comm time %lf \n max send time %lf avg send time %lf \n max recv time %lf avg recv time %lf \n max wait time %lf avg wait time %lf \n", 
            total_bytes_sent, 
            total_bytes_recvd,
			max_run_time, avg_run_time/num_net_traces,
			max_comm_run_time, avg_comm_run_time/num_net_traces,
			total_max_send_time, total_avg_send_time/num_net_traces,
			total_max_recv_time, total_avg_recv_time/num_net_traces,
			total_max_wait_time, total_avg_wait_time/num_net_traces);
    
    printf("\n----------\n");
    printf("Per App Max Elapsed Times:\n");
    for(int i = 0; i < num_total_jobs; i++)
    {
        printf("\tApp %d: %.4f\n",i,max_elapsed_time_per_job[i]);
    }
    printf("----------\n");

    if(synthetic_pattern == PERMUTATION)
        printf("\n Threshold for random permutation %ld ", perm_switch_thresh);
   }
    if (do_lp_io){
        int ret = lp_io_flush(io_handle, MPI_COMM_CODES);
        assert(ret == 0 || !"lp_io_flush failure");
    }
    if(is_synthetic)
        printf("\n PE%d: Synthetic traffic stats: data received per proc %lf bytes \n",rank, g_total_syn_data/num_syn_clients);

   model_net_report_stats(net_id);
   
   if(unmatched && g_tw_mynode == 0) 
       fprintf(stderr, "\n Warning: unmatched send and receive operations found.\n");
        //tw_error(TW_LOC, "\n Unmatched send and receive, terminating simulation");
   
   if(alloc_spec)
       codes_jobmap_destroy(jobmap_ctx);

   if (skip_iter_config != NULL) {
       free(skip_iter_config);
   }

   print_surrogate_stats();
   free_application_surrogate();

#ifdef USE_RDAMARIS
    } // end if(g_st_ross_rank)
#endif
   tw_end();

  return 0;
}
