#include <codes/congestion-controller-core.h>
#include <codes/congestion-controller-model.h>
#include <codes/model-net-lp.h>
#include <codes/codes_mapping.h>
#include <map>
#include <vector>
#include <set>
#include <unordered_set>
#include <string.h>
#include <string>

using namespace std;


unsigned long long stalled_packet_counter;

/************* DEFINITIONS ****************************************/
int g_congestion_control_enabled;
tw_lpid g_cc_supervisory_controller_gid;


//sc lptype function declarations
extern void cc_supervisor_init(sc_state *s, tw_lp *lp);
extern void cc_supervisor_event(sc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp);
extern void cc_supervisor_event_rc(sc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp);
extern void cc_supervisor_finalize(sc_state *s, tw_lp *lp);

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

const tw_lptype* sc_get_lp_type()
{
        return(&cc_supervisor_lp);
}

void congestion_control_register_lp_type()
{
    lp_type_register("supervisory_controller", sc_get_lp_type());
}


/************* GLOBALS ********************************************/
static map< tw_lpid, int > router_lpid_to_id_map = map<tw_lpid, int>();
static map< tw_lpid, int > terminal_lpid_to_id_map = map<tw_lpid, int>();
static map< int, tw_lpid > router_id_to_lpid_map = map<int, tw_lpid>();
static map< int, tw_lpid > terminal_id_to_lpid_map = map<int, tw_lpid>();

static string congestion_pattern_set_filepath;
static unordered_set<unsigned long> congestion_pattern_set = unordered_set<unsigned long>();

static string decongestion_pattern_set_filepath;
static unordered_set<unsigned long> decongestion_pattern_set = unordered_set<unsigned long>();

/************* PROTOTYPES *****************************************/







/************* HELPER FUNCTIONS ***********************************/
tw_stime cc_get_start_of_next_measurement_period_from_given(tw_stime period_length, tw_stime given_time)
{
    tw_stime start_cur = (given_time - fmod(given_time,period_length));
    return start_cur + period_length;
}

// tw_stime cc_get_next_heartbeat_time

double cc_tw_rand_unif(tw_lp *lp)
{
    //based on what was defined in codes_mapping.c, congestion control uses second to last RNG on the LP
    assert(g_tw_nRNG_per_lp > 2); //0 for model, 1 for CC, 2 for codes local latency
    int cc_rng_id = g_tw_nRNG_per_lp - 2;
    return tw_rand_unif(&lp->rng[cc_rng_id]);
}

double cc_tw_rand_reverse_unif(tw_lp *lp)
{
    //based on what was defined in codes_mapping.c, congestion control uses second to last RNG on the LP
    assert(g_tw_nRNG_per_lp > 2); //0 for model, 1 for CC, 2 for codes local latency
    int cc_rng_id = g_tw_nRNG_per_lp - 2;
    return tw_rand_reverse_unif(&lp->rng[cc_rng_id]);
}

/************* CONGESTION CONTROLLER IMPLEMENTATIONS **************/

void cc_load_configuration(sc_state *s)
{
    s->params = (cc_param*)calloc(1,sizeof(cc_param));
    cc_param *p = s->params;

    p->congestion_enabled = g_congestion_control_enabled;

    if (!p->congestion_enabled)
        tw_error(TW_LOC, "Congestion Control: Supervisory controller attempted init but congestion management wasn't enabled\n");

    p->router_lp_name[0] = '\0';
    bool is_router_controller_specified = true;
    int rc = configuration_get_value(&config, "PARAMS", "cc_router_lp_name", NULL, p->router_lp_name, MAX_NAME_LENGTH);
    if (rc == 0) {
        is_router_controller_specified = false;
    }

    p->terminal_lp_name[0] = '\0';
    bool is_terminal_controller_specified = true;
    rc = configuration_get_value(&config, "PARAMS", "cc_terminal_lp_name", NULL, p->terminal_lp_name, MAX_NAME_LENGTH);
    if (rc == 0) {
        is_terminal_controller_specified = false;
    }

    if (p->congestion_enabled && (!(is_router_controller_specified || is_terminal_controller_specified)))
        tw_error(TW_LOC, "Congestion was enabled but neither router nor terminal LP names specified. (cc_router_lp_name and/or cc_terminal_lp_name)");

    p->workload_lp_name[0] = '\0';
    configuration_get_value(&config, "PARAMS", "ccworkloadpname", NULL, p->workload_lp_name, MAX_NAME_LENGTH);
    if (strlen(p->workload_lp_name) <= 0) {
        printf("Congestion Control: Assuming default workload LP name of: 'nw-lp'\n");
        strcpy(p->workload_lp_name, "nw-lp");
    }

    p->total_routers = codes_mapping_get_lp_count(NULL, 0, p->router_lp_name, NULL, 0);
    p->total_terminals = codes_mapping_get_lp_count(NULL, 0, p->terminal_lp_name, NULL, 0);
    p->total_workload_ranks = codes_mapping_get_lp_count(NULL, 0, p->workload_lp_name, NULL,0);

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


    p->nic_congestion_criterion_set->insert(NIC_CONGESTION_ALPHA); //TODO add configurability to this
    p->port_congestion_criterion_set->insert(PORT_CONGESTION_ALPHA); //TODO add configurability to this

    p->node_congestion_percent_threshold = 10.0; //TODO add configurability to this
    p->port_congestion_percent_threshold = 10.0;
    
    char pattern_path[512];
    configuration_get_value(&config, "PARAMS", "cc_congestion_pattern_set_filepath", NULL, pattern_path, 512);
    if (strlen(pattern_path) <= 0) {
        tw_error(TW_LOC, "Congestion Control: No congestion pattern set filepath specified. Congestion control requires");
    }
    
    congestion_pattern_set_filepath = pattern_path;

    char pattern_path2[512];
    configuration_get_value(&config, "PARAMS", "cc_decongestion_pattern_set_filepath", NULL, pattern_path2, 512);
    if (strlen(pattern_path) <= 0) {
        tw_error(TW_LOC, "Congestion Control: No decongestion pattern set filepath specified. Congestion control requires");
    }
    
    decongestion_pattern_set_filepath = pattern_path2;
}

