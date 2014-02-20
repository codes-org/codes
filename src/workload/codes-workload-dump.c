/*
 * Copyright (C) 2014 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#include <assert.h>
#include <getopt.h>
#include <stdio.h>
#include "codes/codes-workload.h"

static char type[128] = {'\0'};
static darshan_params d_params = {NULL, "", 0}; 
static int n = -1;

static struct option long_opts[] = 
{
    {"type", required_argument, NULL, 't'},
    {"num-ranks", required_argument, NULL, 'n'},
    {"d-log", optional_argument, NULL, 'l'},
    {"d-aggregator-cnt", optional_argument, NULL, 'a'},
    {NULL, 0, NULL, 0}
};


int main(int argc, char *argv[])
{
    char ch;
    while ((ch = getopt_long(argc, argv, "t:l:a:", long_opts, NULL)) != -1){
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
        }
    }

    int i;
    char *wparams;
    if (strcmp(type, "darshan_io_workload") == 0){
        wparams = (char*)&d_params;
    }
    else {
        wparams = NULL;
    }
    for (i = 0 ; i < n; i++){
        struct codes_workload_op op;
        printf("loading %s, %d\n", type, i);
        int id = codes_workload_load(type, wparams, i);
        assert(id != -1);
        do {
            codes_workload_get_next(id, i, &op);
            codes_workload_print_op(stdout, &op, i);
        } while (op.op_type != CODES_WK_END);
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
