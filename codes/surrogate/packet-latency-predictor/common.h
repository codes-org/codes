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
    tw_lpid dest_terminal_lpid;            // ROSS id; LPID for terminal
    unsigned int dfdally_dest_terminal_id; // number in [0, total terminals)
    double travel_start_time;
    double workload_injection_time; // this is when the workload passed down the event to model-net
    double
        processing_packet_delay; // delay for this packet to be processed from previous packet in the queue
    uint32_t packet_size;
    bool is_there_another_pckt_in_queue; // is there another packet in queue
    uint64_t caller_lp_gid;              // true ROSS LP gid of the source terminal LP

    /*
     * Optional ML-facing context for LP-aware Torch-JIT mode.
     * Existing predictors may ignore these fields.
     */
    uint32_t src_router_id;
    uint32_t src_group_id;
    uint32_t dst_router_id;
    uint32_t dst_group_id;
    uint32_t terminal_queue_length;
    uint32_t terminal_vc_occupancy;
};

struct packet_end {
    double travel_end_time;
    double next_packet_delay; // Delay to start processing next packet
};

// Definition of functions needed to define a predictor
typedef void (*init_pred_lat_f)(void* predictor_data, tw_lp* lp,
                                unsigned int terminal_id); // Initializes the predictor (eg, LSTM)
typedef void (*reset_pred_lat_f)(void* predictor_data, tw_lp* lp);
typedef void (*feed_pred_lat_f)(
    void* predictor_data, tw_lp* lp, unsigned int terminal_id, struct packet_start const*,
    struct packet_end const*); // Feeds known latency for packet sent at `now`
typedef struct packet_end (*predict_pred_lat_f)(
    void* predictor_data, tw_lp* lp, unsigned int terminal_id,
    struct packet_start const*); // Get prediction for packet sent to `destination` at `now`
typedef void (*predict_pred_lat_rc_f)(
    void* predictor_data,
    tw_lp* lp); // Reverse prediction (reverse state of predictor one prediction)

// API for packet latency predictors
struct packet_latency_predictor {
    init_pred_lat_f init;
    reset_pred_lat_f reset;
    feed_pred_lat_f feed;
    predict_pred_lat_f predict;
    predict_pred_lat_rc_f predict_rc;
    size_t predictor_data_sz; // `predictor_data` size
};

#ifdef __cplusplus
}
#endif

#endif /* end of include guard */
