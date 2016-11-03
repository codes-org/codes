/*
 * Copyright (C) 2014 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */
#include <ross.h>
#include <inttypes.h>

#include "codes/codes-workload.h"
#include "codes/codes.h"
#include "codes/configuration.h"
#include "codes/codes_mapping.h"
#include "codes/model-net.h"
#include "codes/rc-stack.h"
#include "codes/quicklist.h"
#include "codes/codes-jobmap.h"

/* turning on track lp will generate a lot of output messages */
#define MN_LP_NM "modelnet_dragonfly_custom"

#define TRACK_LP -1
#define TRACE -1
#define MAX_WAIT_REQS 512
#define CS_LP_DBG 0
#define lprintf(_fmt, ...) \
        do {if (CS_LP_DBG) printf(_fmt, __VA_ARGS__);} while (0)
#define MAX_STATS 65536

char workload_type[128];
char workload_file[8192];
char offset_file[8192];
static int wrkld_id;
static int num_net_traces = 0;
static int alloc_spec = 0;
static double self_overhead = 10.0;

/* Doing LP IO*/
static char lp_io_dir[256] = {'\0'};
static lp_io_handle io_handle;
static unsigned int lp_io_use_suffix = 0;
static int do_lp_io = 0;

/* variables for loading multiple applications */
/* Xu's additions start */
char workloads_conf_file[8192];
char alloc_file[8192];
int num_traces_of_job[5];
char file_name_of_job[5][8192];

struct codes_jobmap_ctx *jobmap_ctx;
struct codes_jobmap_params_list jobmap_p;
/* Xu's additions end */

typedef struct nw_state nw_state;
typedef struct nw_message nw_message;
typedef int32_t dumpi_req_id;

static int net_id = 0;
static float noise = 5.0;
static int num_net_lps = 0, num_mpi_lps = 0;

FILE * workload_log = NULL;
FILE * workload_agg_log = NULL;
FILE * workload_meta_log = NULL;

static uint64_t sample_bytes_written = 0;

long long num_bytes_sent=0;
long long num_bytes_recvd=0;

double max_time = 0,  max_comm_time = 0, max_wait_time = 0, max_send_time = 0, max_recv_time = 0;
double avg_time = 0, avg_comm_time = 0, avg_wait_time = 0, avg_send_time = 0, avg_recv_time = 0;

/* global variables for codes mapping */
static char lp_group_name[MAX_NAME_LENGTH], lp_type_name[MAX_NAME_LENGTH], annotation[MAX_NAME_LENGTH];
static int mapping_grp_id, mapping_type_id, mapping_rep_id, mapping_offset;

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
    uint64_t num_bytes;
    tw_stime req_init_time;
	dumpi_req_id req_id;
    struct qlist_head ql;
};

/* stores request IDs of completed MPI operations (Isends or Irecvs) */
struct completed_requests
{
	dumpi_req_id req_id;
    struct qlist_head ql;
};

/* for wait operations, store the pending operation and number of completed waits so far. */
struct pending_waits
{
    int op_type;
    int32_t req_ids[MAX_WAIT_REQS];
	int num_completed;
	int count;
    tw_stime start_time;
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

    struct rc_stack * processed_ops;
    struct rc_stack * matched_reqs;

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
	double elapsed_time;
	/* time spent in compute operations */
	double compute_time;
	/* time spent in message send/isend */
	double send_time;
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

    unsigned long num_bytes_sent;
    unsigned long num_bytes_recvd;

    /* For sampling data */
    int sampling_indx;
    int max_arr_size;
    struct mpi_workload_sample * mpi_wkld_samples;
    char output_buf[512];
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
       int16_t req_id;
       int tag;
       int app_id;
       int found_match;
       short wait_completed;
   } fwd;
   struct
   {
       double saved_send_time;
       double saved_recv_time;
       double saved_wait_time;
       double saved_delay;
       int16_t saved_num_bytes;
       struct codes_workload_op * saved_op;
   } rc;
};

/* executes MPI isend and send operations */
static void codes_exec_mpi_send(
        nw_state* s, tw_bf * bf, nw_message * m, tw_lp* lp, struct codes_workload_op * mpi_op);
/* execute MPI irecv operation */
static void codes_exec_mpi_recv(
        nw_state* s, tw_bf * bf, nw_message * m, tw_lp * lp, struct codes_workload_op * mpi_op);
/* reverse of mpi recv function. */
static void codes_exec_mpi_recv_rc(
        nw_state* s, tw_bf * bf, nw_message* m, tw_lp* lp);
/* execute the computational delay */
static void codes_exec_comp_delay(
        nw_state* s, nw_message * m, tw_lp* lp, struct codes_workload_op * mpi_op);
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

