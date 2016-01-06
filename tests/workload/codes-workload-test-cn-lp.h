/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#ifndef CODES_WORKLOAD_TEST_CN_LP_H
#define CODES_WORKLOAD_TEST_CN_LP_H

#include <ross.h>
#include "codes/codes-workload.h"

extern tw_lptype client_lp;
char workload_type[MAX_NAME_LENGTH_WKLD];
struct iolang_params ioparams;

void cn_op_complete(tw_lp *lp, tw_stime svc_time, tw_lpid gid);
void cn_op_complete_rc(tw_lp *lp);
void cn_set_params(int num_clients, int num_servers);

#endif /* CODES_WORKLOAD_TEST_CN_LP_H */

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
