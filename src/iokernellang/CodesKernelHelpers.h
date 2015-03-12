/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#ifndef CODE_KERNEL_HELPERS_H
#define CODE_KERNEL_HELPERS_H

#include <stdint.h>
#include <stdlib.h>
#include <sys/stat.h>

#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 500
#endif
#include <unistd.h>


#include "CodesIOKernelTypes.h"
#include "CodesIOKernelParser.h"
#include "codeslexer.h"
#include "codes/codes-workload.h"

#define CL_INST_MAX_ARGS 10

enum cl_event_t
{
    CL_GETSIZE=1,
    CL_GETRANK,
    CL_WRITEAT,
    CL_READAT,
    CL_OPEN,
    CL_CLOSE,
    CL_SYNC,
    CL_EXIT,
    CL_NOOP,
    CL_DELETE,
    CL_SLEEP,

    /* last item should be an unknown inst */
    CL_UNKNOWN
};

typedef struct app_cf_info
{
    int gid;
    int min;
    int max;
    int lrank;
    int num_lrank;
} app_cf_info_t;

typedef struct codeslang_inst
{
  int event_type;
  int64_t num_var;
  int64_t var[CL_INST_MAX_ARGS];
} codeslang_inst;

int codes_kernel_helper_parse_input(CodesIOKernel_pstate * ps,
        CodesIOKernelContext * c, codeslang_inst * inst);

int codes_kernel_helper_bootstrap(char * io_kernel_path,
        char * io_kernel_meta_path, int rank, int num_ranks, int use_relpath, CodesIOKernelContext * c,
        CodesIOKernel_pstate ** ps, codes_workload_info * task_info,
        codeslang_inst * next_event);

char * code_kernel_helpers_cleventToStr(int inst);
char * code_kernel_helpers_kinstToStr(int inst);
#endif

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