//Supervisory Controller
void cc_supervisor_init(sc_state *s, tw_lp *lp)
{
    cc_load_configuration(s);
    cc_param *p = s->params;

    s->router_port_stallcount_map = new map<unsigned int, unsigned int>();
    s->node_stall_map = new map<unsigned int, short>();
    // s->node_to_job_map = new map<unsigned int, int>();
    s->node_period_congestion_map = new map<unsigned long long, congestion_status>();
    s->port_period_congestion_map = new map<unsigned long long, congestion_status>();
    s->num_completed_workload_ranks = 0;

    for(int i = 0; i < p->total_routers; i++)
    {
        (*s->router_port_stallcount_map)[i] = 0;
    }

    for(int i = 0; i < p->total_terminals; i++)
    {
        (*s->node_stall_map)[i] = 0;
    }

    //these are global static maps, and we would ordinarily have something to make sure that it
    //is only set once per PE, but since there's only one SC in the entire sim, I'm just going
    //to ignore that. If more than one SC exists in the simulation (per PE), this will need to be addressed.
    for(int router_rel_id = 0; router_rel_id < p->total_routers; router_rel_id++)
    {
        tw_lpid router_lpid;
        router_lpid = codes_mapping_get_lpid_from_relative(router_rel_id, NULL, p->router_lp_name, NULL, 0);
        router_lpid_to_id_map[router_lpid] = router_rel_id;
        router_id_to_lpid_map[router_rel_id] = router_lpid;
    }

    for(int terminal_rel_id = 0; terminal_rel_id < p->total_terminals; terminal_rel_id++)
    {
        tw_lpid terminal_lpid;
        terminal_lpid = codes_mapping_get_lpid_from_relative(terminal_rel_id, NULL, p->terminal_lp_name, NULL, 0);
        terminal_lpid_to_id_map[terminal_lpid] = terminal_rel_id;
        terminal_id_to_lpid_map[terminal_rel_id] = terminal_lpid;
    }

    s->current_epoch = 0;
    s->congested_epochs = 0;
    s->is_network_routers_congested = false;
    s->is_network_terminals_congested = false;
    s->is_abatement_active = false;
    s->is_all_workloads_complete = false;
    cc_supervisor_load_pattern_set(s);

    //send first heartbeat

    tw_stime now = tw_now(lp);
    tw_stime noise = cc_tw_rand_unif(lp) * .1;
    tw_stime next_heartbeat_time = cc_get_start_of_next_measurement_period_from_given(s->params->measurement_period, now);
    tw_stime time_to_next_heartbeat = next_heartbeat_time - now;

    tw_event *e;
    congestion_control_message *h_msg;
    e = tw_event_new(lp->gid, time_to_next_heartbeat + noise, lp);
    h_msg = (congestion_control_message*)tw_event_data(e);
    h_msg->current_epoch = s->current_epoch + 1;
    h_msg->type = CC_SC_HEARTBEAT;
    h_msg->sender_lpid = lp->gid;
    // printf("SC: Sending Heartbeat to self: Now=%lf  TS=%lf   %d->%d\n",tw_now(lp), next_heartbeat_time, s->current_epoch, h_msg->current_epoch);
    tw_event_send(e);
}

