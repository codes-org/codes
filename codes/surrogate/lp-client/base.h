#ifndef CODES_SURROGATE_LP_CLIENT_H
#define CODES_SURROGATE_LP_CLIENT_H

/**
 * lp-client/base.h -- Implements a simple client LP that is in charge of
 * estimating time for application
 * -Elkin Cruz
 *
 * Copyright (c) 2024 Rensselaer Polytechnic Institute
 */

#include <ross.h>
#ifdef __cplusplus
extern "C" {
#endif

struct tw_lptype const * client_surrogate_get_lp_type(void);
void client_surrogate_register_lp_type(void);


// enum CLIENT_SURR {
//     CLIENT_SURR_iter_time,
//     CLIENT_SURR_skip
// };


struct client_surr_msg {
    long src_wkld_id;  // Worload that originated message
    tw_lpid src_wkld_lpid;
    // enum CLIENT_SURR msg_type;
    // union {
        // CLIENT_SURR_iter_time
        // struct {
            int iteration;
            float iteration_time;
        // };
        // CLIENT_SURR_skip
        // struct {
        //     int iters_to_skip;  // Iterations to skip
        // };
    // };

    // For rollback
    // TODO(elkin): replace with CODES stack rc so that each client can rollback whatever amount of info they want
    double arrival_time; // To be filled by event handler
};


#ifdef __cplusplus
}
#endif

#endif /* end of include guard */
