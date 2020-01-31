#include "codes/network-manager.h"
#include <algorithm>

void add_as_if_set_int(int value, vector<int>& vec)
{
    vector<int>::iterator it;
    it = std::find(vec.begin(), vec.end(), value);
    if (it == vec.end()) //then it's not in the vector yet - else do nothing
        vec.push_back(value);
}

//*******************    Network Manager Implementation **********************************************
NetworkManager::NetworkManager()
{
    _total_routers = 0;
    _num_routers_per_group = 0;
    _num_groups = 0;
    _num_lc_pr = 0;
    _num_gc_pr = 0;
    _num_cn_pr = 0;
    _is_solidified = false;
}


NetworkManager::NetworkManager(int total_routers, int num_routers_per_group, int num_lc_per_router, int num_gc_per_router, int num_cn_per_router)
{
    _total_routers = total_routers;
    _num_routers_per_group = num_routers_per_group;
    _num_groups = total_routers / num_routers_per_group;
    _num_lc_pr = num_lc_per_router;
    _num_gc_pr = num_gc_per_router;
    _num_cn_pr = num_cn_per_router;

    _num_router_conns = 0;
    _num_router_terminal_conns = 0;
    _num_failed_router_conns = 0;
    _num_failed_router_terminal_conns = 0;

    // _num_router_router_conns_created = 0;
    // _num_router_terminal_conns_created = 0;
    
    if (total_routers % num_routers_per_group != 0)
        tw_error(TW_LOC, "NetworkManager: total routers \% num routers per group is not evenly divisible, non-uniform groups\n");


    for(int i = 0; i < _total_routers; i++)
    {
        int src_id_global = i;
        int src_id_local = i % _num_routers_per_group;
        int src_group = i / _num_routers_per_group;

        ConnectionManager conn_man = ConnectionManager(src_id_local, src_id_global, src_group, _num_lc_pr, _num_gc_pr, _num_cn_pr, _num_routers_per_group);
        _connection_manager_list.push_back(conn_man);
    }
    _is_solidified = false;
}

ConnectionManager& NetworkManager::get_connection_manager_for_router(int router_gid)
{
    return _connection_manager_list[router_gid];
}

void NetworkManager::add_link(Link_Info link)
{
    Connection *conn = (Connection*)malloc(sizeof(Connection));
    conn->port = -1; //will be set by the Connection Manager of the router (src_gid) and defined in add_cons_to_connectoin_managers
    conn->src_lid = link.src_gid % _num_routers_per_group;
    conn->src_gid = link.src_gid;
    conn->src_group_id = link.src_gid / _num_routers_per_group;
    conn->dest_lid = link.dest_gid % _num_routers_per_group;
    conn->dest_gid = link.dest_gid;
    conn->dest_group_id = link.dest_gid / _num_routers_per_group;
    conn->conn_type = link.conn_type;
    conn->is_failed = 0;

    if (link.conn_type != CONN_TERMINAL)
    {
        _num_router_conns++;

        //put the conn into its owning structure
        _router_connections_map[link.src_gid].push_back(conn);

        //put into useful maps
        _router_to_router_connection_map[make_pair(conn->src_gid,conn->dest_gid)].push_back(conn);

        if (conn->conn_type == CONN_GLOBAL)
            _global_group_connection_map[make_pair(conn->src_group_id, conn->dest_group_id)].push_back(conn);

        _router_to_group_connection_map[make_pair(conn->src_gid, conn->dest_group_id)].push_back(conn);
    }
    else //CONN TERMINAL
    {
        _num_router_terminal_conns++;

        conn->dest_group_id = conn->src_group_id;
        conn->dest_lid = conn->dest_gid % _num_cn_pr;
        //put conn into owning structure
        _router_terminal_connections_map[link.src_gid].push_back(conn);

        //put into useful maps
        _router_to_terminal_connection_map[make_pair(conn->src_gid,conn->dest_gid)].push_back(conn);
    }
}

void NetworkManager::add_link_failure_info(Link_Info link)
{
    if (link.conn_type != CONN_TERMINAL)
    {
        _router_link_failure_lists[link.src_gid].push_back(link);
        _num_failed_router_conns++;
    }
    else
    {
        _router_terminal_link_failure_lists[link.src_gid].push_back(link);
        _num_failed_router_terminal_conns++;
    }
}


