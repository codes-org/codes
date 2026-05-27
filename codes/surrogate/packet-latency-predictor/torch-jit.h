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
void surrogate_torch_init(char const * dir);

extern struct packet_latency_predictor torch_latency_predictor;

#ifdef __cplusplus
}
#endif

#endif /* end of include guard */
