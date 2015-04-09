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

#define TRACE -1
#define DEBUG 0

char workload_type[128];
char workload_file[8192];
char offset_file[8192];
static int wrkld_id;

typedef struct nw_state nw_state;
typedef struct nw_message nw_message;

static int net_id = 0;
static float noise = 5.0;
static int num_net_lps, num_nw_lps;
long long num_bytes_sent=0;
long long num_bytes_recvd=0;

double total_time = 0;
double avg_time = 0;
double avg_comm_time = 0;
double avg_wait_time = 0;
double avg_send_time = 0;
double avg_recv_time = 0;
double avg_col_time = 0;
double avg_compute_time = 0;

long total_waits = 0;
long total_collectives = 0;
long total_sends = 0;
long total_recvs = 0;
long total_delays = 0;

/* global variables for codes mapping */
static char lp_group_name[MAX_NAME_LENGTH], lp_type_name[MAX_NAME_LENGTH], annotation[MAX_NAME_LENGTH];
static int mapping_grp_id, mapping_type_id, mapping_rep_id, mapping_offset;

enum MPI_NW_EVENTS
{
	MPI_OP_GET_NEXT=1,
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
	unsigned long num_wait;
	unsigned long num_waitall;
	unsigned long num_waitsome;
	unsigned long num_waitany;

	/* time spent by the LP in executing the app trace*/
	double elapsed_time;

	/* time spent in compute operations */
	double compute_time;

	/* time spent in message send/isend */
	double send_time;

	/* time spent in message receive */
	double recv_time;
	
	/* time spent in wait operation */
	double wait_time;

	/* time spent in collective operations*/
	double col_time;
	
	/* total time spent in operations */
	double total_time;
};

/* network event being sent. msg_type is the type of message being sent, found_match is the index of the list maintained for reverse computation, op is the MPI event to be executed/reversed */
struct nw_message
{
	int msg_type;
        struct codes_workload_op op;
};

/* initialize queues, get next operation */
static void get_next_mpi_operation(nw_state* s, tw_bf * bf, nw_message * m, tw_lp * lp);

/* conversion from seconds to nanaoseconds */
static tw_stime s_to_ns(tw_stime ns);

/* issue next event */
static void codes_issue_next_event(tw_lp* lp);

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

/* convert seconds to ns */
static tw_stime s_to_ns(tw_stime ns)
{
    return(ns * (1000.0 * 1000.0 * 1000.0));
}

/* initializes the network node LP, loads the trace file in the structs, calls the first MPI operation to be executed */
void nw_test_init(nw_state* s, tw_lp* lp)
{
   /* initialize the LP's and load the data */
   char * params;
   scala_trace_params params_sc;
   dumpi_trace_params params_d;
  
   s->nw_id = lp->gid;
   s->wrkld_end = 0;

   s->num_sends = 0;
   s->num_recvs = 0;
   s->num_cols = 0;
   s->num_delays = 0;
   s->num_wait = 0;
   s->num_waitall = 0;
   s->num_waitsome = 0;
   s->num_waitany = 0;
   s->elapsed_time = 0;
   s->compute_time = 0;

   if (strcmp(workload_type, "dumpi") == 0){
       strcpy(params_d.file_name, workload_file);
       params_d.num_net_traces = num_net_lps;

       params = (char*)&params_d;
   }
  /* In this case, the LP will not generate any workload related events*/
   if(s->nw_id >= params_d.num_net_traces)
     {
	//printf("\n network LP not generating events %d ", (int)s->nw_id);
	return;
     }
   wrkld_id = codes_workload_load("dumpi-trace-workload", params, (int)s->nw_id);

   /* clock starts ticking */
   s->elapsed_time = tw_now(lp);
   codes_issue_next_event(lp);

   return;
}

void nw_test_event_handler(nw_state* s, tw_bf * bf, nw_message * m, tw_lp * lp)
{
	switch(m->msg_type)
	{
		case MPI_OP_GET_NEXT:
			get_next_mpi_operation(s, bf, m, lp);	
		break;

		default: 
			printf("\n Incorrect event handler ");
		break;
	}
}

