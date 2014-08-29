/*
 * Copyright (C) 2014 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */
#include <ross.h>

#include "codes/codes-nw-workload.h"
#include "codes/codes.h"
#include "codes/configuration.h"
#include "codes/codes_mapping.h"
#include "codes/model-net.h"

#define TRACE 0
#define DEBUG 0

char workload_type[128];
char workload_file[8192];
char offset_file[8192];
static int wrkld_id;
static int num_net_traces = 0;

typedef struct nw_state nw_state;
typedef struct nw_message nw_message;

static int net_id = 0;
static float noise = 5.0;
static int num_net_lps, num_nw_lps;
long long num_bytes_sent=0;
long long num_bytes_recvd=0;
long long max_time = 0;

/* global variables for codes mapping */
static char lp_group_name[MAX_NAME_LENGTH], lp_type_name[MAX_NAME_LENGTH], annotation[MAX_NAME_LENGTH];
static int mapping_grp_id, mapping_type_id, mapping_rep_id, mapping_offset;

enum MPI_NW_EVENTS
{
	MPI_OP_GET_NEXT=1,
	MPI_SEND_ARRIVED,
	MPI_SEND_POSTED,
};

struct mpi_msgs_queue
{
	mpi_event_list* mpi_op;
	struct mpi_msgs_queue* next;
};

/* maintains the head and tail of the queue, as well as the number of elements currently in queue */
struct mpi_queue_ptrs
{
	int num_elems;
	struct mpi_msgs_queue* queue_head;
	struct mpi_msgs_queue* queue_tail;
};

/* state of the network LP. It contains the pointers to send/receive lists */
struct nw_state
{
	long num_events_per_lp;
	tw_lpid nw_id;
	short wrkld_end;

	/* count of sends, receives, collectives and delays */
	unsigned long num_sends;
	unsigned long num_recvs;
	unsigned long num_cols;
	unsigned long num_delays;

	/* time spent by the LP in executing the app trace*/
	unsigned long long elapsed_time;
	/* time spent in compute operations */
	unsigned long long compute_time;

	/* FIFO for isend messages arrived on destination */
	struct mpi_queue_ptrs* arrival_queue;
	/* list of completed isend operations */
	struct mpi_queue_ptrs* completed_isend_queue;
	/* FIFO for irecv messages posted but not yet matched with send operations */
	struct mpi_queue_ptrs* pending_recvs_queue;
};

/* network event being sent. msg_type is the type of message being sent, found_match is the index of the list maintained for reverse computation, op is the MPI event to be executed/reversed */
struct nw_message
{
	int msg_type;
	int found_match;
        struct mpi_event_list op;
};

/* initialize queues, get next operation */
static void get_next_mpi_operation(nw_state* s, tw_bf * bf, nw_message * m, tw_lp * lp);

/* upon arrival of local completion message, inserts operation in completed send queue */
static void update_send_completion_queue(nw_state*s, tw_bf* bf, nw_message* m, tw_lp * lp);

/* reverse of the above function */
static void update_send_completion_queue_rc(nw_state*s, tw_bf* bf, nw_message* m, tw_lp * lp);

/* upon arrival of an isend operation, updates the arrival queue of the network */
static void update_arrival_queue(nw_state*s, tw_bf* bf, nw_message* m, tw_lp * lp);

/* reverse of the above function */
static void update_arrival_queue_rc(nw_state*s, tw_bf* bf, nw_message* m, tw_lp * lp);

/* insert MPI operation in the queue*/
static void mpi_queue_insert_op(struct mpi_queue_ptrs* mpi_queue, mpi_event_list* mpi_op);

/* remove MPI operation from the queue */
static int mpi_queue_remove_matching_op(tw_lpid lpid, struct mpi_queue_ptrs* mpi_queue, mpi_event_list* mpi_op);

/* remove the tail of the MPI operation */
static int mpi_queue_remove_tail(tw_lpid lpid, struct mpi_queue_ptrs* mpi_queue, mpi_event_list* mpi_op);

/* conversion from seconds to nanaoseconds */
static tw_stime s_to_ns(tw_stime ns);

/* executes MPI isend and send operations */
static void codes_exec_mpi_send(nw_state* s, nw_message* m, tw_lp* lp);

/* execute MPI irecv operation */
static void codes_exec_mpi_irecv(nw_state* s, nw_message* m, tw_lp* lp);

