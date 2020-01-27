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

    _failed_intra_ports = 0;
    _failed_inter_ports = 0;
    _failed_terminal_ports = 0;

    _max_intra_ports = max_intra;
    _max_inter_ports = max_inter;
    _max_terminal_ports = max_term;

    _num_routers_per_group = num_router_per_group;

    is_solidified = false;
}

void ConnectionManager::add_connection(int dest_gid, ConnectionType type)
{
    if (is_solidified)
        tw_error(TW_LOC,"ConnectionManager: Attempting to add connections after manager has been solidified!\n");

    Connection conn;
    conn.src_lid = _source_id_local;
    conn.src_gid = _source_id_global;
    conn.src_group_id = _source_group;
    conn.conn_type = type;
    conn.dest_lid = dest_gid % _num_routers_per_group;
    conn.dest_gid = dest_gid;
    conn.dest_group_id = dest_gid / _num_routers_per_group;
    conn.is_failed = 0;

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

    _portMap[conn.port] = &conn;
}

void ConnectionManager::add_interconnection_information(int connecting_source_gid, int source_group_id, int dest_group_id)
{
    if (_source_group != source_group_id)
        tw_error(TW_LOC, "ConnectionManager: Attempting to add interconnection information but source group ID doesn't match connection manager's group\n");

    _interconnection_route_info_map[dest_group_id].push_back(connecting_source_gid);
    printf("Router %dL %dG: Router %d in my local group %d has a connection to group %d\n", _source_id_local, _source_id_global, connecting_source_gid, source_group_id, dest_group_id);
}

void ConnectionManager::fail_connection(int dest_gid, ConnectionType type)
{
    if (is_solidified)
        tw_error(TW_LOC,"ConnectionManager: Attempting to fail connections after manager has been solidified!\n");

    switch(type)
    {
        case CONN_LOCAL:
        {
            vector<Connection> conns_to_gid = intraGroupConnections[dest_gid%_num_routers_per_group];
            int num_failed_already = get_failed_count_from_vector(conns_to_gid);
            if (num_failed_already == conns_to_gid.size())
                tw_error(TW_LOC, "Attempting to fail more Local links from Router gid %d to Router gid %d than exist. Already Failed %d\n", _source_id_global, dest_gid, num_failed_already);

            vector<Connection>::iterator it = intraGroupConnections[dest_gid%_num_routers_per_group].begin();
            for(; it != intraGroupConnections[dest_gid%_num_routers_per_group].end(); it++)
            {
                if(!it->is_failed)
                {
                    it->is_failed = 1;
                    _failed_intra_ports++;
                    break;
                }
            }
        }
        break;
        case CONN_GLOBAL:
        {
            vector<Connection> conns_to_gid = globalConnections[dest_gid];
            int num_failed_already = get_failed_count_from_vector(conns_to_gid);
            if (num_failed_already == conns_to_gid.size())
                tw_error(TW_LOC, "Attempting to fail more Global links from Router gid %d to Router gid %d than exist. Already Failed %d\n", _source_id_global, dest_gid, num_failed_already);
            
            vector<Connection>::iterator it = globalConnections[dest_gid].begin();
            for(; it != globalConnections[dest_gid].end(); it++)
            {
                if(!it->is_failed)
                {
                    it->is_failed = 1;
                    _failed_inter_ports++;
                    break;
                }
            }
        }
        break;
        case CONN_TERMINAL:
        {
            tw_error(TW_LOC, "Failure of terminal links not yet supported - need to remove links from terminal side too before allowing.\n");
            vector<Connection> conns_to_gid = terminalConnections[dest_gid];
            int num_failed_already = get_failed_count_from_vector(conns_to_gid);
            if (num_failed_already == conns_to_gid.size())
                tw_error(TW_LOC, "Attempting to fail more Terminal links from Router gid %d to Terminal gid %d than exist. Already Failed %d\n", _source_id_global, dest_gid, num_failed_already);
            
            vector<Connection>::iterator it = terminalConnections[dest_gid].begin();
            for(; it != terminalConnections[dest_gid].end(); it++)
            {
                if(!it->is_failed)
                {
                    it->is_failed = 1;
                    _failed_terminal_ports++;
                    break;
                }
            }
        }
        break;
        default:
            assert(false);
    }
}