void cc_supervisor_event(sc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
{
    // printf("SC: event!\n");
    switch (msg->type)
    {
        case CC_SC_HEARTBEAT:
            // printf("SC HEARTBEAT RECEIVE %.5f\n", tw_now(lp));
            cc_supervisor_process_heartbeat(s, bf, msg, lp);
        break;
        case CC_R_PERF_REPORT:
            // printf("SC epoch %d: received msg with epoch %d at time %.5f   - %d/%d\n",s->current_epoch, msg->current_epoch, tw_now(lp), s->received_router_performance_count,s->params->total_routers);
            if(s->is_all_workloads_complete == false) {
                if (msg->current_epoch == s->current_epoch) {
                    bf->c13 = 1;
                    cc_supervisor_process_performance_response(s, bf, msg, lp);
                }
                else
                {
                    tw_error(TW_LOC, "problem: SC epoch =%d   msg epoch = %d\n",s->current_epoch, msg->current_epoch);
                }
            }
        break;
        case CC_N_PERF_REPORT:
            if(s->is_all_workloads_complete == false) {
                if (msg->current_epoch == s->current_epoch) {
                    bf->c14 = 1;
                    cc_supervisor_process_performance_response(s, bf, msg, lp);
                }
                else
                {
                    tw_error(TW_LOC, "problem: SC epoch =%d   msg epoch = %d\n",s->current_epoch, msg->current_epoch);
                }
            }        break;
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
            // printf("SC HEARTBEAT RECEIVE RC %.5f\n", tw_now(lp));
            cc_supervisor_process_heartbeat_rc(s, bf, msg, lp);
        break;
        case CC_R_PERF_REPORT:
            if (bf->c13 == 1)
                cc_supervisor_process_performance_response_rc(s, bf, msg, lp);
        break;
        case CC_N_PERF_REPORT:
            if (bf->c14 == 1)
                cc_supervisor_process_performance_response_rc(s, bf, msg, lp);        break;
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
    printf("Num Epochs: %d\n", s->current_epoch);
    printf("Congested Epochs: %d\n", s->congested_epochs);

    printf("Stalled count running: %d\n",stalled_packet_counter);
}

void cc_supervisor_process_heartbeat(sc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
{
    msg->rc_network_router_congested = s->is_network_routers_congested;
    msg->rc_network_terminal_congested = s->is_network_terminals_congested;
    // bf->c1 = (int) s->is_network_routers_congested; //Risky move here, i'm using the bitfield to store an RC bool
    // bf->c2 = (int) s->is_network_terminals_congested;

    s->is_network_routers_congested = cc_supervisor_congestion_control_detect_on_type(s, bf, lp, CC_ROUTER);
    s->is_network_terminals_congested = cc_supervisor_congestion_control_detect_on_type(s, bf, lp, CC_TERMINAL);


    bool is_congested_epoch = (int) (s->is_network_routers_congested || s->is_network_terminals_congested);
    if (is_congested_epoch) {
        bf->c3 = 1;
        s->congested_epochs += 1;
    }

    msg->check_sum = s->received_router_performance_count;
    s->received_router_performance_count = 0;
    s->received_terminal_performance_count = 0;

    // printf("FC %d->%d\n",s->current_epoch, msg->current_epoch);
    int new_epoch = msg->current_epoch;
    msg->current_epoch = s->current_epoch;
    s->current_epoch = new_epoch;

    cc_supervisor_send_heartbeat(s, bf, msg, lp);
}

void cc_supervisor_process_heartbeat_rc(sc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
{
    cc_supervisor_send_heartbeat_rc(s, bf, msg, lp);
    // printf("RC %d->%d\n",s->current_epoch, msg->current_epoch);
    int new_epoch = msg->current_epoch;
    msg->current_epoch = s->current_epoch;
    s->current_epoch = new_epoch;

    s->received_router_performance_count = msg->check_sum;

    if (bf->c3 == 1)
        s->congested_epochs -= 1;

    cc_supervisor_congestion_control_detect_on_type_rc(s, bf, lp, CC_TERMINAL);
    cc_supervisor_congestion_control_detect_on_type_rc(s, bf, lp, CC_ROUTER);

    s->is_network_routers_congested = msg->rc_network_router_congested;
    s->is_network_terminals_congested = msg->rc_network_terminal_congested;
}


// void cc_supervisor_process_heartbeat(sc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
// {
//     cc_supervisor_congestion_control_detect(s, bf, lp);
//     s->congested_epochs += (int) s->is_network_congested;

//     // cc_supervisor_request_performance_information(s, bf, msg, lp);
//     cc_supervisor_send_heartbeat(s, bf, lp);
//     cc_supervisor_start_new_epoch(s);
// }

// void cc_supervisor_process_heartbeat_rc(sc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
// {
//     cc_supervisor_start_new_epoch_rc(s);
//     cc_supervisor_send_heartbeat_rc(s, bf, lp);
//     // cc_supervisor_request_performance_information_rc(s, bf, msg, lp);

//     s->congested_epochs -= (int) s->is_network_congested;
//     cc_supervisor_congestion_control_detect_rc(s, bf, lp);
// }

void cc_supervisor_send_heartbeat(sc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
{
    if (s->is_all_workloads_complete == false)
    {
        bf->c4=1;
        
        tw_stime now = tw_now(lp);
        tw_stime noise = cc_tw_rand_unif(lp) * .1;
        tw_stime next_heartbeat_time = cc_get_start_of_next_measurement_period_from_given(s->params->measurement_period, now);
        tw_stime time_to_next_heartbeat = next_heartbeat_time - now;

        tw_event *e;
        congestion_control_message *h_msg;
        e = tw_event_new(lp->gid, time_to_next_heartbeat + noise, lp);
        h_msg = (congestion_control_message*)tw_event_data(e);
        h_msg->current_epoch = s->current_epoch + 1;
        h_msg->type = CC_SC_HEARTBEAT;
        h_msg->sender_lpid = lp->gid;
        // printf("SC: Sending Heartbeat to self: Now=%lf  TS=%lf   %d->%d\n",tw_now(lp), next_heartbeat_time, s->current_epoch, h_msg->current_epoch);
        tw_event_send(e);
    }
}

void cc_supervisor_send_heartbeat_rc(sc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
{
    if (bf->c4)
        cc_tw_rand_reverse_unif(lp);
}

void cc_supervisor_process_performance_response(sc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
{
    switch(msg->type)
    {
        case CC_R_PERF_REPORT:
            s->received_router_performance_count++;
            stalled_packet_counter += msg->stalled_count;
            (*s->router_port_stallcount_map)[router_lpid_to_id_map[msg->sender_lpid]] = msg->stalled_count;       
        break;
        case CC_N_PERF_REPORT:
            s->received_terminal_performance_count++;
            (*s->node_stall_map)[terminal_lpid_to_id_map[msg->sender_lpid]] = msg->stalled_count;
        break;
        default:
            tw_error(TW_LOC,"Invalid performance response message processed\n");
        break;
    }

    // s->received_terminal_performance_count = s->params->total_terminals;

    // if (s->received_router_performance_count == s->params->total_routers)
    // {
    //     bf->c5 = 1; //normal bitfield to check if the above if statement was true

    //     bf->c1 = (int) s->is_network_routers_congested; //Risky move here, i'm using the bitfield to store an RC bool
    //     bf->c2 = (int) s->is_network_terminals_congested;

    //     s->is_network_routers_congested = cc_supervisor_congestion_control_detect_on_type(s, bf, lp, CC_ROUTER);
    //     s->is_network_terminals_congested = cc_supervisor_congestion_control_detect_on_type(s, bf, lp, CC_TERMINAL);

    //     s->congested_epochs += (int) (s->is_network_routers_congested || s->is_network_terminals_congested);
    //     cc_supervisor_start_new_epoch(s);

    //     msg->check_sum = s->received_router_performance_count;

    //     s->received_router_performance_count = 0;
    //     s->received_terminal_performance_count = 0;
    // }
}

void cc_supervisor_process_performance_response_rc(sc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
{
    // if(bf->c5) {
    //     // s->received_terminal_performance_count = s->params->total_terminals;
    //     s->received_router_performance_count = msg->check_sum;

    //     cc_supervisor_start_new_epoch_rc(s);
    //     s->congested_epochs -= (int) (s->is_network_routers_congested || s->is_network_terminals_congested);

    //     s->is_network_routers_congested = bf->c1;
    //     s->is_network_terminals_congested = bf->c2;
    // }

    switch(msg->type)
    {
        case CC_R_PERF_REPORT:
            s->received_router_performance_count--;
            stalled_packet_counter -= msg->stalled_count;
            (*s->router_port_stallcount_map).erase(router_lpid_to_id_map[msg->sender_lpid]);
        break;
        case CC_N_PERF_REPORT:
            s->received_terminal_performance_count--;
            (*s->node_stall_map).erase(terminal_lpid_to_id_map[msg->sender_lpid]);
        break;
        default:
            tw_error(TW_LOC,"Invalid performance response message processed\n");
        break;
    }
}

void cc_supervisor_receive_wl_completion(sc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
{
    s->num_completed_workload_ranks++;
    printf("Number of Completed Ranks: %d/%d\n",s->num_completed_workload_ranks, s->params->total_workload_ranks);
    if (s->num_completed_workload_ranks == s->params->total_workload_ranks)
    {
        s->is_all_workloads_complete = true;
        cc_supervisor_broadcast_wl_completion(s, bf, msg, lp);
    }
}

void cc_supervisor_receive_wl_completion_rc(sc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
{
    if (s->num_completed_workload_ranks == s->params->total_workload_ranks)
    {
        s->is_all_workloads_complete = false;
        for(int i = 0; i < s->params->total_routers; i++)
            cc_tw_rand_reverse_unif(lp);
        for(int i = 0; i < s->params->total_terminals; i++)
            cc_tw_rand_reverse_unif(lp);
    }
    s->num_completed_workload_ranks--;
}


void cc_supervisor_broadcast_wl_completion(sc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
{
    //Send requests to Router and Node local controllers for performance information
    map<tw_lpid, int>::iterator it = router_lpid_to_id_map.begin();
    for(; it != router_lpid_to_id_map.end(); it++)
    {
        tw_stime ts_noise = cc_tw_rand_unif(lp) * .1;

        congestion_control_message *m;
        tw_event *e = model_net_method_congestion_event(it->first, ts_noise, lp, (void**)&m, NULL);
        m->current_epoch = s->current_epoch;
        // e = tw_event_new(it->first, ts_noise, lp); //ROSS method to create a new event
        // m = (congestion_control_message*)tw_event_data(e); //Gives you a pointer to the data encoded within event e
        m->type = CC_WORKLOAD_RANK_COMPLETE; //Set the event type so we can know how to classify the event when received
        tw_event_send(e); //ROSS method to send off the event e with the encoded data in m
    }

    it = terminal_lpid_to_id_map.begin();
    for(; it != terminal_lpid_to_id_map.end(); it++)
    {
        tw_stime ts_noise = cc_tw_rand_unif(lp) * .1;

        congestion_control_message *m;
        tw_event *e = model_net_method_congestion_event(it->first, ts_noise, lp, (void**)&m, NULL);
        m->current_epoch = s->current_epoch;

        // e = tw_event_new(it->first, ts_noise, lp); //ROSS method to create a new event
        // m = (congestion_control_message*)tw_event_data(e); //Gives you a pointer to the data encoded within event e
        m->type = CC_WORKLOAD_RANK_COMPLETE; //Set the event type so we can know how to classify the event when received
        tw_event_send(e); //ROSS method to send off the event e with the encoded data in m
        // printf("SC: Sent  request to Terminal %d at %f\n",it->second, tw_now(lp)+ts_noise);
    }
}


// void cc_supervisor_request_performance_information_rc(sc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
// {
//     //Send requests to Router and Node local controllers for performance information
//     map<tw_lpid, int>::iterator it = router_lpid_to_id_map.begin();
//     for(; it != router_lpid_to_id_map.end(); it++)
//     {
//         cc_tw_rand_reverse_unif(lp);
//     }

//     it = terminal_lpid_to_id_map.begin();
//     for(; it != terminal_lpid_to_id_map.end(); it++)
//     {
//         cc_tw_rand_reverse_unif(lp);
//     }
// }

// void cc_supervisor_start_new_epoch(sc_state *s)
// {
//     s->current_epoch++;
// }

// void cc_supervisor_start_new_epoch_rc(sc_state *s)
// {
//     s->current_epoch--;
// }


//returns 1 if broad congestion exists on type, returns 0 if broad non-congested on type. If network is congested prior to calling, will return 0 iff it matches decongestion patterns
bool cc_supervisor_congestion_control_detect_on_type(sc_state *s, tw_bf *bf, tw_lp *lp, controller_type type)
{
    if (!((type == CC_ROUTER) || (type == CC_TERMINAL)))
        tw_error(TW_LOC, "Invalid controller type specified\n");

    bool cur_type_congestion;
    if (type == CC_ROUTER) {
        cc_supervisor_check_port_congestion_criterion(s, bf);
        cur_type_congestion = s->is_network_routers_congested;
    }
    if (type == CC_TERMINAL) {
        cc_supervisor_check_nic_congestion_criterion(s, bf);
        cur_type_congestion = s->is_network_terminals_congested;
    }

    if (cur_type_congestion == false) {
        bool to_congestion = cc_supervisor_check_congestion_patterns(s, type, TO_CONGESTION);
        return to_congestion;
    }
    else { //Network is congested, we need to see if we are no longer congested
        bool to_decongestion = cc_supervisor_check_congestion_patterns(s, type, TO_DECONGESTION);
        return !to_decongestion;
    }
}

//returns 1 if broad congestion exists on type, returns 0 if broad non-congested on type. If network is congested prior to calling, will return 0 iff it matches decongestion patterns
bool cc_supervisor_congestion_control_detect_on_type_rc(sc_state *s, tw_bf *bf, tw_lp *lp, controller_type type)
{
    if (!((type == CC_ROUTER) || (type == CC_TERMINAL)))
        tw_error(TW_LOC, "Invalid controller type specified\n");

    if (type == CC_ROUTER) {
        cc_supervisor_check_port_congestion_criterion_rc(s, bf);
    }
    if (type == CC_TERMINAL) {
        cc_supervisor_check_nic_congestion_criterion_rc(s, bf);
    }
}


// void cc_supervisor_congestion_control_detect(sc_state *s, tw_bf *bf, tw_lp *lp)
// {
//     cc_supervisor_check_nic_congestion_criterion(s, bf);
//     cc_supervisor_check_port_congestion_criterion(s, bf);

//     if (s->is_network_congested == false) {
//         bool to_congestion_router = cc_supervisor_check_congestion_patterns(s, CC_ROUTER, TO_CONGESTION);
//         bool to_congestion_terminal = cc_supervisor_check_congestion_patterns(s, CC_TERMINAL, TO_CONGESTION);
        
//         bool to_congestion = to_congestion_router || to_congestion_terminal;
//         if (to_congestion) {
//             bf->c1 = 1;
//             s->is_network_congested = true;
//             printf("%lu CONGESTION DETECTED\n", s->current_epoch);
//         }
//     }
//     else { //Network is congested, we need to see if we are no longer congested
//         bool to_decongestion_router = cc_supervisor_check_congestion_patterns(s, CC_ROUTER, TO_DECONGESTION);
//         bool to_decongestion_terminal = cc_supervisor_check_congestion_patterns(s, CC_TERMINAL, TO_DECONGESTION);

//         bool to_decongestion = to_decongestion_router && to_decongestion_terminal;
//         if (to_decongestion) {
//             bf->c2 = 1;
//             s->is_network_congested = false;
//             printf("%lu CONGESTION ABATED\n", s->current_epoch);
//         }
//     }
// }

// void cc_supervisor_congestion_control_detect_rc(sc_state *s, tw_bf *bf, tw_lp *lp)
// {
//     cc_supervisor_check_nic_congestion_criterion_rc(s, bf);
//     cc_supervisor_check_port_congestion_criterion_rc(s, bf);

//     if (bf->c1)
//         s->is_network_congested = false;
//     if (bf->c2)
//         s->is_network_congested = true;
// }

void cc_supervisor_check_nic_congestion_criterion(sc_state *s, tw_bf *bf)
{
    set<nic_congestion_criterion>::iterator it = s->params->nic_congestion_criterion_set->begin();
    for(; it != s->params->nic_congestion_criterion_set->end(); it++)
    {
        nic_congestion_criterion criterion = *it;
        switch (criterion)
        {
            case NIC_CONGESTION_ALPHA: //if a percentage of nics are congested, then nics are considered congested for this period
            {    
                unsigned int num_stalled_nics = 0;
                map<unsigned int, short>::iterator it2 = s->node_stall_map->begin();
                for(; it2 != s->node_stall_map->end(); it2++)
                {
                    if (it2->second == 1)
                        num_stalled_nics++;
                }
                double percent_stalled = (double) num_stalled_nics / s->params->total_terminals;
                // printf("percent stalled: %.2f\n",percent_stalled);

                if (percent_stalled*100 >= s->params->node_congestion_percent_threshold)
                    (*s->node_period_congestion_map)[s->current_epoch] = CONGESTED;
                break;
            }
            default:
                tw_error(TW_LOC,"Invalid NIC Congestion Criterion %d", criterion);
            break;
        }
    }
}

void cc_supervisor_check_nic_congestion_criterion_rc(sc_state *s, tw_bf *bf)
{
    (*s->node_period_congestion_map).erase(s->current_epoch);
}


void cc_supervisor_check_port_congestion_criterion(sc_state *s, tw_bf *bf)
{
    set<port_congestion_criterion>::iterator it = s->params->port_congestion_criterion_set->begin();
    for(; it != s->params->port_congestion_criterion_set->end(); it++)
    {
        port_congestion_criterion criterion = *it;
        switch (criterion)
        {
            case PORT_CONGESTION_ALPHA: //if a percentage of ports are congested, then the ports are considerd congested for this period
            {    
                unsigned int num_stalled_ports = 0;
                map<unsigned int, unsigned int>::iterator it2 = s->router_port_stallcount_map->begin();
                //loop over all routers
                for(; it2 != s->router_port_stallcount_map->end(); it2++)
                {
                    num_stalled_ports += it2->second;
                }
                double percent_stalled = (double) num_stalled_ports / s->params->total_ports;
                // printf("percent stalled: %.2f\n",percent_stalled);

                if (percent_stalled*100 >= s->params->port_congestion_percent_threshold){
                    (*s->port_period_congestion_map)[s->current_epoch] = CONGESTED;
                    // printf("marking port congestion in this period\n");
                }
                break;
            }
            default:
                tw_error(TW_LOC,"Invalid Port Congestion Criterion %d", criterion);
        }
    }
}

void cc_supervisor_check_port_congestion_criterion_rc(sc_state *s, tw_bf *bf)
{
    (*s->port_period_congestion_map).erase(s->current_epoch);
}

bool cc_supervisor_check_congestion_patterns(sc_state *s, controller_type type, congestion_change check_for_status)
{
    map<unsigned long long, congestion_status> the_period_congestion_map;
    if (type == CC_ROUTER)
        the_period_congestion_map = (*s->port_period_congestion_map);
    else if (type == CC_TERMINAL)
        the_period_congestion_map = (*s->node_period_congestion_map);

    //get last congestion statuses for last MAX_PATTERN_LEN
    char cur_pattern[s->params->loaded_pattern_length];
    cur_pattern[s->params->loaded_pattern_length] = '\0';
    int back_epoch_i = 0;
    for(int i = s->params->loaded_pattern_length-1; i >= 0; i--)
    {
        cur_pattern[i] = the_period_congestion_map[s->current_epoch - back_epoch_i] + '0'; //the + '0' is a hack to convert the 0/1 stored in the map into its char representation
        back_epoch_i += 1;
    }

    int cur_pattern_long = strtol(cur_pattern, NULL, 2); //converting the pattern of binary numbers into unsigned long
    
    unordered_set<unsigned long> pattern_set;
    if (check_for_status == TO_CONGESTION)
        pattern_set = congestion_pattern_set;
    else
        pattern_set = decongestion_pattern_set;

    if (pattern_set.count(cur_pattern_long) == 1) {
        // printf("Match: %s\n",cur_pattern);
        return true;
    }
    else {
        // printf("No Match: %s\n",cur_pattern);
        return false;
    }
}

//NM: 5/15/20 - changing from a broadcast request incast response pattern to a periodic incast (no broadcast request) pattern
//              broadcast messages are terrible for PDES performance.
// void cc_supervisor_request_performance_information(sc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
// {
//     //Send requests to Router and Node local controllers for performance information
//     map<tw_lpid, int>::iterator it = router_lpid_to_id_map.begin();
//     for(; it != router_lpid_to_id_map.end(); it++)
//     {
//         tw_stime ts_noise = g_tw_lookahead + 1 + cc_tw_rand_unif(lp) * .0001;

//         congestion_control_message *m;
//         tw_event *e = model_net_method_congestion_event(it->first, ts_noise, lp, (void**)&m, NULL);
//         m->current_epoch = s->current_epoch;
//         // e = tw_event_new(it->first, ts_noise, lp); //ROSS method to create a new event
//         // m = (congestion_control_message*)tw_event_data(e); //Gives you a pointer to the data encoded within event e
//         m->type = CC_SC_PERF_REQUEST; //Set the event type so we can know how to classify the event when received
//         tw_event_send(e); //ROSS method to send off the event e with the encoded data in m
//         // printf("SC: Sent performance request to Router %d at %f\n",it->second, tw_now(lp)+ts_noise);
//     }

//     it = terminal_lpid_to_id_map.begin();
//     for(; it != terminal_lpid_to_id_map.end(); it++)
//     {
//         tw_stime ts_noise = g_tw_lookahead + 1 + cc_tw_rand_unif(lp) * .0001;

//         congestion_control_message *m;
//         tw_event *e = model_net_method_congestion_event(it->first, ts_noise, lp, (void**)&m, NULL);
//         m->current_epoch = s->current_epoch;

//         // e = tw_event_new(it->first, ts_noise, lp); //ROSS method to create a new event
//         // m = (congestion_control_message*)tw_event_data(e); //Gives you a pointer to the data encoded within event e
//         m->type = CC_SC_PERF_REQUEST; //Set the event type so we can know how to classify the event when received
//         tw_event_send(e); //ROSS method to send off the event e with the encoded data in m
//         // printf("SC: Sent performance request to Terminal %d at %f\n",it->second, tw_now(lp)+ts_noise);
//     }
// }


// void cc_supervisor_request_performance_information_rc(sc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
// {
//     //Send requests to Router and Node local controllers for performance information
//     map<tw_lpid, int>::iterator it = router_lpid_to_id_map.begin();
//     for(; it != router_lpid_to_id_map.end(); it++)
//     {
//         cc_tw_rand_reverse_unif(lp);
//     }

//     it = terminal_lpid_to_id_map.begin();
//     for(; it != terminal_lpid_to_id_map.end(); it++)
//     {
//         cc_tw_rand_reverse_unif(lp);
//     }
// }

void cc_supervisor_load_pattern_set(sc_state *s)
{
    //attempting to be more portable than GNU's getline()
    char filepath1[congestion_pattern_set_filepath.length()+1];
    strcpy(filepath1, congestion_pattern_set_filepath.c_str());

    FILE *file = fopen(filepath1, "r");
    if (file != NULL)
    {
        char pattern[MAX_PATTERN_LEN];
        while (fgets(pattern, MAX_PATTERN_LEN, file) != NULL)
        {
            s->params->loaded_pattern_length = strlen(pattern)-1;

            pattern[strlen(pattern)-1] = '\0';
            unsigned long pattern_long = strtol(pattern, NULL, 2); //convert pattern from string representation of binary to unsigned long
            congestion_pattern_set.insert(pattern_long); //insert the unsigned long conversion into the pattern set for quick hashing
        }
        fclose(file);
    }
    else
    {
        tw_error(TW_LOC, "Congestion Controller: Failed to open congestion pattern set file %s\n", congestion_pattern_set_filepath); /* why didn't the file open? */
    }

    char filepath2[decongestion_pattern_set_filepath.length()+1];
    strcpy(filepath2, decongestion_pattern_set_filepath.c_str());

    file = fopen(filepath2, "r");
    if (file != NULL)
    {
        char pattern[MAX_PATTERN_LEN];
        while (fgets(pattern, MAX_PATTERN_LEN, file) != NULL)
        {
            s->params->loaded_pattern_length = strlen(pattern)-1;

            pattern[strlen(pattern)-1] = '\0';
            unsigned long pattern_long = strtol(pattern, NULL, 2); //convert pattern from string representation of binary to unsigned long
            decongestion_pattern_set.insert(pattern_long); //insert the unsigned long conversion into the pattern set for quick hashing
        }
        fclose(file);
    }
    else
    {
        tw_error(TW_LOC, "Congestion Controller: Failed to open congestion pattern set file %s\n", congestion_pattern_set_filepath); /* why didn't the file open? */
    }
}    



//Router Local Controller
void cc_router_local_controller_init(rlc_state *s, int router_id)
{
    // printf("CC LOCAL INIT!\n");
    s->local_params = (cc_local_param*)calloc(1, sizeof(cc_local_param));
    cc_local_param *p = s->local_params;

    s->router_id = router_id;

    int radix;
    int rc = configuration_get_value_int(&config, "PARAMS", "cc_radix", NULL, &radix);
    if (rc) {
        tw_error(TW_LOC,"Congestion Control: Congestion management enabled but no 'cc_radix' configuration value specified.");
    }
    p->router_radix = radix;

    tw_stime period;
    rc = configuration_get_value_double(&config, "PARAMS", "cc_measurement_period", NULL, &period);
    if (rc) {
        // printf("Congestion Control: Measurment period not specified, using default 50ns\n");
        period = 50.0;
    }
    p->measurement_period = period;

    p->port_stall_criterion_set = new set<port_stall_criterion>();
    p->port_stall_criterion_set->insert(PORT_STALL_ALPHA);
    p->port_stall_to_pass_ratio_threshold = 1.0;

    s->is_all_workloads_complete = 0;

    s->current_epoch = 0;

    s->port_period_stall_map = new map<unsigned long long, int>();

    // s->last_perf_timestamp = 0.0;
    // s->last_perf_epoch = -1;

}

void cc_router_local_controller_setup_stall_alpha(rlc_state *s, int radix, unsigned long *stalled_chunks_ptr, unsigned long *total_chunks_ptr)
{
    s->stalled_chunks_at_last_epoch = (unsigned long *)calloc(s->local_params->router_radix, sizeof(unsigned long));
    s->total_chunks_at_last_epoch = (unsigned long *)calloc(s->local_params->router_radix, sizeof(unsigned long));
    s->stalled_chunks_ptr = stalled_chunks_ptr;
    s->total_chunks_ptr = total_chunks_ptr;
}

void cc_router_local_controller_kickoff(rlc_state *s, tw_lp *lp)
{
    tw_stime now = tw_now(lp);
    tw_stime noise = cc_tw_rand_unif(lp) * .1;
    tw_stime next_heartbeat_time = cc_get_start_of_next_measurement_period_from_given(s->local_params->measurement_period, now);
    tw_stime time_to_next_heartbeat = next_heartbeat_time - now - 1;

    congestion_control_message *h_msg;
    tw_event *e = model_net_method_congestion_event(lp->gid, time_to_next_heartbeat + noise, lp, (void**)&h_msg, NULL);
    // e = tw_event_new(lp->gid, s->local_params->measurement_period + noise, lp);
    // h_msg = (congestion_control_message*)tw_event_data(e);
    h_msg->current_epoch = s->current_epoch + 1;
    h_msg->type = CC_RLC_HEARTBEAT;
    h_msg->sender_lpid = lp->gid;
    // h_msg->stalled_chunks_at_last_epoch = (unsigned long *)calloc(s->local_params->router_radix, sizeof(unsigned long));
    // h_msg->total_chunks_at_last_epoch = (unsigned long *)calloc(s->local_params->router_radix, sizeof(unsigned long));
    // printf("RLC: Sending Heartbeat to self: Now=%lf  TS=%lf\n",tw_now(lp), next_heartbeat_time);
    tw_event_send(e);
}

int cc_router_local_get_port_stall_count(rlc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
{
    int max_stalled_ports = 0;
    // msg->rc_time = s->last_perf_timestamp;
    // msg->rc_value = s->last_perf_epoch;
    // assert(s->last_perf_epoch != s->current_epoch);
    // printf("%.5f   %d  =?  %d + 1\n",tw_now(lp)-s->last_perf_timestamp, s->current_epoch, s->last_perf_epoch);
    // s->last_perf_timestamp = tw_now(lp);
    // s->last_perf_epoch = s->current_epoch;

    

    set< port_stall_criterion>::iterator it = s->local_params->port_stall_criterion_set->begin();
    for(; it != s->local_params->port_stall_criterion_set->end(); it++)
    {
        switch(*it)
        {
            case PORT_STALL_ALPHA:
            {
                int stalled_ports = 0;
                for (int i = 0; i < s->local_params->router_radix; i++)
                {
                    int packets_stalled_since_last = s->stalled_chunks_ptr[i] - s->stalled_chunks_at_last_epoch[i];
                    int packets_passed_since_last = s->total_chunks_ptr[i] - s->total_chunks_at_last_epoch[i];
                    // printf("Packets stalled since last %d    passed %d\n", packets_stalled_since_last, packets_passed_since_last);
                    double ratio = (double)packets_stalled_since_last / packets_passed_since_last;
                    // printf("%d: %d total stalled and %d total passed\n",s->router_id,s->stalled_chunks_ptr[i], s->total_chunks_ptr[i]);
                    // printf("%d stalled and %d passed    ratio = %.2f\n",packets_stalled_since_last, packets_passed_since_last, ratio);
                    if (ratio >= s->local_params->port_stall_to_pass_ratio_threshold)
                        stalled_ports++;
                }

                if (stalled_ports > max_stalled_ports)
                    max_stalled_ports = stalled_ports;
            }    
            break;
            default:
                tw_error(TW_LOC, "Invalid port stall criterion option\n");
        }
    }

    (*s->port_period_stall_map)[s->current_epoch] = max_stalled_ports;
    return max_stalled_ports;
}

void cc_router_local_get_port_stall_count_rc(rlc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
{
    (*s->port_period_stall_map).erase(s->current_epoch);
    // s->last_perf_timestamp = msg->rc_time;
    // s->last_perf_epoch = msg->rc_value;
}

void cc_router_local_congestion_event(rlc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
{
    
    switch(msg->type)
    {
        case CC_RLC_HEARTBEAT:
        {
            cc_router_local_process_heartbeat(s, bf, msg, lp);
        }
            break;
        case CC_WORKLOAD_RANK_COMPLETE:
        {
            s->is_all_workloads_complete = true;
        }
            break;
        default:
                tw_error(TW_LOC, "Invalid event at cc router local congestion event");
            break;
    }
}

void cc_router_local_congestion_event_rc(rlc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
{
    switch(msg->type)
    {
        case CC_RLC_HEARTBEAT:
        {
            cc_router_local_process_heartbeat_rc(s, bf, msg, lp);
        }
            break;
        case CC_WORKLOAD_RANK_COMPLETE:
        {
            s->is_all_workloads_complete = false;
        }
            break;
        default:
            tw_error(TW_LOC, "Invalid event at cc router local congestion event rc");
            break;
    }
}

void cc_router_local_congestion_event_commit(rlc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
{
    switch(msg->type)
    {
        case CC_RLC_HEARTBEAT:
        {
            cc_router_local_new_epoch_commit(s, bf, msg, lp);
            // free(msg->total_chunks_at_last_epoch);
            // free(msg->stalled_chunks_at_last_epoch);
        }
            break;
        case CC_WORKLOAD_RANK_COMPLETE:
            break;
        default:
            tw_error(TW_LOC, "Invalid event at cc router local congestion event commit");
            break;
    }
}

void cc_router_local_send_heartbeat(rlc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
{
    if(!s->is_all_workloads_complete) {
        bf->c5 = 1;
        tw_stime now = tw_now(lp);
        tw_stime noise = cc_tw_rand_unif(lp) * .1;
        tw_stime next_heartbeat_time = cc_get_start_of_next_measurement_period_from_given(s->local_params->measurement_period, now+1);
        tw_stime time_to_next_heartbeat = next_heartbeat_time - now -1;

        congestion_control_message *h_msg;
        tw_event *e = model_net_method_congestion_event(lp->gid, time_to_next_heartbeat + noise, lp, (void**)&h_msg, NULL);
        // e = tw_event_new(lp->gid, s->local_params->measurement_period + noise, lp);
        // h_msg = (congestion_control_message*)tw_event_data(e);
        h_msg->current_epoch = s->current_epoch + 1;
        h_msg->type = CC_RLC_HEARTBEAT;
        h_msg->sender_lpid = lp->gid;
        
        // h_msg->stalled_chunks_at_last_epoch = (unsigned long *)malloc(s->local_params->router_radix * sizeof(unsigned long));
        // h_msg->total_chunks_at_last_epoch = (unsigned long *)malloc(s->local_params->router_radix * sizeof(unsigned long));
        // memcpy(h_msg->stalled_chunks_at_last_epoch, s->stalled_chunks_ptr, s->local_params->router_radix * sizeof(unsigned long));
        // memcpy(h_msg->total_chunks_at_last_epoch, s->total_chunks_ptr, s->local_params->router_radix * sizeof(unsigned long));

        // printf("RLC: Sending Heartbeat to self: Now=%lf  TS=%lf\n",tw_now(lp), next_heartbeat_time);
        tw_event_send(e);
    }
}

void cc_router_local_send_heartbeat_rc(rlc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
{
    if(bf->c5==1)
        cc_tw_rand_reverse_unif(lp);
}

void cc_router_local_process_heartbeat(rlc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
{
    cc_router_local_get_port_stall_count(s, bf, msg, lp);
    cc_router_local_send_performance(s, bf, msg, lp);
    cc_router_local_new_epoch(s, bf, msg, lp);
    cc_router_local_send_heartbeat(s, bf, msg, lp);
}

void cc_router_local_process_heartbeat_rc(rlc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
{
    cc_router_local_send_heartbeat_rc(s, bf, msg, lp);
    cc_router_local_new_epoch_rc(s, bf, msg ,lp);
    cc_router_local_send_performance_rc(s, bf, msg, lp);
    cc_router_local_get_port_stall_count_rc(s, bf, msg, lp);
}


void cc_router_local_send_performance(rlc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
{
    tw_event *e;
    congestion_control_message *m;
    tw_stime noise = cc_tw_rand_unif(lp) *.1;
    e = tw_event_new(g_cc_supervisory_controller_gid, noise, lp);
    m = (congestion_control_message *)tw_event_data(e);
    m->type = CC_R_PERF_REPORT;
    m->sender_lpid = lp->gid;
    m->current_epoch = s->current_epoch;
    m->stalled_count = (*s->port_period_stall_map)[s->current_epoch];
    tw_event_send(e);
}

void cc_router_local_send_performance_rc(rlc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
{
    cc_tw_rand_reverse_unif(lp);
}

void cc_router_local_new_epoch(rlc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
{
    //RC Saving
    msg->rc_ptr = malloc(s->local_params->router_radix * sizeof(unsigned long));
    msg->rc_ptr2 = malloc(s->local_params->router_radix * sizeof(unsigned long));
    memcpy(msg->rc_ptr, s->stalled_chunks_at_last_epoch, s->local_params->router_radix * sizeof(unsigned long));
    memcpy(msg->rc_ptr2, s->total_chunks_at_last_epoch, s->local_params->router_radix * sizeof(unsigned long));

    // int sum = 0;
    // int sum2 = 0;
    for(int i = 0; i < s->local_params->router_radix; i++)
    {
        // sum += s->stalled_chunks_at_last_epoch[i];
        s->stalled_chunks_at_last_epoch[i] = s->stalled_chunks_ptr[i];
        s->total_chunks_at_last_epoch[i] = s->total_chunks_ptr[i];
        // sum2 += s->stalled_chunks_at_last_epoch[i];
    }
    // msg->check_sum = sum;


    assert(&s->stalled_chunks_at_last_epoch != &s->stalled_chunks_ptr);

    int new_epoch = msg->current_epoch;
    msg->current_epoch = s->current_epoch;
    s->current_epoch = new_epoch;
}

// verified correct RC
void cc_router_local_new_epoch_rc(rlc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
{
    int new_epoch = msg->current_epoch;
    msg->current_epoch = s->current_epoch;
    s->current_epoch = new_epoch;

    memcpy(s->stalled_chunks_at_last_epoch, msg->rc_ptr,  s->local_params->router_radix * sizeof(unsigned long));
    memcpy(s->total_chunks_at_last_epoch, msg->rc_ptr2,  s->local_params->router_radix * sizeof(unsigned long));
    
    // int sum = 0;
    // for(int i = 0; i < s->local_params->router_radix; i++)
    // {
    //     sum+= s->stalled_chunks_at_last_epoch[i];
    // }
    // assert(sum == msg->check_sum);
    
    free(msg->rc_ptr);
    free(msg->rc_ptr2);
}

void cc_router_local_new_epoch_commit(rlc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
{
    free(msg->rc_ptr);
    free(msg->rc_ptr2);
}




//Node Local Controller
void cc_terminal_local_controller_init(tlc_state *s, int term_id)
{
    s->local_params = (cc_local_param*)calloc(1,sizeof(cc_local_param));
    cc_local_param *p = s->local_params;

    s->terminal_id = term_id;

    tw_stime period;
    int rc = configuration_get_value_double(&config, "PARAMS", "cc_measurement_period", NULL, &period);
    if (rc) {
        // printf("Congestion Control: Measurment period not specified, using default 50ns\n");
        period = 50.0;
    }
    p->measurement_period = period;

    p->nic_stall_criterion_set = new set<nic_stall_criterion>();
    p->nic_stall_criterion_set->insert(NIC_STALL_ALPHA);
    p->node_stall_to_pass_ratio_threshold = 1.0;

    s->is_all_workloads_complete = 0;

    s->current_epoch = 0;

    s->nic_period_stall_map = new map<unsigned long long, stall_status>();
}

void cc_terminal_local_controller_setup_stall_alpha(tlc_state *s, unsigned long *stalled_chunks_ptr, unsigned long *total_chunks_ptr)
{
    s->stalled_chunks_at_last_epoch = 0; //TODO: what about multi rail or multi VC terminals
    s->total_chunks_at_last_epoch = 0;
    s->stalled_chunks_ptr = stalled_chunks_ptr;
    s->total_chunks_ptr = total_chunks_ptr;
}

void cc_terminal_local_controller_kickoff(tlc_state *s, tw_lp *lp)
{
    tw_stime now = tw_now(lp);
    tw_stime noise = cc_tw_rand_unif(lp) * .1;
    tw_stime next_heartbeat_time = cc_get_start_of_next_measurement_period_from_given(s->local_params->measurement_period, now);
    tw_stime time_to_next_heartbeat = next_heartbeat_time - now - 1;

    congestion_control_message *h_msg;
    tw_event *e = model_net_method_congestion_event(lp->gid, time_to_next_heartbeat + noise, lp, (void**)&h_msg, NULL);
    // e = tw_event_new(lp->gid, s->local_params->measurement_period + noise, lp);
    // h_msg = (congestion_control_message*)tw_event_data(e);
    h_msg->current_epoch = s->current_epoch + 1;
    h_msg->type = CC_TLC_HEARTBEAT;
    h_msg->sender_lpid = lp->gid;
    // h_msg->stalled_chunks_at_last_epoch = (unsigned long *)calloc(s->local_params->router_radix, sizeof(unsigned long));
    // h_msg->total_chunks_at_last_epoch = (unsigned long *)calloc(s->local_params->router_radix, sizeof(unsigned long));
    // printf("TLC: Sending Heartbeat to self: Now=%lf  TS=%lf\n",tw_now(lp), next_heartbeat_time);
    tw_event_send(e);
}

int cc_terminal_local_get_nic_stall_count(tlc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
{
    stall_status is_stalled = NOT_STALLED;

    set< nic_stall_criterion>::iterator it = s->local_params->nic_stall_criterion_set->begin();
    for(; it != s->local_params->nic_stall_criterion_set->end(); it++)
    {
        switch(*it)
        {
            case NIC_STALL_ALPHA:
            {
                int packets_stalled_since_last = *(s->stalled_chunks_ptr) - s->stalled_chunks_at_last_epoch;
                int packets_passed_since_last = *(s->total_chunks_ptr) - s->total_chunks_at_last_epoch;
                
                if (((double)packets_stalled_since_last)/((double)packets_passed_since_last) >= s->local_params->node_stall_to_pass_ratio_threshold)
                    is_stalled = STALLED;                
            }    
            break;
            default:
                tw_error(TW_LOC, "Invalid port stall criterion option\n");
        }
    }

    (*s->nic_period_stall_map)[s->current_epoch] = is_stalled;
    return (int)is_stalled;
}

void cc_terminal_local_get_nic_stall_count_rc(tlc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
{
    (*s->nic_period_stall_map).erase(s->current_epoch);
}

void cc_terminal_local_congestion_event(tlc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
{
    
    switch(msg->type)
    {
        case CC_TLC_HEARTBEAT:
        {
            cc_terminal_local_process_heartbeat(s, bf, msg, lp);
        }
            break;
        case CC_WORKLOAD_RANK_COMPLETE:
        {
            s->is_all_workloads_complete = true;
        }
            break;
        default:
                tw_error(TW_LOC, "Invalid event at cc terminal local congestion event");
            break;
    }
}

void cc_terminal_local_congestion_event_rc(tlc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
{
    switch(msg->type)
    {
        case CC_TLC_HEARTBEAT:
        {
            cc_terminal_local_process_heartbeat_rc(s, bf, msg, lp);
        }
            break;
        case CC_WORKLOAD_RANK_COMPLETE:
        {
            s->is_all_workloads_complete = false;
        }
            break;
        default:
            tw_error(TW_LOC, "Invalid event at cc terminal local congestion event rc");
            break;
    }
}

void cc_terminal_local_congestion_event_commit(tlc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
{
    switch(msg->type)
    {
        case CC_TLC_HEARTBEAT:
        {
            cc_terminal_local_new_epoch_commit(s, bf, msg, lp);
            // free(msg->total_chunks_at_last_epoch);
            // free(msg->stalled_chunks_at_last_epoch);
        }
            break;
        case CC_WORKLOAD_RANK_COMPLETE:
            break;
        default:
            tw_error(TW_LOC, "Invalid event at cc terminal local congestion event commit");
            break;
    }
}

void cc_terminal_local_send_heartbeat(tlc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
{
    if(!s->is_all_workloads_complete) {
        bf->c5 = 1;
        tw_stime now = tw_now(lp);
        tw_stime noise = cc_tw_rand_unif(lp) * .1;
        tw_stime next_heartbeat_time = cc_get_start_of_next_measurement_period_from_given(s->local_params->measurement_period, now+1);
        tw_stime time_to_next_heartbeat = next_heartbeat_time - now -1;

        congestion_control_message *h_msg;
        tw_event *e = model_net_method_congestion_event(lp->gid, time_to_next_heartbeat + noise, lp, (void**)&h_msg, NULL);
        // e = tw_event_new(lp->gid, s->local_params->measurement_period + noise, lp);
        // h_msg = (congestion_control_message*)tw_event_data(e);
        h_msg->current_epoch = s->current_epoch + 1;
        h_msg->type = CC_TLC_HEARTBEAT;
        h_msg->sender_lpid = lp->gid;
        
        // h_msg->stalled_chunks_at_last_epoch = (unsigned long *)malloc(s->local_params->router_radix * sizeof(unsigned long));
        // h_msg->total_chunks_at_last_epoch = (unsigned long *)malloc(s->local_params->router_radix * sizeof(unsigned long));
        // memcpy(h_msg->stalled_chunks_at_last_epoch, s->stalled_chunks_ptr, s->local_params->router_radix * sizeof(unsigned long));
        // memcpy(h_msg->total_chunks_at_last_epoch, s->total_chunks_ptr, s->local_params->router_radix * sizeof(unsigned long));

        // printf("RLC: Sending Heartbeat to self: Now=%lf  TS=%lf\n",tw_now(lp), next_heartbeat_time);
        tw_event_send(e);
    }
}

void cc_terminal_local_send_heartbeat_rc(tlc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
{
    if(bf->c5==1)
        cc_tw_rand_reverse_unif(lp);
}

void cc_terminal_local_process_heartbeat(tlc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
{
    cc_terminal_local_get_nic_stall_count(s, bf, msg, lp);
    cc_terminal_local_send_performance(s, bf, msg, lp);
    cc_terminal_local_new_epoch(s, bf, msg, lp);
    cc_terminal_local_send_heartbeat(s, bf, msg, lp);
}

void cc_terminal_local_process_heartbeat_rc(tlc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
{
    cc_terminal_local_send_heartbeat_rc(s, bf, msg, lp);
    cc_terminal_local_new_epoch_rc(s, bf, msg ,lp);
    cc_terminal_local_send_performance_rc(s, bf, msg, lp);
    cc_terminal_local_get_nic_stall_count_rc(s, bf, msg, lp);
}

void cc_terminal_local_send_performance(tlc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
{
    tw_event *e;
    congestion_control_message *m;
    tw_stime noise = cc_tw_rand_unif(lp) *.1;
    e = tw_event_new(g_cc_supervisory_controller_gid, noise, lp);
    m = (congestion_control_message *)tw_event_data(e);
    m->type = CC_N_PERF_REPORT;
    m->sender_lpid = lp->gid;
    m->current_epoch = s->current_epoch;
    m->stalled_count = (*s->nic_period_stall_map)[s->current_epoch];
    tw_event_send(e);
}

void cc_terminal_local_send_performance_rc(tlc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
{
    cc_tw_rand_reverse_unif(lp);
}


void cc_terminal_local_new_epoch(tlc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
{
    //RC Saving
    msg->rc_ptr = malloc(sizeof(unsigned long));
    msg->rc_ptr2 = malloc(sizeof(unsigned long));
    memcpy(msg->rc_ptr, &s->stalled_chunks_at_last_epoch, sizeof(unsigned long));
    memcpy(msg->rc_ptr2, &s->total_chunks_at_last_epoch, sizeof(unsigned long));


    s->stalled_chunks_at_last_epoch = *(s->stalled_chunks_ptr);
    s->total_chunks_at_last_epoch = *(s->total_chunks_ptr);

    int new_epoch = msg->current_epoch;
    msg->current_epoch = s->current_epoch;
    s->current_epoch = new_epoch;
}

// verified correct RC
void cc_terminal_local_new_epoch_rc(tlc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
{
    int new_epoch = msg->current_epoch;
    msg->current_epoch = s->current_epoch;
    s->current_epoch = new_epoch;

    memcpy(&s->stalled_chunks_at_last_epoch, msg->rc_ptr, sizeof(unsigned long));
    memcpy(&s->total_chunks_at_last_epoch, msg->rc_ptr2, sizeof(unsigned long));
    
    free(msg->rc_ptr);
    free(msg->rc_ptr2);
}

void cc_terminal_local_new_epoch_commit(tlc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
{
    free(msg->rc_ptr);
    free(msg->rc_ptr2);
}