void nw_test_event_handler_rc(nw_state* s, tw_bf * bf, nw_message * m, tw_lp * lp)
{
        codes_workload_get_next_rc(wrkld_id, (int)s->nw_id, &m->op);
        if(m->op.op_type == CODES_WK_END)
                return;

	s->total_time -= (m->op.end_time - m->op.start_time);
        switch(m->op.op_type)
        {
		case CODES_WK_SEND:
                case CODES_WK_ISEND:
		{
			s->num_sends--;
			s->send_time -= (m->op.end_time - m->op.start_time);
			num_bytes_sent -= m->op.u.send.num_bytes;
		};
		break;

		case CODES_WK_RECV:
		case CODES_WK_IRECV:
		{
			s->num_recvs--;
			s->recv_time -= (m->op.end_time - m->op.start_time);	
			num_bytes_recvd -= m->op.u.recv.num_bytes;
		}
		break;

		case CODES_WK_DELAY:
		{
			s->num_delays--;
			s->compute_time -= (m->op.end_time - m->op.start_time);
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
			s->col_time -= (m->op.end_time - m->op.start_time);
		}
		break;

		case CODES_WK_WAIT:
		{
			s->num_wait--;
			s->wait_time -= (m->op.end_time - m->op.start_time);
		}
		break;

		case CODES_WK_WAITALL:
		{
			s->num_waitall--;
			s->wait_time -= (m->op.end_time - m->op.start_time);
		}
		break;

		case CODES_WK_WAITSOME:
		{
			s->num_waitsome--;
			s->wait_time -= (m->op.end_time - m->op.start_time);
		}
		break;

		case CODES_WK_WAITANY:
		{
			s->num_waitany--;
			s->wait_time -= (m->op.end_time - m->op.start_time);
		}
		break;

		default:
		{
			printf("\n Invalid op type %d", m->op.op_type);
			return;
		}
	}
	tw_rand_reverse_unif(lp->rng);		
}

static void get_next_mpi_operation(nw_state* s, tw_bf * bf, nw_message * m, tw_lp * lp)
{
		struct codes_workload_op mpi_op;
    		codes_workload_get_next(wrkld_id, (int)s->nw_id, &mpi_op);
		memcpy(&m->op, &mpi_op, sizeof(struct codes_workload_op));

    		if(mpi_op.op_type == CODES_WK_END)
    	 	{
			//printf("\n workload ending!!! ");
			return;
     		}
		
		s->total_time += (mpi_op.end_time - mpi_op.start_time);
		
		switch(mpi_op.op_type)
		{
			case CODES_WK_SEND:
			case CODES_WK_ISEND:
			 {
				s->num_sends++;
				s->send_time += (mpi_op.end_time - mpi_op.start_time);
				num_bytes_sent += mpi_op.u.send.num_bytes; 
			 }
			break;
	
			case CODES_WK_RECV:
			case CODES_WK_IRECV:
			  {
				s->num_recvs++;
				s->recv_time += (mpi_op.end_time - mpi_op.start_time);
				num_bytes_recvd += mpi_op.u.recv.num_bytes;
			  }
			break;

			case CODES_WK_DELAY:
			  {
				s->num_delays++;
				s->compute_time += (mpi_op.end_time - mpi_op.start_time);
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
				s->col_time += (mpi_op.end_time - mpi_op.start_time);
			  }
			break;
			case CODES_WK_WAIT:
			{
				s->num_wait++;
				s->wait_time += (mpi_op.end_time - mpi_op.start_time);
			}
			break;
			case CODES_WK_WAITALL:
			{
				s->num_waitall++;
				s->wait_time += (mpi_op.end_time - mpi_op.start_time);	
			}
			break;
			case CODES_WK_WAITSOME:
			{
				s->num_waitsome++;
				s->wait_time += (mpi_op.end_time - mpi_op.start_time);
			}
			break;

			case CODES_WK_WAITANY:
			{
			   s->num_waitany++;
			   s->wait_time += (mpi_op.end_time - mpi_op.start_time);
			}
			break;
			default:
			{
				printf("\n Invalid op type %d ", m->op.op_type);
				return;
			}
		}
		codes_issue_next_event(lp);
}

