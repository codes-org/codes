#ifndef CODES_SURROGATE_APP_SURROGATE_H
#define CODES_SURROGATE_APP_SURROGATE_H

/**
 * switch.h -- DIRECTOR FUNCTION in charge of switching back and forth from high-fidelity and surrogate modes for the application level
 * Elkin Cruz
 *
 * Copyright (c) 2025 Rensselaer Polytechnic Institute
 */

#include <ross.h>
#include "surrogate/app-iteration-predictor/common.h"

#ifdef __cplusplus
extern "C" {
#endif

enum APP_DIRECTOR_OPTS {
    APP_DIRECTOR_OPTS_every_n_gvt = 0, // Call director every `n` GVTs
    APP_DIRECTOR_OPTS_call_every_ns,   // Call director every X (virtual) nanoseconds
};

struct application_director_config {
    enum APP_DIRECTOR_OPTS option;
    union {
        // To use when APP_DIRECTOR_OPTS_every_n_gvt
        int every_n_gvt;
        // To use when APP_DIRECTOR_OPTS_call_every_ns
        double call_every_ns;
    };
    bool use_network_surrogate;
};

// Main function responsible for switching between high-fidelity and (application iteration) surrogate
void application_director_configure(struct application_director_config *, struct app_iteration_predictor *);

#ifdef __cplusplus
}
#endif

#endif /* end of include guard */
