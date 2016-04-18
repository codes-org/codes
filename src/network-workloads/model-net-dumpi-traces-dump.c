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
#include "codes/codes-jobmap.h"
#include "codes/cortex/dfly_bcast.h"

#define TRACE -1
#define DEBUG 0
#define MSG_SIZE 1024

char workload_type[128] = {'\0'};
char workload_file[8192] = {'\0'};
char offset_file[8192] = {'\0'};
char alloc_file[8192] = {'\0'};
char config_file[8192] = {'\0'};
static int wrkld_id;

typedef struct nw_state nw_state;
typedef struct nw_message nw_message;

static int net_id = 0;
static float noise = 5.0;
static int num_net_lps, num_mn_mock_lps;
static int alloc_spec = 0;

long long bytes_sent=0;
long long bytes_recvd=0;

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

struct codes_jobmap_ctx *jobmap_ctx;
struct codes_jobmap_params_list jobmap_p;

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

    /* allocation specific data */
    int app_id;
    int local_rank;
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
void mn_mock_test_init(nw_state* s, tw_lp* lp)
{
   /* initialize the LP's and load the data */
   char * params;
   dumpi_trace_params params_d;
   cortex_wrkld_params params_c;

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
   if (strcmp(workload_type, "dumpi-trace-workload") == 0){
       assert(workload_file);
       strcpy(params_d.file_name, workload_file);
       params_d.num_net_traces = num_net_lps;
       params = (char*)&params_d;

       s->app_id = 0;
       s->local_rank = s->nw_id;
   }
   else if(strcmp(workload_type, "cortex-workload") == 0)
   {
       params_c.nprocs = cortex_dfly_get_job_ranks(0);
       params_c.algo_type = DFLY_BCAST_LLF;
    
       s->local_rank = lid.rank;
       s->app_id = lid.job;

       params_c.root = 0;
       params_c.size = MSG_SIZE;
       params = (char*)&params_c;
   }
   else
       tw_error(TW_LOC, "workload type not known %s ", workload_type);

  /* In this case, the LP will not generate any workload related events*/
   if(s->nw_id >= (tw_lpid)params_d.num_net_traces && !alloc_spec)
     {
	    //printf("\n network LP not generating events %d ", (int)s->nw_id);
	    return;
     }
   //printf("\n Rank %ld loading workload ", s->nw_id);
   wrkld_id = codes_workload_load(workload_type, params, s->app_id, s->local_rank);

   /* clock starts ticking */
   s->elapsed_time = tw_now(lp);
   codes_issue_next_event(lp);

   return;
}

void mn_mock_test_event_handler(nw_state* s, tw_bf * bf, nw_message * m, tw_lp * lp)
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

void mn_mock_test_event_handler_rc(nw_state* s, tw_bf * bf, nw_message * m, tw_lp * lp)
{
        codes_workload_get_next_rc(wrkld_id, s->app_id, s->local_rank, &m->op);
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
			bytes_sent -= m->op.u.send.num_bytes;
		};
		break;

		case CODES_WK_RECV:
		case CODES_WK_IRECV:
		{
			s->num_recvs--;
			s->recv_time -= (m->op.end_time - m->op.start_time);	
			bytes_recvd -= m->op.u.recv.num_bytes;
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
        codes_workload_get_next(wrkld_id, s->app_id, s->local_rank, &mpi_op);
	
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
                bytes_sent += mpi_op.u.send.num_bytes; 
			 }
			break;
	
			case CODES_WK_RECV:
			case CODES_WK_IRECV:
			  {
				s->num_recvs++;
				s->recv_time += (mpi_op.end_time - mpi_op.start_time);
				bytes_recvd += mpi_op.u.recv.num_bytes;
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

void mn_mock_test_finalize(nw_state* s, tw_lp* lp)
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

const tw_optdef app_opt_test [] =
{
	TWOPT_GROUP("Network workload test"),
    TWOPT_CHAR("workload_type", workload_type, "workload type (either \"scalatrace\" or \"dumpi\")"),
	TWOPT_CHAR("workload_file", workload_file, "workload file name"),
	TWOPT_CHAR("offset_file", offset_file, "offset file name"),
	TWOPT_CHAR("alloc_file", alloc_file, "allocation file name"),
	TWOPT_CHAR("config_file", config_file, "network config file name"),
	TWOPT_END()
};

tw_lptype mn_mock_lp = {
    (init_f) mn_mock_test_init,
    (pre_run_f) NULL,
    (event_f) mn_mock_test_event_handler,
    (revent_f) mn_mock_test_event_handler_rc,
    (final_f) mn_mock_test_finalize,
    (map_f) codes_mapping,
    sizeof(nw_state)
};

const tw_lptype* mn_mock_test_get_lp_type()
{
            return(&mn_mock_lp);
}

static void mn_mock_test_add_lp_type()
{
  lp_type_register("nw-lp", mn_mock_test_get_lp_type());
}

int main( int argc, char** argv )
{
  int rank, nprocs;
  int num_nets;
  int* net_ids;

  g_tw_ts_end = s_to_ns(60*60*24*365); /* one year, in nsecs */

  tw_opt_add(app_opt_test);
  tw_init(&argc, &argv);


    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nprocs);

    configuration_load(config_file, MPI_COMM_WORLD, &config);
    mn_mock_test_add_lp_type();

    codes_mapping_setup();

  if((strcmp(workload_type, "dumpi-trace-workload") == 0 && strlen(workload_file) == 0)
          || (strcmp(workload_type, "cortex-workload") == 0 && strlen(alloc_file) == 0))
    {
        if(tw_ismaster())
           printf("\n Usage: mpirun -np n ./model-net-dumpi-traces-dump --sync=1/2/3 --workload_type=type --config-file=config-file-name [--workload_file=workload-file-name --alloc_file=alloc-file-name]\n");
        tw_end();
        return -1;
    }

    if(strcmp(workload_type, "cortex-workload") == 0 )
    {
        assert(strlen(alloc_file) > 0);
        alloc_spec = 1;
        jobmap_p.alloc_file = alloc_file;
        jobmap_ctx = codes_jobmap_configure(CODES_JOBMAP_LIST, &jobmap_p); 
    }
    num_net_lps = codes_mapping_get_lp_count("MODELNET_GRP", 0, "nw-lp", NULL, 0);
   
    tw_run();

    long long total_bytes_sent, total_bytes_recvd;
    double avg_run_time = 0;
    double avg_comm_run_time = 0;
    double avg_col_run_time = 0;
    double total_avg_send_time = 0;
    double total_avg_wait_time = 0;
    double total_avg_recv_time = 0;
    double total_avg_col_time = 0;
    double total_avg_comp_time = 0;
    long overall_sends, overall_recvs, overall_waits, overall_cols;
	
    MPI_Reduce(&bytes_sent, &total_bytes_sent, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce(&bytes_recvd, &total_bytes_recvd, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
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
