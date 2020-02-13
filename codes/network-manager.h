#ifndef CONNECTION_MANAGER_H
#define CONNECTION_MANAGER_H

/**
 * network-manager.h -- Simple, Readable, Connection management interface
 * Neil McGlohon
 *
 * Copyright (c) 2018 Rensselaer Polytechnic Institute
 */
#include <map>
#include <vector>
#include <set>
#include "codes/codes.h"
#include "codes/model-net.h"


using namespace std;

class ConnectionManager;
class NetworkManager;

/**
 * @brief Enum differentiating local router connection types from global.
 * Local connections will have router IDs ranging from [0,num_router_per_group)
 * whereas global connections will have router IDs ranging from [0,total_routers)
 */
enum ConnectionType
{
    CONN_LOCAL = 1,
    CONN_GLOBAL = 2,
    CONN_TERMINAL = 3,
    CONN_INJECTION = 4
};

enum ManagerType
{
    MAN_ROUTER = 1,
    MAN_TERMINAL = 2
};

/**
 * @brief Struct for basic link information for loading network
 */
struct Link_Info
{
    int src_gid;
    int dest_gid;
    int rail_id;
    ConnectionType conn_type;
};

/**
 * @brief Struct for complete connection information
 */
struct Connection
{
    int port; //port ID of the connection
    int src_lid; //local id of the source
    int src_gid; //global id of the source
    int src_group_id; //group id of the source
    int dest_lid; //local id of the destination
    int dest_gid; //global id of the destination
    int dest_group_id; //group id of the destination
    int is_failed; //boolean value for whether or not the link is considered failed
    int rail_or_planar_id; //rail ID if coming to/from terminal, planar ID if router-router
    ConnectionType conn_type; //type of the connection: CONN_LOCAL, CONN_GLOBAL, or CONN_TERMINAL
};

inline bool operator<(const Connection& lhs, const Connection& rhs)
{
  return lhs.port < rhs.port;
}

/**
 * @class NetworkManager
 * 
 * @brief
 * This class is meant to organize the connections that form the network as a whole
 * and also stores all connection managers.
 * 
 * @note
 * This class was designed with dragonfly type topologies in mind (local groups of routers)
 * Certain parts may not make sense for other topologies, they might work fine, but no guarantees.
 */
class NetworkManager {
    int _total_routers;
    int _total_terminals;
    int _num_rails;
    int _num_planes;
    int _num_routers_per_group;
    int _num_groups;
    int _num_lc_pr; //num local conns per router
    int _num_gc_pr; //num global conns per router
    int _num_cn_pr; //num cn conns per router
    int _num_unique_term_pr; //num unique terminals per router


    int _num_router_conns; //total number of router-router links
    int _num_router_terminal_conns; //total number of terminal links
    bool _link_failures_enabled;
    int _num_failed_router_conns;
    int _num_failed_router_terminal_conns;

    map< int, vector<Link_Info> > _router_link_failure_lists; //maps router ID to a vector of Link Infos that contain dest router GIDs that it has a FAILED connection to, one item for each failed connection
    map< int, vector<Link_Info> > _router_terminal_link_failure_lists; //maps router ID to a vector of Link Infos that contain dest terminal GIDs that it has a FAILED connection to, one item for each failed connection

    //storage maps - these are the 'owners' of the connections contained.
    map< int, vector< Connection* > > _router_connections_map; //maps router ID to a vector of all router-router connections that that router has
    map< int, vector< Connection* > > _router_terminal_connections_map; //maps router ID to a vector of all terminal connections that that router has    
    map< int, vector< Connection* > > _terminal_router_connections_map;

    //useful maps - helpful ways of organizing the connection pointers stored above
    map< pair<int,int>, vector< Connection* > > _router_to_router_connection_map; //maps pair(src_gid, dest_gid) to a vector of connection pointers that go from src gid to dest gid
    map< pair<int,int>, vector< Connection* > > _router_to_terminal_connection_map; //maps pair(src_gid, dest_term_gid) to a vector of connection pointers taht go from src gid router to dest_term id
    map< pair<int,int>, vector< Connection* > > _terminal_to_router_connection_map;
    map< pair<int,int>, vector< Connection* > > _router_to_group_connection_map; //maps pair(src_gid, dest_group_id) to a vector of connections that go from src_gid to any router in the dest group
    map< pair<int,int>, vector< Connection* > > _global_group_connection_map; //maps pair(src group id, dest group id) to a vector of connection pointers that match that pattern
    
