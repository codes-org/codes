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
#include <unordered_map>
#include <string.h>
#include <string>

using namespace std;

/* Port/VC Channel tree structure
*  Used to efficiently determine how many packets are currently located on the router
*  and to what terminals they originated from. Each node contains a map of terminal ID to
*  the number of packets on said router, port, or vc. 
*  The root of this tree represents a 'router', the next level is ports, the last level is VCs
*  This structure is designed to obey the child sum property. The values stored in a parent represents
*  the sum of the values of its children
*/
typedef enum portchan_node_type {
    ROOT = 0,
    PORT = 1,
    VC = 2
} portchan_node_type;

class Portchan_node {
private:
    portchan_node_type type; //what level of the tree are we?
    unsigned long long packet_count; //number of packets on this node and children
    bool is_congested;
    unordered_set<unsigned int> abated_terminals_this_node;
    unordered_map<unsigned int, unsigned int> abated_terminal_child_counter; //maps terminal ID to number of children nodes that it is under abatement on
    unordered_map<unsigned int, unsigned long long> term_count_map; //maps terminal ID to number of packets on this node and children
    vector<Portchan_node *> children; //pointers to children

public:
    Portchan_node(portchan_node_type pnt, int router_radix, int vcs_per_port);
    ~Portchan_node();
    unsigned long long get_packet_count();
    unsigned long long get_packet_count_from_term(unsigned int term_id);
    unsigned long long get_packet_count_by_port(int port_no);
    unsigned long long get_packet_count_by_port_from_term(int port_no, unsigned int term_id);
    unsigned long long get_packet_count_by_port_vc(int port_no, int vc_no);
    unsigned long long get_packet_count_by_port_vc_from_term(int port_no, int vc_no, unsigned int term_id);
    unordered_map<unsigned int, unsigned long long> get_term_count_map();
    unordered_map<unsigned int, unsigned long long> get_term_count_map_by_port(int port_no);
    unordered_map<unsigned int, unsigned long long> get_term_count_map_by_port_vc(int port_no, int vc_no);
    bool is_router_congested();
    bool is_port_congested(int port_no);
    bool is_port_vc_congested(int port_no, int vc_no);
    void mark_abated_terminal(unsigned int term_id);
    void mark_abated_terminal(int port_no, unsigned int term_id);
    void mark_abated_terminal(int port_no, int vc_no, unsigned int term_id);
    void mark_unabated_terminal(unsigned int term_id);
    void mark_unabated_terminal(int port_no, unsigned int term_id);
    void mark_unabated_terminal(int port_no, int vc_no, unsigned int term_id);
    bool is_abated_terminal(unsigned int term_id);
    unordered_set<unsigned int> get_abated_terminals();
    unordered_set<unsigned int> get_abated_terminals(int port_no);
    unordered_set<unsigned int> get_abated_terminals(int port_no, int vc_no);
    void enqueue_packet(unsigned int packet_size, int port_no, int vc_no, unsigned int term_id);
    void dequeue_packet(unsigned int packet_size, int port_no, int vc_no, unsigned int term_id);
};

typedef struct cc_param
{    
    int router_radix;
    int router_vc_per_port;
    int* router_vc_sizes_on_each_port;
    int router_total_buffer_size;

    double single_vc_congestion_threshold;
    double single_port_congestion_threshold;
    double single_router_congestion_threshold;

    double single_vc_decongestion_threshold;
    double single_port_decongestion_threshold;
    double single_router_decongestion_threshold;

    double static_throttle_level;
} cc_param;

typedef struct rlc_state
{
    cc_param *params;
    tw_lp *lp;

    int router_id;

    Portchan_node *packet_counting_tree;
} rlc_state;

typedef struct tlc_state
{
    cc_param *params;

    int terminal_id;
    int app_id; //needs to be multiple if multiple jobs per terminal can exist.

    bool is_abatement_active;

    double current_injection_bandwidth_coef;
} tlc_state;


//event method links
void cc_router_local_congestion_event(rlc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp);
void cc_router_local_congestion_event_rc(rlc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp);
void cc_router_local_congestion_event_commit(rlc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp);
void cc_terminal_local_congestion_event(tlc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp);
void cc_terminal_local_congestion_event_rc(tlc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp);
void cc_terminal_local_congestion_event_commit(tlc_state *s, tw_bf *bf, congestion_control_message *msg, tw_lp *lp);


// ------------ Local controllers -----------------------
void cc_router_local_controller_init(rlc_state *s, tw_lp *lp, int total_terminals, int router_id, int radix, int num_vcs_per_port, int *vc_sizes);
void cc_router_received_packet(rlc_state *s, unsigned int packet_size, int port_no, int vc_no, int term_id);
void cc_router_received_packet_rc(rlc_state *s, unsigned int packet_size, int port_no, int vc_no, int term_id);
void cc_router_forwarded_packet(rlc_state *s, unsigned int packet_size, int port_no, int vc_no, int term_id);
void cc_router_forwarded_packet_rc(rlc_state *s, unsigned int packet_size, int port_no, int vc_no, int term_id);
void cc_router_congestion_check(rlc_state *s, int port_no, int vc_no);
void cc_router_local_controller_finalize(rlc_state *s);

void cc_terminal_local_controller_init(tlc_state *s);
void cc_terminal_start_abatement(tlc_state *s);
void cc_terminal_end_abatement(tlc_state *s);
double cc_terminal_get_current_injection_bandwidth_coef(tlc_state *s);
bool cc_terminal_is_abatement_active(tlc_state *s);


/************* LP Definition **************************************/




#endif /* end of include guard */