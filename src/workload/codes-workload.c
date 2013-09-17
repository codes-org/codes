/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */


#include "ross.h"
#include "codes/codes-workload.h"
#include "codes-workload-method.h"

int codes_workload_load(const char* type, const char* params, int rank)
{
    return(-1);
}

void codes_workload_get_next(int wkld_id, struct codes_workload_op *op)
{
    return;
}

void codes_workload_get_next_rc(int wkld_id, const struct codes_workload_op *op)
{
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
