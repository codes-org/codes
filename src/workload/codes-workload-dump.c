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
static darshan_params d_params = {NULL, "", 0}; 
static bgp_params b_params = {0, "", "", "", ""};
static int n = -1;

static struct option long_opts[] = 
{
    {"type", required_argument, NULL, 't'},
    {"num-ranks", required_argument, NULL, 'n'},
    {"d-log", required_argument, NULL, 'l'},
    {"d-aggregator-cnt", required_argument, NULL, 'a'},
    {"i-meta", required_argument, NULL, 'm'},
    {"i-bgp-config", required_argument, NULL, 'b'},
    {"i-rank-cnt", required_argument, NULL, 'r'},
    {NULL, 0, NULL, 0}
};

void usage(){
    fprintf(stderr,
            "Usage: codes-workload-dump --type TYPE --num-ranks N "
            "[--d-log LOG --d-aggregator-cnt CNT]\n"
            "--type: type of workload (\"darshan_io_workload\" or \"bgp_io_workload\")\n"
            "--num-ranks: number of ranks to process\n"
            "--d-log: darshan log file\n"
            "--d-aggregator-cnt: number of aggregators for collective I/O in darshan\n"
            "--i-meta: i/o language kernel meta file path\n"
            "--i-bgp-config: i/o language bgp config file\n"
            "--i-rank-cnt: i/o language rank count\n"
            "-s: print final workload stats\n");
}

int main(int argc, char *argv[])
{
    int print_stats = 0;
    double total_delay = 0.0;
    int64_t num_barriers = 0;
    int64_t num_opens = 0;
    int64_t num_reads = 0;
    int64_t read_size = 0;
    int64_t num_writes = 0;
    int64_t write_size = 0;

    char ch;
    while ((ch = getopt_long(argc, argv, "t:n:l:a:m:b:r:s", long_opts, NULL)) != -1){
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
                strcpy(b_params.io_kernel_meta_path, optarg);
                break;
            case 'b':
                strcpy(b_params.bgp_config_file, optarg);
                break;
            case 'r':
                b_params.num_cns = atoi(optarg);
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
    if (n == -1){
        fprintf(stderr, "Expected \"--num-ranks\" argument\n");
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
    else if (strcmp(type, "bgp_io_workload") == 0){
        if (b_params.num_cns == 0){
            fprintf(stderr, "Expected \"--i-rank-cnt\" argument for bgp io workload\n");
            usage();
            return 1;
        }
        else if (b_params.io_kernel_meta_path[0] == '\0'){
            fprintf(stderr, "Expected \"--i-meta\" argument for bgp io workload\n");
            usage();
            return 1;
        }
        else if (b_params.bgp_config_file[0] == '\0'){
            fprintf(stderr, "Expected \"--i-bgp-conf\" argument for bgp io workload\n");
            usage();
            return 1;
        }
        else{
            wparams = (char *)&b_params;
        }
    }
    else {
        fprintf(stderr, "Invalid type argument\n");
        usage();
        return 1;
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
                case CODES_WK_OPEN:
                    num_opens++;
                    break;
                case CODES_WK_BARRIER:
                    num_barriers++;
                    break;
                case CODES_WK_DELAY:
                    total_delay += op.u.delay.seconds;
                    break;
                case CODES_WK_READ:
                    num_reads++;
                    read_size += op.u.write.size;
                    break;
                case CODES_WK_WRITE:
                    num_writes++;
                    write_size += op.u.write.size;
                    break;
                default:
                    break;
            }
        } while (op.op_type != CODES_WK_END);
    }

    if (print_stats)
    {
        fprintf(stderr, "\n* * * * * FINAL STATS * * * * * *\n");
        fprintf(stderr, "NUM_OPENS:\t%"PRId64"\n", num_opens);
        fprintf(stderr, "NUM_BARRIERS:\t%"PRId64"\n", num_barriers);
        fprintf(stderr, "TOTAL_DELAY:\t%.4lf\n", total_delay);
        fprintf(stderr, "NUM_READS:\t%"PRId64"\n", num_reads);
        fprintf(stderr, "READ_SIZE:\t%"PRId64"\n", read_size);
        fprintf(stderr, "NUM_WRITES:\t%"PRId64"\n", num_writes);
        fprintf(stderr, "WRITE_SIZE:\t%"PRId64"\n", write_size);
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
