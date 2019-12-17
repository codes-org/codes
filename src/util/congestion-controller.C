#include "codes/congestion-controller.h"
#include <codes/model-net-lp.h>
#include <codes/codes_mapping.h>
#include <map>
#include <vector>
#include <set>
#include <unordered_set>
#include <string.h>
#include <string>

using namespace std;


/************* DEFINITIONS ****************************************/
int g_congestion_control_enabled;
tw_lpid g_cc_supervisory_controller_gid;

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
    string router_lp_name;
    string terminal_lp_name;
    string workload_lp_name;

    //NIC ALPHA criterion values
    double node_congestion_percent_threshold;

    //PORT ALPHA criterion values
    double port_congestion_percent_threshold;

    //set of criterion to judge congestion by
    set< nic_congestion_criterion > *nic_congestion_criterion_set;
    set< port_congestion_criterion > *port_congestion_criterion_set;

    string pattern_set_filepath;
}cc_param;

typedef struct sc_state
{
    cc_param *params;

    map< unsigned int, unsigned int > *router_port_stallcount_map; //maps router ID to a vector of its ports indicating their reported congestion
    map< unsigned int, congestion_status > *node_stall_map; // maps nic ID to whether it reports congestion
    map< unsigned int, int > *node_to_job_map; //TODO: This should consider multiple jobs per node as well
    map< unsigned long long, congestion_status > *node_period_congestion_map; // maps an epoch to a status of whether the nic congestion threshold was met //TODO make into a sliding window as optimization by culling stale data
    map< unsigned long long, congestion_status > *port_period_congestion_map; //maps an epoch to a status of whether the port congestion threshold was 
    unsigned long long num_completed_workload_ranks;

    int received_router_performance_count; //number of CC_R_PERF_RESPONSE messages received following a request
    int received_terminal_performance_count; //number of CC_N_PERF_RESPONSE messages received following a request

    set< int > suspect_job_set;
    map< int, bool> guilty_job_map;

    unsigned long long current_epoch;
    bool is_network_congested;
    bool is_abatement_active;
    bool is_all_workloads_complete;
}sc_state;

//sc lptype function declarations
void cc_supervisor_init(sc_state *s, tw_lp *lp);
void cc_supervisor_event(sc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp);
void cc_supervisor_event_rc(sc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp);
void cc_supervisor_finalize(sc_state *s, tw_lp *lp);

tw_lptype cc_supervisor_lp = {
    (init_f) cc_supervisor_init,
    (pre_run_f) NULL,
    (event_f) cc_supervisor_event,
    (revent_f) cc_supervisor_event_rc,
    (commit_f) NULL,
    (final_f)  cc_supervisor_finalize,
    (map_f) codes_mapping,
    sizeof(sc_state),
};

/************* GLOBALS ********************************************/
static map< tw_lpid, int > router_lpid_to_id_map = map<tw_lpid, int>();
static map< tw_lpid, int > terminal_lpid_to_id_map = map<tw_lpid, int>();
static map< int, tw_lpid > router_id_to_lpid_map = map<int, tw_lpid>();
static map< int, tw_lpid > terminal_id_to_lpid_map = map<int, tw_lpid>();
static unordered_set<unsigned long> pattern_set = unordered_set<unsigned long>();

/************* PROTOTYPES *****************************************/

void cc_supervisor_process_heartbeat(sc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp);
void cc_supervisor_process_heartbeat_rc(sc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp);
void cc_supervisor_send_heartbeat(sc_state *s, tw_lp *lp);
void cc_supervisor_receive_wl_completion(sc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp);
void cc_supervisor_receive_wl_completion_rc(sc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp);


void cc_supervisor_start_new_epoch(sc_state *s); //implemented
void cc_supervisor_start_new_epoch_rc(sc_state *s); //implemented
void cc_supervisor_congestion_control_detect(sc_state *s); //implemented
bool cc_supervisor_check_nic_congestion_criterion(sc_state *s); //implemented
bool cc_supervisor_check_port_congestion_criterion(sc_state *s); //implemented
bool sc_check_nic_congestion_patterns(sc_state *s); //implemented
bool cc_supervisor_check_port_congestion_patterns(sc_state *s); //implemented
void cc_supervisor_request_performance_information(sc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp);
void cc_supervisor_process_performance_response(sc_state *s);
void sc_congestion_control_abatement();
bool sc_check_abatement_criterion();
void sc_congestion_control_causation();
void sc_identify_suspect_jobs();
void sc_check_for_guilty_jobs();
void sc_load_pattern_set(sc_state *s);


