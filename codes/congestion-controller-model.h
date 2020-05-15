#ifndef CONGESTION_CONTROLLER_MODEL_H
#define CONGESTION_CONTROLLER_MODEL_H

/**
 * congestion-controller-model.h -- Organizing state and behavior for congestion management
 * this header file differs from congesiton-controller-core.h in that this one has c++ structures
 * in so that c++ is used for network models that use it but not CODES core.
 * Neil McGlohon
 * 
 * Copyright (c) 2019 Rensselaer Polytechnic Institute
 */
#include <ross.h>
#include <codes/codes_mapping.h>
#include <codes/congestion-controller-core.h>
#include <map>
#include <set>
#include <vector>
#include <unordered_set>
#include <string.h>
#include <string>

using namespace std;

typedef struct cc_param
{
    // configuration parameters
    int total_routers;
    int total_terminals;
    unsigned long long total_workload_ranks;
    int router_radix;
    unsigned int total_ports;
    tw_stime measurement_period;
    int congestion_enabled;
    char router_lp_name[MAX_NAME_LENGTH];
    char terminal_lp_name[MAX_NAME_LENGTH];
    char workload_lp_name[MAX_NAME_LENGTH];
    int loaded_pattern_length;

    //NIC ALPHA criterion values
    double node_congestion_percent_threshold;

    //PORT ALPHA criterion values
    double port_congestion_percent_threshold;

    //set of criterion to judge congestion by
    set< nic_congestion_criterion > *nic_congestion_criterion_set;
    set< port_congestion_criterion > *port_congestion_criterion_set;
}cc_param;

//specific to local controller parameters
typedef struct cc_local_param
{
    int router_radix;

    //NIC STALL ALPHA values
    double node_stall_to_pass_ratio_threshold; //if meet or exceed ratio, then the node is STALLED

    //PORT STALL ALPHA values
    double port_stall_to_pass_ratio_threshold; //if meet or exceed ratio, then the port is STALLED

    set< nic_stall_criterion > *nic_stall_criterion_set;
    set< port_stall_criterion > *port_stall_criterion_set;
}cc_local_param;

typedef struct sc_state
{
    cc_param *params;

    map< unsigned int, unsigned int > *router_port_stallcount_map; //maps router ID to a vector of its ports indicating their reported congestion
    map< unsigned int, short > *node_stall_map; // maps nic ID to whether it reports congestion
    // map< unsigned int, int > *node_to_job_map; //TODO: This should consider multiple jobs per node as well
    map< unsigned long long, congestion_status > *node_period_congestion_map; // maps an epoch to a status of whether the nic congestion threshold was met //TODO make into a sliding window as optimization by culling stale data (can be done via pruning during a commit_f on a heartbeat event)
    map< unsigned long long, congestion_status > *port_period_congestion_map; //maps an epoch to a status of whether the port congestion threshold was 
    unsigned long long num_completed_workload_ranks;

    int received_router_performance_count; //number of CC_R_PERF_REPORT messages received following a request
    int received_terminal_performance_count; //number of CC_N_PERF_REPORT messages received following a request

    set< int > suspect_job_set;
    map< int, bool> guilty_job_map;

    unsigned long long current_epoch;
    unsigned long long congested_epochs;
    bool is_network_congested;
    bool is_abatement_active;
    bool is_all_workloads_complete;
}sc_state;


typedef struct rlc_state
{
    cc_local_param *local_params;

    unsigned long long current_epoch;

    // maps an epoch to a count of stalled ports on the router
    // TODO: add pruning functionality for a commit_f function
    map< unsigned long long, int > *port_period_stall_map;

    //PORT STALL ALPHA ------

    // pointer to array of unsigned longs
    // representing "stalled_chunks" on the router. ptr makes RC easier
    unsigned long *stalled_chunks_ptr;

    // pointer to array of unsigned longs
    // representing "total_chunks" on the router
    unsigned long *total_chunks_ptr;
    
    // array of unsigned longs representing the number of 
    // stalled chunks on each port the last time the measurement
    // period was incremented. This is so that we can see how many
    // stalled chunks were observed during THIS epoch.
    unsigned long *stalled_chunks_at_last_epoch;

    unsigned long *total_chunks_at_last_epoch;

} rlc_state;

