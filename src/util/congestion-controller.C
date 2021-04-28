#include <codes/congestion-controller-core.h>
#include <codes/congestion-controller-model.h>
#include <codes/codes-jobmap.h>
#include <codes/model-net-lp.h>
#include <codes/codes_mapping.h>
#include <map>
#include <vector>
#include <set>
#include <set>
#include <string.h>
#include <string>

#define PERMANENT_ABATEMENT 1
#define OUTPUT_BANDWIDTHS 0

using namespace std;

struct codes_jobmap_ctx *jobmap_ctx;
static int is_jobmap_set = 0;

/* global variables for codes mapping */
static char lp_group_name[MAX_NAME_LENGTH];
static int mapping_grp_id, mapping_type_id, mapping_rep_id, mapping_offset;

static int network_id = 0;
static double cc_bandwidth_monitoring_window = 10000;
static int cc_bandwidth_rolling_window_count = 5;

unsigned long long stalled_packet_counter = 0;
unsigned long long stalled_nic_counter = 0;

/************* DEFINITIONS ****************************************/
int g_congestion_control_enabled;
tw_stime g_congestion_control_notif_latency = 0;

const tw_optdef cc_app_opt [] =
{
	TWOPT_GROUP("Congestion Control"),
    TWOPT_STIME("cc_notif_latency", g_congestion_control_notif_latency, "latency for congestion control notifications"),
	TWOPT_END()
};

static char terminal_lp_name[128];
static char router_lp_name[128];

// definition of class methods of Portchan_node class defined in congestion-controller-model.h
Portchan_node::Portchan_node(portchan_node_type pnt, int router_radix, int vcs_per_port)
{
    type = pnt;
    packet_count = 0;
    is_congested = false;

    switch(type)
    {
        case ROOT:
            for(int i = 0; i < router_radix; i++)
            {
                Portchan_node* child = new Portchan_node(PORT, router_radix, vcs_per_port); //delete done in deconstructor
                children.push_back(child);
            }
        break;
        case PORT:
            for(int i = 0; i < vcs_per_port; i++)
            {
                Portchan_node* child = new Portchan_node(VC, router_radix, vcs_per_port); //delete done in deconstructor
                children.push_back(child);            
            }
        break;
        case VC://do nothing else
        break;
        default:
            tw_error(TW_LOC, "Portchan_node enqueue: Invalid node type\n");
    }
}

Portchan_node::~Portchan_node()
{
    //type and packet_count are primitives
    term_count_map.clear(); //not new'd but just to be safe lets clear it
    for(int i = 0; i < children.size(); i++)
    {
        delete children[i]; //deconstruct recursively
    }
}

unsigned long long Portchan_node::get_packet_count()
{
    return packet_count;
}

unsigned long long Portchan_node::get_packet_count_from_term(unsigned int term_id)
{
    try {
    return term_count_map.at(term_id);
    } catch (exception e) {
        return 0;
    }
}

unsigned long long Portchan_node::get_packet_count_by_port(int port_no)
{
    if(type != ROOT)
        tw_error(TW_LOC, "Portchan_node : Invalid node type for this action\n");

    return children[port_no]->get_packet_count();
}

unsigned long long Portchan_node::get_packet_count_by_port_from_term(int port_no, unsigned int term_id)
{
    if(type != ROOT)
        tw_error(TW_LOC, "Portchan_node : Invalid node type for this action\n");
    
    return children[port_no]->get_packet_count_from_term(term_id);
}

unsigned long long Portchan_node::get_packet_count_by_port_vc(int port_no, int vc_no)
{
    if(type != ROOT)
        tw_error(TW_LOC, "Portchan_node : Invalid node type for this action\n");

    return children[port_no]->children[vc_no]->get_packet_count();
}

unsigned long long Portchan_node::get_packet_count_by_port_vc_from_term(int port_no, int vc_no, unsigned int term_id)
{
    if(type != ROOT)
        tw_error(TW_LOC, "Portchan_node : Invalid node type for this action\n");
    
    return children[port_no]->children[vc_no]->get_packet_count_from_term(term_id);
}

map<unsigned int, unsigned long long> Portchan_node::get_term_count_map()
{
    return term_count_map;
}

map<unsigned int, unsigned long long> Portchan_node::get_app_count_map()
{
    return app_count_map;
}

map<unsigned int, unsigned long long> Portchan_node::get_term_count_map_by_port(int port_no)
{
    if(type != ROOT)
        tw_error(TW_LOC, "Portchan_node : Invalid node type for this action\n");

    return children[port_no]->get_term_count_map();
}

map<unsigned int, unsigned long long> Portchan_node::get_app_count_map_by_port(int port_no)
{
    if(type != ROOT)
        tw_error(TW_LOC, "Portchan_node : Invalid node type for this action\n");

    return children[port_no]->get_app_count_map();
}

map<unsigned int, unsigned long long> Portchan_node::get_term_count_map_by_port_vc(int port_no, int vc_no)
{
    if(type != ROOT)
        tw_error(TW_LOC, "Portchan_node : Invalid node type for this action\n");

    return children[port_no]->children[vc_no]->get_term_count_map();
}

map<unsigned int, unsigned long long> Portchan_node::get_app_count_map_by_port_vc(int port_no, int vc_no)
{
    if(type != ROOT)
        tw_error(TW_LOC, "Portchan_node : Invalid node type for this action\n");

    return children[port_no]->children[vc_no]->get_app_count_map();
}

map<unsigned int, unsigned long long> Portchan_node::get_term_count_map_from_app(int app_id)
{
    return app_to_terminal_counter.at(app_id);
}

