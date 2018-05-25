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
    CONN_GLOBAL,
    CONN_TERMINAL
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
    set< int > _other_groups_i_connect_to_set;

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

    int _max_intra_ports; //maximum number of ports for intra connecitons
    int _max_inter_ports; //maximum number of ports for inter connections
    int _max_terminal_ports; //maximum number of ports for terminal connections.

    int _num_routers_per_group; //number of routers per group - used for turning global ID into local and back

public:
    ConnectionManager(int src_id_local, int src_id_global, int src_group, int max_intra, int max_inter, int max_term, int num_router_per_group);

    /**
     * @brief Adds a connection to the manager
     * @param dest_gid the global ID of the destination router
     * @param type the type of the connection, CONN_LOCAL, CONN_GLOBAL, or CONN_TERMINAL
     */
    void add_connection(int dest_gid, ConnectionType type);

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
     */
    vector<int> get_ports(int dest_id, ConnectionType type);

    /**
     * @brief get the connection associated with a specific port number
     * @param port the enumeration of the port in question
     */
    Connection get_connection_on_port(int port);

    /**
     * @brief returns true if a connection exists in the manager from the source to the specified destination ID BY TYPE
     * @param dest_id the ID of the destination depending on the type
     * @param type the type of the connection, CONN_LOCAL, CONN_GLOBAL, or CONN_TERMINAL
     * @note Will not return true if dest_id is within own group and type is CONN_GLOBAL, see is_any_connection_to()
     */
    bool is_connected_to_by_type(int dest_id, ConnectionType type);

    /**
     * @brief returns true if any connection exists in the manager from the soruce to the specified global destination ID
     * @param dest_global_id the global id of the destination
     * @note This is meant to allow for a developer to determine connectivity just from the global ID, even if the two entities
     *       are connected by a local or terminal connection.
     */
    bool is_any_connection_to(int dest_global_id);

    /**
     * @brief returns the total number of used ports by the owner of the manager
     */
    int get_total_used_ports();

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
     * @brief returns a vector of connections to the destination ID based on the connection type
     * @param dest_id the ID of the destination depending on the type
     * @param type the type of the connection, CONN_LOCAL, CONN_GLOBAL, or CONN_TERMINAL
     */
    vector< Connection > get_connections_to_gid(int dest_id, ConnectionType type);

    /**
     * @brief returns a vector of connections to the destination group. connections will be of type CONN_GLOBAL
     * @param dest_group_id the id of the destination group
     */
    vector< Connection > get_connections_to_group(int dest_group_id);

    /**
     * @brief returns a vector of all connections to routers via type specified.
     * @param type the type of the connection, CONN_LOCAL, CONN_GLOBAL, or CONN_TERMINAL
     * @note this will return connections to same destination on different ports as individual connections
     */
    vector< Connection > get_connections_by_type(ConnectionType type);

    /**
     * @brief returns a vector of all group IDs that the router has a global connection to
     * @note this does not include the router's own group as that is a given
     */
    vector< int > get_connected_group_ids();

    /**
    *
    */
    void solidify_connections();

    /**
     * @brief prints out the state of the connection manager
     */
    void print_connections();
};


//*******************    BEGIN IMPLEMENTATION ********************************************************

//*******************    Connection Manager Implementation *******************************************
ConnectionManager::ConnectionManager(int src_id_local, int src_id_global, int src_group, int max_intra, int max_inter, int max_term, int num_router_per_group)
{
    _source_id_local = src_id_local;
    _source_id_global = src_id_global;
    _source_group = src_group;

    _used_intra_ports = 0;
    _used_inter_ports = 0;
    _used_terminal_ports = 0;

    _max_intra_ports = max_intra;
    _max_inter_ports = max_inter;
    _max_terminal_ports = max_term;

    _num_routers_per_group = num_router_per_group;
}

