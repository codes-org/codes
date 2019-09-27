#include "codes/connection-manager.h"


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
            if (intraGroupConnections.size() < _max_intra_ports) {
                conn.port = this->get_used_ports_for(CONN_LOCAL);
                intraGroupConnections[conn.dest_lid].push_back(conn);
                _used_intra_ports++;
            }
            else
                tw_error(TW_LOC,"Attempting to add too many local connections per router - exceeding configuration value: %d",_max_intra_ports);
            break;

        case CONN_GLOBAL:
            if(globalConnections.size() < _max_inter_ports) {
                conn.port = _max_intra_ports + this->get_used_ports_for(CONN_GLOBAL);
                globalConnections[conn.dest_gid].push_back(conn);
                _used_inter_ports++;
            }
            else
                tw_error(TW_LOC,"Attempting to add too many global connections per router - exceeding configuration value: %d",_max_inter_ports);
            break;

        case CONN_TERMINAL:
            if(terminalConnections.size() < _max_terminal_ports){
                conn.port = _max_intra_ports + _max_inter_ports + this->get_used_ports_for(CONN_TERMINAL);
                conn.dest_group_id = _source_group;
                terminalConnections[conn.dest_gid].push_back(conn);
                _used_terminal_ports++;
            }
            else
                tw_error(TW_LOC,"Attempting to add too many terminal connections per router - exceeding configuration value: %d",_max_terminal_ports);
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
    return _connections_to_groups_map[dest_group_id];
}

vector< Connection > ConnectionManager::get_connections_by_type(ConnectionType type)
{
    switch (type)
        {
            case CONN_LOCAL:
                return _all_conns_by_type_map[CONN_LOCAL];
                break;
            case CONN_GLOBAL:
                return _all_conns_by_type_map[CONN_GLOBAL];
                break;
            case CONN_TERMINAL:
                return _all_conns_by_type_map[CONN_TERMINAL];
                break;
            default:
                tw_error(TW_LOC, "Bad enum type\n");
        }
}

vector< int > ConnectionManager::get_connected_group_ids()
{
    return _other_groups_i_connect_to;
}

void ConnectionManager::solidify_connections()
{
    //-- other groups connect to
    set< int >::iterator it;
    for(it = _other_groups_i_connect_to_set.begin(); it != _other_groups_i_connect_to_set.end(); it++)
    {
        _other_groups_i_connect_to.push_back(*it);
    }

    //--connections to group
    for(it = _other_groups_i_connect_to_set.begin(); it != _other_groups_i_connect_to_set.end(); it++)
    {
        int dest_group_id = *it;

        vector< Connection > conns_to_group;
        map< int, vector< Connection > >::iterator itg = globalConnections.begin();
        for(; itg != globalConnections.end(); itg++) //iterate over each router that is connected to source
        {
            vector< Connection >::iterator conns_to_router;
            for(conns_to_router = (itg->second).begin(); conns_to_router != (itg->second).end(); conns_to_router++) //iterate over each connection to a specific router
            {
                if ((*conns_to_router).dest_group_id == dest_group_id) {
                    conns_to_group.push_back(*conns_to_router);
                }
            }
        }

        _connections_to_groups_map[dest_group_id] = conns_to_group;
    }

    //--get connections by type

    map< int, vector< Connection > > theMap;
    for ( int enum_int = CONN_LOCAL; enum_int != CONN_TERMINAL + 1; enum_int++ )
    {
        switch (enum_int)
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
            default:
                tw_error(TW_LOC, "Bad enum type\n");
        }

        vector< Connection > retVec;
        map< int, vector< Connection > >::iterator it;
        for(it = theMap.begin(); it != theMap.end(); it++)
        {
            retVec.insert(retVec.end(), (*it).second.begin(), (*it).second.end());
        }
        _all_conns_by_type_map[enum_int] = retVec;
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
        if( get_port_type(port_num) == CONN_LOCAL ) {
            id = it->second.dest_lid;
            gid = it->second.dest_gid;
            printf("  %d   ->   (%d,%d)        :  %d     -  LOCAL\n", port_num, id, gid, group_id);

        } 
        else if (get_port_type(port_num) == CONN_GLOBAL) {
            id = it->second.dest_gid;
            printf("  %d   ->   %d        :  %d     -  GLOBAL\n", port_num, id, group_id);
        }
        else if (get_port_type(port_num) == CONN_TERMINAL) {
            id = it->second.dest_gid;
            printf("  %d   ->   %d        :  %d     -  TERMINAL\n", port_num, id, group_id);
        }
            
        ports_printed++;
    }
}