map<unsigned int, unsigned long long> Portchan_node::get_term_count_map_by_port_from_app(int port_no, int app_id)
{
    if(type != ROOT)
        tw_error(TW_LOC, "Portchan_node : Invalid node type for this action\n");

    return children[port_no]->get_term_count_map_from_app(app_id);
}

map<unsigned int, unsigned long long> Portchan_node::get_term_count_map_by_port_vc_from_app(int port_no, int vc_no, int app_id)
{
    if(type != ROOT)
        tw_error(TW_LOC, "Portchan_node : Invalid node type for this action\n");

    return children[port_no]->children[vc_no]->get_term_count_map_from_app(app_id);
}

bool Portchan_node::is_router_congested()
{
    return is_congested;
}

bool Portchan_node::is_port_congested(int port_no)
{
    if(type != ROOT)
        tw_error(TW_LOC, "Portchan_node : Invalid node type for this action\n");

    return children[port_no]->is_congested;
}

bool Portchan_node::is_port_vc_congested(int port_no, int vc_no)
{
    if(type != ROOT)
        tw_error(TW_LOC, "Portchan_node : Invalid node type for this action\n");

    return children[port_no]->children[vc_no]->is_congested;
}

void Portchan_node::set_router_congestion_state(bool new_is_congested)
{
    is_congested = new_is_congested;
}

void Portchan_node::set_port_congestion_state(int port_no, bool new_is_congested)
{
    children[port_no]->is_congested = new_is_congested;
}

void Portchan_node::set_port_vc_congested(int port_no, int vc_no, bool new_is_congested)
{
    children[port_no]->children[vc_no]->is_congested = new_is_congested;
}

void Portchan_node::set_next_possible_router_normal_time(tw_stime time)
{
    next_possible_normal_time = time;
}

void Portchan_node::set_next_possible_port_normal_time(int port_no, tw_stime time)
{
    children[port_no]->next_possible_normal_time = time;
}

void Portchan_node::set_next_possible_vc_normal_time(int port_no, int vc_no, tw_stime time)
{
    children[port_no]->children[vc_no]->next_possible_normal_time = time;
}

tw_stime Portchan_node::get_next_possible_router_normal_time()
{
    return next_possible_normal_time;
}

tw_stime Portchan_node::get_next_possible_port_normal_time(int port_no)
{
    return children[port_no]->next_possible_normal_time;
}

tw_stime Portchan_node::get_next_possible_vc_normal_time(int port_no, int vc_no)
{
    return children[port_no]->children[vc_no]->next_possible_normal_time;
}

void Portchan_node::mark_abated_terminal(unsigned int term_id)
{
    if(type != ROOT)
        tw_error(TW_LOC, "Portchan_node : Invalid node type for this action\n");

    abated_terminals_this_node.emplace(term_id);
}


void Portchan_node::mark_abated_terminal(int port_no, unsigned int term_id)
{
    switch(type)
    {
        case ROOT:
            abated_terminal_child_counter[term_id] += 1;
            children[port_no]->mark_abated_terminal(port_no, term_id);
        break;
        case PORT:
            abated_terminals_this_node.emplace(term_id);
        break;
        case VC://do nothing else
        break;
        default:
            tw_error(TW_LOC, "Portchan_node enqueue: Invalid node type\n");
    }
}

void Portchan_node::mark_abated_terminal(int port_no, int vc_no, unsigned int term_id)
{
    switch(type)
    {
        case ROOT:
            abated_terminal_child_counter[term_id] += 1;
            children[port_no]->mark_abated_terminal(port_no, vc_no, term_id);
        break;
        case PORT:
            abated_terminal_child_counter[term_id] += 1;
            children[vc_no]->mark_abated_terminal(port_no, vc_no, term_id);
        break;
        case VC:
            abated_terminals_this_node.emplace(term_id);
        break;
        default:
            tw_error(TW_LOC, "Portchan_node enqueue: Invalid node type\n");
    }
}

void Portchan_node::mark_unabated_terminal(unsigned int term_id)
{
    if(type != ROOT)
        tw_error(TW_LOC, "Portchan_node : Invalid node type for this action\n");

    abated_terminals_this_node.erase(term_id);
}

void Portchan_node::mark_unabated_terminal(int port_no, unsigned int term_id)
{
    switch(type)
    {
        case ROOT:
            abated_terminal_child_counter[term_id] -= 1;
            if(abated_terminal_child_counter[term_id] == 0)
                abated_terminal_child_counter.erase(term_id);
            children[port_no]->mark_unabated_terminal(port_no, term_id);
        break;
        case PORT:
            abated_terminals_this_node.erase(term_id);
        break;
        case VC://do nothing else
        break;
        default:
            tw_error(TW_LOC, "Portchan_node enqueue: Invalid node type\n");
    }
}

void Portchan_node::mark_unabated_terminal(int port_no, int vc_no, unsigned int term_id)
{
    switch(type)
    {
        case ROOT:
            abated_terminal_child_counter[term_id] -= 1;
            if(abated_terminal_child_counter[term_id] == 0)
                abated_terminal_child_counter.erase(term_id);
            children[port_no]->mark_unabated_terminal(port_no, vc_no, term_id);
        break;
        case PORT:
            abated_terminal_child_counter[term_id] -= 1;
            if(abated_terminal_child_counter[term_id] == 0)
                abated_terminal_child_counter.erase(term_id);
            children[vc_no]->mark_unabated_terminal(port_no, vc_no, term_id);
        break;
        case VC:
            abated_terminals_this_node.erase(term_id);
        break;
        default:
            tw_error(TW_LOC, "Portchan_node enqueue: Invalid node type\n");
    }
}