void ConnectionManager::add_interconnection_failure_information(int connecting_source_gid, int source_group_id, int dest_group_id)
{
    if (_source_group != source_group_id)
        tw_error(TW_LOC, "ConnectionManager: Attempting to add failure interconnection information but source group ID doesn't match connection manager's group\n");

    _interconnection_failure_info_map[dest_group_id].push_back(connecting_source_gid);
    printf("Router %dL %dG: Router %d in my local group %d has a FAILED connection to group %d\n", _source_id_local, _source_id_global, connecting_source_gid, source_group_id, dest_group_id);
}

int ConnectionManager::get_failed_count_from_vector(vector<Connection> conns)
{
    int count = 0;
    vector<Connection>::iterator it = conns.begin();
    for(; it != conns.end(); it++)
    {
        if (it->is_failed)
            count++;
    }
    return count;
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

vector<int> ConnectionManager::get_ports(int dest_id, ConnectionType type, bool include_failed)
{
    vector< Connection > conns = this->get_connections_to_gid(dest_id, type);

    vector< int > ports_used;
    vector< Connection >::iterator it = conns.begin();
    for(; it != conns.end(); it++) {
        if (it->is_failed == 0 || include_failed)
            ports_used.push_back((*it).port); //add port from connection list to the used ports list
    }
    return ports_used;
}

vector<int> ConnectionManager::get_ports(int dest_id, ConnectionType type)
{
    return get_ports(dest_id, type, false);
}

Connection ConnectionManager::get_connection_on_port(int port, bool include_failed)
{
    Connection conn = *_portMap[port];
    if (conn.is_failed == 0 || include_failed)
        return conn;
    else
    {
        Connection empty_conn;
        return empty_conn;
    }
}

Connection ConnectionManager::get_connection_on_port(int port)
{
    return get_connection_on_port(port, false);
}

bool ConnectionManager::is_connected_to_by_type(int dest_id, ConnectionType type)
{
    return is_connected_to_by_type(dest_id, type, false); //by default, don't include failed links
}

bool ConnectionManager::is_connected_to_by_type(int dest_id, ConnectionType type, bool include_failed)
{
    map<int, vector<Connection> > the_map;
    map<int, vector<Connection> >::iterator map_it;

    if (type == CONN_LOCAL)
        the_map = intraGroupConnections;
    else if (type == CONN_GLOBAL)
        the_map = globalConnections;
    else if (type == CONN_TERMINAL)
        the_map = terminalConnections;
    else
        assert(false);

    vector<Connection>::iterator vec_it;
    map_it = the_map.find(dest_id);
    if (map_it != the_map.end())
    {
        if (include_failed) //then simply successfully finding the connection in the map means true
            return true;

        vec_it = map_it->second.begin();
        for(; vec_it != map_it->second.end(); vec_it++)
        {
            if (vec_it->is_failed == 0)
                return true;
        }
    }
    return false;
}

bool ConnectionManager::is_any_connection_to(int dest_global_id)
{
    return is_any_connection_to(dest_global_id, false);
}

bool ConnectionManager::is_any_connection_to(int dest_global_id, bool include_failed)
{
    int local_id = dest_global_id % _num_routers_per_group;
    if (is_connected_to_by_type(local_id, CONN_LOCAL, include_failed))
        return true;
    if (is_connected_to_by_type(dest_global_id, CONN_GLOBAL, include_failed))
        return true;
    if (is_connected_to_by_type(dest_global_id, CONN_TERMINAL, include_failed))
        return true;
    return false;
}

int ConnectionManager::get_total_used_ports(bool account_for_failed)
{
    int sum = 0;
    sum = _used_intra_ports + _used_inter_ports + _used_terminal_ports;
    if (account_for_failed)
        sum -= (_failed_intra_ports + _failed_inter_ports + _failed_terminal_ports);
    return sum;
}

int ConnectionManager::get_total_used_ports()
{
    return get_total_used_ports(true);
}

int ConnectionManager::get_used_ports_for(ConnectionType type, bool account_for_failed)
{
    switch (type)
    {
        case CONN_LOCAL:
            return _used_intra_ports - (_failed_intra_ports * account_for_failed);
        case CONN_GLOBAL:
            return _used_inter_ports - (_failed_inter_ports * account_for_failed);
        case CONN_TERMINAL:
            return _used_terminal_ports - (_failed_terminal_ports * account_for_failed);
        default:
            assert(false);
            // TW_ERROR(TW_LOC, "get_used_ports_for(type): Undefined connection type\n");
    }
}

int ConnectionManager::get_used_ports_for(ConnectionType type)
{
    return get_used_ports_for(type, true);
}

ConnectionType ConnectionManager::get_port_type(int port_num)
{
    return _portMap[port_num]->conn_type;
}

bool ConnectionManager::get_port_failed_status(int port_num)
{
    return _portMap[port_num]->is_failed;
}

vector< Connection > ConnectionManager::get_connections_to_gid(int dest_gid, ConnectionType type)
{
    return get_connections_to_gid(dest_gid, type, false);
}

vector< Connection > ConnectionManager::get_connections_to_gid(int dest_gid, ConnectionType type, bool include_failed)
{
    vector<Connection> conn_vec;
    switch (type)
    {
        case CONN_LOCAL:
            if (include_failed)
                return intraGroupConnections[dest_gid%_num_routers_per_group];
            conn_vec = intraGroupConnections[dest_gid%_num_routers_per_group];
        break;
        case CONN_GLOBAL:
            if (include_failed)
                return globalConnections[dest_gid];
            conn_vec = globalConnections[dest_gid];
        break;
        case CONN_TERMINAL:
            if (include_failed)
                return terminalConnections[dest_gid];
            conn_vec = terminalConnections[dest_gid];
        break;
        default:
            assert(false);
            // TW_ERROR(TW_LOC, "get_connections(type): Undefined connection type\n");
    }
    vector<Connection> ret_vec;
    vector<Connection>:: iterator it = conn_vec.begin();
    for(; it != conn_vec.end(); it++)
    {
        if (it->is_failed == 0 || include_failed)
            ret_vec.push_back(*it);
    }
    return ret_vec;
}

vector< Connection > ConnectionManager::get_connections_to_group(int dest_group_id)
{
    return get_connections_to_group(dest_group_id, false);
}

vector< Connection > ConnectionManager::get_connections_to_group(int dest_group_id, bool include_failed)
{
    vector<Connection> retVec;
    vector<Connection>::iterator it = _connections_to_groups_map[dest_group_id].begin();
    for(; it != _connections_to_groups_map[dest_group_id].end(); it++)
    {
        if (it->is_failed == 0 || include_failed)
            retVec.push_back(*it);
    }

    return retVec;
}

vector< Connection > ConnectionManager::get_connections_by_type(ConnectionType type)
{
    return get_connections_by_type(type, false);
}

vector< Connection > ConnectionManager::get_connections_by_type(ConnectionType type, bool include_failed)
{
    vector<Connection> conn_vec;
    switch (type)
        {
            case CONN_LOCAL:
                conn_vec = _all_conns_by_type_map[CONN_LOCAL];
                break;
            case CONN_GLOBAL:
                conn_vec = _all_conns_by_type_map[CONN_GLOBAL];
                break;
            case CONN_TERMINAL:
                conn_vec = _all_conns_by_type_map[CONN_TERMINAL];
                break;
            default:
                tw_error(TW_LOC, "Bad enum type\n");
        }

    vector<Connection> retVec;
    vector<Connection>::iterator it = conn_vec.begin();
    for(; it != conn_vec.end(); it++)
    {
        if (it->is_failed == 0 || include_failed)
            retVec.push_back(*it);
    }
    return retVec;
}

vector< int > ConnectionManager::get_connected_group_ids()
{
    return get_connected_group_ids(false);
}

vector< int > ConnectionManager::get_connected_group_ids(bool include_failed)
{
    if (include_failed)
        return _other_groups_i_connect_to;
    else
        return _other_groups_i_connect_to_after_fails;
}

void ConnectionManager::solidify_connections()
{
    //-- other groups connect to
    set< int >::iterator it;
    for(it = _other_groups_i_connect_to_set.begin(); it != _other_groups_i_connect_to_set.end(); it++)
    {
        _other_groups_i_connect_to.push_back(*it);
    }

    if (_failed_inter_ports > 0)
    {

        map<int, vector<Connection> >::iterator it2 = globalConnections.begin();
        for(; it2 != globalConnections.end(); it2++)
        {
            _other_groups_i_connect_to_set_after_fails.insert((it2->second[0]).dest_group_id);
        }

        for(it = _other_groups_i_connect_to_set_after_fails.begin(); it != _other_groups_i_connect_to_set_after_fails.end(); it++)
        {
            _other_groups_i_connect_to_after_fails.push_back(*it);
        }
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

    // //--interconnection route information
    // map<int, vector< int > >::iterator info_it = _interconnection_route_info_map.begin();
    // for(; info_it != _interconnection_route_info_map.end(); info_it++)
    // {
    //     int dest_group_id = info_it->first;
    //     vector<int> gid_list = info_it->second;
    //     int num_



    //     vector<int>::iterator gid_it = gid_list.begin();
    //     set<int> added_gids;
    //     for(; gid_it != gid_list.end(); gid_it++)
    //     {   
    //         int gid = *gid_it;
    //         if (!added_gids.find(gid)) { //if we haven't already added its connections to the map
    //             vector<Connection> conns_to_gid = get_connections_to_gid(gid, CONN_LOCAL, true);
    //             _interconnection_route_map[dest_group_id].insert(_interconnection_route_map[dest_group_id].end(), conns_to_gid.begin(), conns_to_gid.end());
    //             added_gids.insert(gid);
    //         }
    //     }
    // }

    // for(info_it = _interconnection_route_info_map.begin(); info_it != _interconnection_route_info_map.end(); info_it++)
    // {
    //     int dest_group_id = info_it->first;
    //     vector<int> gid_list = info_it->second;
        
    // }


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

    is_solidified = true;
}

bool ConnectionManager::check_is_solidified()
{
    return is_solidified;
}

void ConnectionManager::print_connections()
{
    printf("Connections for Router: %d ---------------------------------------\n",_source_id_global);

    int ports_printed = 0;
    map<int,Connection*>::iterator it = _portMap.begin();
    for(; it != _portMap.end(); it++)
    {
        if ( (ports_printed == 0) && (_used_intra_ports > 0) )
        {
            printf(" -- Intra-Group Connections -- \n");
            printf("  Port  |  Dest_ID  |  Group  |  Fail Status\n");
        }
        if ( (ports_printed == _used_intra_ports) && (_used_inter_ports > 0) )
        {
            printf(" -- Inter-Group Connections -- \n");
            printf("  Port  |  Dest_ID  |  Group  |  Fail Status\n");
        }
        if ( (ports_printed == _used_intra_ports + _used_inter_ports) && (_used_terminal_ports > 0) )
        {
            printf(" -- Terminal Connections -- \n");
            printf("  Port  |  Dest_ID  |  Group  |  Fail Status\n");
        }

        int port_num = it->first;
        int group_id = it->second->dest_group_id;

        int id,gid;
        if( get_port_type(port_num) == CONN_LOCAL ) {
            id = it->second->dest_lid;
            gid = it->second->dest_gid;
            printf("  %d   ->   (%d,%d)        :  %d     -  LOCAL\n", port_num, id, gid, group_id);

        } 
        else if (get_port_type(port_num) == CONN_GLOBAL) {
            id = it->second->dest_gid;
            printf("  %d   ->   %d        :  %d     -  GLOBAL\n", port_num, id, group_id);
        }
        else if (get_port_type(port_num) == CONN_TERMINAL) {
            id = it->second->dest_gid;
            printf("  %d   ->   %d        :  %d     -  TERMINAL\n", port_num, id, group_id);
        }
            
        ports_printed++;
    }
}
