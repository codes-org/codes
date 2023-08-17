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
#include "codes/codes_mapping.h"
#include "codes/lp-type-lookup.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Variable definitions
 */

// When true (below), the network state will be frozen at switch time (from
// high-def to surrogate) and later reanimated on the switch back (from
// surrogate to high-def). If not, all events will be kept in the network while
// on surrogate mode, which means that the network will vacate completely
extern bool freeze_network_on_switch;
void print_surrogate_stats(void);

/**
 * Terminal-to-terminal packet latency prediction machinery
 */

// Packet latencies
struct packet_start {
    uint64_t packet_ID;
    tw_lpid dest_terminal_lpid;  // ROSS id; LPID for terminal
    unsigned int dfdally_dest_terminal_id; // number in [0, total terminals)
    double travel_start_time;
    double workload_injection_time; // this is when the workload passed down the event to model-net
    double processing_packet_delay;  // delay for this packet to be processed from previous packet in the queue
    uint32_t packet_size;
    bool is_there_another_pckt_in_queue; // is there another packet in queue
    void * message_data;  // Yep, we have to save the entire message just because we might need to resend the message when switching to surrogate-mode. It's wasteful but there is no other way
    void * remote_event_data;  // This and the one above have to be freed. This contains the extra information that the message contains
};

struct packet_end {
    double travel_end_time;
    double next_packet_delay;  // Delay to start processing next packet
};

// Definition of functions needed to define a predictor
typedef void (*init_pred_f) (void * predictor_data, tw_lp * lp, unsigned int terminal_id); // Initializes the predictor (eg, LSTM)
typedef void (*feed_pred_f) (void * predictor_data, tw_lp * lp, unsigned int terminal_id, struct packet_start const *, struct packet_end const *); // Feeds known latency for packet sent at `now`
typedef struct packet_end (*predict_pred_f) (void * predictor_data, tw_lp * lp, unsigned int terminal_id, struct packet_start const *); // Get prediction for packet sent to `destination` at `now`
typedef void (*predict_pred_rc_f) (void * predictor_data, tw_lp * lp); // Reverse prediction (reverse state of predictor one prediction)

// Each network model defines its own way to setup the packet latency predictor
struct packet_latency_predictor {
    init_pred_f        init;
    feed_pred_f        feed;
    predict_pred_f     predict;
    predict_pred_rc_f  predict_rc;
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
    switch_surrogate_f  switch_surrogate; // this function switches the model to and from surrogate-mode on a PE basis. It has to be called on all PEs to switch the entire simulation to its surrogate version
    is_surrogate_on_f   is_surrogate_on;  // determines if the model has switched or not
};


/**
 * Configuration specifics
 */

// Switches back and forth from surrogate mode as defined by network model
// (e.g, by dragonfly-dally.C)
// Parameters: `data` corresponds to the lp sub-state, lp is the lp pointer, and the array of events in queue (to be processed)
typedef void (*model_switch_f) (void * data, tw_lp * lp, tw_event **);
typedef bool (*model_ask_if_freeze_f) (tw_lp * lp, tw_event * event); // Determines whether the event should be "frozen" or should be allowed to run during surrogate-mode

struct lp_types_switch {
    char lpname[MAX_NAME_LENGTH];
    bool trigger_idle_modelnet;  // Trigger idle events for model-net (prevents a model to be stuck in a schedule loop if it is to process packets during surrogate-mode). If this is true and the lpname does not start with 'modelnet_', the behaviour is undefined
    model_switch_f        highdef_to_surrogate;
    model_switch_f        surrogate_to_highdef;
    model_ask_if_freeze_f should_event_be_frozen;  // NULL means event from LP type shouldn't be frozen
};

struct surrogate_config {
    struct director_data director;  //!< functionality needed by the director to switch back and forth from model-level surrogate-mode to (vanilla) high-definition simulation
    int total_terminals;  //!< total number of terminals
    size_t n_lp_types;
    struct lp_types_switch lp_types[MAX_LP_TYPES];
};

/** Loads surrogate configuration, including packet latency predictor. */
void surrogate_configure(
        char const * const annotation,
        struct surrogate_config * const config,
        struct packet_latency_predictor ** pl_pred //!< pointer to save packet latency predictor generated by. Caller must free it
);

#ifdef __cplusplus
}
#endif

#endif /* end of include guard */