/* execute the computational delay */
static void codes_exec_comp_delay(nw_state* s, nw_message* m, tw_lp* lp);

/* execute collective operation */
static void codes_exec_mpi_col(nw_state* s, nw_message* m, tw_lp* lp);

/* issue next event */
static void codes_issue_next_event(tw_lp* lp);

/* initializes the queue and allocates memory */
static struct mpi_queue_ptrs* queue_init()
{
	struct mpi_queue_ptrs* mpi_queue = malloc(sizeof(struct mpi_queue_ptrs));

	mpi_queue->num_elems = 0;
	mpi_queue->queue_head = NULL;
	mpi_queue->queue_tail = NULL;
	
	return mpi_queue;
}

/* counts number of elements in the queue */
static int numQueue(struct mpi_queue_ptrs* mpi_queue)
{
	struct mpi_msgs_queue* tmp = malloc(sizeof(struct mpi_msgs_queue)); 
	assert(tmp);

	tmp = mpi_queue->queue_head;
	int count = 0;

	while(tmp)
	{
		++count;
		tmp = tmp->next;
	}
	return count;
	free(tmp);
}

/* prints elements in a send/recv queue */
static void printQueue(tw_lpid lpid, struct mpi_queue_ptrs* mpi_queue, char* msg)
{
	printf("\n ************ Printing the queue %s *************** ", msg);
	struct mpi_msgs_queue* tmp = malloc(sizeof(struct mpi_msgs_queue));
	assert(tmp);

	tmp = mpi_queue->queue_head;
	
	while(tmp)
	{
		if(tmp->mpi_op->op_type == CODES_NW_SEND || tmp->mpi_op->op_type == CODES_NW_ISEND)
			printf("\n lpid %ld send operation data type %d count %d tag %d source %d", 
				    lpid, tmp->mpi_op->u.send.data_type, tmp->mpi_op->u.send.count, 
				     tmp->mpi_op->u.send.tag, tmp->mpi_op->u.send.source_rank);
		else if(tmp->mpi_op->op_type == CODES_NW_IRECV || tmp->mpi_op->op_type == CODES_NW_RECV)
			printf("\n lpid %ld recv operation data type %d count %d tag %d source %d", 
				   lpid, tmp->mpi_op->u.recv.data_type, tmp->mpi_op->u.recv.count, 
				    tmp->mpi_op->u.recv.tag, tmp->mpi_op->u.recv.source_rank );
		else
			printf("\n Invalid data type in the queue %d ", tmp->mpi_op->op_type);
		tmp = tmp->next;
	}
	free(tmp);
}

/* re-insert element in the queue at the index --- maintained for reverse computation */
static void mpi_queue_update(struct mpi_queue_ptrs* mpi_queue, mpi_event_list* mpi_op, int pos)
{
	struct mpi_msgs_queue* elem = malloc(sizeof(struct mpi_msgs_queue));
	assert(elem);
	elem->mpi_op = mpi_op;
	
	/* inserting at the head */
	if(pos == 0)
	{
	   if(!mpi_queue->queue_tail)
		mpi_queue->queue_tail = elem;
	   elem->next = mpi_queue->queue_head;
	   mpi_queue->queue_head = elem;
	   mpi_queue->num_elems++;
	   return;
	}

	int index = 0;
	struct mpi_msgs_queue* tmp = mpi_queue->queue_head;
	while(index < pos - 1)
	{
		tmp = tmp->next;
		++index;
	}

	if(!tmp)
		printf("\n Invalid index! %d pos %d size %d ", index, pos, numQueue(mpi_queue));
	if(tmp == mpi_queue->queue_tail)
	    mpi_queue->queue_tail = elem;

	elem->next = tmp->next;
	tmp->next = elem;
	mpi_queue->num_elems++;

	return;
}

/* insert MPI send or receive operation in the queues starting from tail. Unmatched sends go to arrival queue and unmatched receives go to pending receives queues. */
static void mpi_queue_insert_op(struct mpi_queue_ptrs* mpi_queue, mpi_event_list* mpi_op)
{
	/* insert mpi operation */
	struct mpi_msgs_queue* elem = malloc(sizeof(struct mpi_msgs_queue));
	assert(elem);

	elem->mpi_op = mpi_op;
     	elem->next = NULL;

	if(!mpi_queue->queue_head)
	  mpi_queue->queue_head = elem;

	if(mpi_queue->queue_tail)
	    mpi_queue->queue_tail->next = elem;
	
        mpi_queue->queue_tail = elem;
	mpi_queue->num_elems++;

	return;
}