    int** adjacency_matrix; //total_routers x total_routers in size, 1 for if any connection exists 0 for if no connection exists
    int** adjacency_matrix_nofail;

    int** _shortest_path_vals;
    map<pair<int,int>, vector<int> > _shortest_path_nexts;
    int** _next;

    vector< ConnectionManager > _connection_manager_list; //list of all connection managers in the network
    vector< ConnectionManager > _terminal_connection_manager_list; // list of all connection mangers for TERMINALS in the network

    bool _is_solidified;

public:
    NetworkManager();

    NetworkManager(int total_routers, int total_terminals, int num_routers_per_group, int num_lc_per_router, int num_gc_per_router, int num_cn_conns_per_router, int num_rails, int num_planes);

    void enable_link_failures();

    bool is_link_failures_enabled();

    ConnectionManager& get_connection_manager_for_router(int router_gid);

    ConnectionManager& get_connection_manager_for_terminal(int terminal_gid);

    void add_link(Link_Info link);

    void add_link_failure_info(Link_Info failed_link);

    void fail_connection(Link_Info failed_link);

    int get_failed_count_from_vector(vector<Connection*> conns);

    void calculate_floyd_warshall_shortest_paths();

    int get_shortest_dist_between_routers(int src_gid, int dest_gid);

    void add_conns_to_connection_managers();

    void solidify_network();
};

/**
 * @class ConnectionManager
 *
 * @brief
 * This class is meant to make organization of the connections between routers more
 * streamlined. It provides a simple, readable interface which helps reduce
 * semantic errors during development.
 *
 * @note
 * This class was designed with dragonfly type topologies in mind. Certain parts may not
 * make sense for other types of topologies, they might work fine, but no guarantees.
 *
 * @note
 * This class assumes that each router group has the same number of routers in it: _num_routers_per_group.
 */
class ConnectionManager {
    ManagerType _manType; //whether this is a router or a terminal connection manager
    map< int, Connection > _portMap; //Mapper for ports to connection references - includes failed connections

    // includes failed connections
    map< int, vector< Connection > > intraGroupConnections; //direct connections within a group - IDs are group local - maps local id to list of connections to it
    map< int, vector< Connection > > globalConnections; //direct connections between routers not in same group - IDs are global router IDs - maps global id to list of connections to it
    map< int, vector< Connection > > terminalConnections; //direct connections between this router and its compute node terminals - maps terminal id to connections to it
    map< int, vector< Connection > > injectionConnections; //this is specific for terminal origins, maps a router ID to connections to it

    map< int, vector< Connection > > _connections_to_groups_map; //maps group ID to connections to said group
    vector< int > _other_groups_i_connect_to;
    map< int, vector< Connection > > _all_conns_by_type_map;
    map< int, vector< Connection > > _routed_connections_to_group_map; //maps group ID to connections within local group that go to specified group
    map< int, vector<int> > _routed_router_gids_to_group_map; //maps group ID to a vector of Router GIDs within current group that have a connection to the group ID
    map< int, vector<int> > _group_group_connection_map;

    // doesn't include failed connections - these are copies with the failed links removed for optimized getter performance
    map< int, vector< Connection > > intraGroupConnections_nofail;
    map< int, vector< Connection > > globalConnections_nofail;
    map< int, vector< Connection > > terminalConnections_nofail;
    map< int, vector< Connection > > injectionConnections_nofail;

    map< int, vector< Connection > > _connections_to_groups_map_nofail;
    vector< int > _other_groups_i_connect_to_nofail;
    map< int, vector< Connection > > _all_conns_by_type_map_nofail;
    map< int, vector< Connection > > _routed_connections_to_group_map_nofail; //maps group ID to connections within local group that go to specified group
    map< int, vector<int> > _routed_router_gids_to_group_map_nofail; //maps group ID to a vector of Router GIDs within current group that have a connection to the group ID
    map< int, vector<int> > _group_group_connection_map_nofail;


    // other information
    int _source_id_local; //local id (within group) of owner of this connection manager
    int _source_id_global; //global id (not lp gid) of owner of this connection manager
    int _source_group; //group id of the owner of this connection manager

    int _used_intra_ports; //number of used ports for intra connections
    int _used_inter_ports; //number of used ports for inter connections
    int _used_terminal_ports; //number of used ports for terminal connections
    int _used_injection_ports; //number of used ports for packet injection into the network

    int _failed_intra_ports; //number of failed ports for intra connections
    int _failed_inter_ports; //number of failed ports for inter connections
    int _failed_terminal_ports; //number of failed ports for terminal connections
    int _failed_injection_ports; //number of failed ports for packet injection into the network

