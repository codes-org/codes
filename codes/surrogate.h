#ifndef CODES_SURROGATE_H
#define CODES_SURROGATE_H

/**
 * surrogate.h -- Defining all functions to implement in order to run CODES in surrogate mode
 * Elkin Cruz
 *
 * Copyright (c) 2023 Rensselaer Polytechnic Institute
 */
#include <ross.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Terminal-to-terminal packet latency prediction machinery
 */

// Packet latencies
struct packet_start {
    uint64_t packet_ID;
    // tw_lpid dest_terminal_id;  // ROSS id; LPID for terminal
    unsigned int dfdally_dest_terminal_id; // number in [0, total terminals)
    double travel_start_time;
};

struct packet_end {
    uint64_t packet_ID;
    double travel_end_time;
};

// Definition of functions needed to define a predictor
typedef void (*init_pred_f) (void * predictor_data, tw_lp * lp, unsigned int terminal_id); // Initializes the predictor (eg, LSTM)
typedef void (*feed_pred_f) (void * predictor_data, tw_lp * lp, unsigned int terminal_id, struct packet_start, struct packet_end); // Feeds known latency for packet sent at `now`
typedef double (*predict_pred_f) (void * predictor_data, tw_lp * lp, unsigned int terminal_id, struct packet_start); // Get prediction for packet sent to `destination` at `now`
typedef void (*predict_rc_pred_f) (void * predictor_data, tw_lp * lp); // Reverse prediction (reverse state of predictor one prediction)

// Each network model defines its own way to setup the packet latency predictor
struct packet_latency_predictor {
    init_pred_f        init;
    feed_pred_f        feed;
    predict_pred_f     predict;
    predict_rc_pred_f  predict_rc;
    size_t             predictor_data_sz; // `predictor_data` size
};

/**
 * Director machinery.
 * The director is in charge of switching back and forth from
 * surrogate mode to "high-def simulation"/vanilla mode
 */

// Functions that director should have access to
typedef void (*switch_surrogate_f) (void); // Switches back and forth from surrogate mode as defined by network model (e.g, by dragonfly-dally.C)
typedef bool (*is_surrogate_on_f) (void); // Switches back and forth from surrogate mode as defined by network model (e.g, by dragonfly-dally.C)

struct director_data {
    switch_surrogate_f  switch_surrogate;
    is_surrogate_on_f   is_surrogate_on;
};

typedef void (*director_init_f) (struct director_data self);
typedef void (*director_f) (void); // This is the function that is to be called at each GVT computation

#ifdef __cplusplus
}
#endif

#endif /* end of include guard */