void nw_test_finalize(nw_state* s, tw_lp* lp)
{
		total_waits += (s->num_wait + s->num_waitall + s->num_waitsome + s->num_waitany);
		total_recvs += (s->num_recvs);
		total_sends += (s->num_sends);
		total_delays += s->num_delays;
		total_collectives += s->num_cols;
		
		printf("\n LP %ld total sends %ld receives %ld wait_alls %ld waits %ld ", lp->gid, s->num_sends,s->num_recvs, s->num_waitall, s->num_wait);
	        avg_time += s->total_time;
		avg_compute_time += s->compute_time;
                avg_comm_time += (s->total_time - s->compute_time);
                avg_wait_time += s->wait_time;
                avg_send_time += s->send_time;
                avg_recv_time += s->recv_time;
		avg_col_time += s->col_time;
}

const tw_optdef app_opt [] =
{
	TWOPT_GROUP("Network workload test"),
    	TWOPT_CHAR("workload_type", workload_type, "workload type (either \"scalatrace\" or \"dumpi\")"),
	TWOPT_CHAR("workload_file", workload_file, "workload file name"),
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

   codes_mapping_setup();

   num_net_lps = codes_mapping_get_lp_count("MODELNET_GRP", 0, "nw-lp", NULL, 0);
   
    tw_run();

    long long total_bytes_sent, total_bytes_recvd;
    double avg_run_time;
    double avg_comm_run_time;
    double avg_col_run_time;
    double total_avg_send_time;
    double total_avg_wait_time;
    double total_avg_recv_time;
    double total_avg_col_time;
    double total_avg_comp_time;
    long overall_sends, overall_recvs, overall_waits, overall_cols;
	
    MPI_Reduce(&num_bytes_sent, &total_bytes_sent, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce(&num_bytes_recvd, &total_bytes_recvd, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
   MPI_Reduce(&avg_time, &avg_run_time, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);

   MPI_Reduce(&avg_recv_time, &total_avg_recv_time, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
   MPI_Reduce(&avg_comm_time, &avg_comm_run_time, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
   MPI_Reduce(&avg_col_time, &avg_col_run_time, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
  MPI_Reduce(&avg_wait_time, &total_avg_wait_time, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
   MPI_Reduce(&avg_send_time, &total_avg_send_time, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
   MPI_Reduce(&avg_compute_time, &total_avg_comp_time, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
   MPI_Reduce(&total_sends, &overall_sends, 1, MPI_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
   MPI_Reduce(&total_recvs, &overall_recvs, 1, MPI_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
   MPI_Reduce(&total_waits, &overall_waits, 1, MPI_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
   MPI_Reduce(&total_collectives, &overall_cols, 1, MPI_LONG, MPI_SUM, 0, MPI_COMM_WORLD);

   if(!g_tw_mynode)
	printf("\n Total bytes sent %lld recvd %lld \n avg runtime %lf \n avg comm time %lf avg compute time %lf \n avg collective time %lf avg send time %lf \n avg recv time %lf \n avg wait time %lf \n total sends %ld total recvs %ld total waits %ld total collectives %ld ", total_bytes_sent, total_bytes_recvd, 
			avg_run_time/num_net_lps,
			avg_comm_run_time/num_net_lps,
			total_avg_comp_time/num_net_lps,
			total_avg_col_time/num_net_lps,
			total_avg_send_time/num_net_lps,
			total_avg_recv_time/num_net_lps,
			total_avg_wait_time/num_net_lps,
			overall_sends, overall_recvs, overall_waits, overall_cols);
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