bool Portchan_node::is_abated_terminal(unsigned int term_id)
{
    if (abated_terminals_this_node.count(term_id))
        return true;
    else
    {
        try {
            if (abated_terminal_child_counter.at(term_id) > 0)
                return true;
        } catch (exception e)
        {
            return false;
        }
    }
    return false;
}

set<unsigned int> Portchan_node::get_abated_terminals()
{
    return abated_terminals_this_node;
}

set<unsigned int> Portchan_node::get_abated_terminals(int port_no)
{
    if(type != ROOT)
        tw_error(TW_LOC, "Portchan_node : Invalid node type for this action\n");

    return children[port_no]->get_abated_terminals();
}

set<unsigned int> Portchan_node::get_abated_terminals(int port_no, int vc_no)
{
    if(type != ROOT)
        tw_error(TW_LOC, "Portchan_node : Invalid node type for this action\n");

    return children[port_no]->children[vc_no]->get_abated_terminals();
}


void Portchan_node::enqueue_packet(unsigned int packet_size, int port_no, int vc_no, unsigned int term_id, unsigned int app_id)
{
    packet_count += packet_size;
    term_count_map[term_id] += packet_size;
    app_count_map[app_id] += packet_size;
    app_to_terminal_counter[app_id][term_id]  += packet_size;

    switch(type)
    {
        case ROOT:
            children[port_no]->enqueue_packet(packet_size, port_no, vc_no, term_id, app_id);
        break;
        case PORT:
            children[vc_no]->enqueue_packet(packet_size, port_no, vc_no, term_id, app_id);
        break;
        case VC://do nothing else
        break;
        default:
            tw_error(TW_LOC, "Portchan_node enqueue: Invalid node type\n");
    }
}

void Portchan_node::dequeue_packet(unsigned int packet_size, int port_no, int vc_no, unsigned int term_id, unsigned int app_id)
{
    assert(packet_count >= packet_size);

    packet_count -= packet_size;
    term_count_map[term_id] -= packet_size;
    if(term_count_map.at(term_id)== 0)
        term_count_map.erase(term_id);
    app_count_map[app_id] -= packet_size;
    if(app_count_map.at(app_id) == 0)
        app_count_map.erase(app_id);

    assert(app_to_terminal_counter[app_id][term_id] >= packet_size);
    app_to_terminal_counter[app_id][term_id] -= packet_size;
    if(app_to_terminal_counter.at(app_id).at(term_id) == 0)
        app_to_terminal_counter.at(app_id).erase(term_id);
    if(app_to_terminal_counter.at(app_id).begin() == app_to_terminal_counter.at(app_id).end())
        app_to_terminal_counter.erase(app_id);
    
    switch(type)
    {
        case ROOT:
            children[port_no]->dequeue_packet(packet_size, port_no, vc_no, term_id, app_id);
        break;
        case PORT:
            children[vc_no]->dequeue_packet(packet_size, port_no, vc_no, term_id, app_id);
        break;
        case VC://do nothing else
        break;
        default:
            tw_error(TW_LOC, "Portchan_node dequeue: Invalid node type\n");
    }
}

/************* GLOBALS ********************************************/
static map< tw_lpid, int > router_lpid_to_id_map = map<tw_lpid, int>();
static map< tw_lpid, int > terminal_lpid_to_id_map = map<tw_lpid, int>();
static map< int, tw_lpid > router_id_to_lpid_map = map<int, tw_lpid>();
static map< int, tw_lpid > terminal_id_to_lpid_map = map<int, tw_lpid>();

static string congestion_pattern_set_filepath;
static set<unsigned long> congestion_pattern_set = set<unsigned long>();

static string decongestion_pattern_set_filepath;
static set<unsigned long> decongestion_pattern_set = set<unsigned long>();

/************* PROTOTYPES *****************************************/






/************* HELPER FUNCTIONS ***********************************/

/* convert GiB/s and bytes to ns */
static tw_stime bytes_to_ns(uint64_t bytes, double GB_p_s)
{
    tw_stime time;

    /* bytes to GB */
    time = ((double)bytes)/(1024.0*1024.0*1024.0);
    /* GiB to s */
    time = time / GB_p_s;
    /* s to ns */
    time = time * 1000.0 * 1000.0 * 1000.0;

    return(time);
}

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

congestion_control_message* cc_msg_rc_storage_create()
{
    congestion_control_message *msg = (congestion_control_message*)calloc(1, sizeof(congestion_control_message));
    return msg;
}

void cc_msg_rc_storage_delete(void * ptr)
{
    congestion_control_message *rc_msg = (congestion_control_message*)ptr;
    if (rc_msg->size_abated > 0)
        free(rc_msg->danger_rc_abated);
    if (rc_msg->size_deabated > 0)
        free(rc_msg->danger_rc_deabated);
    free(ptr);
}

void congestion_control_register_terminal_lpname(char lp_name[])
{
    strcpy(terminal_lp_name, lp_name);
}

void congestion_control_register_router_lpname(char lp_name[])
{
    strcpy(router_lp_name, lp_name);
}

int congestion_control_set_jobmap(struct codes_jobmap_ctx *ctx, int net_id)
{
    // if (g_congestion_control_enabled == 0) 
    //     return -1;

    if (ctx == NULL) {
        // tw_printf("Congestion Control: No jobmap passed to control module - Causation Detection Not Enabled")
        return -2;
    }
    else {
        jobmap_ctx = ctx;
        network_id = net_id;
        is_jobmap_set = 1;
        return 0;
    }
}

int congestion_control_is_jobmap_set()
{
    return is_jobmap_set;
}

