#include <codes/congestion-controller-core.h>
#include <codes/congestion-controller-model.h>
#include <codes/codes-jobmap.h>
#include <codes/model-net-lp.h>
#include <codes/codes_mapping.h>
#include <map>
#include <vector>
#include <set>
#include <unordered_set>
#include <string.h>
#include <string>

#define PERMANENT_ABATEMENT 1

using namespace std;

struct codes_jobmap_ctx *jobmap_ctx;
static int is_jobmap_set = 0;
static int network_id = 0;

unsigned long long stalled_packet_counter = 0;
unsigned long long stalled_nic_counter = 0;

/************* DEFINITIONS ****************************************/
int g_congestion_control_enabled;

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

unordered_map<unsigned int, unsigned long long> Portchan_node::get_term_count_map()
{
    return term_count_map;
}

unordered_map<unsigned int, unsigned long long> Portchan_node::get_term_count_map_by_port(int port_no)
{
    if(type != ROOT)
        tw_error(TW_LOC, "Portchan_node : Invalid node type for this action\n");

    return children[port_no]->get_term_count_map();
}

unordered_map<unsigned int, unsigned long long> Portchan_node::get_term_count_map_by_port_vc(int port_no, int vc_no)
{
    if(type != ROOT)
        tw_error(TW_LOC, "Portchan_node : Invalid node type for this action\n");

    return children[port_no]->children[vc_no]->get_term_count_map();
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
}

unordered_set<unsigned int> Portchan_node::get_abated_terminals()
{
    return abated_terminals_this_node;
}

unordered_set<unsigned int> Portchan_node::get_abated_terminals(int port_no)
{
    if(type != ROOT)
        tw_error(TW_LOC, "Portchan_node : Invalid node type for this action\n");

    return children[port_no]->get_abated_terminals();
}

unordered_set<unsigned int> Portchan_node::get_abated_terminals(int port_no, int vc_no)
{
    if(type != ROOT)
        tw_error(TW_LOC, "Portchan_node : Invalid node type for this action\n");

    return children[port_no]->children[vc_no]->get_abated_terminals();
}


void Portchan_node::enqueue_packet(unsigned int packet_size, int port_no, int vc_no, unsigned int term_id)
{
    packet_count += packet_size;
    term_count_map[term_id] += packet_size;

    switch(type)
    {
        case ROOT:
            children[port_no]->enqueue_packet(packet_size, port_no, vc_no, term_id);
        break;
        case PORT:
            children[vc_no]->enqueue_packet(packet_size, port_no, vc_no, term_id);
        break;
        case VC://do nothing else
        break;
        default:
            tw_error(TW_LOC, "Portchan_node enqueue: Invalid node type\n");
    }
}