void ConnectionManager::add_connection(int dest_gid, ConnectionType type)
{
    Connection conn;
    conn.src_lid = _source_id_local;
    conn.src_gid = _source_id_global;
    conn.src_group_id = _source_group;
    conn.conn_type = type;
    conn.dest_lid = dest_gid % _num_routers_per_group;
    conn.dest_gid = dest_gid;
    conn.dest_group_id = dest_gid / _num_routers_per_group;

    switch (type)
    {
        case CONN_LOCAL:
            conn.port = this->get_used_ports_for(CONN_LOCAL);
            intraGroupConnections[conn.dest_lid].push_back(conn);
            _used_intra_ports++;
            break;

        case CONN_GLOBAL:
            conn.port = _max_intra_ports + this->get_used_ports_for(CONN_GLOBAL);
            globalConnections[conn.dest_gid].push_back(conn);
            _used_inter_ports++;
            break;

        case CONN_TERMINAL:
            conn.port = _max_intra_ports + _max_inter_ports + this->get_used_ports_for(CONN_TERMINAL);
            conn.dest_group_id = _source_group;
            terminalConnections[conn.dest_gid].push_back(conn);
            _used_terminal_ports++;
            break;

        default:
            assert(false);
            // TW_ERROR(TW_LOC, "add_connection(dest_id, type): Undefined connection type\n");
    }

    if(conn.dest_group_id != conn.src_group_id)
        _other_groups_i_connect_to_set.insert(conn.dest_group_id);

    _portMap[conn.port] = conn;
}

// void ConnectionManager::add_route_to_group(Connection conn, int dest_group_id)
// {
//     intermediateRouterToGroupMap[dest_group_id].push_back(conn);
// }

// vector< Connection > ConnectionManager::get_intm_conns_to_group(int dest_group_id)
// {
//     return intermediateRouterToGroupMap[dest_group_id];
// }

// vector< int > ConnectionManager::get_intm_routers_to_group(int dest_group_id)
// {
//     vector< Connection > intm_router_conns = get_intm_conns_to_group(dest_group_id);

//     vector< int > loc_intm_router_ids;
//     vector< Connection >::iterator it;
//     for(it = intm_router_conns.begin(); it != intm_router_conns.end(); it++)
//     {
//         loc_intm_router_ids.push_back((*it).other_id);
//     }
//     return loc_intm_router_ids;
// }

int ConnectionManager::get_source_id(ConnectionType type)
{
    switch (type)
    {
        case CONN_LOCAL:
            return _source_id_local;
        case CONN_GLOBAL:
            return _source_id_global;
        default:
            assert(false);
            // TW_ERROR(TW_LOC, "get_source_id(type): Unsupported connection type\n");
    }
}

vector<int> ConnectionManager::get_ports(int dest_id, ConnectionType type)
{
    vector< Connection > conns = this->get_connections_to_gid(dest_id, type);

    vector< int > ports_used;
    vector< Connection >::iterator it = conns.begin();
    for(; it != conns.end(); it++) {
        ports_used.push_back((*it).port); //add port from connection list to the used ports list
    }
    return ports_used;
}

Connection ConnectionManager::get_connection_on_port(int port)
{
    return _portMap[port];
}

bool ConnectionManager::is_connected_to_by_type(int dest_id, ConnectionType type)
{
    switch (type)
    {
        case CONN_LOCAL:
            if (intraGroupConnections.find(dest_id) != intraGroupConnections.end())
                return true;
            break;
        case CONN_GLOBAL:
            if (globalConnections.find(dest_id) != globalConnections.end())
                return true;
            break;
        case CONN_TERMINAL:
            if (terminalConnections.find(dest_id) != terminalConnections.end())
                return true;
            break;
        default:
            assert(false);
            // TW_ERROR(TW_LOC, "get_used_ports_for(type): Undefined connection type\n");
    }
    return false;
}

bool ConnectionManager::is_any_connection_to(int dest_global_id)
{
    int local_id = dest_global_id % _num_routers_per_group;
    if (intraGroupConnections.find(local_id) != intraGroupConnections.end())
        return true;
    if (globalConnections.find(dest_global_id) != globalConnections.end())
        return true;
    if (terminalConnections.find(dest_global_id) != terminalConnections.end())
        return true;

    return false;
}

int ConnectionManager::get_total_used_ports()
{
    return _used_intra_ports + _used_inter_ports + _used_terminal_ports;
}