struct codes_jobmap_ctx* congestion_control_get_jobmap()
{
    if (is_jobmap_set == 0)
        tw_error(TW_LOC,"Codes Jobmap was never passed to the congestion controller\n");
    else
        return jobmap_ctx;
}

int congestion_control_get_job_count()
{
    if (is_jobmap_set == 0)
        return 1;
    else
        return codes_jobmap_get_num_jobs(jobmap_ctx);
}

struct pair_hash {
    inline std::size_t operator()(const std::pair<unsigned int,unsigned int> & v) const {
        return v.first*28657+v.second;
    }
};

//Router Local Controller
void cc_router_local_controller_init(rlc_state *s, tw_lp* lp, int total_terminals, int router_id, int radix, int num_vcs_per_port, int* vc_sizes, double* bandwidths,  int* workload_finished_flag_ptr)
{
    // printf("CC LOCAL INIT!\n");
    s->params = (cc_param*)calloc(1, sizeof(cc_param));
    cc_param *p = s->params;

    p->router_radix = radix;
    p->router_vc_per_port = num_vcs_per_port;
    s->router_vc_sizes_on_each_port = vc_sizes;
    s->router_bandwidths_on_each_port = bandwidths;

    int rc = configuration_get_value_double(&config, "PARAMS", "cc_single_port_congestion_threshold", NULL, &p->single_port_congestion_threshold);
    if(rc) {
        p->single_port_congestion_threshold = .30;
    }

    rc = configuration_get_value_double(&config, "PARAMS", "cc_single_port_decongestion_threshold", NULL, &p->single_port_decongestion_threshold);
    if(rc) {
        p->single_port_decongestion_threshold = .05;
    }

    rc = configuration_get_value_double(&config, "PARAMS", "cc_single_port_aggressor_usage_threshold", NULL, &p->single_port_aggressor_usage_threshold);
    if(rc) {
        p->single_port_aggressor_usage_threshold = .10;
    }

    rc = configuration_get_value_double(&config, "PARAMS", "cc_minimum_abatement_active_time", NULL, &p->minimum_abatement_time);
    if(rc) {
        p->minimum_abatement_time = 10000000;
    }

    p->notification_latency = g_congestion_control_notif_latency;

    s->router_id = router_id;
    s->lp = lp;
    // s->vc_occupancy_ptr = &vc_occupancy;
    // s->port_vc_to_term_count_map = map<pair<int,int>,map<int,int> >();

    s->workloads_finished_flag_ptr = workload_finished_flag_ptr;
    s->output_ports = set<int>();
    s->packet_counting_tree = new Portchan_node(ROOT, radix, num_vcs_per_port);
}

void cc_router_local_controller_add_output_port(rlc_state *s, int port_no)
{
    s->output_ports.insert(port_no);
}