typedef struct tlc_state
{
    cc_local_param *local_params;

    unsigned long long current_epoch;

        // maps an epoch to whether the nic was stalled
    // TODO: add pruning functionality for a commit_f function
    map<unsigned long long, stall_status > *nic_period_stall_map;

    // pointer to counter of stalled chunks on the terminal
    unsigned long* stalled_chunks_ptr;

    // pointer to counter of total chunks processed on the terminal
    // representing "total_chunks" on the router
    unsigned long *total_chunks_ptr;

    // counter of stalled chunks at the last turn of the epoch
    unsigned long stalled_chunks_at_last_epoch;

    unsigned long total_chunks_at_last_epoch;

} tlc_state;



// ----------- Supervisory Controller --------------
extern void cc_supervisor_process_heartbeat(sc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp);
extern void cc_supervisor_process_heartbeat_rc(sc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp);
extern void cc_supervisor_send_heartbeat(sc_state *s, tw_bf *bf, tw_lp *lp);
extern void cc_supervisor_send_heartbeat_rc(sc_state *s, tw_bf *bf, tw_lp *lp);
extern void cc_supervisor_receive_wl_completion(sc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp);
extern void cc_supervisor_receive_wl_completion_rc(sc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp);
extern void cc_supervisor_broadcast_wl_completion(sc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp);
extern void cc_supervisor_broadcast_wl_completion_rc(sc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp);
extern void cc_supervisor_start_new_epoch(sc_state *s); //implemented
extern void cc_supervisor_start_new_epoch_rc(sc_state *s); //implemented
extern void cc_supervisor_congestion_control_detect(sc_state *s, tw_bf *bf, tw_lp *lp); //implemented
extern void cc_supervisor_congestion_control_detect_rc(sc_state *s, tw_bf *bf, tw_lp *lp); //implemented
extern void cc_supervisor_check_nic_congestion_criterion(sc_state *s, tw_bf *bf); //implemented
extern void cc_supervisor_check_port_congestion_criterion(sc_state *s, tw_bf *bf); //implemented
extern void cc_supervisor_check_nic_congestion_criterion_rc(sc_state *s, tw_bf *bf); //implemented
extern void cc_supervisor_check_port_congestion_criterion_rc(sc_state *s, tw_bf *bf); //implemented
extern bool cc_supervisor_check_congestion_patterns(sc_state *s, controller_type type, congestion_change check_for_status);
extern void cc_supervisor_request_performance_information(sc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp);
extern void cc_supervisor_request_performance_information_rc(sc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp);
extern void cc_supervisor_process_performance_response(sc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp);
extern void cc_supervisor_process_performance_response_rc(sc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp);
extern void sc_congestion_control_abatement();
extern bool sc_check_abatement_criterion();
extern void sc_congestion_control_causation();
extern void sc_identify_suspect_jobs();
extern void sc_check_for_guilty_jobs();
extern void cc_supervisor_load_pattern_set(sc_state *s);


// ------------ Local controllers -----------------------
extern void cc_router_local_controller_init(rlc_state *s);

extern void cc_router_local_send_heartbeat(rlc_state *s, tw_bf *bf, tw_lp *lp);
extern void cc_router_local_send_heartbeat_rc(rlc_state *s, tw_bf *bf, tw_lp *lp);
extern void cc_router_local_process_heartbeat(rlc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp);
extern void cc_router_local_process_heartbeat_rc(rlc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp);

extern void cc_router_local_congestion_event(rlc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp);
extern void cc_router_local_congestion_event_rc(rlc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp);

extern void cc_router_local_controller_setup_stall_alpha(rlc_state *s, int radix, unsigned long *stalled_chunks_ptr, unsigned long *total_chunks_ptr);

extern int cc_router_local_get_port_stall_count(rlc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp);
extern void cc_router_local_get_port_stall_count_rc(rlc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp);
extern void cc_router_local_send_performance(rlc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp);
void cc_router_local_send_performance_rc(rlc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp);
extern void cc_router_local_new_epoch(rlc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp);
extern void cc_router_local_new_epoch_rc(rlc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp);
extern void cc_router_local_new_epoch_commit(rlc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp); 

extern void cc_terminal_local_controller_init(tlc_state *s);

extern void cc_terminal_local_send_heartbeat(tlc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp);
extern void cc_terminal_local_send_heartbeat_rc(tlc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp);
extern void cc_terminal_local_process_heartbeat(tlc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp);
extern void cc_terminal_local_process_heartbeat_rc(tlc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp);

extern void cc_terminal_local_congestion_event(tlc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp);
extern void cc_terminal_local_congestion_event_rc(tlc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp);

extern void cc_terminal_local_get_nic_stall_count(tlc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp);
extern void cc_terminal_local_send_performance(tlc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp);


/************* LP Definition **************************************/




#endif /* end of include guard */