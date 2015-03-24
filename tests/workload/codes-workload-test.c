/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

/* SUMMARY:
 *
 * This is a test harness for the codes workload API.  It sets up two LP
 * types: clients (which consume operations from the workload generator) and
 * servers (which service operations submitted by clients).
 *
 */

#include <string.h>
#include <assert.h>
#include <ross.h>

#include "codes/lp-io.h"
#include "codes/codes.h"
#include "codes/codes-workload.h"
#include "codes/configuration.h"
#include "codes-workload-test-svr-lp.h"
#include "codes-workload-test-cn-lp.h"

#define NUM_SERVERS 16  /* number of servers */
#define NUM_CLIENTS 48  /* number of clients */

const tw_optdef app_opt[] = {
    TWOPT_GROUP("CODES Workload Test Model"),
    TWOPT_END()
};

static int num_clients_per_lp = -1;

void workload_set_params()
{
    char io_kernel_meta_path[MAX_NAME_LENGTH_WKLD];
    
    configuration_get_value(&config, "PARAMS", "workload_type", NULL, workload_type, MAX_NAME_LENGTH_WKLD);
    if(strcmp(workload_type,"iolang_workload") == 0)
    {
        strcpy(ioparams.io_kernel_path,"");
	    ioparams.num_cns = NUM_CLIENTS;

        configuration_get_value(&config, "PARAMS", "io_kernel_meta_path", NULL, io_kernel_meta_path, MAX_NAME_LENGTH_WKLD);
        strcpy(ioparams.io_kernel_meta_path, io_kernel_meta_path);
    }
}

int main(
    int argc,
    char **argv)
{
    int nprocs;
    int rank;
    int lps_per_proc;
    int i;
    int ret;
    lp_io_handle handle;

    g_tw_ts_end = 60*60*24*365;

    tw_opt_add(app_opt);
    tw_init(&argc, &argv);
 
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nprocs);
  
    if((NUM_SERVERS + NUM_CLIENTS) % nprocs)
    {
        fprintf(stderr, "Error: number of server LPs (%d total) is not evenly divisible by the number of MPI processes (%d)\n", NUM_SERVERS+NUM_CLIENTS, nprocs);
        exit(-1);
    }

    if(argc < 2)
    {
        printf("\n Usage: mpirun <args> --sync=2/3 mapping_file_name.conf (optional --nkp) ");
        exit(-1);
    }

    lps_per_proc = (NUM_SERVERS+NUM_CLIENTS) / nprocs;

    num_clients_per_lp = NUM_CLIENTS / nprocs;

    configuration_load(argv[2], MPI_COMM_WORLD, &config);


    tw_define_lps(lps_per_proc, 512, 0);

    for(i=0; i<lps_per_proc; i++)
    {
        if((rank*lps_per_proc + i) < NUM_CLIENTS)
            tw_lp_settype(i, &client_lp);
        else
            tw_lp_settype(i, &svr_lp);
    }

    cn_set_params(NUM_CLIENTS, NUM_SERVERS);

    g_tw_lookahead = 100;

    ret = lp_io_prepare("codes-workload-test-results", LP_IO_UNIQ_SUFFIX, &handle, MPI_COMM_WORLD);
    if(ret < 0)
    {
       return(-1); 
    }

    workload_set_params();
    tw_run();

    ret = lp_io_flush(handle, MPI_COMM_WORLD);
    assert(ret == 0);

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
