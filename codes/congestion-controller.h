#ifndef CONGESTION_CONTROLLER_H
#define CONGESTION_CONTROLLER_H

/**
 * congestion-controller.h -- Organizing state and behavior for congestion management
 * Neil McGlohon
 * 
 * Copyright (c) 2019 Rensselaer Polytechnic Institute
 */
#include <ross.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_PATTERN_LEN 16
#define MAX_PORT_COUNT 256

typedef enum congestion_status
{
    UNCONGESTED = 0,
    CONGESTED = 1
} congestion_status;

/* Enumeration of types of events sent between congestion controllers */
typedef enum cc_event_t
{
    CC_SC_HEARTBEAT = 1001,
    CC_SC_PERF_REQUEST,
    CC_R_PERF_RESPONSE,
    CC_N_PERF_RESPONSE,
    CC_WORKLOAD_RANK_COMPLETE,
} cc_event_t;

typedef enum nic_congestion_criterion
{
    NIC_ALPHA = 1
} nic_congestion_criterion; 

typedef enum port_congestion_criterion
{
    PORT_ALPHA = 1
} port_congestion_criterion;

typedef struct congestion_control_message
{
    short type; //type of event
    tw_lpid sender_lpid; //lpid of the sender
    unsigned int stalled_port_count; //used by both routers and terminals, if router then is is the number of port stalled, if terminal nonzero implies congestion
    unsigned long long current_epoch; //the measurement period that these numbers apply to
    unsigned long long rc_value; //rc value storage - dependent on context
} congestion_control_message;

const tw_lptype* sc_get_lp_type();
void congestion_control_register_lp_type();


#ifdef __cplusplus
}
#endif

#endif /* end of include guard */