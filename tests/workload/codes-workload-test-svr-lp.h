/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#ifndef CODES_WORKLOAD_TEST_SERVER_H
#define CODES_WORKLOAD_TEST_SERVER_H

#include <ross.h>
#include "codes/codes-workload.h"

extern tw_lptype svr_lp;

void svr_op_start(tw_lp *lp, tw_lpid gid, const struct codes_workload_op *op);
void svr_op_start_rc(tw_lp *lp);

#endif /* CODES_WORKLOAD_TEST_SERVER_H */

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
