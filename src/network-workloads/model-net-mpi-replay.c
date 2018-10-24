/*
* Copyright (C) 2014 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */
#include <ross.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include "codes/codes-workload.h"
#include "codes/codes.h"
#include "codes/configuration.h"
#include "codes/codes_mapping.h"
#include "codes/model-net.h"
#include "codes/rc-stack.h"
#include "codes/quicklist.h"
#include "codes/quickhash.h"
#include "codes/codes-jobmap.h"

/* turning on track lp will generate a lot of output messages */
#define MN_LP_NM "modelnet_dragonfly_custom"
#define CONTROL_MSG_SZ 64
#define TRACE -1
#define MAX_WAIT_REQS 1024
#define CS_LP_DBG 1
#define RANK_HASH_TABLE_SZ 2000
#define NW_LP_NM "nw-lp"
#define lprintf(_fmt, ...) \
        do {if (CS_LP_DBG) printf(_fmt, __VA_ARGS__);} while (0)
#define MAX_STATS 65536
#define COL_TAG 1235
#define BAR_TAG 1234

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
static unsigned long long max_gen_data = 1310720;
tw_lpid TRACK_LP = -1;
int nprocs = 0;
static double total_syn_data = 0;
static int unmatched = 0;
char workload_type[128];
char workload_name[128];
char workload_file[8192];
char offset_file[8192];
static int wrkld_id;
static int num_net_traces = 0;
static int priority_type = 0;
static int num_dumpi_traces = 0;
static int64_t EAGER_THRESHOLD = 8192;

static long num_ops = 0;
static int upper_threshold = 1048576;
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
char alloc_file[8192];
int num_traces_of_job[5];
tw_stime soft_delay_mpi = 2500;
tw_stime nic_delay = 1000;
tw_stime copy_per_byte_eager = 0.55;
char file_name_of_job[5][8192];

struct codes_jobmap_ctx *jobmap_ctx;
struct codes_jobmap_params_list jobmap_p;

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
static float noise = 1.0;
static int num_nw_lps = 0, num_mpi_lps = 0;

static int num_syn_clients;
static int syn_type = 0;

FILE * workload_log = NULL;
FILE * msg_size_log = NULL;
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

/* set group context */
struct codes_mctx group_ratio;

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
    CLI_NBR_FINISH,
};

/* type of synthetic traffic */
enum TRAFFIC
{
    UNIFORM = 1, /* sends message to a randomly selected node */
    NEAREST_NEIGHBOR = 2, /* sends message to the next node (potentially connected to the same router) */
    ALLTOALL = 3, /* sends message to all other nodes */
    STENCIL = 4, /* sends message to 4 nearby neighbors */
    PERMUTATION = 5
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
    int64_t seq_id;
    tw_stime req_init_time;
	dumpi_req_id req_id;
    struct qlist_head ql;
};

/* stores request IDs of completed MPI operations (Isends or Irecvs) */
struct completed_requests
{
	unsigned int req_id;
    struct qlist_head ql;
    int index;
};

/* for wait operations, store the pending operation and number of completed waits so far. */
struct pending_waits
{
    int op_type;
    unsigned int req_ids[MAX_WAIT_REQS];
	int num_completed;
	int count;
    tw_stime start_time;
    struct qlist_head ql;
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
typedef struct mpi_msgs_queue mpi_msgs_queue;
typedef struct completed_requests completed_requests;
typedef struct pending_waits pending_waits;

/* state of the network LP. It contains the pointers to send/receive lists */
struct nw_state
{
	long num_events_per_lp;
	tw_lpid nw_id;
	short wrkld_end;
    int app_id;
    int local_rank;

    int synthetic_pattern;
    int is_finished;
    int neighbor_completed;

    struct rc_stack * processed_ops;
    struct rc_stack * processed_wait_op;
    struct rc_stack * matched_reqs;
//    struct rc_stack * indices;

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
    
    /* Pending wait operation */
    struct pending_waits * wait_op;

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
};

/* data for handling reverse computation.
* saved_matched_req holds the request ID of matched receives/sends for wait operations.
* ptr_match_op holds the matched MPI operation which are removed from the queues when a send is matched with the receive in forward event handler.
* network event being sent. op is the MPI operation issued by the network workloads API. rv_data holds the data for reverse computation (TODO: Fill this data structure only when the simulation runs in optimistic mode). */
struct nw_message
{
   // forward message handler
   int msg_type;
   int op_type;
   model_net_event_return event_rc;
   struct codes_workload_op * mpi_op;

