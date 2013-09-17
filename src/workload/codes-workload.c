/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#include <assert.h>

#include "ross.h"
#include "codes/codes-workload.h"
#include "codes-workload-method.h"

/* list of available methods.  These are statically compiled for now, but we
 * could make generators optional via autoconf tests etc. if needed 
 */ 
extern struct codes_workload_method test_workload_method;
static struct codes_workload_method *method_array[] = 
    {&test_workload_method, NULL};

int codes_workload_load(const char* type, const char* params, int rank)
{
    int i;
    int ret;

    for(i=0; method_array[i] != NULL; i++)
    {
        if(strcmp(method_array[i]->method_name, type) == 0)
        {
            ret = method_array[i]->codes_workload_load(params, rank);
            if(ret < 0)
            {
                return(-1);
            }
            return(i);
        }
    }

    fprintf(stderr, "Error: failed to find workload generator %s\n", type);
    return(-1);
}

void codes_workload_get_next(int wkld_id, int rank, struct codes_workload_op *op)
{
    method_array[wkld_id]->codes_workload_get_next(rank, op);

    return;
}

void codes_workload_get_next_rc(int wkld_id, int rank, const struct codes_workload_op *op)
{
    /* TODO: fill this in */

    assert(0);
    return;
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