void NetworkManager::fail_connection(Link_Info link)
{
    if (link.conn_type != CONN_TERMINAL)
    {
        vector<Connection*> conns_to_gid = _router_to_router_connection_map[make_pair(link.src_gid,link.dest_gid)];
        int num_failed_already = get_failed_count_from_vector(conns_to_gid);
        if (num_failed_already == conns_to_gid.size())
            tw_error(TW_LOC, "Attempting to fail more links from Router GID %d to Router GID %d than exist. Already Failed %d\n", link.src_gid, link.dest_gid, num_failed_already);

        vector<Connection*>:: iterator it = _router_to_router_connection_map[make_pair(link.src_gid,link.dest_gid)].begin();
        for(; it != _router_to_router_connection_map[make_pair(link.src_gid,link.dest_gid)].end(); it++)
        {
            if(!(*it)->is_failed)
            {
                (*it)->is_failed = 1;
            }
        }
    }
    else
    {
        vector<Connection*> conns_to_term_gid = _router_to_terminal_connection_map[make_pair(link.src_gid,link.dest_gid)];
        int num_failed_already = get_failed_count_from_vector(conns_to_term_gid);
        if (num_failed_already == conns_to_term_gid.size())
            tw_error(TW_LOC, "Attempting to fail more links from Router GID %d to Terminal GID %d than exist. Already Failed %d\n", link.src_gid, link.dest_gid, num_failed_already);

        vector<Connection*>:: iterator it = _router_to_terminal_connection_map[make_pair(link.src_gid,link.dest_gid)].begin();
        for(; it != _router_to_terminal_connection_map[make_pair(link.src_gid,link.dest_gid)].end(); it++)
        {
            if(!(*it)->is_failed)
            {
                (*it)->is_failed = 1;
            }
        }
    }
}

int NetworkManager::get_failed_count_from_vector(vector<Connection*> conns)
{
    int count = 0;
    vector<Connection*>::iterator it = conns.begin();
    for(; it != conns.end(); it++)
    {
        if ((*it)->is_failed)
            count++;
    }
    return count;
}

void NetworkManager::add_conns_to_connection_managers()
{
    for(int i = 0; i < _total_routers; i++)
    {
        vector<Connection*> conn_vec = _router_connections_map[i];
        for(int j = 0; j < conn_vec.size(); j++)
        {
            int port_no = _connection_manager_list[i].add_connection(*conn_vec[j]);
            conn_vec[j]->port = port_no;
        }
    }

    for(int i = 0; i < _total_routers; i++)
    {
        vector<Connection*> conn_vec = _router_terminal_connections_map[i];
        for(int j = 0; j < conn_vec.size(); j++)
        {
            int port_no = _connection_manager_list[i].add_connection(*conn_vec[j]);
            conn_vec[j]->port = port_no;
        }
    }

    for(int i = 0; i < _total_routers; i++)
    {
        int src_grp_id = i / _num_routers_per_group;

        map< int, vector< Connection > > map_for_this_router_to_groups;
        for(int j = 0; j < _num_groups; j++)
        {
            int dest_grp_id = j;
            pair<int,int> group_pair = make_pair(src_grp_id, dest_grp_id);
            vector<Connection> derefd_vec;
            for(vector<Connection*>::iterator it = _global_group_connection_map[group_pair].begin(); it!= _global_group_connection_map[group_pair].end(); it++)
            {
                derefd_vec.push_back(*(*it));
            }
            map_for_this_router_to_groups[dest_grp_id] = derefd_vec;
        }
        _connection_manager_list[i].set_routed_connections_to_groups(map_for_this_router_to_groups);
    }
}

void NetworkManager::solidify_network()
{


    //add copies of all of the connections to the connection managers
    add_conns_to_connection_managers();
    for(vector<ConnectionManager>::iterator it = _connection_manager_list.begin(); it != _connection_manager_list.end(); it++)
    {
        it->solidify_connections(); //solidify those connection managers
    }
    _is_solidified = true; //the network is now solidified
}

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

int ConnectionManager::add_connection(int dest_gid, ConnectionType type)
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

    int port = add_connection(conn);
    return port;
}

int ConnectionManager::add_connection(Connection conn)
{
    if (is_solidified)
        tw_error(TW_LOC,"ConnectionManager: Attempting to add connections after manager has been solidified!\n");

    switch (conn.conn_type)
    {
        case CONN_LOCAL:
            if (_used_intra_ports < _max_intra_ports){
                conn.port = this->get_used_ports_for(CONN_LOCAL);
                intraGroupConnections[conn.dest_lid].push_back(conn);
                _used_intra_ports++;
            }
            else
                tw_error(TW_LOC,"Attempting to add too many local connections per router - exceeding configuration value: %d",_max_intra_ports);
            break;
        case CONN_GLOBAL:
            if(_used_inter_ports < _max_inter_ports) {
                conn.port = _max_intra_ports + this->get_used_ports_for(CONN_GLOBAL);
                globalConnections[conn.dest_gid].push_back(conn);
                _used_inter_ports++;
            }
            else
                tw_error(TW_LOC,"Attempting to add too many global connections per router - exceeding configuration value: %d",_max_inter_ports);
            break;
        case CONN_TERMINAL:
            if(_used_terminal_ports < _max_terminal_ports){
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
    }

    _portMap[conn.port] = conn;
    return conn.port;
}

void ConnectionManager::set_routed_connections_to_groups(map<int, vector<Connection> > conn_map)
{
    _routed_connections_to_group_map = conn_map;
}

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
    Connection conn = _portMap[port];
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
    return get_connection_on_port(port, true);
}