/* match the send/recv operations */
static int match_receive(tw_lpid lpid, mpi_event_list* op1, mpi_event_list* op2)
{
	/* Match the MPI send with the receive */
	if(op1->op_type == CODES_NW_ISEND || op1->op_type == CODES_NW_SEND)
	{
		if((op2->u.recv.num_bytes >= op1->u.send.num_bytes) &&
 	   	   ((op2->u.recv.tag == op1->u.send.tag) || op2->u.recv.tag == -1) &&
		   ((op2->u.recv.source_rank == op1->u.send.source_rank) || op2->u.recv.source_rank == -1))
		   {
			return 1;
		   }
	}
	else
	if(op1->op_type == CODES_NW_IRECV || op1->op_type == CODES_NW_RECV)
	{
		if((op1->u.recv.num_bytes >= op2->u.send.num_bytes) &&
 	   	   ((op1->u.recv.tag == op2->u.send.tag) || op1->u.recv.tag == -1) &&
		   ((op1->u.recv.source_rank == op2->u.send.source_rank) || op1->u.recv.source_rank == -1))
		   {
			return 1;
		   }
	}
	return 0;
}

/* used for reverse computation. removes the tail of the queue */
static int mpi_queue_remove_tail(tw_lpid lpid, struct mpi_queue_ptrs* mpi_queue, mpi_event_list* mpi_op)
{
	assert(mpi_queue->queue_tail);
	if(mpi_queue->queue_tail == NULL)
	{
		printf("\n Error! tail not updated ");	
		return 0;
	}
	struct mpi_msgs_queue* tmp = mpi_queue->queue_head;

	if(mpi_queue->queue_head == mpi_queue->queue_tail)
	{
		mpi_queue->queue_head = NULL;
		mpi_queue->queue_tail = NULL;
		free(tmp);
		mpi_queue->num_elems--;
		 return 1;
	}

	struct mpi_msgs_queue* elem = mpi_queue->queue_tail;

	while(tmp->next != mpi_queue->queue_tail)
		tmp = tmp->next;

	mpi_queue->queue_tail = tmp;
	mpi_queue->queue_tail->next = NULL;
	mpi_queue->num_elems--;

	free(elem);
	return 1;
}

/* search for a matching mpi operation and remove it from the list. 
 * Record the index in the list from where the element got deleted. 
 * Index is used for inserting the element once again in the queue for reverse computation. */