   struct
   {
       tw_lpid src_rank;
       int dest_rank;
       int64_t num_bytes;
       int num_matched;
       int data_type;
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
   } fwd;
   struct
   {
       int saved_perm;
       double saved_send_time;
       double saved_recv_time;
       double saved_wait_time;
       double saved_delay;
       int64_t saved_num_bytes;
       int saved_syn_length;
       unsigned long saved_prev_switch;
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
        tw_rand_reverse_unif(lp->rng); 
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
            if(other_id == jid.job)
                continue;

            struct codes_jobmap_id other_jid;
            other_jid.job = other_id;

            int num_other_ranks = codes_jobmap_get_num_ranks(other_id, jobmap_ctx);

            lprintf("\n Other ranks %d ", num_other_ranks);
            tw_stime ts = (1.1 * g_tw_lookahead) + tw_rand_exponential(lp->rng, noise);
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
static void notify_neighbor_rc(
	    struct nw_state * ns,
        tw_lp * lp,
        tw_bf * bf,
        struct nw_message * m)
{
       if(bf->c0)
       {
            notify_background_traffic_rc(ns, lp, bf, m);
            return;
       }
   
       if(bf->c1)
       {
          tw_rand_reverse_unif(lp->rng); 
       }
} 
static void notify_neighbor(
	    struct nw_state * ns,
        tw_lp * lp,
        tw_bf * bf,
        struct nw_message * m)
{
    if(ns->local_rank == num_dumpi_traces - 1 
            && ns->is_finished == 1
            && ns->neighbor_completed == 1)
    {
//        printf("\n All workloads completed, notifying background traffic ");
        bf->c0 = 1;
        notify_background_traffic(ns, lp, bf, m);
        return;
    }
    
    struct codes_jobmap_id nbr_jid;
    nbr_jid.job = ns->app_id;
    tw_lpid global_dest_id;

    if(ns->is_finished == 1 && (ns->neighbor_completed == 1 || ns->local_rank == 0))
    {
        bf->c1 = 1;

//        printf("\n Local rank %d notifying neighbor %d ", ns->local_rank, ns->local_rank+1);
        tw_stime ts = (1.1 * g_tw_lookahead) + tw_rand_exponential(lp->rng, noise);
        nbr_jid.rank = ns->local_rank + 1;
        
        /* Send a notification to the neighbor about completion */
        int intm_dest_id = codes_jobmap_to_global_id(nbr_jid, jobmap_ctx); 
        global_dest_id = codes_mapping_get_lpid_from_relative(intm_dest_id, NULL, NW_LP_NM, NULL, 0);
       
        tw_event * e;
        struct nw_message * m_new;  
        e = tw_event_new(global_dest_id, ts, lp);
        m_new = (struct nw_message*)tw_event_data(e); 
        m_new->msg_type = CLI_NBR_FINISH;
        tw_event_send(e);   
    }
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
        lprintf("\n LP %llu completed sending data %lu completed at time %lf ", LLU(lp->gid), ns->gen_data, tw_now(lp));
        
        return;
}

void finish_nbr_wkld_rc(
    struct nw_state * ns,
    tw_bf * b,
    struct nw_message * msg,
    tw_lp * lp)
{
    ns->neighbor_completed = 0;
    
    notify_neighbor_rc(ns, lp, b, msg);
}

void finish_nbr_wkld(
    struct nw_state * ns,
    tw_bf * b,
    struct nw_message * msg,
    tw_lp * lp)
{
    ns->neighbor_completed = 1;

    notify_neighbor(ns, lp, b, msg);
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
        s->prev_switch = m->rc.saved_prev_switch;
        s->saved_perm_dest = m->rc.saved_perm;
        tw_rand_reverse_unif(lp->rng);
    }
    int i;
    for (i=0; i < m->rc.saved_syn_length; i++){
        model_net_event_rc2(lp, &m->event_rc);
        s->gen_data -= payload_sz;
        num_syn_bytes_sent -= payload_sz;
        s->num_bytes_sent -= payload_sz;
    }
        tw_rand_reverse_unif(lp->rng);
        s->num_sends--;

     if(bf->c5)
         s->is_finished = 0;
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
            m->rc.saved_prev_switch = s->prev_switch; //for reverse computation

