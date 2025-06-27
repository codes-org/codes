#ifndef CODES_SURROGATE_INIT_H
#define CODES_SURROGATE_INIT_H

/**
 * init.h -- Config/initialization point
 * Elkin Cruz
 *
 * Copyright (c) 2023 Rensselaer Polytechnic Institute
 */
#include "codes/surrogate/packet-latency-predictor/common.h"
#include "codes/surrogate/app-iteration-predictor/common.h"
#include "codes/surrogate/network-surrogate.h"

// Basic level of debugging is 1. It should be always turned on
// because it tells us when a switch to or from surrogate-mode happened.
// It can be deactivated (set to 0) if it ends up being too obnoxious
// Level 0: don't show anything
// Level 1: show when surrogate-mode is activated and deactivated
// Level 2: level 1 and some information at each GVT
// Level 3: level 1 and show extended information at each GVT
#define DEBUG_DIRECTOR 1

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Variable definitions
 */

// Time spent switching from high-fidelity to surrogate and viceversa
extern double surrogate_switching_time;
// Total time spent in surrogate mode (between switches)
extern double time_in_surrogate;
// Time at which we transitioned into surrogate (zero means that we are in high-fidelity)
extern double surrogate_time_last;

void print_surrogate_stats(void);

/** Loads surrogate configuration, including packet latency predictor. */
bool network_surrogate_configure(
        char const * const annotation,
        struct network_surrogate_config * const config,
        struct packet_latency_predictor ** pl_pred //!< pointer to save packet latency predictor. Caller does not need to free pointer
);

void application_surrogate_configure(
    int num_terminals_on_pe,
    int num_apps,
    struct app_iteration_predictor ** iter_pred //!< pointer to save application iteration predictor. No need to free pointer
);
void surrogates_finalize(void);

#ifdef __cplusplus
}
#endif

#endif /* end of include guard */