/* Debugging functions, may generate unused function warning */
static void print_waiting_reqs(int32_t * reqs, int count)
{
    printf("\n Waiting reqs: ");
    int i;
    for(i = 0; i < count; i++ )
        printf(" %d ", reqs[i]);
}
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
            printf(" \n Source %d Dest %d bytes %llu tag %d ", current->source_rank, current->dest_rank, current->num_bytes, current->tag);
       }
}
static void print_completed_queue(struct qlist_head * head)
{
    printf("\n Completed queue: ");
      struct qlist_head * ent = NULL;
      struct completed_requests* current = NULL;
      qlist_for_each(ent, head)
       {
            current = qlist_entry(ent, completed_requests, ql);
            printf(" %d ", current->req_id);
       }
}
static int clear_completed_reqs(nw_state * s,
        tw_lp * lp,
        int32_t * reqs, int count)
{
    int i, matched = 0;
    for( i = 0; i < count; i++)
    {
      struct qlist_head * ent = NULL;
      qlist_for_each(ent, &s->completed_reqs)
       {
            struct completed_requests* current =
                qlist_entry(ent, completed_requests, ql);
            if(current->req_id == reqs[i])
            {
                ++matched;
                qlist_del(&current->ql);
                rc_stack_push(lp, current, free, s->matched_reqs);
            }
       }
    }
    return matched;
}
static void add_completed_reqs(nw_state * s,
        tw_lp * lp,
        int count)
{
    int i;
    for( i = 0; i < count; i++)
    {
       struct completed_requests * req = rc_stack_pop(s->matched_reqs);
       qlist_add(&req->ql, &s->completed_reqs);
    }
}

/* helper function - maps an MPI rank to an LP id */
static tw_lpid rank_to_lpid(int rank)
{
    return codes_mapping_get_lpid_from_relative(rank, NULL, "nw-lp", NULL, 0);
}

static int notify_posted_wait(nw_state* s,
        tw_bf * bf, nw_message * m, tw_lp * lp,
        dumpi_req_id completed_req)
{
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
                if(wait_elem->num_completed > wait_elem->count)
                    printf("\n Num completed %d count %d LP %llu ",
                            wait_elem->num_completed,
                            wait_elem->count,
                            lp->gid);
//                if(wait_elem->num_completed > wait_elem->count)
//                    tw_lp_suspend(lp, 1, 0);

                if(wait_elem->num_completed == wait_elem->count)
                {
                    if(enable_debug)
                        fprintf(workload_log, "\n(%lf) APP ID %d MPI WAITALL COMPLETED AT %llu ", tw_now(lp), s->app_id, s->nw_id);
                    wait_completed = 1;
                }

                m->fwd.wait_completed = 1;
            }
        }
    }
    return wait_completed;
}

/* reverse handler of MPI wait operation */
static void codes_exec_mpi_wait_rc(nw_state* s, tw_lp* lp)
{
    if(s->wait_op)
     {
         struct pending_waits * wait_op = s->wait_op;
         free(wait_op);
         s->wait_op = NULL;
     }
   else
    {
        codes_issue_next_event_rc(lp);
        completed_requests * qi = rc_stack_pop(s->processed_ops);
        qlist_add(&qi->ql, &s->completed_reqs);
    }
    return;
}

