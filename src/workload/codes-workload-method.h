/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

/* I/O workload generator API to be used by workload generators.  It mimics
 * the top level codes-workload.h API, except that there is no reverse
 * handler.
 */

#ifndef CODES_WORKLOAD_METHOD_H
#define CODES_WORKLOAD_METHOD_H

#include "ross.h"
#include "codes/codes-workload.h"

int codes_workload_load(const char* type, const char* params, int rank);

void codes_workload_get_next(int wkld_id, struct codes_workload_op *op);

#endif /* CODES_WORKLOAD_METHOD_H */

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
