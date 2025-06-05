#ifndef CODES_SURROGATE_ITERATION_PREDICTOR_AVERAGE_H
#define CODES_SURROGATE_ITERATION_PREDICTOR_AVERAGE_H

/**
 * This predictor collects the time that it takes to complete an iteration, and
 * uses this information as the prediction. The trigger becomes 
 */

#include "surrogate/app-iteration-predictor/common.h"

struct avg_app_config {
    int num_apps;
    int num_nodes_in_pe;
    int num_of_iters_to_feed;
};

struct app_iteration_predictor avg_app_iteration_predictor(struct avg_app_config *);

void free_avg_app_iteration_predictor(void);

#endif /* end of include guard */