static int mpi_queue_remove_matching_op(tw_lpid lpid, struct mpi_queue_ptrs* mpi_queue,  mpi_event_list* mpi_op)
{
	if(mpi_queue->queue_head == NULL)
		return -1;

	/* remove mpi operation */
	struct mpi_msgs_queue* tmp = mpi_queue->queue_head;
	int indx = 0;

	/* if head of the list has the required mpi op to be deleted */
	if(match_receive(lpid, tmp->mpi_op, mpi_op))
	{
		if(mpi_queue->queue_head == mpi_queue->queue_tail)
		   {
			mpi_queue->queue_tail = NULL;
			mpi_queue->queue_head = NULL;
			 free(tmp);
		   }
		 else
		   {
			mpi_queue->queue_head = tmp->next;
			free(tmp);	
		   }
		mpi_queue->num_elems--;
		return indx;
	}

	/* record the index where matching operation has been found */
	struct mpi_msgs_queue* elem;

	while(tmp->next)	
	{
	   indx++;
	   elem = tmp->next;
	   if(match_receive(lpid, elem->mpi_op, mpi_op))
		{
		    if(elem == mpi_queue->queue_tail)
			mpi_queue->queue_tail = tmp;
		    tmp->next = elem->next;

		    free(elem);
		    mpi_queue->num_elems--;
		    return indx;
		}

	   tmp = tmp->next;
     }
	return -1;
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
static void codes_exec_comp_delay(nw_state* s, nw_message* m, tw_lp* lp)
{
	struct mpi_event_list* mpi_op = &(m->op);
	tw_event* e;
	tw_stime ts;
	nw_message* msg;

	s->compute_time += mpi_op->u.delay.nsecs;
	ts = mpi_op->u.delay.nsecs + g_tw_lookahead + 0.1;
	ts += tw_rand_exponential(lp->rng, noise);
	
	e = tw_event_new( lp->gid, ts , lp );
	msg = tw_event_data(e);
	msg->msg_type = MPI_OP_GET_NEXT;

	tw_event_send(e); 
}

/* reverse computation operation for MPI irecv */
static void codes_exec_mpi_irecv_rc(nw_state* s, nw_message* m, tw_lp* lp)
{
	num_bytes_recvd -= m->op.u.recv.num_bytes;
	if(m->found_match >= 0)
	  {
		//int count = numQueue(s->arrival_queue);
		mpi_queue_update(s->arrival_queue, &m->op, m->found_match);
		/*if(lp->gid == TRACE)
			printf("\n Reverse- after adding: arrival queue num_elems %d ", s->arrival_queue->num_elems);*/
	  }
	else if(m->found_match < 0)
	    {
		mpi_queue_remove_tail(lp->gid, s->pending_recvs_queue,  &m->op);
		/*if(lp->gid == TRACE)
			printf("\n Reverse- after removing: pending receive queue num_elems %d ", s->pending_recvs_queue->num_elems);*/
	    }
			
	tw_rand_reverse_unif(lp->rng); 
}

/* Execute MPI Irecv operation (non-blocking receive) */ 
static void codes_exec_mpi_irecv(nw_state* s, nw_message* m, tw_lp* lp)
{
/* Once an irecv is posted, list of completed sends is checked to find a matching isend.
   If no matching isend is found, the receive operation is queued in the pending queue of
   receive operations. */
	struct mpi_event_list* mpi_op = &(m->op);
	assert(mpi_op->op_type == CODES_NW_IRECV);

	num_bytes_recvd += mpi_op->u.recv.num_bytes;
	int count_before = numQueue(s->arrival_queue); 
	int found_matching_sends = mpi_queue_remove_matching_op(lp->gid, s->arrival_queue, mpi_op);

	if(found_matching_sends < 0)
	  {
		m->found_match = -1;
		mpi_queue_insert_op(s->pending_recvs_queue, mpi_op);
		
		/*if(lp->gid == TRACE)
			printf("\n After adding: pending receives queue num_elems %d ", s->pending_recvs_queue->num_elems);*/
	  }
	else 
	  {
		/*if(lp->gid == TRACE)
			printf("\n After removing: arrival queue num_elems %d ", s->arrival_queue->num_elems);*/
		int count_after = numQueue(s->arrival_queue);
		assert(count_before == (count_after+1));
	   	m->found_match = found_matching_sends;
	 }

	/* issue next MPI operation */
	codes_issue_next_event(lp);	
}

/* executes MPI send and isend operations */
static void codes_exec_mpi_send(nw_state* s, nw_message* m, tw_lp* lp)
{
	struct mpi_event_list* mpi_op = &(m->op);
	/* model-net event */
	tw_lpid dest_rank;

	codes_mapping_get_lp_info(lp->gid, lp_group_name, &mapping_grp_id, 
	    lp_type_name, &mapping_type_id, annotation, &mapping_rep_id, &mapping_offset);

	if(net_id == DRAGONFLY) /* special handling for the dragonfly case */
	{
		int num_routers, lps_per_rep, factor;
		num_routers = codes_mapping_get_lp_count("MODELNET_GRP", 1,
                  "dragonfly_router", NULL, 1);
	 	lps_per_rep = (2 * num_nw_lps) + num_routers;	
		factor = mpi_op->u.send.dest_rank / num_nw_lps;
		dest_rank = (lps_per_rep * factor) + (mpi_op->u.send.dest_rank % num_nw_lps);	
		//printf("\n local dest %d final dest %d ", mpi_op->u.send.dest_rank, dest_rank);
	}
	else
	{
		/* other cases like torus/simplenet/loggp etc. */
		codes_mapping_get_lp_id(lp_group_name, lp_type_name, NULL, 1,  
	    	  mpi_op->u.send.dest_rank, mapping_offset, &dest_rank);
	}

	num_bytes_sent += mpi_op->u.send.num_bytes;

	nw_message* local_m = malloc(sizeof(nw_message));
	nw_message* remote_m = malloc(sizeof(nw_message));
	assert(local_m && remote_m);

	local_m->op = *mpi_op;
	local_m->msg_type = MPI_SEND_POSTED;
	
	remote_m->op = *mpi_op;
	remote_m->msg_type = MPI_SEND_ARRIVED;
	model_net_event(net_id, "test", dest_rank, mpi_op->u.send.num_bytes, 0.0, 
	    sizeof(nw_message), (const void*)remote_m, sizeof(nw_message), (const void*)local_m, lp);
	
	/* isend executed, now get next MPI operation from the queue */ 
	if(mpi_op->op_type == CODES_NW_ISEND)
	   codes_issue_next_event(lp);
}

/* MPI collective operations */
static void codes_exec_mpi_col(nw_state* s, nw_message* m, tw_lp* lp)
{
	codes_issue_next_event(lp);
}

/* convert seconds to ns */
static tw_stime s_to_ns(tw_stime ns)
{
    return(ns * (1000.0 * 1000.0 * 1000.0));
}


static void update_send_completion_queue_rc(nw_state* s, tw_bf * bf, nw_message * m, tw_lp * lp)
{
	//mpi_queue_remove_matching_op(&s->completed_isend_queue_head, &s->completed_isend_queue_tail, &m->op, SEND);

	if(m->op.op_type == CODES_NW_SEND)
		tw_rand_reverse_unif(lp->rng);	
}

/* completed isends are added in the list */
static void update_send_completion_queue(nw_state* s, tw_bf * bf, nw_message * m, tw_lp * lp)
{
	//if(m->op.op_type == CODES_NW_SEND)
	//	printf("\n LP %ld Local isend operation completed ", lp->gid);
	
	 //mpi_queue_insert_op(&s->completed_isend_queue_head, &s->completed_isend_queue_tail, &m->op);

	/* blocking send operation */
	if(m->op.op_type == CODES_NW_SEND)
		codes_issue_next_event(lp);	

	 return;
}

/* reverse handler for updating arrival queue function */
static void update_arrival_queue_rc(nw_state* s, tw_bf * bf, nw_message * m, tw_lp * lp)
{
	if(m->found_match >= 0)
	{
		//int count = numQueue(s->pending_recvs_queue);
		mpi_queue_update(s->pending_recvs_queue, &m->op, m->found_match);
		
		/*if(lp->gid == TRACE)
			printf("\n Reverse: after adding pending recvs queue %d ", s->pending_recvs_queue->num_elems);*/
	}
	else if(m->found_match < 0)
	{
		mpi_queue_remove_tail(lp->gid, s->arrival_queue, &(m->op));	
		/*if(lp->gid == TRACE)
			printf("\n Reverse: after removing arrivals queue %d ", s->arrival_queue->num_elems);*/
	}
}

/* once an isend operation arrives, the pending receives queue is checked to find out if there is a irecv that has already been posted. If no isend has been posted, */
static void update_arrival_queue(nw_state* s, tw_bf * bf, nw_message * m, tw_lp * lp)
{
	int count_before = numQueue(s->pending_recvs_queue);
	int found_matching_recv = mpi_queue_remove_matching_op(lp->gid, s->pending_recvs_queue, &(m->op));

	if(found_matching_recv < 0)
	 {
		m->found_match = -1;
		mpi_queue_insert_op(s->arrival_queue, &(m->op));
		/*if(lp->gid == TRACE)
			printf("\n After adding arrivals queue %d ", s->arrival_queue->num_elems);*/
	}
	else
	  {
		/*if(lp->gid == TRACE)
			printf("\n After removing pending receives queue %d ", s->pending_recvs_queue->num_elems);*/
		int count_after = numQueue(s->pending_recvs_queue);
		assert(count_before == (count_after + 1));
		m->found_match = found_matching_recv;
	  }
	return;
}

/* initializes the network node LP, loads the trace file in the structs, calls the first MPI operation to be executed */
void nw_test_init(nw_state* s, tw_lp* lp)
{
   /* initialize the LP's and load the data */
   char * params;
   scala_trace_params params_sc;
   dumpi_trace_params params_d;
  
   codes_mapping_get_lp_info(lp->gid, lp_group_name, &mapping_grp_id, lp_type_name, 
	&mapping_type_id, annotation, &mapping_rep_id, &mapping_offset);
  
   s->nw_id = (mapping_rep_id * num_nw_lps) + mapping_offset;
   //printf("\n LP %ld network ID %ld ", lp->gid, s->nw_id);
   s->wrkld_end = 0;

   s->num_sends = 0;
   s->num_recvs = 0;
   s->num_cols = 0;
   s->num_delays = 0;
   s->elapsed_time = 0;
   s->compute_time = 0;

   if(!num_net_traces) 
	num_net_traces = num_net_lps;

   if (strcmp(workload_type, "scalatrace") == 0){
       if (params_sc.offset_file_name[0] == '\0'){
           tw_error(TW_LOC, "required argument for scalatrace offset_file");
           return;
       }
       strcpy(params_sc.offset_file_name, offset_file);
       strcpy(params_sc.nw_wrkld_file_name, workload_file);
       params = (char*)&params_sc;
   }
   else if (strcmp(workload_type, "dumpi") == 0){
       strcpy(params_d.file_name, workload_file);
       params_d.num_net_traces = num_net_traces;

       params = (char*)&params_d;
   }
  /* In this case, the LP will not generate any workload related events*/
   if(s->nw_id >= params_d.num_net_traces)
     {
	//printf("\n network LP not generating events %d ", (int)s->nw_id);
	return;
     }
   wrkld_id = codes_nw_workload_load("dumpi-trace-workload", params, (int)s->nw_id);

   s->arrival_queue = queue_init(); 
   s->pending_recvs_queue = queue_init();
   s->completed_isend_queue = queue_init();

   /* clock starts ticking */
   s->elapsed_time = tw_now(lp);
   codes_issue_next_event(lp);

   return;
}

void nw_test_event_handler(nw_state* s, tw_bf * bf, nw_message * m, tw_lp * lp)
{
	switch(m->msg_type)
	{
		case MPI_SEND_POSTED:
			update_send_completion_queue(s, bf, m, lp);
		break;

		case MPI_SEND_ARRIVED:
			update_arrival_queue(s, bf, m, lp);
		break;

		case MPI_OP_GET_NEXT:
			get_next_mpi_operation(s, bf, m, lp);	
		break; 
	}
}

static void get_next_mpi_operation_rc(nw_state* s, tw_bf * bf, nw_message * m, tw_lp * lp)
{
	codes_nw_workload_get_next_rc(wrkld_id, (int)s->nw_id, &m->op);
	if(m->op.op_type == CODES_NW_END)
		return;
	switch(m->op.op_type)
	{
		case CODES_NW_SEND:
		case CODES_NW_ISEND:
		{
			model_net_event_rc(net_id, lp, m->op.u.send.num_bytes);
			if(m->op.op_type == CODES_NW_ISEND)
				tw_rand_reverse_unif(lp->rng);	
			s->num_sends--;
			num_bytes_sent -= m->op.u.send.num_bytes;
		}
		break;

		case CODES_NW_IRECV:
		case CODES_NW_RECV:
		{
			codes_exec_mpi_irecv_rc(s, m, lp);
			s->num_recvs--;
		}
		break;
		case CODES_NW_DELAY:
		{
			tw_rand_reverse_unif(lp->rng);
			s->num_delays--;
			s->compute_time -= m->op.u.delay.nsecs;
		}
		break;
		case CODES_NW_BCAST:
		case CODES_NW_ALLGATHER:
		case CODES_NW_ALLGATHERV:
		case CODES_NW_ALLTOALL:
		case CODES_NW_ALLTOALLV:
		case CODES_NW_REDUCE:
		case CODES_NW_ALLREDUCE:
		case CODES_NW_COL:
		{
			s->num_cols--;
			tw_rand_reverse_unif(lp->rng);
		}
		break;
		default:
			printf("\n Invalid op type %d ", m->op.op_type);
	}
}

static void get_next_mpi_operation(nw_state* s, tw_bf * bf, nw_message * m, tw_lp * lp)
{
		mpi_event_list mpi_op;
    		codes_nw_workload_get_next(wrkld_id, (int)s->nw_id, &mpi_op);
		memcpy(&m->op, &mpi_op, sizeof(struct mpi_event_list));

    		if(mpi_op.op_type == CODES_NW_END)
    	 	{
			return;
     		}
		switch(mpi_op.op_type)
		{
			case CODES_NW_SEND:
			case CODES_NW_ISEND:
			 {
				s->num_sends++;
				codes_exec_mpi_send(s, m, lp);
			 }
			break;

			case CODES_NW_IRECV:
			case CODES_NW_RECV:
			  {
				s->num_recvs++;
				codes_exec_mpi_irecv(s, m, lp);
			  }
			break;

			case CODES_NW_DELAY:
			  {
				s->num_delays++;
				codes_exec_comp_delay(s, m, lp);
			  }
			break;

			case CODES_NW_BCAST:
			case CODES_NW_ALLGATHER:
			case CODES_NW_ALLGATHERV:
			case CODES_NW_ALLTOALL:
			case CODES_NW_ALLTOALLV:
			case CODES_NW_REDUCE:
			case CODES_NW_ALLREDUCE:
			case CODES_NW_COL:
			  {
				s->num_cols++;
				codes_exec_mpi_col(s, m, lp);
			  }
			break;
			default:
				printf("\n Invalid op type %d ", m->op.op_type);
		}
}

void nw_test_finalize(nw_state* s, tw_lp* lp)
{
	if(s->nw_id < num_net_traces)
	{
		int count_irecv = numQueue(s->pending_recvs_queue);
        	int count_isend = numQueue(s->arrival_queue);
		printf("\n LP %ld unmatched irecvs %d unmatched sends %d Total sends %ld receives %ld collectives %ld delays %ld ", 
			lp->gid, count_irecv, count_isend, s->num_sends, s->num_recvs, s->num_cols, s->num_delays);
		if(lp->gid == TRACE)
		{
		  printQueue(lp->gid, s->pending_recvs_queue, "irecv ");
		  printQueue(lp->gid, s->arrival_queue, "isend");
	        }
		s->elapsed_time = tw_now(lp) - s->elapsed_time;
		assert(s->elapsed_time >= s->compute_time);
		if(s->elapsed_time - s->compute_time > max_time)
			max_time = s->elapsed_time - s->compute_time;
		//printf("\n LP %ld Time spent in communication %llu bytes transferred %ld ", lp->gid, s->elapsed_time - s->compute_time, s->num_bytes_sent);
		free(s->arrival_queue);
		free(s->completed_isend_queue);
		free(s->pending_recvs_queue);
	}
}

void nw_test_event_handler_rc(nw_state* s, tw_bf * bf, nw_message * m, tw_lp * lp)
{
	switch(m->msg_type)
	{
		case MPI_SEND_POSTED:
			update_send_completion_queue_rc(s, bf, m, lp);
		break;

		case MPI_SEND_ARRIVED:
			update_arrival_queue_rc(s, bf, m, lp);
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
	TWOPT_UINT("num_net_traces", num_net_traces, "number of network traces"),
	TWOPT_CHAR("offset_file", offset_file, "offset file name"),
	TWOPT_END()
};

tw_lptype nw_lp = {
    (init_f) nw_test_init,
    (pre_run_f) NULL,
    (event_f) nw_test_event_handler,
    (revent_f) nw_test_event_handler_rc,
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

  g_tw_ts_end = s_to_ns(60*60*24*365); /* one year, in nsecs */

  workload_type[0]='\0';
  tw_opt_add(app_opt);
  tw_init(&argc, &argv);

  if(strlen(workload_file) == 0)
    {
	if(tw_ismaster())
		printf("\n Usage: mpirun -np n ./codes-nw-test --sync=1/2/3 --workload_type=type --workload_file=workload-file-name");
	tw_end();
	return -1;
    }

    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nprocs);

   configuration_load(argv[2], MPI_COMM_WORLD, &config);

   nw_add_lp_type();
   model_net_register();

   net_ids = model_net_configure(&num_nets);
   assert(num_nets == 1);
   net_id = *net_ids;
   free(net_ids);


   codes_mapping_setup();

   num_net_lps = codes_mapping_get_lp_count("MODELNET_GRP", 0, "nw-lp", NULL, 0);
   
   num_nw_lps = codes_mapping_get_lp_count("MODELNET_GRP", 1, 
			"nw-lp", NULL, 1);	
   tw_run();

    long long total_bytes_sent, total_bytes_recvd, max_run_time;	
    MPI_Reduce(&num_bytes_sent, &total_bytes_sent, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce(&num_bytes_recvd, &total_bytes_recvd, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
   MPI_Reduce(&max_time, &max_run_time, 1, MPI_LONG_LONG, MPI_MAX, 0, MPI_COMM_WORLD);
   if(!g_tw_mynode)
	printf("\n Total bytes sent %lld recvd %lld max runtime %lld ns \n", total_bytes_sent, total_bytes_recvd, max_run_time);
   tw_end();
  
  return 0;
}
