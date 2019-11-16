#ifndef CONGESTION_CONTROLLER_H
#define CONGESTION_CONTROLLER_H

/**
 * congestion-controller.h -- Organizing state and behavior for congestion management
 * Neil McGlohon
 * 
 * Copyright (c) 2019 Rensselaer Polytechnic Institute
 */

#include <map>
#include <vector>
#include <set>
#include "codes/codes.h"
#include "codes/model-net.h"

using namespace std;

/**
 * @class SupervisoryController
 * 
 * @brief This class organizes the behavior and necessary state of a supervisory controller LP.
 * 
 */
class SupervisoryController {
    map< unsigned int, vector< unsigned long > > router_port_stall_counter_map;
    map< unsigned int, unsigned long > > nic_stall_coutner_map;
    map< unsigned int, int > node_to_job_map;

    set< int > suspect_job_set;
    map< int, bool> guilty_job_map;

    tw_stime t_period;
    bool is_network_congested;
    bool is_abatement_active;


public:
    SupervisoryController();

    void start_new_epoch();

    void congestion_control_detect();

    bool check_nic_congestion_criterion();

    bool check_port_congestion_criterion();

    void collect_performance_information();

    void congestion_control_abatement();

    bool check_abatement_criterion();

    void congestion_control_causation();

    void identify_suspect_jobs();

    void check_for_guilty_jobs();
}

/**
 * @class RouterLocalController
 * 
 * @brief This class organizes state that is ON a router LP that this controller monitors.
 * No non-pointer-to-LP-state properties that change throughout the simulation should be considered
 * RC safe. These should be considered invalid upon a context change and are thus named as
 * private values with a preceeding underscore. This is simply because this state doesn't 
 * generally need to be accessed except immediately after being set and this is a way to simplify
 * router RC.
 * 
 * 
 */
class RouterLocalController {
    unsigned long* *port_stall_counter; //pointer to 2d array of unsigned longs representing "stalled_chunks", this is a pointer to avoid RC hassle

    map < unsigned int, bool > _nic_stall_map; //temporary storage for results of generate_nic_stall_count(); 

public:
    void stall_detector();

    void generate_nic_stall_count();

    bool check_nic_stall_criterion();
}

class NodeLocalController {
    unsigned long nic_stall_counter;
    unsigned long ejection_counter;

public:
    void check_nic_stall_criterion();

    void congestion_control_abatement();

    void send_ejection_count();
}


#endif /* end of include guard */