            length = 1;
            dest_svr = (int*) calloc(1, sizeof(int));
            if(s->gen_data - s->prev_switch >= perm_switch_thresh)
            {
                // printf("%d - %d >= %d\n",s->gen_data,s->prev_switch,perm_switch_thresh);
                bf->c2 = 1;
                m->rc.saved_prev_switch = s->prev_switch;
                s->prev_switch = s->gen_data; //Amount of data pushed at time when switch initiated
                dest_svr[0] = tw_rand_integer(lp->rng, 0, num_clients - 1);
                if(dest_svr[0] == s->local_rank)
                    dest_svr[0] = (s->local_rank + num_clients/2) % num_clients;
                /* TODO: Fix random number generation code */
                m->rc.saved_perm = s->saved_perm_dest;
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
    m->rc.saved_syn_length = length;

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
            m->event_rc = model_net_event(net_id, "medium", global_dest_id, payload_sz, 0.0, 
                    sizeof(nw_message), (const void*)&remote_m, 
                    0, NULL, lp);
            
            s->gen_data += payload_sz;
            s->num_bytes_sent += payload_sz;
            num_syn_bytes_sent += payload_sz; 
        }
    }
    s->num_sends++;

    /* New event after MEAN_INTERVAL */  
    tw_stime ts = mean_interval  + tw_rand_exponential(lp->rng, noise); 
    tw_event * e;
    nw_message * m_new;
    e = tw_event_new(lp->gid, ts, lp);
    m_new = (struct nw_message*)tw_event_data(e);
    m_new->msg_type = CLI_BCKGND_GEN;
    tw_event_send(e);
    
