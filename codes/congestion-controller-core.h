#ifndef CONGESTION_CONTROLLER_CORE_H
#define CONGESTION_CONTROLLER_CORE_H

/**
 * congestion-controller.h -- Organizing state and behavior for congestion management
 * Neil McGlohon
 * 
 * Copyright (c) 2019 Rensselaer Polytechnic Institute
 */
#include <ross.h>
#include <codes/codes-jobmap.h>
#define MAX_PATTERN_LEN 32
#define MAX_PORT_COUNT 256

#ifdef __cplusplus
extern "C" {
#endif

extern int g_congestion_control_enabled; 
extern tw_stime g_congestion_control_notif_latency;
extern const tw_optdef cc_app_opt [];

// Defines congestion (aggregate of stall)
typedef enum congestion_status
{
    UNCONGESTED = 0,
    CONGESTED = 1
} congestion_status;

typedef enum controller_type
{
    CC_ROUTER = 1,
    CC_TERMINAL = 2
} controller_type;

/* Enumeration of types of events sent between congestion controllers */
typedef enum cc_event_t
{
    CC_SIGNAL_NORMAL = 1001,
    CC_SIGNAL_ABATE,
    CC_BANDWIDTH_CHECK,
    CC_SIM_ACK // A 'simulated' ack message sent from receiving terminal to original to let it know that its packet was ejected
} cc_event_t;

typedef struct congestion_control_message
{
    short type; //type of event
    tw_lpid sender_lpid; //lpid of the sender
    int app_id;

    // Reverse computation values
    double saved_window;
    double saved_rate;
    double saved_bw;
    double saved_new_bw; //for commit
    tw_stime msg_time; // for commit
    unsigned int saved_ejected_bytes;
    int num_cc_rngs;
    short to_congest;
    short to_decongest;

    short received_new_while_congested;
    int saved_term_id;
    double saved_expire_time;

    // Dangerous - same LP dynamic RC state -- if this message is to be sent between two LPs, DON'T USE THIS FIELD
    size_t size_abated;
    size_t size_deabated;
    unsigned int* danger_rc_abated;
    unsigned int* danger_rc_deabated;
} congestion_control_message;

extern void congestion_control_register_terminal_lpname(char lp_name[]);
extern void congestion_control_register_router_lpname(char lp_name[]);

extern int congestion_control_set_jobmap(struct codes_jobmap_ctx *jobmap_ctx, int net_id);
extern int congestion_control_is_jobmap_set();
extern int congestion_control_get_job_count();
extern struct codes_jobmap_ctx* congestion_control_get_jobmap();
extern void congestion_control_notify_rank_completion(tw_lp *lp);
extern void congestion_control_notify_rank_completion_rc(tw_lp *lp);
extern void congestion_control_notify_job_completion(tw_lp *lp, int app_id);
extern void congestion_control_notify_job_completion_rc(tw_lp *lp, int app_id);

#ifdef __cplusplus
}
#endif

#endif /* end of include guard */