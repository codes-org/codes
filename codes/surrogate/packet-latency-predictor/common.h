#ifndef CODES_SURROGATE_LATENCY_PREDICTOR_COMMON_H
#define CODES_SURROGATE_LATENCY_PREDICTOR_COMMON_H

/**
 * common.h -- common datatypes and functionality to all latency predictors
 * -Elkin Cruz
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

#ifdef __cplusplus
}
#endif

#endif /* end of include guard */
