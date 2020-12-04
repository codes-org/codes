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


// definition of class methods of Portchan_node class defined in congestion-controller-model.h
Portchan_node::Portchan_node(portchan_node_type pnt, int router_radix, int vcs_per_port)
{
    type = pnt;
    packet_count = 0;
    term_count_map = map<unsigned int, unsigned long long>();
    children = vector<Portchan_node*>();

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
    return term_count_map[term_id];
}

unsigned long long Portchan_node::get_packet_count_by_port(unsigned int port_no)
{
    if(type != ROOT)
        tw_error(TW_LOC, "Portchan_node : Invalid node type for this action\n");

    return children[port_no]->get_packet_count();
}

unsigned long long Portchan_node::get_packet_count_by_port_from_term(unsigned int port_no, unsigned int term_id)
{
    if(type != ROOT)
        tw_error(TW_LOC, "Portchan_node : Invalid node type for this action\n");
    
    return children[port_no]->get_packet_count_from_term(term_id);
}

unsigned long long Portchan_node::get_packet_count_by_port_vc(unsigned int port_no, unsigned int vc_no)
{
    if(type != ROOT)
        tw_error(TW_LOC, "Portchan_node : Invalid node type for this action\n");

    return children[port_no]->children[vc_no]->get_packet_count();
}

unsigned long long Portchan_node::get_packet_count_by_port_vc_from_term(unsigned int port_no, unsigned int vc_no, unsigned int term_id)
{
    if(type != ROOT)
        tw_error(TW_LOC, "Portchan_node : Invalid node type for this action\n");
    
    return children[port_no]->children[vc_no]->get_packet_count_from_term(term_id);
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
    if(term_count_map[term_id] == 0)
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

//Router Local Controller
void cc_router_local_controller_init(rlc_state *s, int total_terminals, int router_id, int radix, int num_vcs_per_port, int* vc_sizes)
{
    // printf("CC LOCAL INIT!\n");
    s->params = (cc_param*)calloc(1, sizeof(cc_param));
    cc_param *p = s->params;

    p->router_radix = radix;
    p->router_vc_per_port = num_vcs_per_port;
    p->router_vc_sizes_on_each_port = vc_sizes;

    // p->single_vc_congestion_threshold = .50;
    p->single_port_congestion_threshold = .70;
    // p->single_router_congestion_threshold = .10;

    s->router_id = router_id;
    // s->vc_occupancy_ptr = &vc_occupancy;
    // s->port_vc_to_term_count_map = map<pair<int,int>,map<int,int> >();

    s->packet_counting_tree = new Portchan_node(ROOT, radix, num_vcs_per_port);
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
    // s->port_vc_to_term_count_map[make_pair(port_no, vc_no)][term_id]--;
    // if (s->port_vc_to_term_count_map[make_pair(port_no, vc_no)][term_id] == 0)
    //     s->port_vc_to_term_count_map[make_pair(port_no, vc_no)].erase(term_id);
}

void cc_router_forwarded_packet_rc(rlc_state *s, unsigned int packet_size, int port_no, int vc_no, int term_id)
{
    s->packet_counting_tree->enqueue_packet(packet_size, port_no, vc_no, term_id);
}

void cc_router_congestion_check(rlc_state *s, int port_no, int vc_no)
{
    int port_size = s->params->router_vc_sizes_on_each_port[port_no]*s->params->router_vc_per_port;

    unsigned long long port_occupancy = s->packet_counting_tree->get_packet_count_by_port(port_no);

    if (port_occupancy >= s->params->single_port_congestion_threshold * port_size) {
        printf("CONGESTION DETECTED %llu\n", port_occupancy);
        
    }



}

void cc_router_local_controller_finalize(rlc_state *s)
{
    delete s->packet_counting_tree;
}