/* execute MPI wait operation */
static void codes_exec_mpi_wait(nw_state* s, tw_lp* lp, struct codes_workload_op * mpi_op)
{
    /* check in the completed receives queue if the request ID has already been completed.*/
    assert(!s->wait_op);
    dumpi_req_id req_id = mpi_op->u.wait.req_id;
    struct completed_requests* current = NULL;

    struct qlist_head * ent = NULL;
    qlist_for_each(ent, &s->completed_reqs)
    {
        current = qlist_entry(ent, completed_requests, ql);
        if(current->req_id == req_id)
        {
            qlist_del(&current->ql);
            rc_stack_push(lp, current, free, s->processed_ops);
            codes_issue_next_event(lp);
            return;
        }
    }
    /* If not, add the wait operation in the pending 'waits' list. */
    struct pending_waits* wait_op = malloc(sizeof(struct pending_waits));
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
    fprintf(workload_log, "\n MPI WAITALL POSTED AT %llu ", s->nw_id);

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
        struct mpi_workload_sample * tmp = calloc((MAX_STATS + s->max_arr_size), sizeof(struct mpi_workload_sample));
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

  /*if(lp->gid == TRACK)
  {
      printf("\n MPI Wait all posted ");
      print_waiting_reqs(mpi_op->u.waits.req_ids, count);
      print_completed_queue(&s->completed_reqs);
  }*/
      /* check number of completed irecvs in the completion queue */
  for(i = 0; i < count; i++)
  {
      dumpi_req_id req_id = mpi_op->u.waits.req_ids[i];
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
	  struct pending_waits* wait_op = malloc(sizeof(struct pending_waits));
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
    struct qlist_head *ent = NULL;
    mpi_msgs_queue * qi = NULL;

    qlist_for_each(ent, &ns->pending_recvs_queue){
        qi = qlist_entry(ent, mpi_msgs_queue, ql);
        if((qi->num_bytes == qitem->num_bytes)
                && ((qi->tag == qitem->tag) || qi->tag == -1)
                && ((qi->source_rank == qitem->source_rank) || qi->source_rank == -1))
        {
            matched = 1;
            break;
        }
        ++index;
    }

    if(matched)
    {
        m->rc.saved_recv_time = ns->recv_time;
        ns->recv_time += (tw_now(lp) - qi->req_init_time);

        if(qi->op_type == CODES_WK_IRECV)
            update_completed_queue(ns, bf, m, lp, qi->req_id);
        else if(qi->op_type == CODES_WK_RECV)
            codes_issue_next_event(lp);

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
        if((qi->num_bytes == qitem->num_bytes)
                && (qi->tag == qitem->tag || qitem->tag == -1)
                && ((qi->source_rank == qitem->source_rank) || qitem->source_rank == -1))
        {
            matched = 1;
            break;
        }
        ++index;
    }

    if(matched)
    {
        m->rc.saved_recv_time = ns->recv_time;
        ns->recv_time += (tw_now(lp) - qitem->req_init_time);

        if(qitem->op_type == CODES_WK_IRECV)
            update_completed_queue(ns, bf, m, lp, qitem->req_id);

        qlist_del(&qi->ql);

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
   e = tw_event_new( lp->gid, ts, lp );
   msg = tw_event_data(e);

   msg->msg_type = MPI_OP_GET_NEXT;
   tw_event_send(e);
}

/* Simulate delays between MPI operations */
static void codes_exec_comp_delay(
        nw_state* s, nw_message * m, tw_lp* lp, struct codes_workload_op * mpi_op)
{
	tw_event* e;
	tw_stime ts;
	nw_message* msg;

    m->rc.saved_delay = s->compute_time;
    s->compute_time += s_to_ns(mpi_op->u.delay.seconds);
    ts = s_to_ns(mpi_op->u.delay.seconds);

	ts += g_tw_lookahead + 0.1 + tw_rand_exponential(lp->rng, noise);

	e = tw_event_new( lp->gid, ts , lp );
	msg = tw_event_data(e);
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
	num_bytes_recvd -= m->rc.saved_num_bytes;
	ns->recv_time = m->rc.saved_recv_time;
	if(m->fwd.found_match >= 0)
	  {
		ns->recv_time = m->rc.saved_recv_time;
        int queue_count = qlist_count(&ns->arrival_queue);

        mpi_msgs_queue * qi = rc_stack_pop(ns->processed_ops);

        if(!m->fwd.found_match)
        {
            qlist_add(&qi->ql, &ns->arrival_queue);
        }
        else if(m->fwd.found_match >= queue_count)
        {
            qlist_add_tail(&qi->ql, &ns->arrival_queue);
        }
        else if(m->fwd.found_match > 0 && m->fwd.found_match < queue_count)
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
        if(qi->op_type == CODES_WK_IRECV)
        {
            update_completed_queue_rc(ns, bf, m, lp);
        }
        codes_issue_next_event_rc(lp);
      }
	else if(m->fwd.found_match < 0)
	    {
            struct qlist_head * ent = qlist_pop_back(&ns->pending_recvs_queue);
            mpi_msgs_queue * qi = qlist_entry(ent, mpi_msgs_queue, ql);
            free(qi);

            if(m->op_type == CODES_WK_IRECV)
                codes_issue_next_event_rc(lp);
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

	num_bytes_recvd += mpi_op->u.recv.num_bytes;

    mpi_msgs_queue * recv_op = (mpi_msgs_queue*) malloc(sizeof(mpi_msgs_queue));
    recv_op->req_init_time = tw_now(lp);
    recv_op->op_type = mpi_op->op_type;
    recv_op->source_rank = mpi_op->u.recv.source_rank;
    recv_op->dest_rank = mpi_op->u.recv.dest_rank;
    recv_op->num_bytes = mpi_op->u.recv.num_bytes;
    recv_op->tag = mpi_op->u.recv.tag;
    recv_op->req_id = mpi_op->u.recv.req_id;

    if(s->nw_id == (tw_lpid)TRACK_LP)
        printf("\n Receive op posted num bytes %llu source %d ", recv_op->num_bytes,
                recv_op->source_rank);

	int found_matching_sends = rm_matching_send(s, bf, m, lp, recv_op);

	/* save the req id inserted in the completed queue for reverse computation. */
	if(found_matching_sends < 0)
	  {
	   	  m->fwd.found_match = -1;
          qlist_add_tail(&recv_op->ql, &s->pending_recvs_queue);

	       /* for mpi irecvs, this is a non-blocking receive so just post it and move on with the trace read. */
		if(mpi_op->op_type == CODES_WK_IRECV)
		   {
			codes_issue_next_event(lp);
			return;
		   }
      }
	else
	  {
        m->fwd.found_match = found_matching_sends;
        codes_issue_next_event(lp);
	    rc_stack_push(lp, recv_op, free, s->processed_ops);
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
/* executes MPI send and isend operations */
static void codes_exec_mpi_send(nw_state* s,
        tw_bf * bf,
        nw_message * m,
        tw_lp* lp,
        struct codes_workload_op * mpi_op)
{
	/* model-net event */
    int global_dest_rank = mpi_op->u.send.dest_rank;

    if(alloc_spec)
    {
        global_dest_rank = get_global_id_of_job_rank(mpi_op->u.send.dest_rank, s->app_id);
    }

    //printf("\n Sender rank %d global dest rank %d ", s->nw_id, global_dest_rank);
    m->rc.saved_num_bytes = mpi_op->u.send.num_bytes;
	/* model-net event */
	tw_lpid dest_rank = codes_mapping_get_lpid_from_relative(global_dest_rank, NULL, "nw-lp", NULL, 0);
	
    num_bytes_sent += mpi_op->u.send.num_bytes;
    s->num_bytes_sent += mpi_op->u.send.num_bytes;

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
            struct mpi_workload_sample * tmp = calloc((MAX_STATS + s->max_arr_size), sizeof(struct mpi_workload_sample));
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

    local_m.fwd.sim_start_time = tw_now(lp);
    local_m.fwd.dest_rank = mpi_op->u.send.dest_rank;
    local_m.fwd.src_rank = mpi_op->u.send.source_rank;
    local_m.op_type = mpi_op->op_type;
    local_m.msg_type = MPI_SEND_POSTED;
    local_m.fwd.tag = mpi_op->u.send.tag;
    local_m.fwd.num_bytes = mpi_op->u.send.num_bytes;
    local_m.fwd.req_id = mpi_op->u.send.req_id;
    local_m.fwd.app_id = s->app_id;

    remote_m = local_m;
	remote_m.msg_type = MPI_SEND_ARRIVED;

	m->event_rc = model_net_event_mctx(net_id, &group_ratio, &group_ratio, 
            "test", dest_rank, mpi_op->u.send.num_bytes, self_overhead,
	    sizeof(nw_message), (const void*)&remote_m, sizeof(nw_message), (const void*)&local_m, lp);

    if(enable_debug)
    {
        if(mpi_op->op_type == CODES_WK_ISEND)
        {
            fprintf(workload_log, "\n (%lf) APP %d MPI ISEND SOURCE %llu DEST %d TAG %d BYTES %llu ",
                    tw_now(lp), s->app_id, s->nw_id, global_dest_rank, mpi_op->u.send.tag, mpi_op->u.send.num_bytes);
        }
        else
            fprintf(workload_log, "\n (%lf) APP ID %d MPI SEND SOURCE %llu DEST %d TAG %d BYTES %llu ",
                    tw_now(lp), s->app_id, s->nw_id, global_dest_rank, mpi_op->u.send.tag, mpi_op->u.send.num_bytes);
    }
	/* isend executed, now get next MPI operation from the queue */
	if(mpi_op->op_type == CODES_WK_ISEND)
	   codes_issue_next_event(lp);
}

/* convert seconds to ns */
static tw_stime s_to_ns(tw_stime ns)
{
    return(ns * (1000.0 * 1000.0 * 1000.0));
}

static void update_completed_queue_rc(nw_state * s, tw_bf * bf, nw_message * m, tw_lp * lp)
{

    if(bf->c0)
    {
       struct qlist_head * ent = qlist_pop_back(&s->completed_reqs);

        completed_requests * req = qlist_entry(ent, completed_requests, ql);
      /*if(lp->gid == TRACK)
      {
          printf("\n After popping %ld ", req->req_id);
        print_completed_queue(&s->completed_reqs);
      }*/
       free(req);
    }
    else if(bf->c1)
    {
       struct pending_waits* wait_elem = rc_stack_pop(s->processed_ops);
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
    bf->c0 = 0;
    bf->c1 = 0;
    m->fwd.num_matched = 0;

    int waiting = 0;
    waiting = notify_posted_wait(s, bf, m, lp, req_id);

    if(!waiting)
    {
        bf->c0 = 1;
        completed_requests * req = malloc(sizeof(completed_requests));
        req->req_id = req_id;
        qlist_add_tail(&req->ql, &s->completed_reqs);

        /*if(lp->gid == TRACK)
        {
            printf("\n Forward mode adding %ld ", req_id);
            print_completed_queue(&s->completed_reqs);
        }*/
    }
    else
     {
            bf->c1 = 1;
            m->fwd.num_matched = clear_completed_reqs(s, lp, s->wait_op->req_ids, s->wait_op->count);
            m->rc.saved_wait_time = s->wait_time;
            s->wait_time += (tw_now(lp) - s->wait_op->start_time);

            struct pending_waits* wait_elem = s->wait_op;
            rc_stack_push(lp, wait_elem, free, s->processed_ops);
            s->wait_op = NULL;
            codes_issue_next_event(lp);
     }
}

/* reverse handler for updating arrival queue function */
static void update_arrival_queue_rc(nw_state* s,
        tw_bf * bf,
        nw_message * m, tw_lp * lp)
{
	s->recv_time = m->rc.saved_recv_time;
    s->num_bytes_recvd -= m->fwd.num_bytes;

    codes_local_latency_reverse(lp);

    if(m->fwd.found_match >= 0)
	{
        mpi_msgs_queue * qi = rc_stack_pop(s->processed_ops);
        int queue_count = qlist_count(&s->pending_recvs_queue);

        if(!m->fwd.found_match)
        {
            qlist_add(&qi->ql, &s->pending_recvs_queue);
        }
        else if(m->fwd.found_match >= queue_count)
        {
            qlist_add_tail(&qi->ql, &s->pending_recvs_queue);
        }
        else if(m->fwd.found_match > 0 && m->fwd.found_match < queue_count)
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
        if(qi->op_type == CODES_WK_IRECV)
            update_completed_queue_rc(s, bf, m, lp);
        else if(qi->op_type == CODES_WK_RECV)
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
                m->fwd.app_id, s->app_id, s->nw_id);
    assert(s->app_id == m->fwd.app_id);

    //if(s->local_rank != m->fwd.dest_rank)
    //    printf("\n Dest rank %d local rank %d ", m->fwd.dest_rank, s->local_rank);
	m->rc.saved_recv_time = s->recv_time;
    s->num_bytes_recvd += m->fwd.num_bytes;

    // send a callback to the sender to increment times
    // find the global id of the source
    int global_src_id = m->fwd.src_rank;
    if(alloc_spec)
    {
        global_src_id = get_global_id_of_job_rank(m->fwd.src_rank, s->app_id);
    }
    tw_event *e_callback =
        tw_event_new(rank_to_lpid(global_src_id),
                codes_local_latency(lp), lp);
    nw_message *m_callback = tw_event_data(e_callback);
    m_callback->msg_type = MPI_SEND_ARRIVED_CB;
    m_callback->fwd.msg_send_time = tw_now(lp) - m->fwd.sim_start_time;
    tw_event_send(e_callback);

    /* Now reconstruct the queue item */
    mpi_msgs_queue * arrived_op = (mpi_msgs_queue *) malloc(sizeof(mpi_msgs_queue));
    arrived_op->req_init_time = m->fwd.sim_start_time;
    arrived_op->op_type = m->op_type;
    arrived_op->source_rank = m->fwd.src_rank;
    arrived_op->dest_rank = m->fwd.dest_rank;
    arrived_op->num_bytes = m->fwd.num_bytes;
    arrived_op->tag = m->fwd.tag;

    if(s->nw_id == (tw_lpid)TRACK_LP)
        printf("\n Send op arrived source rank %d num bytes %llu ", arrived_op->source_rank,
                arrived_op->num_bytes);

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
    m->rc.saved_send_time = s->send_time;
    s->send_time += m->fwd.msg_send_time;
}

static void update_message_time_rc(
        nw_state * s,
        tw_bf * bf,
        nw_message * m,
        tw_lp * lp)
{
    s->send_time = m->rc.saved_send_time;
}

/* initializes the network node LP, loads the trace file in the structs, calls the first MPI operation to be executed */
void nw_test_init(nw_state* s, tw_lp* lp)
{
   /* initialize the LP's and load the data */
   char * params = NULL;
   dumpi_trace_params params_d;

   memset(s, 0, sizeof(*s));
   s->nw_id = codes_mapping_get_lp_relative_id(lp->gid, 0, 0);
   s->mpi_wkld_samples = calloc(MAX_STATS, sizeof(struct mpi_workload_sample));
   s->sampling_indx = 0;

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
       strcpy(params_d.file_name, file_name_of_job[lid.job]);
       params_d.num_net_traces = num_traces_of_job[lid.job];
       params = (char*)&params_d;
       s->app_id = lid.job;
       s->local_rank = lid.rank;
//       printf("network LP nw id %d app id %d local rank %d generating events, lp gid is %ld \n", s->nw_id, 
//               s->app_id, s->local_rank, lp->gid);
   }

   wrkld_id = codes_workload_load("dumpi-trace-workload", params, s->app_id, s->local_rank);

   double overhead;
   int rc = configuration_get_value_double(&config, "PARAMS", "self_msg_overhead", NULL, &overhead);

   if(overhead)
       self_overhead = overhead;

   INIT_QLIST_HEAD(&s->arrival_queue);
   INIT_QLIST_HEAD(&s->pending_recvs_queue);
   INIT_QLIST_HEAD(&s->completed_reqs);

   /* Initialize the RC stack */
   rc_stack_create(&s->processed_ops);
   rc_stack_create(&s->matched_reqs);

   assert(s->processed_ops != NULL);
   assert(s->matched_reqs != NULL);


   /* clock starts ticking when the first event is processed */
   s->start_time = tw_now(lp);
   codes_issue_next_event(lp);
   s->num_bytes_sent = 0;
   s->num_bytes_recvd = 0;
   s->compute_time = 0;
   s->elapsed_time = 0;

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
	if(s->app_id < 0)
        printf("\n msg type %d ", m->msg_type);

    assert(s->app_id >= 0 && s->local_rank >= 0);

    *(int *)bf = (int)0;
    rc_stack_gc(lp, s->matched_reqs);
    rc_stack_gc(lp, s->processed_ops);

    switch(m->msg_type)
	{
		case MPI_SEND_ARRIVED:
			update_arrival_queue(s, bf, m, lp);
		break;

		case MPI_SEND_ARRIVED_CB:
			update_message_time(s, bf, m, lp);
		break;

        case MPI_SEND_POSTED:
        {
           if(m->op_type == CODES_WK_SEND)
               codes_issue_next_event(lp);
           else
            if(m->op_type == CODES_WK_ISEND)
            {
              update_completed_queue(s, bf, m, lp, m->fwd.req_id);
            }
        }
        break;
		case MPI_OP_GET_NEXT:
			get_next_mpi_operation(s, bf, m, lp);
		break;
	}
}

static void get_next_mpi_operation_rc(nw_state* s, tw_bf * bf, nw_message * m, tw_lp * lp)
{
    codes_workload_get_next_rc2(wrkld_id, s->app_id, s->local_rank);

	if(m->op_type == CODES_WK_END)
    {
		return;
    }
	switch(m->op_type)
	{
		case CODES_WK_SEND:
		case CODES_WK_ISEND:
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
            model_net_event_rc2(lp, &m->event_rc);
			if(m->op_type == CODES_WK_ISEND)
				codes_issue_next_event_rc(lp);
			s->num_sends--;
            s->num_bytes_sent += m->rc.saved_num_bytes;
			num_bytes_sent -= m->rc.saved_num_bytes;
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
                tw_rand_reverse_unif(lp->rng);
                s->compute_time = m->rc.saved_delay;
            }
		}
		break;
		case CODES_WK_BCAST:
		case CODES_WK_ALLGATHER:
		case CODES_WK_ALLGATHERV:
		case CODES_WK_ALLTOALL:
		case CODES_WK_ALLTOALLV:
		case CODES_WK_REDUCE:
		case CODES_WK_ALLREDUCE:
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
			codes_exec_mpi_wait_rc(s, lp);
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
        struct codes_workload_op mpi_op;
        codes_workload_get_next(wrkld_id, s->app_id, s->local_rank, &mpi_op);

        m->op_type = mpi_op.op_type;

        if(mpi_op.op_type == CODES_WK_END)
        {
            s->elapsed_time = tw_now(lp) - s->start_time;
            return;
        }
		switch(mpi_op.op_type)
		{
			case CODES_WK_SEND:
			case CODES_WK_ISEND:
			 {
				s->num_sends++;
				codes_exec_mpi_send(s, bf, m, lp, &mpi_op);
			 }
			break;

			case CODES_WK_RECV:
			case CODES_WK_IRECV:
			  {
				s->num_recvs++;
				codes_exec_mpi_recv(s, bf, m, lp, &mpi_op);
			  }
			break;

			case CODES_WK_DELAY:
			  {
				s->num_delays++;
                if(disable_delay)
                    codes_issue_next_event(lp);
                else
				    codes_exec_comp_delay(s, m, lp, &mpi_op);
			  }
			break;

            case CODES_WK_WAITSOME:
            case CODES_WK_WAITANY:
            {
                s->num_waitsome++;
                codes_issue_next_event(lp);
            }
            break;

			case CODES_WK_WAITALL:
			  {
				s->num_waitall++;
			    codes_exec_mpi_wait_all(s, bf, m, lp, &mpi_op);
              }
			break;
			case CODES_WK_WAIT:
			{
				s->num_wait++;
                codes_exec_mpi_wait(s, lp, &mpi_op);
			}
			break;
			case CODES_WK_BCAST:
			case CODES_WK_ALLGATHER:
			case CODES_WK_ALLGATHERV:
			case CODES_WK_ALLTOALL:
			case CODES_WK_ALLTOALLV:
			case CODES_WK_REDUCE:
			case CODES_WK_ALLREDUCE:
			case CODES_WK_COL:
			{
				s->num_cols++;
			    codes_issue_next_event(lp);
            }
			break;
			default:
				printf("\n Invalid op type %d ", mpi_op.op_type);
		}
        return;
}

void nw_test_finalize(nw_state* s, tw_lp* lp)
{
    int written = 0;
    if(!s->nw_id)
        written = sprintf(s->output_buf, "# Format <LP ID> <Terminal ID> <Total sends> <Total Recvs> <Bytes sent> <Bytes recvd> <Send time> <Comm. time> <Compute time>");

    /*if(s->wait_op)
    {
        printf("\n Incomplete wait operation Rank %ld ", s->nw_id);
        print_waiting_reqs(s->wait_op->req_ids, s->wait_op->count);
    }*/
    if(alloc_spec == 1)
    {
        struct codes_jobmap_id lid;
        lid = codes_jobmap_to_local_id(s->nw_id, jobmap_ctx);

        if(lid.job < 0)
            return;
    }
    else
    {
        if(s->nw_id >= (tw_lpid)num_net_traces)
            return;
    }
		int count_irecv = qlist_count(&s->pending_recvs_queue);
        int count_isend = qlist_count(&s->arrival_queue);
		if(count_irecv || count_isend)
        printf("\n LP %llu unmatched irecvs %d unmatched sends %d Total sends %ld receives %ld collectives %ld delays %ld wait alls %ld waits %ld send time %lf wait %lf",
			lp->gid, count_irecv, count_isend, s->num_sends, s->num_recvs, s->num_cols, s->num_delays, s->num_waitall, s->num_wait, s->send_time, s->wait_time);

        written += sprintf(s->output_buf + written, "\n %llu %llu %ld %ld %ld %ld %lf %lf %lf", lp->gid, s->nw_id, s->num_sends, s->num_recvs, s->num_bytes_sent,
                s->num_bytes_recvd, s->send_time, s->elapsed_time - s->compute_time, s->compute_time);
        lp_io_write(lp->gid, "mpi-replay-stats", written, s->output_buf);

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

		avg_time += s->elapsed_time;
		avg_comm_time += (s->elapsed_time - s->compute_time);
		avg_wait_time += s->wait_time;
		avg_send_time += s->send_time;
		 avg_recv_time += s->recv_time;

		//printf("\n LP %ld Time spent in communication %llu ", lp->gid, total_time - s->compute_time);
	    rc_stack_destroy(s->matched_reqs);
	    rc_stack_destroy(s->processed_ops);
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
         if(m->op_type == CODES_WK_SEND)
             codes_issue_next_event_rc(lp);
         else if(m->op_type == CODES_WK_ISEND)
            update_completed_queue_rc(s, bf, m, lp);
        }
        break;

        case MPI_OP_GET_NEXT:
			get_next_mpi_operation_rc(s, bf, m, lp);
		break;
	}
}

const tw_optdef app_opt [] =
{
	TWOPT_GROUP("Network workload test"),
    TWOPT_CHAR("workload_type", workload_type, "workload type (either \"scalatrace\" or \"dumpi\")"),
	TWOPT_CHAR("workload_file", workload_file, "workload file name"),
	TWOPT_CHAR("alloc_file", alloc_file, "allocation file name"),
	TWOPT_CHAR("workload_conf_file", workloads_conf_file, "workload config file name"),
	TWOPT_UINT("num_net_traces", num_net_traces, "number of network traces"),
    TWOPT_UINT("disable_compute", disable_delay, "disable compute simulation"),
    TWOPT_UINT("enable_mpi_debug", enable_debug, "enable debugging of MPI sim layer (works with sync=1 only)"),
    TWOPT_UINT("sampling_interval", sampling_interval, "sampling interval for MPI operations"),
	TWOPT_UINT("enable_sampling", enable_sampling, "enable sampling"),
    TWOPT_STIME("sampling_end_time", sampling_end_time, "sampling_end_time"),
    TWOPT_CHAR("lp-io-dir", lp_io_dir, "Where to place io output (unspecified -> no output"),
    TWOPT_UINT("lp-io-use-suffix", lp_io_use_suffix, "Whether to append uniq suffix to lp-io directory (default 0)"),
	TWOPT_CHAR("offset_file", offset_file, "offset file name"),
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

int main( int argc, char** argv )
{
  int rank, nprocs;
  int num_nets;
  int* net_ids;

  g_tw_ts_end = s_to_ns(60*5); /* five minutes, in nsecs */

  workload_type[0]='\0';
  tw_opt_add(app_opt);
  tw_init(&argc, &argv);

  if(strcmp(workload_type, "dumpi") != 0)
    {
	if(tw_ismaster())
		printf("Usage: mpirun -np n ./modelnet-mpi-replay --sync=1/3"
                " --workload_type=dumpi --workload_conf_file=prefix-workload-file-name"
                " --alloc_file=alloc-file-name -- config-file-name\n"
                "See model-net/doc/README.dragonfly.txt and model-net/doc/README.torus.txt"
                " for instructions on how to run the models with network traces ");
	tw_end();
	return -1;
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
            ref = fscanf(name_file, "%d %s", &num_traces_of_job[i], file_name_of_job[i]);
            if(ref!=EOF)
            {
                if(enable_debug)
                    printf("\n%d traces of app %s \n", num_traces_of_job[i], file_name_of_job[i]);

                num_net_traces += num_traces_of_job[i];
                i++;
            }
        }
        fclose(name_file);
        assert(strlen(alloc_file) != 0);
        alloc_spec = 1;
        jobmap_p.alloc_file = alloc_file;
        jobmap_ctx = codes_jobmap_configure(CODES_JOBMAP_LIST, &jobmap_p);
    }
    else
    {
        assert(num_net_traces > 0 && strlen(workload_file));
        strcpy(file_name_of_job[0], workload_file);
        num_traces_of_job[0] = num_net_traces;
        alloc_spec = 0;
    }
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nprocs);

   configuration_load(argv[2], MPI_COMM_WORLD, &config);

   nw_add_lp_type();
   model_net_register();

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
   char agg_log_name[512];
   sprintf(agg_log_name, "mpi-aggregate-logs-%d.bin", rank);
   workload_agg_log = fopen(agg_log_name, "w+");
   workload_meta_log = fopen("mpi-workload-meta-log", "w+");
   
   group_ratio = codes_mctx_set_group_ratio(NULL, true);

   if(!workload_agg_log || !workload_meta_log)
   {
       printf("\n Error logging MPI operations... quitting ");
       MPI_Finalize();
       return -1;
   }
   if(enable_sampling)
       model_net_enable_sampling(sampling_interval, sampling_end_time);

   codes_mapping_setup();

   num_mpi_lps = codes_mapping_get_lp_count("MODELNET_GRP", 0, "nw-lp", NULL, 0);
   num_net_lps = codes_mapping_get_lp_count("MODELNET_GRP", 1, MN_LP_NM, NULL, 0);
    if (lp_io_dir[0]){
        do_lp_io = 1;
        /* initialize lp io */
        int flags = lp_io_use_suffix ? LP_IO_UNIQ_SUFFIX : 0;
        int ret = lp_io_prepare(lp_io_dir, flags, &io_handle, MPI_COMM_WORLD);
        assert(ret == 0 || !"lp_io_prepare failure");
    }
   tw_run();

    fclose(workload_agg_log);
    fclose(workload_meta_log);

    if(enable_debug)
        fclose(workload_log);

    long long total_bytes_sent, total_bytes_recvd;
    double max_run_time, avg_run_time;
   double max_comm_run_time, avg_comm_run_time;
    double total_avg_send_time, total_max_send_time;
     double total_avg_wait_time, total_max_wait_time;
     double total_avg_recv_time, total_max_recv_time;

    MPI_Reduce(&num_bytes_sent, &total_bytes_sent, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce(&num_bytes_recvd, &total_bytes_recvd, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
   MPI_Reduce(&max_comm_time, &max_comm_run_time, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);
   MPI_Reduce(&max_time, &max_run_time, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);
   MPI_Reduce(&avg_time, &avg_run_time, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);

   MPI_Reduce(&avg_recv_time, &total_avg_recv_time, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
   MPI_Reduce(&avg_comm_time, &avg_comm_run_time, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
   MPI_Reduce(&max_wait_time, &total_max_wait_time, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);
   MPI_Reduce(&max_send_time, &total_max_send_time, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);
   MPI_Reduce(&max_recv_time, &total_max_recv_time, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);
   MPI_Reduce(&avg_wait_time, &total_avg_wait_time, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
   MPI_Reduce(&avg_send_time, &total_avg_send_time, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);

   assert(num_net_traces);

   if(!g_tw_mynode)
	printf("\n Total bytes sent %llu recvd %llu \n max runtime %lf ns avg runtime %lf \n max comm time %lf avg comm time %lf \n max send time %lf avg send time %lf \n max recv time %lf avg recv time %lf \n max wait time %lf avg wait time %lf \n", total_bytes_sent, total_bytes_recvd,
			max_run_time, avg_run_time/num_net_traces,
			max_comm_run_time, avg_comm_run_time/num_net_traces,
			total_max_send_time, total_avg_send_time/num_net_traces,
			total_max_recv_time, total_avg_recv_time/num_net_traces,
			total_max_wait_time, total_avg_wait_time/num_net_traces);
    if (do_lp_io){
        int ret = lp_io_flush(io_handle, MPI_COMM_WORLD);
        assert(ret == 0 || !"lp_io_flush failure");
    }
   model_net_report_stats(net_id);
   if(alloc_spec)
       codes_jobmap_destroy(jobmap_ctx);

   tw_end();

  return 0;
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