/************* LP Definition **************************************/

const tw_lptype* sc_get_lp_type()
{
        return(&cc_supervisor_lp);
}

void congestion_control_register_lp_type()
{
    lp_type_register("supervisory_controller", sc_get_lp_type());
}

/************* CONGESTION CONTROLLER IMPLEMENTATIONS **************/

void cc_load_configuration(sc_state *s)
{
    s->params = (cc_param*)calloc(1, sizeof(cc_param));
    cc_param *p = s->params;

    p->congestion_enabled = g_congestion_control_enabled;

    if (!p->congestion_enabled)
        tw_error(TW_LOC, "Congestion Control: Supervisory controller attempted init but congestion management wasn't enabled\n");

    char router_name[MAX_NAME_LENGTH];
    router_name[0] = '\0';
    bool is_router_controller_specified = true;
    int rc = configuration_get_value(&config, "PARAMS", "cc_router_lp_name", NULL, router_name, MAX_NAME_LENGTH);
    if (rc == 0) {
        is_router_controller_specified = false;
    }
    else {
        p->router_lp_name = router_name;
    }

    char terminal_name[MAX_NAME_LENGTH];
    terminal_name[0] = '\0';
    bool is_terminal_controller_specified = true;
    rc = configuration_get_value(&config, "PARAMS", "cc_terminal_lp_name", NULL, terminal_name, MAX_NAME_LENGTH);
    if (rc == 0) {
        is_terminal_controller_specified = false;
    }
    else {
        p->terminal_lp_name = terminal_name;
    }
    if (p->congestion_enabled && (!(is_router_controller_specified || is_terminal_controller_specified)))
        tw_error(TW_LOC, "Congestion was enabled but neither router nor terminal LP names specified. (cc_router_lp_name and/or cc_terminal_lp_name)");

    char wl_name[MAX_NAME_LENGTH];
    wl_name[0] = '\0';
    configuration_get_value(&config, "PARAMS", "ccworkloadpname", NULL, wl_name, MAX_NAME_LENGTH);
    if (strlen(wl_name) <= 0) {
        printf("Congestion Control: Assuming default workload LP name of: 'nw-lp'\n");
        p->workload_lp_name = "nw-lp";
        strcpy(wl_name, "nw-lp");
    }
    else {
        p->workload_lp_name = wl_name;
    }

    p->total_routers = codes_mapping_get_lp_count(NULL, 0, router_name, NULL, 0);
    p->total_terminals = codes_mapping_get_lp_count(NULL, 0, terminal_name, NULL, 0);
    p->total_workload_ranks = codes_mapping_get_lp_count(NULL, 0, wl_name, NULL,0);

    int radix;
    rc = configuration_get_value_int(&config, "PARAMS", "cc_radix", NULL, &radix);
    if (rc) {
        tw_error(TW_LOC,"Congestion Control: Congestion management enabled but no 'cc_radix' configuration value specified.");
    }
    p->router_radix = radix;
    p->total_ports = p->total_routers * p->router_radix;

    tw_stime period;
    rc = configuration_get_value_double(&config, "PARAMS", "cc_measurement_period", NULL, &period);
    if (rc) {
        printf("Congestion Control: Measurment period not specified, using default 50ns\n");
        period = 50.0;
    }
    p->measurement_period = period;

    p->nic_congestion_criterion_set = new set<nic_congestion_criterion>();
    p->port_congestion_criterion_set = new set<port_congestion_criterion>();


    p->nic_congestion_criterion_set->insert(NIC_ALPHA); //TODO add configurability to this
    p->port_congestion_criterion_set->insert(PORT_ALPHA); //TODO add configurability to this

    p->node_congestion_percent_threshold = 75.0; //TODO add configurability to this
    p->port_congestion_percent_threshold = 75.0;
    char pattern_path[512];
    configuration_get_value(&config, "PARAMS", "cc_pattern_set_filepath", NULL, pattern_path, 512);
    if (strlen(pattern_path) <= 0) {
        tw_error(TW_LOC, "Congestion Control: No pattern set filepath specified. Congestion control requires");
    }
    p->pattern_set_filepath = pattern_path;
        // pattern_set_filepath = "/home/neil/RossDev/codes/scripts/congestion-control/ex_pattern.txt"; //TODO configurability
}

