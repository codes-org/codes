#ifndef CODES_SURROGATE_TORCHJIT_H
#define CODES_SURROGATE_TORCHJIT_H

#include <ross.h>
#include <stdbool.h>
#include "codes/surrogate/init.h"

#ifdef __cplusplus
extern "C" {
#endif

void surrogate_torch_set_lp_aware_mode(bool enabled);
void surrogate_torch_set_debug_prints(bool enabled);
void surrogate_torch_init(char const* dir);

struct router_timing_prediction_start {
    float router_id;
    float group_id;
    float output_port;
    float output_chan;
    float to_terminal;
    float is_global;
    float packet_size;
    float chunk_size;
    float output_vc_occupancy;
    float output_queued_count;
    float next_output_available_delta;
    float nominal_router_delay;
};

void surrogate_torch_init_lp_type_models(char const* terminal_model_path,
                                         char const* router_timing_model_path,
                                         char const* default_model_path);

bool surrogate_torch_router_timing_model_enabled(void);

double
surrogate_torch_predict_router_queueing_delay(struct router_timing_prediction_start const* start,
                                              double fallback_queueing_delay);

extern struct packet_latency_predictor torch_latency_predictor;

#ifdef __cplusplus
}
#endif

#endif /* end of include guard */