void Portchan_node::dequeue_packet(unsigned int packet_size, int port_no, int vc_no, unsigned int term_id)
{
    assert(packet_count >= packet_size);

    packet_count -= packet_size;
    term_count_map[term_id] -= packet_size;
    if(term_count_map.at(term_id)== 0)
        term_count_map.erase(term_id);
    
    switch(type)
    {
        case ROOT:
            children[port_no]->dequeue_packet(packet_size, port_no, vc_no, term_id);
        break;
        case PORT:
            children[vc_no]->dequeue_packet(packet_size, port_no, vc_no, term_id);
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
static unordered_set<unsigned long> congestion_pattern_set = unordered_set<unsigned long>();

static string decongestion_pattern_set_filepath;
static unordered_set<unsigned long> decongestion_pattern_set = unordered_set<unsigned long>();

/************* PROTOTYPES *****************************************/






/************* HELPER FUNCTIONS ***********************************/

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

void congestion_control_register_terminal_lpname(char lp_name[])
{
    strcpy(terminal_lp_name, lp_name);
}

void congestion_control_register_router_lpname(char lp_name[])
{
    strcpy(router_lp_name, lp_name);
}

//Router Local Controller
void cc_router_local_controller_init(rlc_state *s, tw_lp* lp, int total_terminals, int router_id, int radix, int num_vcs_per_port, int* vc_sizes)
{
    // printf("CC LOCAL INIT!\n");
    s->params = (cc_param*)calloc(1, sizeof(cc_param));
    cc_param *p = s->params;

    p->router_radix = radix;
    p->router_vc_per_port = num_vcs_per_port;
    p->router_vc_sizes_on_each_port = vc_sizes;

    // p->single_vc_congestion_threshold = .50;
    p->single_port_congestion_threshold = .50;
    p->single_port_decongestion_threshold = .20;
    // p->single_router_congestion_threshold = .10;

    s->router_id = router_id;
    s->lp = lp;
    // s->vc_occupancy_ptr = &vc_occupancy;
    // s->port_vc_to_term_count_map = map<pair<int,int>,map<int,int> >();

    s->packet_counting_tree = new Portchan_node(ROOT, radix, num_vcs_per_port);
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

void cc_router_received_packet(rlc_state *s, unsigned int packet_size, int port_no, int vc_no, int term_id)
{
    // s->port_vc_to_term_count_map[make_pair(port_no, vc_no)][term_id]++;
    s->packet_counting_tree->enqueue_packet(packet_size, port_no, vc_no, term_id);
    cc_router_congestion_check(s, port_no, vc_no);
}

extern void cc_router_received_packet_rc(rlc_state *s, unsigned int packet_size, int port_no, int vc_no, int term_id)
{
    s->packet_counting_tree->dequeue_packet(packet_size, port_no, vc_no, term_id);
}

void cc_router_forwarded_packet(rlc_state *s, unsigned int packet_size, int port_no, int vc_no, int term_id)
{
    s->packet_counting_tree->dequeue_packet(packet_size, port_no, vc_no, term_id);
    cc_router_congestion_check(s, port_no, vc_no);
}

void cc_router_forwarded_packet_rc(rlc_state *s, unsigned int packet_size, int port_no, int vc_no, int term_id)
{
    s->packet_counting_tree->enqueue_packet(packet_size, port_no, vc_no, term_id);
}

void cc_router_congestion_check(rlc_state *s, int port_no, int vc_no)
{

    //PORT CONGESTION/DECONGESTION
    int port_size = s->params->router_vc_sizes_on_each_port[port_no]*s->params->router_vc_per_port;

    unsigned long long port_occupancy = s->packet_counting_tree->get_packet_count_by_port(port_no);

    if(s->packet_counting_tree->is_port_congested(port_no) == false)
    {
        if (port_occupancy >= s->params->single_port_congestion_threshold * port_size) {
            // printf("CONGESTION DETECTED %llu\n", port_occupancy);
            vector<int> term_ids;
            unordered_map<unsigned int, unsigned long long> term_map = s->packet_counting_tree->get_term_count_map_by_port(port_no);
            unordered_map<unsigned int, unsigned long long>::iterator it = term_map.begin();
            for(; it != term_map.end(); it++)
            {
                term_ids.push_back(it->first);
            }

            //send an abatement signal to each terminal on this port if they aren't already abated
            for(int i = 0; i < term_ids.size(); i++)
            {
                unsigned int term_id = term_ids[i];
                //check if its abated already, then mark the additional abatement
                if (s->packet_counting_tree->is_abated_terminal(term_id) == 0)
                {
                    s->packet_counting_tree->mark_abated_terminal(port_no, term_id);
                    tw_lpid term_lpgid = codes_mapping_get_lpid_from_relative(term_id, NULL, terminal_lp_name, NULL, 0);
                    congestion_control_message *c_msg;
                    tw_event *e = model_net_method_congestion_event(term_lpgid, .1, s->lp, (void**)&c_msg, NULL);
                    c_msg->type = CC_SIGNAL_ABATE;
                    tw_event_send(e);
                }
            }
        }
    }
    else
    {
        if (port_occupancy < s->params->single_port_decongestion_threshold * port_size) {
            ///decongestion on this port!

            //get the terminals abated by this port
            unordered_set<unsigned int> abated_terms = s->packet_counting_tree->get_abated_terminals(port_no);
            unordered_set<unsigned int>::iterator it = abated_terms.begin();

            for(; it != abated_terms.end(); it++)
            {
                //mark them unabated on this port
                s->packet_counting_tree->mark_unabated_terminal(port_no, *it);
                if (s->packet_counting_tree->is_abated_terminal(*it) == false)
                {
                    //if any are no longer marked abated at all, then send a normal signal
                    tw_lpid term_lpgid = codes_mapping_get_lpid_from_relative(*it, NULL, terminal_lp_name, NULL, 0);
                    congestion_control_message *c_msg;
                    tw_event *e = model_net_method_congestion_event(term_lpgid, .1, s->lp, (void**)&c_msg, NULL);
                    c_msg->type = CC_SIGNAL_NORMAL;
                    tw_event_send(e);
                }
            }
        }

        // //check if any terminals have left the port entirely
        // unordered_set<unsigned int> abated_terms = s->packet_counting_tree->get_abated_terminals(port_no);
        // unordered_set<unsigned int>::iterator it = abated_terms.begin();
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

void cc_router_local_controller_finalize(rlc_state *s)
{
    delete s->packet_counting_tree;
}


void cc_terminal_local_controller_init(tlc_state *s)
{
    s->is_abatement_active = false;
    s->current_injection_bandwidth_coef = 1;
}

void cc_terminal_start_abatement(tlc_state *s)
{
    s->is_abatement_active = true;
    s->current_injection_bandwidth_coef = .1;
}

void cc_terminal_end_abatement(tlc_state *s)
{
    s->is_abatement_active = false;
    s->current_injection_bandwidth_coef = 1;
}

double cc_terminal_get_current_injection_bandwidth_coef(tlc_state *s)
{
    return s->current_injection_bandwidth_coef;
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
            cc_terminal_start_abatement(s);
        break;
        case CC_SIGNAL_NORMAL:
            cc_terminal_end_abatement(s);
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
        default:
            tw_error(TW_LOC, "Invalid event at cc terminal local congestion event rc");
            break;
    }
}

void cc_terminal_local_congestion_event_commit(tlc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp)
{
    // switch(msg->type)
    // {
    //     default:
    //         tw_error(TW_LOC, "Invalid event at cc terminal local congestion event commit");
    //         break;
    // }
}