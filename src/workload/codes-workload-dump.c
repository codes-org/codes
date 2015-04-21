/*
 * Copyright (C) 2014 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#include <assert.h>
#include <getopt.h>
#include <stdio.h>
#include "codes/codes-workload.h"
#include <inttypes.h>

static char type[128] = {'\0'};
static darshan_params d_params = {"", 0}; 
static iolang_params i_params = {0, 0, "", ""};
static recorder_params r_params = {"", 0};
static dumpi_trace_params du_params = {"", 0}; 
static int n = -1;

static struct option long_opts[] = 
{
    {"type", required_argument, NULL, 't'},
    {"num-ranks", required_argument, NULL, 'n'},
    {"d-log", required_argument, NULL, 'l'},
    {"d-aggregator-cnt", required_argument, NULL, 'a'},
    {"i-meta", required_argument, NULL, 'm'},
    {"i-use-relpath", no_argument, NULL, 'p'},
    {"r-trace-dir", required_argument, NULL, 'd'},
    {"r-nprocs", required_argument, NULL, 'x'},
    {"dumpi-log", required_argument, NULL, 'w'},
    {NULL, 0, NULL, 0}
};

void usage(){
    fprintf(stderr,
            "Usage: codes-workload-dump --type TYPE --num-ranks N [OPTION...]"
            "--type: type of workload (\"darshan_io_workload\", \"iolang_workload\", dumpi-trace-workload\" etc.)\n"
            "--num-ranks: number of ranks to process (if not set, it is set by the workload)\n"
            "DARSHAN OPTIONS (darshan_io_workload)\n"
            "--d-log: darshan log file\n"
            "--d-aggregator-cnt: number of aggregators for collective I/O in darshan\n"
            "IOLANG OPTIONS (iolang_workload)\n"
            "--i-meta: i/o language kernel meta file path\n"
            "--i-use-relpath: use i/o kernel path relative meta file path\n"
            "RECORDER OPTIONS (recorder_io_workload)\n"
            "--r-trace-dir: directory containing recorder trace files\n"
            "--r-nprocs: number of ranks in original recorder workload\n"
            "-s: print final workload stats\n"
	    "DUMPI TRACE OPTIONS (dumpi-trace-workload) \n"
	    "--dumpi-log: dumpi log file \n");
}

int main(int argc, char *argv[])
{
    int print_stats = 0;
    double total_delay = 0.0;
    int64_t num_barriers = 0;
    int64_t num_opens = 0;
    int64_t num_closes = 0;
    int64_t num_reads = 0;
    int64_t read_size = 0;
    int64_t num_writes = 0;
    int64_t write_size = 0;
    int64_t num_sends = 0;
    int64_t send_size = 0;
    int64_t num_recvs = 0;
    int64_t recv_size = 0;
    int64_t num_isends = 0;
    int64_t isend_size = 0;
    int64_t num_irecvs = 0;
    int64_t irecv_size = 0;
    int64_t num_bcasts = 0;
    int64_t bcast_size = 0;
    int64_t num_allgathers = 0;
    int64_t allgather_size = 0;
    int64_t num_allgathervs = 0;
    int64_t allgatherv_size = 0;
    int64_t num_alltoalls = 0;
    int64_t alltoall_size = 0;
    int64_t num_alltoallvs = 0;
    int64_t alltoallv_size = 0;
    int64_t num_reduces = 0;
    int64_t reduce_size = 0;
    int64_t num_allreduces = 0;
    int64_t allreduce_size = 0;
    int64_t num_collectives = 0;
    int64_t collective_size = 0;
    int64_t num_waitalls = 0;
    int64_t num_waits = 0;
    int64_t num_waitsomes = 0;
    int64_t num_waitanys = 0;
    int64_t num_testalls = 0;

    char ch;
    while ((ch = getopt_long(argc, argv, "t:n:l:a:m:sp:w", long_opts, NULL)) != -1){
        switch (ch){
            case 't':
                strcpy(type, optarg);
                break;
            case 'n':
                n = atoi(optarg);
                assert(n>0);
                break;
            case 'l':
                strcpy(d_params.log_file_path, optarg);
                break;
            case 'a':
                d_params.aggregator_cnt = atol(optarg);
                break;
            case 'm':
                strcpy(i_params.io_kernel_meta_path, optarg);
                break;
            case 'p':
                i_params.use_relpath = 1;
                break;
            case 'd':
                strcpy(r_params.trace_dir_path, optarg);
                break;
            case 'x':
                r_params.nprocs = atol(optarg);
                break;
	    case 'w':
		strcpy(du_params.file_name, optarg);
		break;
            case 's':
                print_stats = 1;
                break;
        }
    }

    if (type[0] == '\0'){
        fprintf(stderr, "Expected \"--type\" argument\n");
        usage();
        return 1;
    }

    int i;
    char *wparams;
    if (strcmp(type, "darshan_io_workload") == 0){
        if (d_params.log_file_path[0] == '\0'){
            fprintf(stderr, "Expected \"--d-log\" argument for darshan workload\n");
            usage();
            return 1;
        }
        else if (d_params.aggregator_cnt == 0){
            fprintf(stderr, "Expected \"--d-aggregator-cnt\" argument for darshan workload\n");
            usage();
            return 1;
        }
        else{
            wparams = (char*)&d_params;
        }
    }
    else if (strcmp(type, "iolang_workload") == 0){
        if (n == -1){
            fprintf(stderr,
                    "Expected \"--num-ranks\" argument for iolang workload\n");
            usage();
            return 1;
        }
        else{
            i_params.num_cns = n;
        }
        if (i_params.io_kernel_meta_path[0] == '\0'){
            fprintf(stderr,
                    "Expected \"--i-meta\" argument for iolang workload\n");
            usage();
            return 1;
        }

        wparams = (char *)&i_params;
    }
    else if (strcmp(type, "recorder_io_workload") == 0){
        if (r_params.trace_dir_path[0] == '\0'){
            fprintf(stderr, "Expected \"--r-trace-dir\" argument for recorder workload\n");
            usage();
            return 1;
        }
        if (r_params.nprocs == 0){
            fprintf(stderr, "Expected \"--r-nprocs\" argument for recorder workload\n");
            usage();
            return 1;
        }
        else{
            wparams = (char *)&r_params;
        }
    }
   else if(strcmp(type, "dumpi-trace-workload") == 0)
	{
	if(n == -1){
            fprintf(stderr,
                    "Expected \"--num-ranks\" argument for dumpi workload\n");
            usage();
            return 1;		
	}
	else{
	    du_params.num_net_traces = n;	
	}

	if(du_params.file_name[0] == '\0' ){
            fprintf(stderr, "Expected \"--r-trace-dir\" argument for dumpi workload\n");
            usage();
            return 1;
	}
	else
	{
	  wparams = (char*)&du_params;
	}
	}
    else {
        fprintf(stderr, "Invalid type argument\n");
        usage();
        return 1;
    }

    /* if num_ranks not set, pull it from the workload */
    if (n == -1){
        n = codes_workload_get_rank_cnt(type, wparams);
        if (n == -1) {
            fprintf(stderr,
                    "Unable to get rank count from workload. "
                    "Specify option --num-ranks\n");
            return 1;
        }
    }

    for (i = 0 ; i < n; i++){
        struct codes_workload_op op;
        printf("loading %s, %d\n", type, i);
        int id = codes_workload_load(type, wparams, i);
        assert(id != -1);
        do {
            codes_workload_get_next(id, i, &op);
            codes_workload_print_op(stdout, &op, i);

            switch(op.op_type)
            {
                case CODES_WK_DELAY:
                    total_delay += op.u.delay.seconds;
                    break;
                case CODES_WK_BARRIER:
                    num_barriers++;
                    break;
                case CODES_WK_OPEN:
                    num_opens++;
                    break;
                case CODES_WK_CLOSE:
                    num_closes++;
                    break;
                case CODES_WK_WRITE:
                    num_writes++;
                    write_size += op.u.write.size;
                    break;
                case CODES_WK_READ:
                    num_reads++;
                    read_size += op.u.write.size;
                    break;
                case CODES_WK_SEND:
                    num_sends++;
                    send_size += op.u.send.num_bytes;
                    break;
                case CODES_WK_RECV:
                    num_recvs++;
                    recv_size += op.u.recv.num_bytes;
                    break;
                case CODES_WK_ISEND:
                    num_irecvs++;
                    irecv_size += op.u.send.num_bytes;
                    break;
                case CODES_WK_IRECV:
                    num_irecvs++;
                    irecv_size += op.u.recv.num_bytes;
                    break;
                /* NOTE: all collectives are currently represented as the
                 * generic "collective" type */
                case CODES_WK_BCAST:
                    num_bcasts++;
                    bcast_size += op.u.collective.num_bytes;
                    break;
		case CODES_WK_ALLGATHER:
                    num_allgathers++;
                    allgather_size += op.u.collective.num_bytes;
                    break;
                case CODES_WK_ALLGATHERV:
                    num_allgathervs++;
                    allgatherv_size += op.u.collective.num_bytes;
                    break;
                case CODES_WK_ALLTOALL:
                    num_alltoalls++;
                    alltoall_size += op.u.collective.num_bytes;
                    break;
                case CODES_WK_ALLTOALLV:
                    num_alltoallvs++;
                    alltoallv_size += op.u.collective.num_bytes;
                    break;
                case CODES_WK_REDUCE:
                    num_reduces++;
                    reduce_size += op.u.collective.num_bytes;
                    break;
                case CODES_WK_ALLREDUCE:
                    num_allreduces++;
                    allreduce_size += op.u.collective.num_bytes;
                    break;
                case CODES_WK_COL:
                    num_collectives++;
                    collective_size += op.u.collective.num_bytes;
                    break;
                case CODES_WK_WAITALL:
                    num_waitalls++;
                    break;
                case CODES_WK_WAIT:
                    num_waits++;
                    break;
                case CODES_WK_WAITSOME:
                    num_waitsomes++;
                    break;
                case CODES_WK_WAITANY:
                    num_waitanys++;
                    break;
                case CODES_WK_TESTALL:
                    num_testalls++;
                    break;
                case CODES_WK_END:
                    break;
                case CODES_WK_IGNORE:
                    break;
                default:
                    fprintf(stderr,
                            "WARNING: unknown workload op type (code %d)\n",
                            op.op_type);
            }
        } while (op.op_type != CODES_WK_END);
    }

    if (print_stats)
    {
        fprintf(stderr, "\n* * * * * FINAL STATS * * * * * *\n");
        fprintf(stderr, "NUM_OPENS:       %"PRId64"\n", num_opens);
        fprintf(stderr, "NUM_CLOSES:      %"PRId64"\n", num_closes);
        fprintf(stderr, "NUM_BARRIERS:    %"PRId64"\n", num_barriers);
        fprintf(stderr, "TOTAL_DELAY:     %.4lf\n", total_delay);
        fprintf(stderr, "NUM_READS:       %"PRId64"\n", num_reads);
        fprintf(stderr, "READ_SIZE:       %"PRId64"\n", read_size);
        fprintf(stderr, "NUM_WRITES:      %"PRId64"\n", num_writes);
        fprintf(stderr, "WRITE_SIZE:      %"PRId64"\n", write_size);
        fprintf(stderr, "NUM_SENDS:       %"PRId64"\n", num_sends);
        fprintf(stderr, "SEND_SIZE:       %"PRId64"\n", send_size);
        fprintf(stderr, "NUM_RECVS:       %"PRId64"\n", num_recvs);
        fprintf(stderr, "RECV_SIZE:       %"PRId64"\n", recv_size);
        fprintf(stderr, "NUM_ISENDS:      %"PRId64"\n", num_isends);
        fprintf(stderr, "ISEND_SIZE:      %"PRId64"\n", isend_size);
        fprintf(stderr, "NUM_IRECVS:      %"PRId64"\n", num_irecvs);
        fprintf(stderr, "IRECV_SIZE:      %"PRId64"\n", irecv_size);
        fprintf(stderr, "NUM_BCASTS:      %"PRId64"\n", num_bcasts);
        fprintf(stderr, "BCAST_SIZE:      %"PRId64"\n", bcast_size);
        fprintf(stderr, "NUM_ALLGATHERS:  %"PRId64"\n", num_allgathers);
        fprintf(stderr, "ALLGATHER_SIZE:  %"PRId64"\n", allgather_size);
        fprintf(stderr, "NUM_ALLGATHERVS: %"PRId64"\n", num_allgathervs);
        fprintf(stderr, "ALLGATHERV_SIZE: %"PRId64"\n", allgatherv_size);
        fprintf(stderr, "NUM_ALLTOALLS:   %"PRId64"\n", num_alltoalls);
        fprintf(stderr, "ALLTOALL_SIZE:   %"PRId64"\n", alltoall_size);
        fprintf(stderr, "NUM_ALLTOALLVS:  %"PRId64"\n", num_alltoallvs);
        fprintf(stderr, "ALLTOALLV_SIZE:  %"PRId64"\n", alltoallv_size);
        fprintf(stderr, "NUM_REDUCES:     %"PRId64"\n", num_reduces);
        fprintf(stderr, "REDUCE_SIZE:     %"PRId64"\n", reduce_size);
        fprintf(stderr, "NUM_ALLREDUCE:   %"PRId64"\n", num_allreduces);
        fprintf(stderr, "ALLREDUCE_SIZE:  %"PRId64"\n", allreduce_size);
        fprintf(stderr, "NUM_COLLECTIVE:  %"PRId64"\n", num_collectives);
        fprintf(stderr, "COLLECTIVE_SIZE: %"PRId64"\n", collective_size);
        fprintf(stderr, "NUM_WAITALLS:    %"PRId64"\n", num_waitalls);
        fprintf(stderr, "NUM_WAITS:       %"PRId64"\n", num_waits);
        fprintf(stderr, "NUM_WAITSOMES:   %"PRId64"\n", num_waitsomes);
        fprintf(stderr, "NUM_WAITANYS:    %"PRId64"\n", num_waitanys);
        fprintf(stderr, "NUM_TESTALLS:    %"PRId64"\n", num_testalls);
    }

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