void cc_router_local_congestion_event(rlc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
{
    switch(msg->type)
    {
        default:
                tw_error(TW_LOC, "Invalid event at cc router local congestion event");
            break;
    }
}

void cc_router_local_congestion_event_rc(rlc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
{
    switch(msg->type)
    {
        default:
            tw_error(TW_LOC, "Invalid event at cc router local congestion event rc");
            break;
    }
}

void cc_router_local_congestion_event_commit(rlc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
{
    switch(msg->type)
    {
        default:
            tw_error(TW_LOC, "Invalid event at cc router local congestion event commit");
            break;
    }
}

void cc_router_received_packet(rlc_state *s, tw_lp *lp, unsigned int packet_size, int port_no, int vc_no, int term_id, int app_id, congestion_control_message *rc_msg)
{
    // s->port_vc_to_term_count_map[make_pair(port_no, vc_no)][term_id]++;
    if (s->output_ports.size() > 0 && s->output_ports.count(port_no) > 0) {
        s->packet_counting_tree->enqueue_packet(packet_size, port_no, vc_no, term_id, app_id);

        double cur_expire_time = s->packet_counting_tree->get_next_possible_port_normal_time(port_no);
        rc_msg->saved_expire_time = cur_expire_time;

        if (s->packet_counting_tree->is_port_congested(port_no) == true) {
            // Then abatement for this port is already active, we need to see if this is a packet from a new aggresssor
            // regardless, we also need to extend the abatement policy

            if (s->packet_counting_tree->is_abated_terminal(term_id) == false)
            {
                int this_app_occupancy = s->packet_counting_tree->get_app_count_map_by_port(port_no)[app_id];
                unsigned long long port_occupancy = s->packet_counting_tree->get_packet_count_by_port(port_no);
                if ((this_app_occupancy / port_occupancy) >= s->params->single_port_aggressor_usage_threshold) {
                    //then we haven't yet abated the terminal that send this message and we are currently in congestion
                    rc_msg->received_new_while_congested = true;
                    rc_msg->saved_term_id = term_id;

                    //need to send abatement to it
                    tw_lpid term_lpgid = codes_mapping_get_lpid_from_relative(term_id, NULL, terminal_lp_name, NULL, 0);
                    congestion_control_message *c_msg;
                    tw_event *e = model_net_method_congestion_event(term_lpgid, s->params->notification_latency, s->lp, (void**)&c_msg, NULL);
                    c_msg->type = CC_SIGNAL_ABATE;
                    c_msg->app_id = app_id;
                    tw_event_send(e);

                    s->packet_counting_tree->mark_abated_terminal(port_no, term_id); //increments the abatement counter
                }
            }

            s->packet_counting_tree->set_next_possible_port_normal_time(port_no, cur_expire_time+bytes_to_ns(packet_size, s->router_bandwidths_on_each_port[port_no]));
        }
        else { //TODO NOTE: this if else only is valid if we're only monitoring ports
            cc_router_congestion_check(s, lp, port_no, vc_no, rc_msg);
        }
    }
}

void cc_router_received_packet_rc(rlc_state *s, tw_lp *lp, unsigned int packet_size, int port_no, int vc_no, int term_id, int app_id, congestion_control_message *rc_msg)
{
    if (s->output_ports.size() > 0 && s->output_ports.count(port_no) > 0) {
        cc_router_congestion_check_rc(s, lp, port_no, vc_no, rc_msg);

        if (rc_msg->received_new_while_congested == true)
        {
            s->packet_counting_tree->mark_unabated_terminal(port_no, rc_msg->saved_term_id);
        }

        s->packet_counting_tree->set_next_possible_port_normal_time(port_no, rc_msg->saved_expire_time);


        s->packet_counting_tree->dequeue_packet(packet_size, port_no, vc_no, term_id, app_id);
    }
}

void cc_router_forwarded_packet(rlc_state *s, tw_lp *lp, unsigned int packet_size, int port_no, int vc_no, int term_id, int app_id, congestion_control_message *rc_msg)
{
    if (s->output_ports.size() > 0 && s->output_ports.count(port_no) > 0) {
        s->packet_counting_tree->dequeue_packet(packet_size, port_no, vc_no, term_id, app_id);
        cc_router_congestion_check(s, lp, port_no, vc_no, rc_msg);
    }
}

void cc_router_forwarded_packet_rc(rlc_state *s, tw_lp *lp, unsigned int packet_size, int port_no, int vc_no, int term_id, int app_id, congestion_control_message *rc_msg)
{
    if (s->output_ports.size() > 0 && s->output_ports.count(port_no) > 0) {
        cc_router_congestion_check_rc(s, lp, port_no, vc_no, rc_msg);
        s->packet_counting_tree->enqueue_packet(packet_size, port_no, vc_no, term_id, app_id);
    }
}

void cc_router_congestion_check(rlc_state *s, tw_lp *lp, int port_no, int vc_no, congestion_control_message *rc_msg)
{
    int num_rngs = 0;
    //PORT CONGESTION/DECONGESTION
    int port_size = s->router_vc_sizes_on_each_port[port_no]*s->params->router_vc_per_port;
    int port_congestion_threshold_size = s->params->single_port_congestion_threshold * port_size;
    int port_decongestion_threshold_size = s->params->single_port_decongestion_threshold * port_size;

    unsigned long long port_occupancy = s->packet_counting_tree->get_packet_count_by_port(port_no);
    rc_msg->size_abated = 0;

    //have we already registered congestion on this port?
    if(s->packet_counting_tree->is_port_congested(port_no) == false)
    {
        if (port_occupancy >= port_congestion_threshold_size) {
            s->packet_counting_tree->set_port_congestion_state(port_no, true);

            tw_stime expiration_delay = 2*bytes_to_ns(port_congestion_threshold_size-port_decongestion_threshold_size, s->router_bandwidths_on_each_port[port_no]);
            // expiration_delay = max(expiration_delay, s->params->minimum_abatement_time);
            s->packet_counting_tree->set_next_possible_port_normal_time(port_no, tw_now(lp)+expiration_delay);

            rc_msg->to_congest = 1;
            // printf("CONGESTION DETECTED %llu\n", port_occupancy);
            set< pair<unsigned int,unsigned int> > aggressor_term_ids;

            map<unsigned int, unsigned long long> app_map = s->packet_counting_tree->get_app_count_map_by_port(port_no);
            map<unsigned int, unsigned long long>::iterator it = app_map.begin();
            for (; it != app_map.end(); it++)
            {
                if ((it->second / port_occupancy) >= s->params->single_port_aggressor_usage_threshold)
                {
                    map<unsigned int, unsigned long long> tcm = s->packet_counting_tree->get_term_count_map_by_port_from_app(port_no, it->first);
                    map<unsigned int, unsigned long long>::iterator agg_it = tcm.begin();
                    for (; agg_it != tcm.end(); agg_it++)
                    {
                        aggressor_term_ids.insert( make_pair(agg_it->first, it->first) );
                    }
                }
            }

            //create a store for the list of terms getting a mark of abatement
            rc_msg->size_abated = aggressor_term_ids.size();
            rc_msg->danger_rc_abated = (unsigned int*)malloc(sizeof(unsigned int)*rc_msg->size_abated);
            // printf("size abated %d\n", rc_msg->size_abated);
            int stored = 0;

            //send an abatement signal to each aggressor terminal on this port if they aren't already abated
            set< pair<unsigned int, unsigned int> >::iterator set_it = aggressor_term_ids.begin();
            for(; set_it != aggressor_term_ids.end(); set_it++)
            {
                unsigned int term_id = set_it->first;
                unsigned int app_id = set_it->second;

                //store into the rc storage
                rc_msg->danger_rc_abated[stored] = term_id;
                stored++;

                //check if its abated already, then mark the additional abatement
                if (s->packet_counting_tree->is_abated_terminal(term_id) == 0)
                {
                    tw_lpid term_lpgid = codes_mapping_get_lpid_from_relative(term_id, NULL, terminal_lp_name, NULL, 0);
                    congestion_control_message *c_msg;
                    tw_event *e = model_net_method_congestion_event(term_lpgid, s->params->notification_latency, s->lp, (void**)&c_msg, NULL);
                    c_msg->type = CC_SIGNAL_ABATE;
                    c_msg->app_id = app_id;
                    tw_event_send(e);
                }
                s->packet_counting_tree->mark_abated_terminal(port_no, term_id); //increments the abatement counter
            }
        }
    }
    else
    {
        if (tw_now(lp) > s->packet_counting_tree->get_next_possible_port_normal_time(port_no)) {
            if (port_occupancy < port_decongestion_threshold_size) {
                ///decongestion on this port!
                rc_msg->to_decongest = 1;
                s->packet_counting_tree->set_port_congestion_state(port_no, false);
                // printf("Want to send Normal\n");

                //get the terminals abated by this port
                set<unsigned int> abated_terms = s->packet_counting_tree->get_abated_terminals(port_no);
                set<unsigned int>::iterator it = abated_terms.begin();

                //store the list of terms receiving a removal of a mark of abatement
                rc_msg->size_deabated = abated_terms.size();
                rc_msg->danger_rc_deabated = (unsigned int*)malloc(sizeof(unsigned int)*rc_msg->size_deabated);
                // printf("size deabated %d\n", rc_msg->size_deabated);
                int stored = 0;

                for(; it != abated_terms.end(); it++)
                {
                    rc_msg->danger_rc_deabated[stored] = *it;
                    stored++;

                    //mark them unabated on this port
                    s->packet_counting_tree->mark_unabated_terminal(port_no, *it);
                    if (s->packet_counting_tree->is_abated_terminal(*it) == 0)
                    {
                        // printf("Sending Normal\n");
                        //if any are no longer marked abated at all, then send a normal signal
                        tw_lpid term_lpgid = codes_mapping_get_lpid_from_relative(*it, NULL, terminal_lp_name, NULL, 0);
                        congestion_control_message *c_msg;
                        tw_event *e = model_net_method_congestion_event(term_lpgid, s->params->notification_latency, s->lp, (void**)&c_msg, NULL);
                        c_msg->type = CC_SIGNAL_NORMAL;
                        tw_event_send(e);
                    }
                }
            }
        }

        // //check if any terminals have left the port entirely
        // set<unsigned int> abated_terms = s->packet_counting_tree->get_abated_terminals(port_no);
        // set<unsigned int>::iterator it = abated_terms.begin();
        // for(; it != abated_terms.end(); it++)
        // {
        //     if (s->packet_counting_tree->get_packet_count_by_port_from_term(port_no, *it) == 0)
        //     {
        //         //mark them unabated on this port
        //         s->packet_counting_tree->mark_unabated_terminal(port_no, *it);
        //         if (s->packet_counting_tree->is_abated_terminal(*it) == false)
        //         {
        //             //if any are no longer marked abated at all, then send a normal signal
        //             tw_lpid term_lpgid = codes_mapping_get_lpid_from_relative(*it, NULL, terminal_lp_name, NULL, 0);
        //             congestion_control_message *c_msg;
        //             tw_event *e = model_net_method_congestion_event(term_lpgid, .1, s->lp, (void**)&c_msg, NULL);
        //             c_msg->type = CC_SIGNAL_NORMAL;
        //             tw_event_send(e);
        //         }
        //     }
        // }
    }
    //other congestion checks would go here
}

void cc_router_congestion_check_rc(rlc_state *s, tw_lp *lp, int port_no, int vc_no, congestion_control_message *rc_msg)
{
    if (rc_msg->to_congest)
    {
        //used danger_rc_abated to store size_abated unsigned int for terminal IDs
        for (int i = 0; i < rc_msg->size_abated; i++)
        {
            s->packet_counting_tree->mark_unabated_terminal(port_no, rc_msg->danger_rc_abated[i]);
        }
        s->packet_counting_tree->set_port_congestion_state(port_no, false);
    }
    if (rc_msg->to_decongest)
    {
        //used danger_rc_deabated to store size_deabated unsigned ints for terminal IDs
        for (int i = 0; i < rc_msg->size_deabated; i++)
        {
            s->packet_counting_tree->mark_abated_terminal(port_no, rc_msg->danger_rc_deabated[i]);
        }
        s->packet_counting_tree->set_port_congestion_state(port_no, true);
    }

    // for(int i = 0; i < rc_msg->num_cc_rngs; i++)
    // {
    //     cc_tw_rand_reverse_unif(s->lp);
    // }
}

void cc_router_local_controller_finalize(rlc_state *s)
{
    delete s->packet_counting_tree;
}

static double calculate_bandwidth_usage_percent(int bytes_transmitted, double max_configured_bandwidth, int multiplier)
{
    double max_bw = max_configured_bandwidth* 1024.0 * 1024.0 * 1024.0;
    double max_bw_per_ns = max_bw / (1000.0 * 1000.0 * 1000.0);
    double max_bytes_per_win = max_bw_per_ns * cc_bandwidth_monitoring_window;
    double percent_bw = (bytes_transmitted/ max_bytes_per_win);
    // printf("%.2f percent bw\n", percent_bw);

    return percent_bw;
}

void cc_terminal_process_bandwidth_check(tlc_state *s, congestion_control_message *msg, tw_lp *lp)
{
    double usage_percent = calculate_bandwidth_usage_percent(s->ejected_packet_bytes, s->params->terminal_configured_bandwidth, 1); //multiplier for multiple rails but right now we're just using 1
    double removed_window = s->ejected_rate_windows[s->window_epoch % cc_bandwidth_rolling_window_count];
    msg->saved_window = removed_window;
    s->ejected_rate_windows[s->window_epoch % cc_bandwidth_rolling_window_count] = usage_percent;

    msg->saved_rate = s->cur_average_rate;
    s->cur_average_rate = s->cur_average_rate * cc_bandwidth_rolling_window_count;
    s->cur_average_rate = (s->cur_average_rate - removed_window + usage_percent)/cc_bandwidth_rolling_window_count;
    if (s->cur_average_rate < 0)
        s->cur_average_rate = 0;
    
    msg->saved_bw = s->current_injection_bandwidth_coef;
    s->current_injection_bandwidth_coef = s->cur_average_rate;
    if (s->is_abatement_active)
        msg->saved_new_bw = s->current_injection_bandwidth_coef; //for commit m processing
    else
        msg->saved_new_bw = 1.0;
    msg->msg_time = tw_now(lp);

    msg->saved_ejected_bytes = s->ejected_packet_bytes;
    s->ejected_packet_bytes = 0;
    s->window_epoch++;

    //schedule for the next heartbeat message if there's still workloads left
    if (*s->workloads_finished_flag_ptr == 0) {
        congestion_control_message *hb_msg;
        tw_event * hb_e = model_net_method_congestion_event(s->lp->gid, cc_bandwidth_monitoring_window, s->lp, (void**)&hb_msg, NULL);
        hb_msg->type = CC_BANDWIDTH_CHECK;
        tw_event_send(hb_e);
    }
}

void cc_terminal_process_bandwidth_check_rc(tlc_state *s, congestion_control_message *msg, tw_lp *lp)
{
    s->window_epoch--;
    s->ejected_packet_bytes = msg->saved_ejected_bytes;
    s->current_injection_bandwidth_coef = msg->saved_bw;
    s->cur_average_rate = msg->saved_rate;
    s->ejected_rate_windows[s->window_epoch % cc_bandwidth_rolling_window_count] = msg->saved_window;
}

void cc_terminal_local_controller_init(tlc_state *s, tw_lp *lp, int terminal_id, int* workload_finished_flag_ptr)
{
    s->params = (cc_param*)calloc(1, sizeof(cc_param));
    cc_param *p = s->params;
    s->lp = lp;
    s->terminal_id = terminal_id;

    //below is a brute force way to figure out what application a terminal is serving
    // codes_mapping_get_lp_info(lp->gid, lp_group_name, &mapping_grp_id, NULL,
    //         &mapping_type_id, NULL, &mapping_rep_id, &mapping_offset);

    // struct codes_jobmap_ctx* ctx = congestion_control_get_jobmap();
    // char workload_lp_name[MAX_NAME_LENGTH];
    // workload_lp_name[0] = '\0';
    // configuration_get_value(&config, "PARAMS", "cc_workload_lpname", NULL, workload_lp_name, MAX_NAME_LENGTH);
    // if (strlen(workload_lp_name) <= 0) {
    //     strcpy(workload_lp_name, "nw-lp"); //default workload LP name
    // }
    // int num_workload_lps = codes_mapping_get_lp_count(lp_group_name, 0, workload_lp_name, NULL, 0);

    // for (int work_rel_id = 0; work_rel_id < num_workload_lps; work_rel_id++) {
    //     tw_lpid work_gid = codes_mapping_get_lpid_from_relative(work_rel_id, NULL, workload_lp_name, NULL, 0);
    //     struct codes_jobmap_id job_ident = codes_jobmap_to_local_id(work_rel_id, ctx); //work rel id is job global id - TODO DOUBLE CHECK
    //     tw_lpid attached_term_id = model_net_find_local_device(DRAGONFLY_DALLY, NULL, 0, work_gid);

    //     if (attached_term_id == lp->gid && job_ident.job != -1) {
    //         s->app_id = job_ident.job; //right now there's only one app per terminal, this may change in the future
    //         // s->workload_lpid_to_app_id[work_gid] = job_ident.job;
    //         // s->app_ids.insert(job_ident.job);
    //     }
    // }

    int rc = configuration_get_value_double(&config, "PARAMS", "cn_bandwidth", NULL, &p->terminal_configured_bandwidth);
    if(rc) {
        if(!g_tw_mynode)
            tw_error(TW_LOC, "Bandwidth of terminal links not specified.");
    }

    rc = configuration_get_value_int(&config, "PARAMS", "chunk_size", NULL, &p->chunk_size);
    if(rc) {
        if(!g_tw_mynode)
            tw_error(TW_LOC, "Chunk size not specified.");
    }

    rc = configuration_get_value_double(&config, "PARAMS", "cc_measurement_period", NULL, &cc_bandwidth_monitoring_window);
    if (rc) {
        cc_bandwidth_monitoring_window = 10000;
    }

    p->notification_latency = g_congestion_control_notif_latency;

    s->is_abatement_active = false;
    s->current_injection_bandwidth_coef = 1;
    s->abatement_signal_count = 0;
    s->ejected_rate_windows = (double*)calloc(cc_bandwidth_rolling_window_count, sizeof(double));

    s->workloads_finished_flag_ptr = workload_finished_flag_ptr;

    congestion_control_message *hb_msg;
    tw_event * hb_e = model_net_method_congestion_event(lp->gid, cc_bandwidth_monitoring_window, s->lp, (void**)&hb_msg, NULL);
    hb_msg->type = CC_BANDWIDTH_CHECK;
    tw_event_send(hb_e);
}

void cc_terminal_send_ack(tlc_state *s, tw_lpid original_terminal_lpgid)
{
    congestion_control_message *ack_msg;
    tw_event * ack_e = model_net_method_congestion_event(original_terminal_lpgid, s->params->notification_latency + .00001, s->lp, (void**)&ack_msg, NULL);
    ack_msg->type = CC_SIM_ACK;
    tw_event_send(ack_e);
}

void cc_terminal_send_ack_rc(tlc_state *s)
{
    // cc_tw_rand_reverse_unif(s->lp);
}

void cc_terminal_receive_ack(tlc_state *s)
{
    s->ejected_packet_bytes += s->params->chunk_size;
}

void cc_terminal_receive_ack_rc(tlc_state *s)
{
    s->ejected_packet_bytes -= s->params->chunk_size;
}

void cc_terminal_start_abatement(tlc_state *s, congestion_control_message *msg)
{
    s->is_abatement_active = true;
    msg->saved_bw = s->current_injection_bandwidth_coef;
    s->current_injection_bandwidth_coef = s->cur_average_rate;
    if (s->current_injection_bandwidth_coef < .01)
        s->current_injection_bandwidth_coef = .01;
    // printf("%d from app%d: Abating at %.2f\n", s->terminal_id, app_id, s->current_injection_bandwidth_coef);
}

void cc_terminal_start_abatement_rc(tlc_state *s, congestion_control_message *msg)
{
    s->current_injection_bandwidth_coef = msg->saved_bw;
    s->is_abatement_active = false;
}

void cc_terminal_end_abatement(tlc_state *s, congestion_control_message *msg)
{
    s->is_abatement_active = false;
    msg->saved_bw = s->current_injection_bandwidth_coef;
    s->current_injection_bandwidth_coef = 1;
    // printf("Returning to normal\n");
}

void cc_terminal_end_abatement_rc(tlc_state *s, congestion_control_message *msg)
{
    s->current_injection_bandwidth_coef = msg->saved_bw;
    s->is_abatement_active = true;
}

void cc_terminal_receive_abatement_signal(tlc_state *s, congestion_control_message *msg)
{
    s->abatement_signal_count++;
    if (s->abatement_signal_count == 1) //if > 1, then it's already started
        cc_terminal_start_abatement(s, msg);
}

void cc_terminal_receive_abatement_signal_rc(tlc_state *s, congestion_control_message *msg)
{
    s->abatement_signal_count--;
    if (s->abatement_signal_count == 0)
        cc_terminal_start_abatement_rc(s, msg);
}

void cc_terminal_receive_normal_signal(tlc_state *s, congestion_control_message *msg)
{
    s->abatement_signal_count--;
    if (s->abatement_signal_count == 0)
        cc_terminal_end_abatement(s, msg);
}

void cc_terminal_receive_normal_signal_rc(tlc_state *s, congestion_control_message *msg)
{
    s->abatement_signal_count++;
    if (s->abatement_signal_count == 1)
        cc_terminal_end_abatement_rc(s, msg);
}

double cc_terminal_get_current_injection_bandwidth_coef(tlc_state *s)
{
    if (!s->is_abatement_active)
        return 1;
    else if (s->abatement_signal_count > 0) {
        double ret_val;
        double calculated_injection = s->current_injection_bandwidth_coef;
        // double min_injection = (1.0/codes_jobmap_get_num_ranks(s->app_id, jobmap_ctx)); //TODO s->app_id is never set because it's hard for the network side to know this
        double min_injection = (1.0/100.0);
        if (calculated_injection < min_injection)
            ret_val = min_injection;
        else
            ret_val = calculated_injection;
        
        if (ret_val < 0)
            printf("%.2f = %.2f / %d\n", ret_val, s->current_injection_bandwidth_coef, s->abatement_signal_count);
        if (ret_val > 1)
            ret_val = 1;
        assert(ret_val <= 1);
        assert(ret_val > 0);
        return ret_val;
    }
    else {
        tw_error(TW_LOC, "Abatement inactive but signal count > 0\n");
        return 1.0;
    }
}

bool cc_terminal_is_abatement_active(tlc_state *s)
{
    return s->is_abatement_active;
}


void cc_terminal_local_congestion_event(tlc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
{
    switch(msg->type)
    {
        case CC_SIGNAL_ABATE:
            cc_terminal_receive_abatement_signal(s, msg);
        break;
        case CC_SIGNAL_NORMAL:
            cc_terminal_receive_normal_signal(s, msg);
        break;
        case CC_BANDWIDTH_CHECK:
            cc_terminal_process_bandwidth_check(s, msg, lp);
        break;
        case CC_SIM_ACK:
            cc_terminal_receive_ack(s);
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
        case CC_SIGNAL_ABATE:
            cc_terminal_receive_abatement_signal_rc(s, msg);
        break;
        case CC_SIGNAL_NORMAL:
            cc_terminal_receive_normal_signal_rc(s, msg);
        break;
        case CC_BANDWIDTH_CHECK:
            cc_terminal_process_bandwidth_check_rc(s, msg, lp);
        break;
        case CC_SIM_ACK:
            cc_terminal_receive_ack_rc(s);
        break;
        default:
                tw_error(TW_LOC, "Invalid event at cc terminal local congestion event");
            break;
    }
}

void cc_terminal_local_congestion_event_commit(tlc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
{
    switch(msg->type)
    {
        case CC_BANDWIDTH_CHECK:
            if (OUTPUT_BANDWIDTHS) {
                int written1;
                char bandwidth_filename[128];
                written1 = sprintf(bandwidth_filename, "congestion-control-bandwidths");
                bandwidth_filename[written1] = '\0';

                char tag_line[32];
                int written;
                written = sprintf(tag_line, "%d %.2f %.5f\n",s->terminal_id, msg->saved_new_bw, msg->msg_time);
                lp_io_write(lp->gid, bandwidth_filename, written, tag_line);
            }
        break;
        case CC_SIGNAL_ABATE:
        break;
        case CC_SIGNAL_NORMAL:
        break;
        case CC_SIM_ACK:
        break;
        default:
            tw_error(TW_LOC, "Invalid event at cc terminal local congestion event commit");
            break;
    }
}