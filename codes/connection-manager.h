#ifndef CONNECTION_MANAGER_H
#define CONNECTION_MANAGER_H

/**
 * connection-manager.h -- Simple, Readable, Connection management interface
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

/**
 * @brief Enum differentiating local router connection types from global.
 * Local connections will have router IDs ranging from [0,num_router_per_group)
 * whereas global connections will have router IDs ranging from [0,total_routers)
 */
enum ConnectionType
{
    CONN_LOCAL = 1,
    CONN_GLOBAL = 2,
    CONN_TERMINAL = 3
};

/**
 * @brief Struct for connection information.
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
    ConnectionType conn_type; //type of the connection: CONN_LOCAL, CONN_GLOBAL, or CONN_TERMINAL
};

inline bool operator<(const Connection& lhs, const Connection& rhs)
{
  return lhs.port < rhs.port;
}

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
 * There is the property intermediateRouterToGroupMap and related methods that are implemented but the
 * logistics to get this information from input file is more complicated than its worth so I have commented
 * them out.
 *
 * @note
 * This class assumes that each router group has the same number of routers in it: _num_routers_per_group.
 */
class ConnectionManager {
    map< int, vector< Connection > > intraGroupConnections; //direct connections within a group - IDs are group local - maps local id to list of connections to it
    map< int, vector< Connection > > globalConnections; //direct connections between routers not in same group - IDs are global router IDs - maps global id to list of connections to it
    map< int, vector< Connection > > terminalConnections; //direct connections between this router and its compute node terminals - maps terminal id to connections to it

    map< int, Connection > _portMap; //Mapper for ports to connections

    vector< int > _other_groups_i_connect_to;
    vector< int > _other_groups_i_connect_to_after_fails;
    set< int > _other_groups_i_connect_to_set;
    set< int > _other_groups_i_connect_to_set_after_fails;

    map< int, vector< Connection > > _connections_to_groups_map; //maps group ID to connections to said group
    map< int, vector< Connection > > _all_conns_by_type_map;

    // map< int, vector< Connection > > intermediateRouterToGroupMap; //maps group id to list of routers that connect to it.
    //                                                                //ex: intermediateRouterToGroupMap[3] returns a vector
    //                                                                //of connections from this router to routers that have
    //                                                                //direct connections to group 3

    int _source_id_local; //local id (within group) of owner of this connection manager
    int _source_id_global; //global id (not lp gid) of owner of this connection manager
    int _source_group; //group id of the owner of this connection manager

    int _used_intra_ports; //number of used ports for intra connections
    int _used_inter_ports; //number of used ports for inter connections
    int _used_terminal_ports; //number of used ports for terminal connections

    int _failed_intra_ports; //number of failed ports for intra connections
    int _failed_inter_ports; //number of failed ports for inter connections
    int _failed_terminal_ports; //number of failed ports for terminal connections

    int _max_intra_ports; //maximum number of ports for intra connecitons
    int _max_inter_ports; //maximum number of ports for inter connections
    int _max_terminal_ports; //maximum number of ports for terminal connections.

    int _num_routers_per_group; //number of routers per group - used for turning global ID into local and back

    bool is_solidified; //flag for whether or not solidification has taken place so that this can be checked at the end of router init

public:
    ConnectionManager(int src_id_local, int src_id_global, int src_group, int max_intra, int max_inter, int max_term, int num_router_per_group);

    /**
     * @brief Adds a connection to the manager
     * @param dest_gid the global ID of the destination router
     * @param type the type of the connection, CONN_LOCAL, CONN_GLOBAL, or CONN_TERMINAL
     */
    void add_connection(int dest_gid, ConnectionType type);

    /**
     * @brief Marks a connection to dest_gid from the local router as failed if one exists, error if not enough links remain unfailed
     * @param dest_gid the gid of the dest
     * @param type the type of the connection that will fail
     */
    void fail_connection(int dest_gid, ConnectionType type);

    /**
     * @brief returns a count of the number of failed links from a vector of connections
     * @param conns a vector of connections that the function will iterate over to count failed links
     */
    int get_failed_count_from_vector(vector<Connection> conns);

    // /**
    //  * @brief adds knowledge of what next hop routers have connections to specific groups
    //  * @param local_intm_id the local intra group id of the router that has the connection to dest_group_id
    //  * @param dest_group_id the id of the group that the connection goes to
    //  */
    // void add_route_to_group(int local_intm_id, int dest_group_id);

    // /**
    //  * @brief returns a vector of connections to routers that have direct connections to the specified group id
    //  * @param dest_group_id the id of the destination group that all connections returned have a direct connection to
    //  */
    // vector< Connection > get_intm_conns_to_group(int dest_group_id);

    // /**
    //  * @brief returns a vector of local router ids that have direct connections to the specified group id
    //  * @param dest_group_id the id of the destination group that all routers returned have a direct connection to
    //  * @note if a router has multiple intra group connections to a single router and that router has a connection
    //  *      to the dest group then that router will appear multiple times in the returned vector.
    //  */
    // vector< int > get_intm_routers_to_group(int dest_group_id)

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

//implementation found in util/connection-manager.C


#endif /* end of include guard:*/