    if(s->gen_data >= max_gen_data)
    {
        bf->c5 = 1;
        s->is_finished = 1;
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
    int data = m->fwd.num_bytes;
    s->syn_data -= data;
    num_syn_bytes_recvd -= data;
    s->num_bytes_recvd -= data;
    s->send_time = m->rc.saved_send_time;
}
void arrive_syn_tr(nw_state * s, tw_bf * bf, nw_message * m, tw_lp * lp)
{
    (void)bf;
    (void)lp;

//    printf("\n Data arrived %d total data %ld ", m->fwd.num_bytes, s->syn_data);
    if(s->local_rank == 0)
     {
    	printf("\n Data arrived %lld rank %llu total data %ld ", m->fwd.num_bytes, s->nw_id, s->syn_data);
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
    m->rc.saved_send_time = s->send_time;
    if((tw_now(lp) - m->fwd.sim_start_time) > s->max_time)
        s->max_time = tw_now(lp) - m->fwd.sim_start_time;

    s->send_time += (tw_now(lp) - m->fwd.sim_start_time);
    s->num_recvs++;
    int data = m->fwd.num_bytes;
    s->syn_data += data;
    s->num_bytes_recvd += data;
    num_syn_bytes_recvd += data;
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

    int i, matched = 0;

    for( i = 0; i < count; i++)
    {
      struct qlist_head * ent = NULL;
      struct completed_requests * current = NULL;
      struct completed_requests * prev = NULL;

      int index = 0;
      qlist_for_each(ent, &s->completed_reqs)
       {
           if(prev)
           {
              rc_stack_push(lp, prev, free, s->matched_reqs);
              prev = NULL;
           }
            
           current = qlist_entry(ent, completed_requests, ql);
           current->index = index; 
            if(current->req_id == reqs[i])
            {
                ++matched;
                qlist_del(&current->ql);
                prev = current;
            }
            ++index;
       }

      if(prev)
      {
         rc_stack_push(lp, prev, free, s->matched_reqs);
         prev = NULL;
      }
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
       qlist_add(&req->ql, &s->completed_reqs);
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
            m->fwd.wait_completed = 1;
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
                        fprintf(workload_log, "\n(%lf) APP ID %d MPI WAITALL COMPLETED AT %llu ", tw_now(lp), s->app_id, LLU(s->nw_id));
                    wait_completed = 1;
                }

                m->fwd.wait_completed = 1;
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
                
//    printf("\n Wait posted rank id %d ", s->nw_id);
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
    fprintf(workload_log, "\n MPI WAITALL POSTED AT %llu ", LLU(s->nw_id));

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
            m->rc.saved_recv_time = ns->recv_time;
            ns->recv_time += (tw_now(lp) - m->fwd.sim_start_time);
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

        m->rc.saved_recv_time = ns->recv_time;
        ns->recv_time += (tw_now(lp) - qitem->req_init_time);

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
	    tw_rand_reverse_unif(lp->rng);
}

/* Trigger getting next event at LP */
static void codes_issue_next_event(tw_lp* lp)
{
   tw_event *e;
   nw_message* msg;

   tw_stime ts;

   ts = g_tw_lookahead + 0.1 + tw_rand_exponential(lp->rng, noise);
   assert(ts > 0);
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

    m->rc.saved_delay = s->compute_time;
    s->compute_time += mpi_op->u.delay.nsecs;
    ts = mpi_op->u.delay.nsecs;
    if(ts <= g_tw_lookahead)
    {
        bf->c28 = 1;
        ts = g_tw_lookahead + 0.1 + tw_rand_exponential(lp->rng, noise);
    }

	//ts += g_tw_lookahead + 0.1 + tw_rand_exponential(lp->rng, noise);
    assert(ts > 0);

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
	ns->recv_time = m->rc.saved_recv_time;

    if(bf->c11)
        codes_issue_next_event_rc(lp);

    if(bf->c6)
        codes_issue_next_event_rc(lp);
	if(m->fwd.found_match >= 0)
	  {
		ns->recv_time = m->rc.saved_recv_time;
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

	m->rc.saved_recv_time = s->recv_time;
    m->rc.saved_num_bytes = mpi_op->u.recv.num_bytes;

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
           s->mpi_wkld_samples[indx].num_bytes_sample -= m->rc.saved_num_bytes;

           if(bf->c1)
           {
               s->sampling_indx--;
               s->cur_interval_end -= sampling_interval;
           }
        }
        if(bf->c15 || bf->c16)
            s->num_sends--;

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
            s->num_bytes_sent -= m->rc.saved_num_bytes;
            num_bytes_sent -= m->rc.saved_num_bytes;
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
   
    char prio[12];
    if(priority_type == 0)
    {
        if(s->app_id == 0) 
          strcpy(prio, "high");
        else
          strcpy(prio, "medium");
    }
    else if(priority_type == 1)
    {
        if(mpi_op->u.send.tag == COL_TAG || mpi_op->u.send.tag == BAR_TAG)
        {
            strcpy(prio, "high");
        }
        else
            strcpy(prio, "medium");
    }
    else
        tw_error(TW_LOC, "\n Invalid priority type %d", priority_type);

    int is_eager = 0;
	/* model-net event */
    int global_dest_rank = mpi_op->u.send.dest_rank;

    if(alloc_spec)
    {
        global_dest_rank = get_global_id_of_job_rank(mpi_op->u.send.dest_rank, s->app_id);
    }

    if(lp->gid == TRACK_LP)
        printf("\n Sender rank %llu global dest rank %d dest-rank %d bytes %"PRIu64" Tag %d", LLU(s->nw_id), global_dest_rank, mpi_op->u.send.dest_rank, mpi_op->u.send.num_bytes, mpi_op->u.send.tag);
    m->rc.saved_num_bytes = mpi_op->u.send.num_bytes;
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
        tw_stime copy_overhead = copy_per_byte_eager * mpi_op->u.send.num_bytes;
        local_m.fwd.sim_start_time = tw_now(lp);

        remote_m = local_m;
        remote_m.msg_type = MPI_SEND_ARRIVED;
    	m->event_rc = model_net_event_mctx(net_id, &group_ratio, &group_ratio, 
            prio, dest_rank, mpi_op->u.send.num_bytes, (self_overhead + copy_overhead + soft_delay_mpi + nic_delay),
	    sizeof(nw_message), (const void*)&remote_m, sizeof(nw_message), (const void*)&local_m, lp);
    }
    else if (is_rend == 0)
    {
        /* Initiate the handshake. Issue a control message to the destination first. No local message,
         * only remote message sent. */
        bf->c16 = 1;
        s->num_sends++;
        remote_m.fwd.sim_start_time = tw_now(lp);
        remote_m.fwd.dest_rank = mpi_op->u.send.dest_rank;   
        remote_m.fwd.src_rank = mpi_op->u.send.source_rank;
        remote_m.msg_type = MPI_SEND_ARRIVED;
        remote_m.op_type = mpi_op->op_type;
        remote_m.fwd.tag = mpi_op->u.send.tag; 
        remote_m.fwd.num_bytes = mpi_op->u.send.num_bytes;
        remote_m.fwd.req_id = mpi_op->u.send.req_id;  
        remote_m.fwd.app_id = s->app_id;

    	m->event_rc = model_net_event_mctx(net_id, &group_ratio, &group_ratio, 
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
    	
       m->event_rc = model_net_event_mctx(net_id, &group_ratio, &group_ratio, 
            prio, dest_rank, mpi_op->u.send.num_bytes, (self_overhead + soft_delay_mpi + nic_delay),
	    sizeof(nw_message), (const void*)&remote_m, sizeof(nw_message), (const void*)&local_m, lp);
    }
    if(enable_debug && !is_rend)
    {
        if(mpi_op->op_type == CODES_WK_ISEND)
        {
            fprintf(workload_log, "\n (%lf) APP %d MPI ISEND SOURCE %llu DEST %d TAG %d BYTES %"PRId64,
                    tw_now(lp), s->app_id, LLU(s->nw_id), global_dest_rank, mpi_op->u.send.tag, mpi_op->u.send.num_bytes);
        }
        else
            fprintf(workload_log, "\n (%lf) APP ID %d MPI SEND SOURCE %llu DEST %d TAG %d BYTES %"PRId64,
                    tw_now(lp), s->app_id, LLU(s->nw_id), global_dest_rank, mpi_op->u.send.tag, mpi_op->u.send.num_bytes);
    }
    if(is_rend || is_eager)    
    {
       bf->c3 = 1;
       s->num_bytes_sent += mpi_op->u.send.num_bytes;
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
       s->wait_time = m->rc.saved_wait_time;
       add_completed_reqs(s, lp, m->fwd.num_matched);
       codes_issue_next_event_rc(lp);
    }
    if(m->fwd.wait_completed > 0)
           s->wait_op->num_completed--;
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

    int waiting = 0;
    waiting = notify_posted_wait(s, bf, m, lp, req_id);

    if(!waiting)
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
    
            m->rc.saved_wait_time = s->wait_time;
            s->wait_time += (tw_now(lp) - s->wait_op->start_time);

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

    char prio[12];
    if(priority_type == 0)
    {
        if(s->app_id == 0) 
          strcpy(prio, "high");
        else if(s->app_id == 1)
          strcpy(prio, "medium");
    }
    else if(priority_type == 1)
    {
        if(mpi_op->tag == COL_TAG || mpi_op->tag == BAR_TAG)
        {
            strcpy(prio, "high");
        }
        else
            strcpy(prio, "medium");
    }
    else
       tw_error(TW_LOC, "\n Invalid app id");
    
    m->event_rc = model_net_event_mctx(net_id, &group_ratio, &group_ratio, 
        prio, dest_rank, CONTROL_MSG_SZ, (self_overhead + soft_delay_mpi + nic_delay),
    sizeof(nw_message), (const void*)&remote_m, 0, NULL, lp);

}
/* reverse handler for updating arrival queue function */
static void update_arrival_queue_rc(nw_state* s,
        tw_bf * bf,
        nw_message * m, tw_lp * lp)
{
    s->num_bytes_recvd -= m->fwd.num_bytes;
    num_bytes_recvd -= m->fwd.num_bytes;

    if(bf->c1)
        codes_local_latency_reverse(lp);

    if(bf->c10)
        send_ack_back_rc(s, bf, m, lp);

    if(m->fwd.found_match >= 0)
	{
        mpi_msgs_queue * qi = (mpi_msgs_queue*)rc_stack_pop(s->processed_ops);
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
	        s->recv_time = m->rc.saved_recv_time;
        
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
	m->rc.saved_recv_time = s->recv_time;
    s->num_bytes_recvd += m->fwd.num_bytes;
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
        tw_stime ts = codes_local_latency(lp);
        assert(ts > 0);
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

    m->rc.saved_send_time = s->send_time;
    s->send_time += m->fwd.msg_send_time;
}

static void update_message_time_rc(
        nw_state * s,
        tw_bf * bf,
        nw_message * m,
        tw_lp * lp)
{
    (void)bf;
    (void)lp;
    s->send_time = m->rc.saved_send_time;
}

/* initializes the network node LP, loads the trace file in the structs, calls the first MPI operation to be executed */
void nw_test_init(nw_state* s, tw_lp* lp)
{
   /* initialize the LP's and load the data */

   memset(s, 0, sizeof(*s));
   s->nw_id = codes_mapping_get_lp_relative_id(lp->gid, 0, 0);
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

   char type_name[512];

   if(!num_net_traces)
	num_net_traces = num_mpi_lps;

   assert(num_net_traces <= num_mpi_lps);

   struct codes_jobmap_id lid;

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

   if (strcmp(workload_type, "dumpi") == 0){
       dumpi_trace_params params_d;
       strcpy(params_d.file_name, file_name_of_job[lid.job]);
       params_d.num_net_traces = num_traces_of_job[lid.job];
       params_d.nprocs = nprocs; 
       params = (char*)&params_d;
       strcpy(params_d.file_name, file_name_of_job[lid.job]);
       params_d.num_net_traces = num_traces_of_job[lid.job];
       params = (char*)&params_d;
       strcpy(type_name, "dumpi-trace-workload");
//       printf("network LP nw id %d app id %d local rank %d generating events, lp gid is %ld \n", s->nw_id, 
//               s->app_id, s->local_rank, lp->gid);
#ifdef ENABLE_CORTEX_PYTHON
	strcpy(params_d.cortex_script, cortex_file);
	strcpy(params_d.cortex_class, cortex_class);
	strcpy(params_d.cortex_gen, cortex_gen);
#endif
   }
   else if(strcmp(workload_type, "online") == 0){
           
       online_comm_params oc_params;
       
       if(strlen(workload_name) > 0)
       {
           strcpy(oc_params.workload_name, workload_name); 
       }
       else if(strlen(workloads_conf_file) > 0)
       {
            strcpy(oc_params.workload_name, file_name_of_job[lid.job]);
       
       }

       //assert(strcmp(oc_params.workload_name, "lammps") == 0 || strcmp(oc_params.workload_name, "nekbone") == 0);
       /*TODO: nprocs is different for dumpi and online workload. for
        * online, it is the number of ranks to be simulated. */
       oc_params.nprocs = num_traces_of_job[lid.job]; 
       params = (char*)&oc_params;
       strcpy(type_name, "online_comm_workload");
   }
       
   s->app_id = lid.job;
   s->local_rank = lid.rank;

   double overhead;
   int rc = configuration_get_value_double(&config, "PARAMS", "self_msg_overhead", NULL, &overhead);

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
//   rc_stack_create(&s->indices);
    
   assert(s->processed_ops != NULL);
   assert(s->processed_wait_op != NULL);
   assert(s->matched_reqs != NULL);
//   assert(s->indices != NULL);

   /* clock starts ticking when the first event is processed */
   s->start_time = tw_now(lp);
   s->num_bytes_sent = 0;
   s->num_bytes_recvd = 0;
   s->compute_time = 0;
   s->elapsed_time = 0;
        
   s->app_id = lid.job;
   s->local_rank = lid.rank;

   if(strncmp(file_name_of_job[lid.job], "synthetic", 9) == 0)
   {
        sscanf(file_name_of_job[lid.job], "synthetic%d", &synthetic_pattern);
        if(synthetic_pattern <=0 || synthetic_pattern > 5)
        {
            printf("\n Undefined synthetic pattern: setting to uniform random ");
            s->synthetic_pattern = 1;
        }
        else
        {
            s->synthetic_pattern = synthetic_pattern;
        }

        tw_event * e;
        nw_message * m_new;
        tw_stime ts = tw_rand_exponential(lp->rng, noise);
        e = tw_event_new(lp->gid, ts, lp);
        m_new = (nw_message*)tw_event_data(e);
        m_new->msg_type = CLI_BCKGND_GEN;
        tw_event_send(e);
        is_synthetic = 1;

   }
   else 
   {
   wrkld_id = codes_workload_load(type_name, params, s->app_id, s->local_rank);
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
   return;
}

void nw_test_event_handler(nw_state* s, tw_bf * bf, nw_message * m, tw_lp * lp)
{
    assert(s->app_id >= 0 && s->local_rank >= 0);

    //*(int *)bf = (int)0;
    rc_stack_gc(lp, s->matched_reqs);
//    rc_stack_gc(lp, s->indices);
    rc_stack_gc(lp, s->processed_ops);
    rc_stack_gc(lp, s->processed_wait_op);

    switch(m->msg_type)
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
                codes_local_latency(lp), lp);
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
            
            m->rc.saved_recv_time = s->recv_time;
            s->recv_time += (tw_now(lp) - m->fwd.sim_start_time);

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

        case CLI_BCKGND_ARRIVE:
            arrive_syn_tr(s, bf, m, lp);
            break;
        
        case CLI_NBR_FINISH:
            finish_nbr_wkld(s, bf, m, lp);
            break;
        
        case CLI_BCKGND_FIN:
            finish_bckgnd_traffic(s, bf, m, lp);
            break;

	}
}

static void get_next_mpi_operation_rc(nw_state* s, tw_bf * bf, nw_message * m, tw_lp * lp)
{
    codes_workload_get_next_rc(wrkld_id, s->app_id, s->local_rank, m->mpi_op);

	if(m->op_type == CODES_WK_END)
    {
        s->is_finished = 0;

        if(bf->c9)
            return;

        if(bf->c19)
            return;

        notify_neighbor_rc(s, lp, bf, m);
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
		}
		break;


        case CODES_WK_DELAY:
		{
			s->num_delays--;
            if(disable_delay)
                codes_issue_next_event_rc(lp);
            else
            {
                if (bf->c28)
                    tw_rand_reverse_unif(lp->rng);
                s->compute_time = m->rc.saved_delay;
            }
		}
		break;
		case CODES_WK_ALLREDUCE:
        {
            if(bf->c27)
            {
                s->num_all_reduce--;
                s->col_time = m->rc.saved_send_time; 
                s->all_reduce_time = m->rc.saved_delay;
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
		default:
			printf("\n Invalid op type %d ", m->op_type);
	}
}

static void get_next_mpi_operation(nw_state* s, tw_bf * bf, nw_message * m, tw_lp * lp)
{
		//struct codes_workload_op * mpi_op = malloc(sizeof(struct codes_workload_op));
//        printf("\n App id %d local rank %d ", s->app_id, s->local_rank);
    //    struct codes_workload_op mpi_op;
    //    codes_workload_get_next(wrkld_id, s->app_id, s->local_rank, &mpi_op);

	    struct codes_workload_op * mpi_op = (struct codes_workload_op*)malloc(sizeof(struct codes_workload_op));
        codes_workload_get_next(wrkld_id, s->app_id, s->local_rank, mpi_op);
        m->mpi_op = mpi_op; 
        m->op_type = mpi_op->op_type;

        if(mpi_op->op_type == CODES_WK_END)
        {
            s->elapsed_time = tw_now(lp) - s->start_time;
            s->is_finished = 1;

            
            if(!alloc_spec)
            {
                bf->c9 = 1;
                return;
            }
            
            /* Notify ranks from other job that checkpoint traffic has
             * completed */
             printf("\n Network node %d Rank %d finished at %lf ", s->local_rank, s->nw_id, tw_now(lp));
            int num_jobs = codes_jobmap_get_num_jobs(jobmap_ctx); 
             if(num_jobs <= 1 || is_synthetic == 0)
             {
                bf->c19 = 1;
                return;
             }

               //notify_neighbor(s, lp, bf, m);
//             printf("Client rank %llu completed workload, local rank %d .\n", s->nw_id, s->local_rank);

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
                    m->rc.saved_delay = s->all_reduce_time;
                    s->all_reduce_time += (tw_now(lp) - s->col_time);
                    m->rc.saved_send_time = s->col_time;
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
        else if(strcmp(workload_type, "online") == 0) 
        codes_workload_finalize("online_comm_workload", params, s->app_id, s->local_rank);
    }
    else
    {
        if(s->nw_id >= (tw_lpid)num_net_traces)
            return;
        
        if(strcmp(workload_type, "online") == 0) 
            codes_workload_finalize("online_comm_workload", params, s->app_id, s->local_rank);
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
            printf("\n nw-id %lld unmatched irecvs %d unmatched sends %d Total sends %ld receives %ld collectives %ld delays %ld wait alls %ld waits %ld send time %lf wait %lf",
			    s->nw_id, count_irecv, count_isend, s->num_sends, s->num_recvs, s->num_cols, s->num_delays, s->num_waitall, s->num_wait, s->send_time, s->wait_time);
        }
        written = 0;
    
        if(!s->nw_id)
            written = sprintf(s->output_buf, "# Format <LP ID> <Terminal ID> <Job ID> <Local Rank> <Total sends> <Total Recvs> <Bytes sent> <Bytes recvd> <Send time> <Comm. time> <Compute time> <Avg msg time> <Max Msg Time>");

        written += sprintf(s->output_buf + written, "\n %llu %llu %d %d %ld %ld %ld %ld %lf %lf %lf %lf %lf", LLU(lp->gid), LLU(s->nw_id), s->app_id, s->local_rank, s->num_sends, s->num_recvs, s->num_bytes_sent,
                s->num_bytes_recvd, s->send_time, s->elapsed_time - s->compute_time, s->compute_time, avg_msg_time, s->max_time);
        lp_io_write(lp->gid, (char*)"mpi-replay-stats", written, s->output_buf);

		if(s->elapsed_time - s->compute_time > max_comm_time)
			max_comm_time = s->elapsed_time - s->compute_time;

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
//	    rc_stack_destroy(s->indices);
	    rc_stack_destroy(s->processed_ops);
	    rc_stack_destroy(s->processed_wait_op);
}

void nw_test_event_handler_rc(nw_state* s, tw_bf * bf, nw_message * m, tw_lp * lp)
{
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
            codes_local_latency_reverse(lp);

            if(bf->c10)
                codes_issue_next_event_rc(lp);

            if(bf->c8)
                update_completed_queue_rc(s, bf, m, lp);
            
            s->recv_time = m->rc.saved_recv_time;
        }
        break;

        case MPI_OP_GET_NEXT:
			get_next_mpi_operation_rc(s, bf, m, lp);
		break;
        
        case CLI_BCKGND_GEN:
            gen_synthetic_tr_rc(s, bf, m, lp);
            break;

        case CLI_BCKGND_ARRIVE:
            arrive_syn_tr_rc(s, bf, m, lp);
            break;
        
        case CLI_NBR_FINISH:
            finish_nbr_wkld_rc(s, bf, m, lp);
            break;
        
        case CLI_BCKGND_FIN:
            finish_bckgnd_traffic_rc(s, bf, m, lp);
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
	TWOPT_UINT("num_net_traces", num_net_traces, "number of network traces"),
	TWOPT_UINT("priority_type", priority_type, "Priority type (zero): high priority to foreground traffic and low to background/2nd job, (one): high priority to collective operations "),
	TWOPT_UINT("payload_sz", payload_sz, "size of payload for synthetic traffic "),
	TWOPT_ULONGLONG("max_gen_data", max_gen_data, "maximum data to be generated for synthetic traffic "),
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
	TWOPT_END()
};

tw_lptype nw_lp = {
    (init_f) nw_test_init,
    (pre_run_f) NULL,
    (event_f) nw_test_event_handler,
    (revent_f) nw_test_event_handler_rc,
    (commit_f) NULL,
    (final_f) nw_test_finalize,
    (map_f) codes_mapping,
    sizeof(nw_state)
};

const tw_lptype* nw_get_lp_type()
{
            return(&nw_lp);
}

static void nw_add_lp_type()
{
  lp_type_register("nw-lp", nw_get_lp_type());
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

st_model_types nw_lp_model_types[] = {
    {(ev_trace_f) nw_lp_event_collect,
     sizeof(int),
     (model_stat_f) nw_lp_model_stat_collect,
     0,
     NULL,
     NULL,
     0},
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
  tw_init(argc, argv);
#ifdef USE_RDAMARIS
    if(g_st_ross_rank)
    { // keep damaris ranks from running code between here up until tw_end()
#endif
  codes_comm_update();

  if(strcmp(workload_type, "dumpi") != 0 && strcmp(workload_type, "online") != 0)
    {
	if(tw_ismaster())
		printf("Usage: mpirun -np n ./modelnet-mpi-replay --sync=1/3"
                " --workload_type=dumpi/online"
		" --workload_conf_file=prefix-workload-file-name"
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

	jobmap_ctx = NULL; // make sure it's NULL if it's not used

    sprintf(sampling_dir, "sampling-dir");
    mkdir(sampling_dir, S_IRUSR | S_IWUSR | S_IXUSR);

    sprintf(mpi_msg_dir, "synthetic%d", syn_type);
    mkdir(mpi_msg_dir, S_IRUSR | S_IWUSR | S_IXUSR);
    if(strlen(workloads_conf_file) > 0)
    {
        FILE *name_file = fopen(workloads_conf_file, "r");
        if(!name_file)
            tw_error(TW_LOC, "\n Could not open file %s ", workloads_conf_file);

        int i = 0;
        char ref = '\n';
        while(!feof(name_file))
        {
            ref = fscanf(name_file, "%d %s", &num_traces_of_job[i], file_name_of_job[i]);
            
            if(ref != EOF && strncmp(file_name_of_job[i], "synthetic", 9) == 0)
            {
              num_syn_clients = num_traces_of_job[i];
              num_net_traces += num_traces_of_job[i];
            }
            else if(ref!=EOF)
            {
                if(enable_debug)
                    printf("\n%d traces of app %s \n", num_traces_of_job[i], file_name_of_job[i]);

                num_net_traces += num_traces_of_job[i];
                num_dumpi_traces += num_traces_of_job[i];
            }
                i++;
        }
        printf("\n num_net_traces %d ", num_net_traces);
        fclose(name_file);
        assert(strlen(alloc_file) != 0);
        alloc_spec = 1;
        jobmap_p.alloc_file = alloc_file;
        jobmap_ctx = codes_jobmap_configure(CODES_JOBMAP_LIST, &jobmap_p);
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
   
   group_ratio = codes_mctx_set_group_ratio(NULL, true);

   if(enable_sampling)
       model_net_enable_sampling(sampling_interval, sampling_end_time);

   codes_mapping_setup();

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
   tw_run();

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
     double g_total_syn_data;

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
    
    if(synthetic_pattern == PERMUTATION)
        printf("\n Threshold for random permutation %ld ", perm_switch_thresh);
   }
    if (do_lp_io){
        int ret = lp_io_flush(io_handle, MPI_COMM_CODES);
        assert(ret == 0 || !"lp_io_flush failure");
    }
    if(is_synthetic)
        printf("\n Synthetic traffic stats: data received per proc %lf bytes \n", g_total_syn_data/num_syn_clients);

   model_net_report_stats(net_id);
   
   if(unmatched && g_tw_mynode == 0) 
       fprintf(stderr, "\n Warning: unmatched send and receive operations found.\n");
        //tw_error(TW_LOC, "\n Unmatched send and receive, terminating simulation");
   
   if(alloc_spec)
       codes_jobmap_destroy(jobmap_ctx);

#ifdef USE_RDAMARIS
    } // end if(g_st_ross_rank)
#endif
   tw_end();

  return 0;
}
