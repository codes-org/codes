/*
 * Copyright (C) 2014 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */
#include "codes/codes-nw-workload.h"
#include "codes/codes.h"

char workload_type[128];
char workload_file[8192];
char offset_file[8192];
static int total_nw_lps = 8;
static int nlp_per_pe;
static int wrkld_id;

typedef struct nw_state nw_state;
typedef struct nw_message nw_message;

FILE * data_log = NULL;

struct nw_state
{
	long num_events_per_lp;
};

struct nw_message
{
        struct mpi_event_list op;
	int dummy_data;
};

tw_peid nw_test_map(tw_lpid gid)
{
        return (tw_peid) gid / g_tw_nlp;
}

void nw_test_init(nw_state* s, tw_lp* lp)
{
   /* initialize the LP's and load the data */
   char * params;
   scala_trace_params params_sc;
#if USE_DUMPI
   dumpi_trace_params params_d;
#endif
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
#if USE_DUMPI
       strcpy(params_d.file_name, workload_file);
       params = (char*)&params_d;
#else
       tw_error(TW_LOC, "dumpi support not enable");
       return;
#endif
   }
   wrkld_id = codes_nw_workload_load("dumpi-trace-workload", params, (int)lp->gid);
   
   tw_event *e;
   tw_stime kickoff_time;
   
   kickoff_time = g_tw_lookahead + tw_rand_unif(lp->rng);

   e = codes_event_new(lp->gid, kickoff_time, lp);
   tw_event_send(e);

   return;
}

void nw_test_event_handler(nw_state* s, tw_bf * bf, nw_message * m, tw_lp * lp)
{
    codes_nw_workload_get_next(wrkld_id, (int)lp->gid, &m->op);

    codes_nw_workload_print_op(data_log, &m->op, lp->gid);
    if(m->op.op_type == CODES_NW_END)
	return;

   tw_event *e;
   tw_stime kickoff_time;

   kickoff_time = g_tw_lookahead + tw_rand_unif(lp->rng);

   e = codes_event_new(lp->gid, kickoff_time, lp);
   tw_event_send(e);
}

void nw_test_finalize(nw_state* s, tw_lp* lp)
{

}

void nw_test_event_handler_rc(nw_state* s, tw_bf * bf, nw_message * m, tw_lp * lp)
{
	codes_nw_workload_get_next_rc(wrkld_id, (int)lp->gid, &m->op);
}

const tw_optdef app_opt [] =
{
	TWOPT_GROUP("Network workload test"),
    TWOPT_CHAR("workload_type", workload_type, "workload type (either \"scalatrace\" or \"dumpi\")"),
	TWOPT_CHAR("workload_file", workload_file, "workload file name"),
	TWOPT_CHAR("offset_file", offset_file, "offset file name"),
	TWOPT_UINT("total_nw_lps", total_nw_lps, "total number of LPs"),
	TWOPT_END()
};

tw_lptype nwlps[] = {
        {
	 (init_f) nw_test_init,
     (pre_run_f) NULL,
         (event_f) nw_test_event_handler,
         (revent_f) nw_test_event_handler_rc,
         (final_f) nw_test_finalize,
         (map_f) nw_test_map,
         sizeof(nw_state)
	},
        {0},
};

int main( int argc, char** argv )
{
  int 	i;
  char log[32];

  workload_type[0]='\0';
  tw_opt_add(app_opt);
  tw_init(&argc, &argv);

  if(strlen(workload_file) == 0 || total_nw_lps == 0)
    {
	if(tw_ismaster())
		printf("\n Usage: mpirun -np n ./codes-nw-test --sync=1/2/3 --total_nw_lps=n --workload_type=type --workload_file=workload-file-name");
	tw_end();
	return -1;
    }
  nlp_per_pe = total_nw_lps / tw_nnodes();
  tw_define_lps(nlp_per_pe, sizeof(nw_message), 0);

  for(i = 0; i < nlp_per_pe; i++)
        tw_lp_settype(i, &nwlps[0]);
 
  sprintf( log, "mpi-data-log.%d", (int)g_tw_mynode );
  data_log = fopen( log, "w+");

  if(data_log == NULL)
     tw_error( TW_LOC, "Failed to open MPI event Log file \n");
 
  tw_run();
  tw_end();
  
  fclose(data_log);
  return 0;
}