bool ConnectionManager::is_connected_to_by_type(int dest_id, ConnectionType type)
{
    return is_connected_to_by_type(dest_id, type, false); //by default, don't include failed links
}

bool ConnectionManager::is_connected_to_by_type(int dest_id, ConnectionType type, bool include_failed)
{
    map<int, vector<Connection> > the_map;
    map<int, vector<Connection> >::iterator map_it;

    if (include_failed)
    {
        if (type == CONN_LOCAL)
            the_map = intraGroupConnections;
        else if (type == CONN_GLOBAL)
            the_map = globalConnections;
        else if (type == CONN_TERMINAL)
            the_map = terminalConnections;
        else
            assert(false);
    }
    else {
        if (type == CONN_LOCAL)
            the_map = intraGroupConnections_nofail;
        else if (type == CONN_GLOBAL)
            the_map = globalConnections_nofail;
        else if (type == CONN_TERMINAL)
            the_map = terminalConnections_nofail;
        else
            assert(false);
    }

    vector<Connection>::iterator vec_it;
    map_it = the_map.find(dest_id);
    if (map_it != the_map.end())
    {
        if (map_it->second.size() > 0) //verify that there is at least one connection - empty vector shouldn't count
            return true;
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
    return _portMap[port_num].conn_type;
}

bool ConnectionManager::get_port_failed_status(int port_num)
{
    return _portMap[port_num].is_failed;
}

vector< Connection > ConnectionManager::get_connections_to_gid(int dest_gid, ConnectionType type)
{
    return get_connections_to_gid(dest_gid, type, false);
}

vector< Connection > ConnectionManager::get_connections_to_gid(int dest_gid, ConnectionType type, bool include_failed)
{
    vector<Connection> conn_vec;
    try {
        if (include_failed) 
        {
            switch (type)
            {
                case CONN_LOCAL:
                        return intraGroupConnections.at(dest_gid%_num_routers_per_group);
                break;
                case CONN_GLOBAL:
                        return globalConnections.at(dest_gid);
                break;
                case CONN_TERMINAL:
                        return terminalConnections.at(dest_gid);
                break;
                default:
                    assert(false);
                    // TW_ERROR(TW_LOC, "get_connections(type): Undefined connection type\n");
            }
        }
        else 
        {
            switch (type)
            {
                case CONN_LOCAL:
                        return intraGroupConnections_nofail.at(dest_gid%_num_routers_per_group);
                break;
                case CONN_GLOBAL:
                        return globalConnections_nofail.at(dest_gid);
                break;
                case CONN_TERMINAL:
                        return terminalConnections_nofail.at(dest_gid);
                break;
                default:
                    assert(false);
                    // TW_ERROR(TW_LOC, "get_connections(type): Undefined connection type\n");
            }
        }
    } catch (exception e)
    {
        return vector<Connection>(); //so we don't accidentally add a key with []
    }
}

vector< Connection > ConnectionManager::get_connections_to_group(int dest_group_id)
{
    return get_connections_to_group(dest_group_id, false);
}

vector< Connection > ConnectionManager::get_connections_to_group(int dest_group_id, bool include_failed)
{
    try {
        if(include_failed)
        {
            return _connections_to_groups_map.at(dest_group_id);
        }
        else
        {
            return _connections_to_groups_map_nofail.at(dest_group_id);
        }
    } catch (exception e) {
        return vector<Connection>(); //so we don't accidentally add a key with []
    }
}

vector< Connection > ConnectionManager::get_connections_by_type(ConnectionType type)
{
    return get_connections_by_type(type, false);
}

vector< Connection > ConnectionManager::get_connections_by_type(ConnectionType type, bool include_failed)
{
    try {
        if(include_failed)
            return _all_conns_by_type_map.at(type);
        else
            return _all_conns_by_type_map_nofail.at(type);
    } catch (exception e) {
        return vector<Connection>(); //so we don't accidentally add a key with []
    }
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
        return _other_groups_i_connect_to_nofail;
}

void ConnectionManager::solidify_connections()
{
    //--connections to group
    for(map<int, vector<Connection> >::iterator it = globalConnections.begin(); it != globalConnections.end(); it++)
    {   
        vector< Connection >::iterator vec_it = it->second.begin();
        for(; vec_it != it->second.end(); vec_it++)
        {
            int dest_group_id = vec_it->dest_group_id;
            _connections_to_groups_map[dest_group_id].push_back(*vec_it);
        }

    }

    //other groups i connect to
    for(map<int, vector<Connection> >::iterator it = _connections_to_groups_map.begin(); it != _connections_to_groups_map.end(); it++)
    {
        if(it->second.size() > 0) //empty vectors can be inserted if [] is used on a non-exist key
            _other_groups_i_connect_to.push_back(it->first);
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


    // make copies of data structures but without failed links -----------------------------------------
    map< int, vector< Connection > >::iterator it;
    for(it = intraGroupConnections.begin(); it != intraGroupConnections.end(); it++)
    {
        int id = it->first;
        vector<Connection> conns_to_id = it->second;
        vector< Connection >::iterator vec_it;
        for(vec_it = conns_to_id.begin(); vec_it != conns_to_id.end(); vec_it++)
        {
            if(vec_it->is_failed == 0)
                intraGroupConnections_nofail[id].push_back(*vec_it);
        }
    }

    for(it = globalConnections.begin(); it != globalConnections.end(); it++)
    {
        int id = it->first;
        vector<Connection> conns_to_id = it->second;
        vector< Connection >::iterator vec_it;
        for(vec_it = conns_to_id.begin(); vec_it != conns_to_id.end(); vec_it++)
        {
            if(vec_it->is_failed == 0)
                globalConnections_nofail[id].push_back(*vec_it);
        }
    }
        
    for(it = terminalConnections.begin(); it != terminalConnections.end(); it++)
    {
        int id = it->first;
        vector<Connection> conns_to_id = it->second;
        vector< Connection >::iterator vec_it;
        for(vec_it = conns_to_id.begin(); vec_it != conns_to_id.end(); vec_it++)
        {
            if(vec_it->is_failed == 0)
                terminalConnections_nofail[id].push_back(*vec_it);
        }
    }

    for(it = _connections_to_groups_map.begin(); it != _connections_to_groups_map.end(); it++)
    {
        int id = it->first;
        vector<Connection> conns_to_id = it->second;
        vector< Connection >::iterator vec_it;
        for(vec_it = conns_to_id.begin(); vec_it != conns_to_id.end(); vec_it++)
        {
            if(vec_it->is_failed == 0)
                _connections_to_groups_map_nofail[id].push_back(*vec_it);
        }
    }

    for(it = _connections_to_groups_map_nofail.begin(); it != _connections_to_groups_map_nofail.end(); it++)
    {
        if(it->second.size() > 0) //empty vectors can be inserted if [] is used on a non-exist key
        {
            add_as_if_set_int(it->first, _other_groups_i_connect_to_nofail);
        }
    }


    for(it = _all_conns_by_type_map.begin(); it != _all_conns_by_type_map.end(); it++)
    {
        int id = it->first;
        vector<Connection> conns_to_id = it->second;
        vector< Connection >::iterator vec_it;
        for(vec_it = conns_to_id.begin(); vec_it != conns_to_id.end(); vec_it++)
        {
            if(vec_it->is_failed == 0)
                _all_conns_by_type_map_nofail[id].push_back(*vec_it);
        }
    }

    for(it = _routed_connections_to_group_map.begin(); it != _routed_connections_to_group_map.end(); it++)
    {
        int group_id = it->first;
        vector<Connection> conns_to_group = it->second;
        vector<Connection>::iterator vec_it;
        for(vec_it = conns_to_group.begin(); vec_it != conns_to_group.end(); vec_it++)
        {
            if(vec_it->is_failed == 0)
                _routed_connections_to_group_map_nofail[group_id].push_back(*vec_it);
        }
    }

    for(it = _routed_connections_to_group_map.begin(); it != _routed_connections_to_group_map.end(); it++)
    {
        int group_id = it->first;
        vector<Connection> conns_to_group = it->second;
        vector<Connection>::iterator vec_it;
        for(vec_it = conns_to_group.begin(); vec_it != conns_to_group.end(); vec_it++)
        {
            int src_gid = vec_it->src_gid;
            add_as_if_set_int(src_gid, _routed_router_gids_to_group_map[group_id]);

            if (vec_it->is_failed == 0) {
                add_as_if_set_int(src_gid, _routed_router_gids_to_group_map_nofail[group_id]);
            }
        }
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
    map<int,Connection>::iterator it = _portMap.begin();
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