//Supervisory Controller
void cc_supervisor_init(sc_state *s, tw_lp *lp)
{
    cc_load_configuration(s);
    cc_param *p = s->params;

    s->router_port_stallcount_map = new map<unsigned int, unsigned int>();
    s->node_stall_map = new map<unsigned int, congestion_status>();
    s->node_to_job_map = new map<unsigned int, int>();
    s->node_period_congestion_map = new map<unsigned long long, congestion_status>();
    s->port_period_congestion_map = new map<unsigned long long, congestion_status>();
    s->num_completed_workload_ranks = 0;

    for(int i = 0; i < p->total_routers; i++)
    {
        (*s->router_port_stallcount_map)[i] = 0;
    }

    for(int i = 0; i < p->total_terminals; i++)
    {
        (*s->node_stall_map)[i] = UNCONGESTED;
    }

    //these are global static maps, and we would ordinarily have something to make sure that it
    //is only set once per PE, but since there's only one SC in the entire sim, I'm just going
    //to ignore that. If more than one SC exists in the simulation (per PE), this will need to be addressed.
    for(int router_rel_id = 0; router_rel_id < p->total_routers; router_rel_id++)
    {
        tw_lpid router_lpid;
        router_lpid = codes_mapping_get_lpid_from_relative(router_rel_id, NULL, p->router_lp_name.c_str(), NULL, 0);
        router_lpid_to_id_map[router_lpid] = router_rel_id;
        router_id_to_lpid_map[router_rel_id] = router_lpid;
    }

    for(int terminal_rel_id = 0; terminal_rel_id < p->total_terminals; terminal_rel_id++)
    {
        tw_lpid terminal_lpid;
        terminal_lpid = codes_mapping_get_lpid_from_relative(terminal_rel_id, NULL, p->terminal_lp_name.c_str(), NULL, 0);
        terminal_lpid_to_id_map[terminal_lpid] = terminal_rel_id;
        terminal_id_to_lpid_map[terminal_rel_id] = terminal_lpid;
    }

    s->current_epoch = 0;
    s->is_network_congested = false;
    s->is_abatement_active = false;
    s->is_all_workloads_complete = false;
    sc_load_pattern_set(s);
    cc_supervisor_send_heartbeat(s, lp);
}