    int _max_intra_ports; //maximum number of ports for intra connecitons
    int _max_inter_ports; //maximum number of ports for inter connections
    int _max_terminal_ports; //maximum number of ports for terminal connections.
    int _max_injection_ports; //maximum number of ports for packet injection into the network

    int _num_routers_per_group; //number of routers per group - used for turning global ID into local and back
    int _num_planes;
    int _num_groups;
    int _source_plane;

    bool is_solidified; //flag for whether or not solidification has taken place so that this can be checked at the end of router init

public:
    ConnectionManager(int src_id_local, int src_id_global, int src_group, int max_intra, int max_inter, int max_term, int num_router_per_group, int num_groups);
    ConnectionManager(int src_id_local, int src_id_global, int src_group, int max_intra, int max_inter, int max_term, int max_injection, int num_router_per_group, int num_groups, int num_planes, ManagerType manType);
    /**
     * @brief Adds a connection to the manager, returns a reference to it
     * @param dest_gid the global ID of the destination router
     * @param type the type of the connection, CONN_LOCAL, CONN_GLOBAL, or CONN_TERMINAL
     * @return returns the port number that it was added to

     */
    int add_connection(int dest_gid, ConnectionType type);

    /**
     * @brief Adds a reference to a connection to the manager
     * @param conn connection to a connection created by the NetworkManager
     * @return returns the port number that it was added to
     */
    int add_connection(Connection conn);

    void add_group_group_connection_information(map<pair<int,int>, vector<Connection*> > group_group_connections);

    void set_routed_connections_to_groups(map<int, vector<Connection> > conn_map);

    vector< Connection > get_routed_connections_to_group(int group_id, bool force_next_hop, bool include_failed);

    vector< Connection > get_routed_connections_to_group(int group_id, bool force_next_hop);

    // vector< Connection > get_routed_connections_in_group(int dest_lid, bool include_failed);

    // vector< Connection > get_routed_connections_in_group(int dest_lid);

    vector< int > get_groups_that_connect_to_group(int dest_group, bool include_failed);

    vector< int > get_groups_that_connect_to_group(int dest_group);


    /**
     * @brief get the source ID of the owner of the manager
     * @param type the type of the connection, CONN_LOCAL, CONN_GLOBAL, or CONN_TERMINAL
     */
    int get_source_id(ConnectionType type);

    /**
     * @brief get the port(s) associated with a specific destination ID
     * @param dest_id the ID (local or global depending on type) of the destination
     * @param type the type of the connection, CONN_LOCAL, CONN_GLOBAL, or CONN_TERMINAL
     * @param include_failed whether or not to include failed links in query
     */
    vector<int> get_ports(int dest_id, ConnectionType type, bool include_failed);

    /**
     * @brief get the port(s) associated with a specific destination ID
     * @param dest_id the ID (local or global depending on type) of the destination
     * @param type the type of the connection, CONN_LOCAL, CONN_GLOBAL, or CONN_TERMINAL
     */
    vector<int> get_ports(int dest_id, ConnectionType type);

    /**
     * @brief get the connection associated with a specific port number
     * @param port the enumeration of the port in question
     */
    Connection get_connection_on_port(int port, bool include_failed);

    /**
     * @brief get the connection associated with a specific port number
     * @param port the enumeration of the port in question
     */
    Connection get_connection_on_port(int port);

    /**
     * @brief returns true if a connection exists in the manager from the source to the specified destination ID BY TYPE
     * @param dest_id the ID of the destination depending on the type
     * @param type the type of the connection, CONN_LOCAL, CONN_GLOBAL, or CONN_TERMINAL
     * @param include_failed boolean whether or not to include failed links as possible connections
     * @note Will not return true if dest_id is within own group and type is CONN_GLOBAL, see is_any_connection_to()
     */
    bool is_connected_to_by_type(int dest_id, ConnectionType type, bool include_failed);

    /**
     * @brief returns true if a connection exists in the manager from the source to the specified destination ID BY TYPE - will not return true if only existing links are failed
     * @param dest_id the ID of the destination depending on the type
     * @param type the type of the connection, CONN_LOCAL, CONN_GLOBAL, or CONN_TERMINAL
     * @note Will not return true if dest_id is within own group and type is CONN_GLOBAL, see is_any_connection_to()
     */
    bool is_connected_to_by_type(int dest_id, ConnectionType type);

