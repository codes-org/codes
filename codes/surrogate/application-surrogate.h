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

// Main function responsible for switching between high-fidelity and (application iteration) surrogate
void application_director_configure(int every_n_gvt, struct app_iteration_predictor *);

#ifdef __cplusplus
}
#endif

#endif /* end of include guard */
