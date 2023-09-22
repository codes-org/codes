#ifndef CODES_SURROGATE_LATENCY_PREDICTOR_AVERAGE_H
#define CODES_SURROGATE_LATENCY_PREDICTOR_AVERAGE_H

/**
 * average.h -- implements a strategy to determine how long will it take for a
 * packet to arrive at its destination based on averaging the time that takes
 * to send packets from source to destination terminals
 * -Elkin Cruz
 *
 * Copyright (c) 2023 Rensselaer Polytechnic Institute
 */

#include "codes/surrogate/packet-latency-predictor/common.h"

#ifdef __cplusplus
extern "C" {
#endif

extern struct packet_latency_predictor average_latency_predictor;
extern double ignore_until;

#ifdef __cplusplus
}
#endif

#endif /* end of include guard */