    /**
     * @brief returns true if any connection exists in the manager from the soruce to the specified global destination ID
     * @param dest_global_id the global id of the destination
     * @param include_failed whether or not to include failed links in query
     * @note This is meant to allow for a developer to determine connectivity just from the global ID, even if the two entities
     *       are connected by a local or terminal connection.
     */
    bool is_any_connection_to(int dest_global_id, bool include_failed);

    /**
     * @brief returns true if any connection exists in the manager from the soruce to the specified global destination ID
     * @param dest_global_id the global id of the destination
     * @note This is meant to allow for a developer to determine connectivity just from the global ID, even if the two entities
     *       are connected by a local or terminal connection.
     */
    bool is_any_connection_to(int dest_global_id);

    /**
     * @brief returns the total number of used ports by the owner of the manager
     * @param account_for_failed do we adjust the count due to failed links?
     */
    int get_total_used_ports(bool account_for_failed);

    /**
     * @brief returns the total number of used ports by the owner of the manager
     */
    int get_total_used_ports();

    /**
     * @brief returns the number of used ports for a specific connection type
     * @param type the type of the connection, CONN_LOCAL, CONN_GLOBAL, or CONN_TERMINAL
     * @param account_for_failed do we adjust the count due to failed links?
     */
    int get_used_ports_for(ConnectionType type, bool account_for_failed);

    /**
     * @brief returns the number of used ports for a specific connection type
     * @param type the type of the connection, CONN_LOCAL, CONN_GLOBAL, or CONN_TERMINAL
     */
    int get_used_ports_for(ConnectionType type);

    /**
     * @brief returns the type of connection associated with said port
     * @param port_num the number of the port in question
     */
    ConnectionType get_port_type(int port_num);

    /**
     * @brief returns the boolean status of whether or not the specified port has failed
     * @param port_num the number of the port in question
     */
    bool get_port_failed_status(int port_num);

    /**
     * @brief returns a vector of connections to the destination ID based on the connection type
     * @param dest_id the ID of the destination depending on the type
     * @param type the type of the connection, CONN_LOCAL, CONN_GLOBAL, or CONN_TERMINAL
     */
    vector< Connection > get_connections_to_gid(int dest_id, ConnectionType type, bool include_failed);

    /**
     * @brief returns a vector of connections to the destination ID based on the connection type
     * @param dest_id the ID of the destination depending on the type
     * @param type the type of the connection, CONN_LOCAL, CONN_GLOBAL, or CONN_TERMINAL
     */
    vector< Connection > get_connections_to_gid(int dest_id, ConnectionType type);

    /**
     * @brief returns a vector of connections to the destination group. connections will be of type CONN_GLOBAL
     * @param dest_group_id the id of the destination group
     * @param include_failed whether or not to include failed links in query
     */
    vector< Connection > get_connections_to_group(int dest_group_id, bool include_failed);

    /**
     * @brief returns a vector of connections to the destination group. connections will be of type CONN_GLOBAL
     * @param dest_group_id the id of the destination group
     */
    vector< Connection > get_connections_to_group(int dest_group_id);

    /**
     * @brief returns a vector of all connections to routers via type specified.
     * @param type the type of the connection, CONN_LOCAL, CONN_GLOBAL, or CONN_TERMINAL
     * @param include_failed whether or not to include failed links in query
     * @note this will return connections to same destination on different ports as individual connections
     */
    vector< Connection > get_connections_by_type(ConnectionType type, bool include_failed);

    /**
     * @brief returns a vector of all connections to routers via type specified.
     * @param type the type of the connection, CONN_LOCAL, CONN_GLOBAL, or CONN_TERMINAL
     * @note this will return connections to same destination on different ports as individual connections
     */
    vector< Connection > get_connections_by_type(ConnectionType type);

    /**
     * @brief returns a vector of all group IDs that the router has a global connection to
     * @param include_failed whether or not to include failed links in query
     * @note this does not include the router's own group as that is a given
     */
    vector< int > get_connected_group_ids(bool include_failed);

    /**
     * @brief returns a vector of all group IDs that the router has a global connection to
     * @note this does not include the router's own group as that is a given
     */
    vector< int > get_connected_group_ids();

    /**
    * @brief function to populate various optimized data structures with various connections
    * @note must be executed before simulation start
    */
    void solidify_connections();

    /**
     * @brief returns true if manager has been solidified - should be run in router init
     */
    bool check_is_solidified();
    /**
     * @brief prints out the state of the connection manager
     */
    void print_connections();
};

//implementation found in util/network-manager.C


#endif /* end of include guard:*/