int ConnectionManager::get_used_ports_for(ConnectionType type)
{
    switch (type)
    {
        case CONN_LOCAL:
            return _used_intra_ports;
        case CONN_GLOBAL:
            return _used_inter_ports;
        case CONN_TERMINAL:
            return _used_terminal_ports;
        default:
            assert(false);
            // TW_ERROR(TW_LOC, "get_used_ports_for(type): Undefined connection type\n");
    }
}

ConnectionType ConnectionManager::get_port_type(int port_num)
{
    return _portMap[port_num].conn_type;
}


vector< Connection > ConnectionManager::get_connections_to_gid(int dest_gid, ConnectionType type)
{
    switch (type)
    {
        case CONN_LOCAL:
            return intraGroupConnections[dest_gid%_num_routers_per_group];
        case CONN_GLOBAL:
            return globalConnections[dest_gid];
        case CONN_TERMINAL:
            return terminalConnections[dest_gid];
        default:
            assert(false);
            // TW_ERROR(TW_LOC, "get_connections(type): Undefined connection type\n");
    }
}

vector< Connection > ConnectionManager::get_connections_to_group(int dest_group_id)
{
    vector< Connection > conns_to_group;

    map< int, vector< Connection > >::iterator it = globalConnections.begin();
    for(; it != globalConnections.end(); it++) //iterate over each router that is connected to source
    {
        vector< Connection >::iterator conns_to_router;
        for(conns_to_router = (it->second).begin(); conns_to_router != (it->second).end(); conns_to_router++) //iterate over each connection to a specific router
        {
            if ((*conns_to_router).dest_group_id == dest_group_id) {
                conns_to_group.push_back(*conns_to_router);
            }
        }
    }
    return conns_to_group;
}

vector< Connection > ConnectionManager::get_connections_by_type(ConnectionType type)
{
    map< int, vector< Connection > > theMap;
    switch (type)
    {
        case CONN_LOCAL:
            theMap = intraGroupConnections;
            break;
        case CONN_GLOBAL:
            theMap = globalConnections;
            break;
        case CONN_TERMINAL:
            theMap = terminalConnections;
            break;
    }

    vector< Connection > retVec;
    map< int, vector< Connection > >::iterator it;
    for(it = theMap.begin(); it != theMap.end(); it++)
    {
        retVec.insert(retVec.end(), (*it).second.begin(), (*it).second.end());
    }

    return retVec;
}

vector< int > ConnectionManager::get_connected_group_ids()
{
    return _other_groups_i_connect_to;
}

void ConnectionManager::solidify_connections()
{
    set< int >::iterator it;
    for(it = _other_groups_i_connect_to_set.begin(); it != _other_groups_i_connect_to_set.end(); it++)
    {
        _other_groups_i_connect_to.push_back(*it);
    }
}


void ConnectionManager::print_connections()
{
    printf("Connections for Router: %d ---------------------------------------\n",_source_id_global);

    int ports_printed = 0;
    map<int,Connection>::iterator it = _portMap.begin();
    for(; it != _portMap.end(); it++)
    {
        if ( (ports_printed == 0) && (_used_intra_ports > 0) )
        {
            printf(" -- Intra-Group Connections -- \n");
            printf("  Port  |  Dest_ID  |  Group\n");
        }
        if ( (ports_printed == _used_intra_ports) && (_used_inter_ports > 0) )
        {
            printf(" -- Inter-Group Connections -- \n");
            printf("  Port  |  Dest_ID  |  Group\n");
        }
        if ( (ports_printed == _used_intra_ports + _used_inter_ports) && (_used_terminal_ports > 0) )
        {
            printf(" -- Terminal Connections -- \n");
            printf("  Port  |  Dest_ID  |  Group\n");
        }

        int port_num = it->first;
        int group_id = it->second.dest_group_id;

        int id,gid;
        if( get_port_type(port_num) == CONN_LOCAL )
        {
            id = it->second.dest_lid;
            gid = it->second.dest_gid;
            printf("  %d   ->   (%d,%d)        :  %d  \n", port_num, id, gid, group_id);

        }
            
        else {
            id = it->second.dest_gid;
            printf("  %d   ->   %d        :  %d  \n", port_num, id, group_id);

        }
            
        ports_printed++;
    }
}



#endif /* end of include guard:*/