void cc_supervisor_event(sc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
{
    // printf("SC: event!\n");
    switch (msg->type)
    {
        case CC_SC_HEARTBEAT:
            cc_supervisor_process_heartbeat(s, bf, msg, lp);
        break;
        case CC_R_PERF_RESPONSE:
            printf("SC: perf response received: ROUTER\n");
        break;
        case CC_N_PERF_RESPONSE:
            printf("SC: perf response received: TERMINAL\n");
        break;
        case CC_WORKLOAD_RANK_COMPLETE:
            cc_supervisor_receive_wl_completion(s, bf, msg, lp);
        break;
        default:
            tw_error(TW_LOC,"SC Received invalid event\n");
    }
}

void cc_supervisor_event_rc(sc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
{
    // printf("SC: event!\n");
    switch (msg->type)
    {
        case CC_SC_HEARTBEAT:
            cc_supervisor_process_heartbeat_rc(s, bf, msg, lp);
        break;
        case CC_R_PERF_RESPONSE:
            printf("SC: perf response RC: ROUTER\n");
        break;
        case CC_N_PERF_RESPONSE:
            printf("SC: perf response RC: TERMINAL\n");
        break;
        case CC_WORKLOAD_RANK_COMPLETE:
            cc_supervisor_receive_wl_completion_rc(s, bf, msg, lp);
        break;
        default:
            tw_error(TW_LOC,"SC Received invalid event for RC %d\n", msg->type);
    }
}

void cc_supervisor_finalize(sc_state *s, tw_lp *lp)
{
    printf("SC: Finalize\n");
    printf("Num Epochs: %d", s->current_epoch);
}

void cc_supervisor_process_heartbeat(sc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
{
        cc_supervisor_request_performance_information(s, bf, msg, lp);
        cc_supervisor_send_heartbeat(s, lp);
        cc_supervisor_start_new_epoch(s);
}

void cc_supervisor_process_heartbeat_rc(sc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
{
        
        cc_supervisor_start_new_epoch_rc(s);
}

void cc_supervisor_send_heartbeat(sc_state *s, tw_lp *lp)
{
    if (s->is_all_workloads_complete == false)
    {
        tw_stime next_heartbeat_time = tw_now(lp) + s->params->measurement_period;

        tw_event *e;
        congestion_control_message *h_msg;
        e = tw_event_new(lp->gid, s->params->measurement_period, lp);
        h_msg = (congestion_control_message*)tw_event_data(e);
        h_msg->type = CC_SC_HEARTBEAT;
        h_msg->sender_lpid = lp->gid;
        // printf("SC: Sending Heartbeat to self: Now=%lf  TS=%lf\n",tw_now(lp), next_heartbeat_time);
        tw_event_send(e);
    }
}

void cc_supervisor_receive_wl_completion(sc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
{
    s->num_completed_workload_ranks++;
    printf("Number of Completed Ranks: %d/%d\n",s->num_completed_workload_ranks, s->params->total_workload_ranks);
    if (s->num_completed_workload_ranks == s->params->total_workload_ranks)
        s->is_all_workloads_complete = true;
}

void cc_supervisor_receive_wl_completion_rc(sc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
{
    s->num_completed_workload_ranks--;
    if (s->num_completed_workload_ranks < s->params->total_workload_ranks)
        s->is_all_workloads_complete = false;
}

void cc_supervisor_start_new_epoch(sc_state *s)
{
    s->current_epoch++;
}

void cc_supervisor_start_new_epoch_rc(sc_state *s)
{
    s->current_epoch--;
}

void cc_supervisor_congestion_control_detect(sc_state *s)
{
    bool is_congested = false;

    is_congested += cc_supervisor_check_nic_congestion_criterion(s);
    is_congested += cc_supervisor_check_port_congestion_criterion(s);

    if (is_congested) {
        s->is_network_congested = true;
    } else {
        s->is_network_congested = false;
    }
}

bool cc_supervisor_check_nic_congestion_criterion(sc_state *s)
{
    set<nic_congestion_criterion>::iterator it = s->params->nic_congestion_criterion_set->begin();
    for(; it != s->params->nic_congestion_criterion_set->end(); it++)
    {
        nic_congestion_criterion criterion = *it;
        switch (criterion)
        {
            case NIC_ALPHA: //if a percentage of nics are congested, then nics are considered congested for this period
            {    
                unsigned int num_stalled_nics = 0;
                map<unsigned int, congestion_status>::iterator it2 = s->node_stall_map->begin();
                for(; it2 != s->node_stall_map->end(); it2++)
                {
                    if (it2->second == CONGESTED)
                        num_stalled_nics++;
                }
                double percent_stalled = (double) num_stalled_nics / s->params->total_terminals;

                if (percent_stalled >= s->params->node_congestion_percent_threshold)
                    (*s->node_period_congestion_map)[s->current_epoch] = CONGESTED;
                break;
            }
            default:
                tw_error(TW_LOC,"Invalid NIC Congestion Criterion %d", criterion);
            break;
        }
    }

    bool is_congested = sc_check_nic_congestion_patterns(s); //TODO do something with this
}

bool cc_supervisor_check_port_congestion_criterion(sc_state *s)
{
    set<port_congestion_criterion>::iterator it = s->params->port_congestion_criterion_set->begin();
    for(; it != s->params->port_congestion_criterion_set->end(); it++)
    {
        port_congestion_criterion criterion = *it;
        switch (criterion)
        {
            case PORT_ALPHA: //if a percentage of ports are congested, then the ports are considerd congested for this period
            {    
                unsigned int num_stalled_ports = 0;
                map<unsigned int, unsigned int>::iterator it2 = s->router_port_stallcount_map->begin();
                //loop over all routers
                for(; it2 != s->router_port_stallcount_map->end(); it2++)
                {
                    num_stalled_ports += it2->second;
                    // //sum over all ports for this router
                    // vector<congestion_status>::iterator it2 = it->second.begin();
                    // for(; it2 != it->second.end(); it2++)
                    // {
                    //     if (it2->second == CONGESTED)
                    //         num_stalled_ports++;
                    // }
                }
                double percent_stalled = (double) num_stalled_ports / s->params->total_ports;

                if (percent_stalled >= s->params->port_congestion_percent_threshold)
                    (*s->port_period_congestion_map)[s->current_epoch] = CONGESTED;
                break;
            }
            default:
                tw_error(TW_LOC,"Invalid Port Congestion Criterion %d", criterion);
        }
    }

    bool is_congested = cc_supervisor_check_port_congestion_patterns(s); //TODO do something with this
}

bool sc_check_nic_congestion_patterns(sc_state *s)
{
    //get last congestion statuses for last MAX_PATTERN_LEN
    char cur_pattern[MAX_PATTERN_LEN];
    int back_epoch_i = 0;
    for(int i = MAX_PATTERN_LEN-1; i > 0; i--)
    {
        cur_pattern[i] = (*s->node_period_congestion_map)[s->current_epoch - back_epoch_i] + '0'; //the + '0' is a hack to convert the 0/1 stored in the map into its char representation
        back_epoch_i += 1;
    }

    int cur_pattern_long = strtol(cur_pattern, NULL, 2); //converting the pattern of binary numbers into unsigned long

    if (pattern_set.count(cur_pattern_long) == 1)
        return true;
    else
        return false;
}

bool cc_supervisor_check_port_congestion_patterns(sc_state *s)
{
    //get last congestion statuses for last MAX_PATTERN_LEN
    char cur_pattern[MAX_PATTERN_LEN];
    int back_epoch_i = 0;
    for(int i = MAX_PATTERN_LEN-1; i > 0; i--)
    {
        cur_pattern[i] = (*s->port_period_congestion_map)[s->current_epoch - back_epoch_i] + '0'; //the + '0' is a hack to convert the 0/1 stored in the map into its char representation
        back_epoch_i += 1;
    }

    int cur_pattern_long = strtol(cur_pattern, NULL, 2); //converting the pattern of binary numbers into unsigned long

    if (pattern_set.count(cur_pattern_long) == 1)
        return true;
    else
        return false;
}

void cc_supervisor_request_performance_information(sc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
{
    //Send requests to Router and Node local controllers for performance information
    map<tw_lpid, int>::iterator it = router_lpid_to_id_map.begin();
    for(; it != router_lpid_to_id_map.end(); it++)
    {
        tw_stime ts_noise = g_tw_lookahead + tw_rand_unif(lp->rng) * .0001;

        congestion_control_message *m;
        tw_event *e = model_net_method_congestion_request_event(it->first, ts_noise, lp, (void**)&m, NULL);

        // e = tw_event_new(it->first, ts_noise, lp); //ROSS method to create a new event
        // m = (congestion_control_message*)tw_event_data(e); //Gives you a pointer to the data encoded within event e
        m->type = CC_SC_PERF_REQUEST; //Set the event type so we can know how to classify the event when received
        tw_event_send(e); //ROSS method to send off the event e with the encoded data in m
        printf("SC: Sent performance request to Router %d at %f\n",it->second, tw_now(lp)+ts_noise);
    }

    it = terminal_lpid_to_id_map.begin();
    for(; it != terminal_lpid_to_id_map.end(); it++)
    {
        tw_stime ts_noise = g_tw_lookahead + tw_rand_unif(lp->rng) * .0001;

        congestion_control_message *m;
        tw_event *e = model_net_method_congestion_request_event(it->first, ts_noise, lp, (void**)&m, NULL);

        // e = tw_event_new(it->first, ts_noise, lp); //ROSS method to create a new event
        // m = (congestion_control_message*)tw_event_data(e); //Gives you a pointer to the data encoded within event e
        m->type = CC_SC_PERF_REQUEST; //Set the event type so we can know how to classify the event when received
        tw_event_send(e); //ROSS method to send off the event e with the encoded data in m
        printf("SC: Sent performance request to Terminal %d at %f\n",it->second, tw_now(lp)+ts_noise);
    }
}

void sc_load_pattern_set(sc_state *s)
{
    //attempting to be more portable than GNU's getline()
    char filepath[s->params->pattern_set_filepath.length()+1];
    strcpy(filepath, s->params->pattern_set_filepath.c_str());

    FILE *file = fopen(filepath, "r");
    if (file != NULL)
    {
        char pattern[MAX_PATTERN_LEN];
        while (fgets(pattern, MAX_PATTERN_LEN, file) != NULL)
        {
            unsigned long pattern_long = strtol(pattern, NULL, 2); //convert pattern from string representation of binary to unsigned long
            pattern_set.insert(pattern_long); //insert the unsigned long conversion into the pattern set for quick hashing
        }
        fclose(file);
    }
    else
    {
        tw_error(TW_LOC, "Congestion Controller: Failed to open pattern set file %s\n", s->params->pattern_set_filepath); /* why didn't the file open? */
    }
}    


//Router Local Controller









//Node Local Controller


/************* CRITERION IMPLEMENTATIONS **************/
