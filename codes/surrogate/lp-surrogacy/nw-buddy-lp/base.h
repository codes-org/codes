#ifndef CODES_SURROGATE_LP_NW_BUDDY_H
#define CODES_SURROGATE_LP_NW_BUDDY_H

/**
 * nw-buddy-lp/base.h -- Implements a simple client LP that is in charge of
 * estimating time for application
 * -Elkin Cruz
 *
 * Copyright (c) 2024 Rensselaer Polytechnic Institute
 */

#include <ross.h>
#ifdef __cplusplus
extern "C" {
#endif

struct tw_lptype const * nw_buddy_get_lp_type(void);
void nw_buddy_surrogate_register_lp_type(void);


struct nw_buddy_msg {
    // TODO(helq): origin workload id's are not needed, can be removed or can be used to assert correctness of simulation
    long src_wkld_id;  // Worload that originated message
    tw_lpid src_wkld_lpid;
    int iteration; // non-negative number (comes from app/workload)
    double iteration_time; // time stamp at which this iteration was completed
};


#ifdef __cplusplus
}
#endif

#endif /* end of include guard */
