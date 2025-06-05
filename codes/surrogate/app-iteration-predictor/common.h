#ifndef CODES_SURROGATE_ITERATION_PREDICTOR_COMMON_H
#define CODES_SURROGATE_ITERATION_PREDICTOR_COMMON_H

/**
 * common.h -- common datatypes and functionality to all application iteration predictors
 * -Elkin Cruz
 *
 * Copyright (c) 2025 Rensselaer Polytechnic Institute
 */
#include <ross.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Iteration application prediction machinery. Notice that any of these predictors have to know how many iterations to run in total, thus they need data about the number of steps the application will take.
 */

struct app_iter_node_config {
    int app_id;
    int app_ending_iter;
};

// This returns how much to skip ahead and when to restart
struct iteration_pred {
    int resume_at_iter;
    double restart_at;
};

enum FAST_FORWARD {
    FAST_FORWARD_switching = 0,
    FAST_FORWARD_restart, // Stop accumulating data (we gain nothing from switching to surrogate-mode) and restart at future point in time
};
struct fast_forward_values {
    enum FAST_FORWARD status;  // Are we switching to surrogate-mode
    // Only needed for "switching" and "restart"
    double restarting_at;      // Time at which we will have fully restarted (or expect to)
};


// Model calls to predictor
typedef void (*init_pred_iter_f) (tw_lp * lp, int nw_id_in_pe, struct app_iter_node_config *); // Initializes the predictor (eg, average)
typedef void (*feed_pred_iter_f) (tw_lp * lp, int nw_id_in_pe, int iteration_id, double iteration_time); // Feeds last iteration time
typedef void (*end_pred_iter_f) (tw_lp * lp, int nw_id_in_pe, double time); // Tells the predictor that the application has stopped running
typedef struct iteration_pred (*predict_pred_iter_f) (tw_lp * lp, int nw_id_in_pe); // Get prediction
typedef void (*predict_pred_iter_rc_f) (tw_lp * lp, int nw_id_in_pe); // Reverse prediction (reverse state of predictor one prediction)
// Director calls to predictor module
typedef bool (*have_we_hit_switch_f) (tw_lp * lp, int nw_id_in_pe, int iteration_id); // Are we ready to switch to a future iterationº
typedef bool (*is_predictor_read_f) (void); // Checking if it is a good time to switch (enough data has been collected or we have received some notification of an application ending, forcing us to restart collecting data). This might trigger an MPI_Allreduce call, thus has to be called by all PEs!
typedef void (*reset_pred_iter_f) (void); // Resets the predictor (eg, average)
typedef struct fast_forward_values (*prepare_fast_forward_f) (void); // Checking if it is a good time to switch (enough data has been collected)

// API that predictors have to comply with and 
struct app_iteration_predictor {
    struct {
        init_pred_iter_f        init;
        feed_pred_iter_f        feed;
        end_pred_iter_f         ended;
        predict_pred_iter_f     predict;
        predict_pred_iter_rc_f  predict_rc;
        have_we_hit_switch_f    have_we_hit_switch;
    } model;
    struct {
        reset_pred_iter_f       reset;
        is_predictor_read_f     is_predictor_ready;
        prepare_fast_forward_f  prepare_fast_forward_jump;
    } director;
};

#ifdef __cplusplus
}
#endif

#endif /* end of include guard */
