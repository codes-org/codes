/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

/* Example of a workload generator that plugs into the codes workload
 * generator API.  This example just produces a hard-coded pattern for
 * testing/validation purposes.
 */

#include "ross.h"
#include "codes/codes-workload.h"
#include "codes-workload-method.h"

int test_workload_load(const char* params, int rank);
void test_workload_get_next(int rank, struct codes_workload_op *op);

struct codes_workload_method test_workload_method = 
{
    .method_name = "test",
    .codes_workload_load = test_workload_load,
    .codes_workload_get_next = test_workload_get_next,
};

int test_workload_load(const char* params, int rank)
{
    /* no params in this case; this example will work with any number of
     * ranks
     */
    return(0);
}

void test_workload_get_next(int rank, struct codes_workload_op *op)
{
    /* TODO: fill in a more complex example... */

    op->op_type = CODES_WK_END;
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
