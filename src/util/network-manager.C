#include "codes/network-manager.h"
#include <algorithm>

#define MAX_PATH_VAL 999

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


NetworkManager::NetworkManager(int total_routers, int total_terminals, int num_routers_per_group, int num_lc_per_router, int num_gc_per_router, int num_cn_conns_per_router, int num_rails, int num_planes, int max_local_hops, int max_global_hops)
{
    _total_routers = total_routers;
    _total_terminals = total_terminals;
    _total_groups = total_routers / num_routers_per_group;
    _num_rails = num_rails;
    _num_planes = num_planes;
    _num_routers_per_group = num_routers_per_group;
    _num_groups = _total_groups / num_planes;
    _num_lc_pr = num_lc_per_router;
    _num_gc_pr = num_gc_per_router;
    _num_cn_pr = num_cn_conns_per_router;
    // _num_unique_term_pr = num_cn_conns_per_router / num_rails;

    _max_local_hops_per_group = max_local_hops;
    _max_global_hops = max_global_hops;

    _num_router_conns = 0;
    _num_router_terminal_conns = 0;
    _link_failures_enabled = false;
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

        ConnectionManager conn_man = ConnectionManager(src_id_local, src_id_global, src_group, _num_lc_pr, _num_gc_pr, _num_cn_pr, 0, _num_routers_per_group, _num_groups, _num_planes, MAN_ROUTER);
        _connection_manager_list.push_back(conn_man);
    }

    for(int i = 0; i < _total_terminals; i++)
    {
        int src_id_global = i;
        int src_id_local = src_id_global % num_cn_conns_per_router;
        int src_group = -1;

        ConnectionManager conn_man = ConnectionManager(src_id_local, src_id_global, src_group, 0, 0, 0, num_rails, _num_routers_per_group, _num_groups, _num_planes, MAN_TERMINAL);
        _terminal_connection_manager_list.push_back(conn_man);
    }

    _is_solidified = false;
}

void NetworkManager::enable_link_failures()
{
    _link_failures_enabled = true;
}

bool NetworkManager::is_link_failures_enabled()
{
    return _link_failures_enabled;
}

ConnectionManager& NetworkManager::get_connection_manager_for_router(int router_gid)
{
    return _connection_manager_list[router_gid];
}

ConnectionManager& NetworkManager::get_connection_manager_for_terminal(int terminal_gid)
{
    return _terminal_connection_manager_list[terminal_gid];
}

void NetworkManager::add_link(Link_Info link)
{
    Connection *conn = (Connection*)malloc(sizeof(Connection));
    conn->port = -1; //will be set by the Connection Manager of the router (src_gid) and defined in add_cons_to_connectoin_managers
    conn->src_lid = (link.src_gid) % _num_routers_per_group;
    conn->src_gid = link.src_gid;
    conn->src_group_id = link.src_gid / _num_routers_per_group;
    conn->dest_lid = (link.dest_gid) % _num_routers_per_group;
    conn->dest_gid = link.dest_gid;
    conn->dest_group_id = link.dest_gid / _num_routers_per_group;
    conn->conn_type = link.conn_type;
    conn->rail_or_planar_id = link.rail_id;
    conn->is_failed = false;

    if (link.conn_type != CONN_TERMINAL)
    {
        _num_router_conns++;

        //put the conn into its owning structure
        _router_connections_map[link.src_gid].push_back(conn);

        //put into useful maps
        _router_to_router_connection_map[make_pair(conn->src_gid,conn->dest_gid)].push_back(conn);

        if (conn->conn_type == CONN_GLOBAL)
        {
            _global_group_connection_map[make_pair(conn->src_group_id, conn->dest_group_id)].push_back(conn);
            _router_to_router_global_conn_map[link.src_gid].push_back(conn);
        }
        if (conn->conn_type == CONN_LOCAL)
        {
            _router_to_router_local_conn_map[link.src_gid].push_back(conn);
        }

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

        //Terminals don't have their own interconnection mapping file that has both links so we need to create a
        //new connection for the terminal connection manager too that goes from the terminal to the router.
        Connection *term_conn = (Connection*)malloc(sizeof(Connection));
        term_conn->port = conn->rail_or_planar_id;
        term_conn->src_lid = conn->dest_lid;
        term_conn->src_gid = conn->dest_gid;
        term_conn->dest_lid = conn->src_lid;
        term_conn->dest_gid = conn->src_gid;
        term_conn->src_group_id = -1;
        term_conn->dest_group_id = conn->src_group_id;
        term_conn->conn_type = CONN_INJECTION;
        term_conn->rail_or_planar_id = conn->rail_or_planar_id;
        term_conn->is_failed = false;

        _terminal_router_connections_map[term_conn->src_gid].push_back(term_conn);
        _terminal_to_router_connection_map[make_pair(term_conn->src_gid, term_conn->dest_gid)].push_back(term_conn);
    }
}

void NetworkManager::add_link_failure_info(Link_Info link)
{
    if(_link_failures_enabled == false)
        tw_error(TW_LOC,"Network Manager: attempting to add link failure info but link failure has not been enabled via NetworkManager.allow_link_failures()\n");

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
    if(_link_failures_enabled == false)
        tw_error(TW_LOC,"Network Manager: attempting to fail link but link failure has not been enabled via NetworkManager.allow_link_failures()\n");

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
                break;
            }
        }
    }
    else
    {
        vector<Connection*> conns_to_term_gid = _router_to_terminal_connection_map[make_pair(link.src_gid,link.dest_gid)];
        int num_failed_already = get_failed_count_from_vector(conns_to_term_gid);
        if (num_failed_already == conns_to_term_gid.size())
            tw_error(TW_LOC, "Attempting to fail more links from Router GID %d to Terminal GID %d than exist. Already Failed %d\n", link.src_gid, link.dest_gid, num_failed_already);

        int failed_rail = 0;
        vector<Connection*>:: iterator it = _router_to_terminal_connection_map[make_pair(link.src_gid,link.dest_gid)].begin();
        for(; it != _router_to_terminal_connection_map[make_pair(link.src_gid,link.dest_gid)].end(); it++)
        {
            if(!(*it)->is_failed)
            {
                (*it)->is_failed = 1;
                failed_rail = (*it)->rail_or_planar_id;
                break;
            }
        }

        //we also need to fail the corresponding injection link
        int router_id = link.src_gid;
        int term_id = link.dest_gid;
        
        it =_terminal_to_router_connection_map[make_pair(term_id,router_id)].begin();
        for(; it != _terminal_to_router_connection_map[make_pair(term_id,router_id)].end(); it++)
        {
            if((*it)->rail_or_planar_id == failed_rail)
            {
                (*it)->is_failed = 1;
                break;
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

void NetworkManager::calculate_floyd_warshall_shortest_paths()
{
    if(!g_tw_mynode)
        printf("\nNetwork Manager: Performing Shortest Path Calculations...\n");

    _shortest_path_vals = (int**)calloc(_total_routers, sizeof(int*));
    _next = (int**)calloc(_total_routers, sizeof(int*));
    int** costMat = (int**)malloc(_total_routers* sizeof(int*));
    int** dist = (int**)malloc(_total_routers* sizeof(int*));
    for(int i = 0; i < _total_routers; i++)
    {
        _shortest_path_vals[i] = (int*)calloc(_total_routers, sizeof(int));
        _next[i] = (int*)calloc(_total_routers, sizeof(int));
        costMat[i] = (int*)calloc(_total_routers, sizeof(int));
        dist[i] = (int*)calloc(_total_routers, sizeof(int));
        for(int j = 0; j < _total_routers; j++)
        {
            _shortest_path_nexts[make_pair(i,j)] = vector<int>();
        }
    }


    //set up cost matrix
    // int costMat[_total_routers][_total_routers];
    for(int i = 0; i < _total_routers; i++)
    {
        for(int j = 0; j <_total_routers; j++)
        {
            int plane_id = j / (_total_routers / _num_planes);
            int src_gid = i;
            int dest_gid = j;
            int is_adj = adjacency_matrix_nofail[plane_id][i][j];
            // printf("%d ",adjacency_matrix_nofail[i][j]);
            if (is_adj)
                costMat[i][j] = 1;
            else if (i == j)
                costMat[i][j] = 0;
            else
                costMat[i][j] = MAX_PATH_VAL;
        }
        // printf("\n");
    }

    // int dist[_total_routers][_total_routers];
    for(int i = 0; i < _total_routers; i++)
    {
        for(int j = 0; j < _total_routers; j++)
        {
            dist[i][j] = costMat[i][j];
            if(dist[i][j] == 1)
            {
                _next[i][j] = j;
                _shortest_path_nexts[make_pair(i,j)].push_back(j);
            }
        }
    }
    for(int i = 0; i < _total_routers; i++)
    {
        dist[i][i] = 0;
        _next[i][i] = i;
        _shortest_path_nexts[make_pair(i,i)].push_back(i);
    }

    int rpp = _total_routers / _num_planes;
    for(int p = 0; p < _num_planes; p++) //we don't need to attempt to calculate distances between planes
    {
        for(int k = p*rpp; k < rpp + p*rpp; k++)
        {
            for(int i = p*rpp; i < rpp + p*rpp; i++)
            {
                for(int j = p*rpp; j < rpp + p*rpp; j++)
                {
                    if(dist[i][j] > dist[i][k] + dist[k][j])
                    {
                        dist[i][j] = dist[i][k] + dist[k][j];
                        _shortest_path_nexts[make_pair(i,j)].clear();
                        _shortest_path_nexts[make_pair(i,j)].push_back(_next[i][k]);
                        _next[i][j] = _next[i][k];

                    }
                    else if (dist[i][k] + dist[k][j] == dist[i][j] && dist[i][j] != MAX_PATH_VAL && k != j && k != i)
                    {
                        _shortest_path_nexts[make_pair(i,j)].push_back(k);
                    }
                }
            }
        }
    }


    // int rpp = (_total_routers / _num_planes);

    // for(int p = 0; p < _num_planes; p++)
    // {
    //     for(int i = 0; i < rpp; i++)
    //     {
    //         for(int j = 0; j < rpp; j++)
    //         {
    //             int src_gid = i + (p*rpp);
    //             int dest_gid = j + (p*rpp);

    //             _shortest_path_vals[src_gid][dest_gid] = dist[src_gid][dest_gid];
    //             printf("%d ",_shortest_path_vals[src_gid][dest_gid]);
    //         }
    //         printf("\n");
    //     }
    //     printf("\n");
    // }

    for(int i = 0; i <_total_routers; i++)
    {
        for(int j = 0; j < _total_routers; j++)
        {

            _shortest_path_vals[i][j] = dist[i][j];
            // printf("%d ",_shortest_path_vals[i][j]);
        }
        // printf("\n");
    }

    for(int i = 0; i < _total_routers; i++)
    {
        free(dist[i]);
        free(costMat[i]);
    }
    free(dist);
    free(costMat);
}

int NetworkManager::get_shortest_dist_between_routers(int src_gid, int dest_gid)
{
    return _shortest_path_vals[src_gid][dest_gid];
}

//dynamic programming attempt at determining path validity. Depth first search on all local and global links available to the router
//toward the destination with an upper bound on local and global hops from said source
bool NetworkManager::is_valid_path_between(int src_gid, int dest_gid, int max_local, int max_global, set<int> visited)
{
    if((src_gid / (_total_routers/_num_planes)) != (dest_gid / (_total_routers/_num_planes)))
        return false; //different planes have no valid path between them

    if(src_gid == dest_gid)
        return true;

    bool has_valid_path = false;
    visited.insert(src_gid);

    vector<Connection*> local_conns_from_src = _router_to_router_local_conn_map[src_gid];
    vector<Connection*> global_conns_from_src = _router_to_router_global_conn_map[src_gid];

    if (max_local > 0)
    {
        for(int i = 0; i < local_conns_from_src.size(); i++)
        {
            if (local_conns_from_src[i]->is_failed == false && visited.count(local_conns_from_src[i]->dest_gid) == 0)
            {
                bool ret_valid;
                try {
                    ret_valid =_valid_connection_map.at(make_tuple(local_conns_from_src[i]->dest_gid,dest_gid, max_local-1, max_global));
                }catch (exception e) {
                    ret_valid = is_valid_path_between(local_conns_from_src[i]->dest_gid, dest_gid, max_local-1, max_global, visited); 
                    _valid_connection_map[make_tuple(local_conns_from_src[i]->dest_gid, dest_gid, max_local-1, max_global)] = ret_valid;
                }
                has_valid_path = has_valid_path || ret_valid;
            }
            if (has_valid_path == true) //then we already know the answer 
                return true; //otherwise we keep searching
        }
    }
    if (max_global > 0)
    {
        for(int i = 0; i < global_conns_from_src.size(); i++)
        {
            if (global_conns_from_src[i]->is_failed == false && visited.count(global_conns_from_src[i]->dest_gid) == 0)
            {
                bool ret_valid;
                try {
                    ret_valid =_valid_connection_map.at(make_tuple(global_conns_from_src[i]->dest_gid,dest_gid, _max_local_hops_per_group, max_global-1));
                }catch (exception e) {
                    ret_valid = is_valid_path_between(global_conns_from_src[i]->dest_gid, dest_gid, _max_local_hops_per_group, max_global-1, visited); 
                    _valid_connection_map[make_tuple(global_conns_from_src[i]->dest_gid, dest_gid, _max_local_hops_per_group, max_global-1)] = ret_valid;
                }
                has_valid_path = has_valid_path || ret_valid;
            }
            if (has_valid_path == true) //then we already know the answer 
                return true; //otherwise we keep searching
        }
    }
    return false;
}

bool NetworkManager::is_valid_path_between(int src_gid, int dest_gid, int max_local, int max_global)
{
    set<int> visited;
    return is_valid_path_between(src_gid, dest_gid, max_local, max_global, visited);

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

    for(int i = 0; i < _total_terminals; i++)
    {
        vector<Connection*> conn_vec = _terminal_router_connections_map[i];
        for(int j = 0; j < conn_vec.size(); j++)
        {
            _terminal_connection_manager_list[i].add_connection(*conn_vec[j]);
        }
    }

    for(int i = 0; i < _total_routers; i++)
    {
        int src_grp_id = i / _num_routers_per_group;

        map< int, vector< Connection > > map_for_this_router_to_groups;
        for(int j = 0; j < _total_groups; j++)
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
        _connection_manager_list[i].add_group_group_connection_information(_global_group_connection_map);

    }
}

void NetworkManager::solidify_network()
{

    adjacency_matrix = (int***)calloc(_num_planes, sizeof(int**));
    adjacency_matrix_nofail = (int***)calloc(_num_planes, sizeof(int**));

    for(int i = 0; i < _num_planes; i++)
    {
        adjacency_matrix[i] = (int**)calloc(_total_routers, sizeof(int*));
        adjacency_matrix_nofail[i] = (int**)calloc(_total_routers, sizeof(int*));
        for(int j = 0; j < _total_routers; j++)
        {
            adjacency_matrix[i][j] = (int*)calloc(_total_routers,sizeof(int));
            adjacency_matrix_nofail[i][j] = (int*)calloc(_total_routers,sizeof(int));
        }
    }


    for(int i = 0; i < _total_routers; i++)
    {
        int src_gid = i;
        vector<Connection*> conns_from_src = _router_connections_map[src_gid];
        vector<Connection*>::iterator it = conns_from_src.begin();
        for(; it != conns_from_src.end(); it++)
        {
            int plane_id = (*it)->rail_or_planar_id;
            int dest_gid = (*it)->dest_gid;
            adjacency_matrix[plane_id][src_gid][dest_gid] = 1; //bidirectional will be handled later in the loop
        }
    }

    map<int, vector<Link_Info> >::iterator it;
    //fail the router router connections
    for(it = _router_link_failure_lists.begin(); it != _router_link_failure_lists.end(); it++)
    {
        //iterate over vector of link info
        for(int i = 0; i < it->second.size(); i++)
        {
            Link_Info link = it->second[i];
            fail_connection(link);
        }
    }

    //fail the router terminal connections
    for(it = _router_terminal_link_failure_lists.begin(); it != _router_terminal_link_failure_lists.end(); it++)
    {
        //iterate over vector of link info
        for(int i = 0; i < it->second.size(); i++)
        {
            Link_Info link = it->second[i];
            fail_connection(link);
        }
    }

    for(int i = 0; i < _total_routers; i++)
    {
        int src_gid = i;
        vector<Connection *> conns_from_src = _router_connections_map[src_gid];
        vector<Connection*>::iterator it = conns_from_src.begin();
        for(; it != conns_from_src.end(); it++)
        {
            int plane_id = (*it)->rail_or_planar_id;
            int dest_gid = (*it)->dest_gid;
            if((*it)->is_failed == false)
                adjacency_matrix_nofail[plane_id][src_gid][dest_gid] = 1; //bidirectional will be handled later in the loop
        }
    }

    calculate_floyd_warshall_shortest_paths();

    //add copies of all of the connections to the connection managers
    add_conns_to_connection_managers();
    for(vector<ConnectionManager>::iterator it = _connection_manager_list.begin(); it != _connection_manager_list.end(); it++)
    {
        it->solidify_connections(); //solidify those connection managers
    }
    for(vector<ConnectionManager>::iterator it = _terminal_connection_manager_list.begin(); it != _terminal_connection_manager_list.end(); it++)
    {
        it->solidify_connections();
    }

    printf("Network Manager: calculating valid paths\n");
    int rpp = _total_routers / _num_planes;
    for(int p = 0; p < _num_planes; p++)
    {
        for(int i = 0; i < rpp;i++)
        {
            for(int j = 0; j < rpp;j++)
            {
                int src_gid = i + (p*rpp);
                int dest_gid = j + (p*rpp);
                bool is_valid = is_valid_path_between(src_gid,dest_gid,_max_local_hops_per_group,_max_global_hops);
                _valid_connection_map[make_tuple(src_gid,dest_gid,_max_local_hops_per_group,_max_global_hops)] = is_valid;
                // printf("%d - - > %d  == %d\n",src_gid,dest_gid, is_valid);
            }
        }
    }

    // for(int p = 0; p < _num_planes; p++)
    // {
    //     for(int i = 0; i < rpp;i++)
    //     {
    //         for(int j = 0; j < rpp;j++)
    //         {
    //             int src_gid = i + (p*rpp);
    //             int dest_gid = j + (p*rpp);
    //             bool is_valid = is_valid_path_between(src_gid,dest_gid,1,1);
    //             _valid_connection_map[make_tuple(src_gid,dest_gid,1,1)] = is_valid;
    //             // printf("%d - - > %d  == %d MINIMAL\n",src_gid,dest_gid, is_valid);
    //         }
    //     }
    // }

    _is_solidified = true; //the network is now solidified
}

//*******************    Connection Manager Implementation *******************************************
ConnectionManager::ConnectionManager(int src_id_local, int src_id_global, int src_group, int max_intra, int max_inter, int max_term, int num_router_per_group, int num_groups)
{
    ConnectionManager(src_id_local, src_id_global, src_group, max_intra, max_inter, max_term, 0, num_router_per_group, num_groups, 1, MAN_ROUTER);
}

ConnectionManager::ConnectionManager(int src_id_local, int src_id_global, int src_group, int max_intra, int max_inter, int max_term, int max_injection, int num_router_per_group, int num_groups, int num_planes, ManagerType manType)
{
    _manType = manType;

    _source_id_local = src_id_local;
    _source_id_global = src_id_global;
    _source_group = src_group;

    _used_intra_ports = 0;
    _used_inter_ports = 0;
    _used_terminal_ports = 0;
    _used_injection_ports = 0;

    _failed_intra_ports = 0;
    _failed_inter_ports = 0;
    _failed_terminal_ports = 0;
    _failed_injection_ports = 0;

    _max_intra_ports = max_intra;
    _max_inter_ports = max_inter;
    _max_terminal_ports = max_term;
    _max_injection_ports = max_injection;

    _num_routers_per_group = num_router_per_group;
    _num_planes = num_planes;
    _total_groups = num_groups * num_planes;
    _num_groups = num_groups;

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
            if (_manType == MAN_TERMINAL)
                tw_error(TW_LOC, "Attempting to add local connections to a terminal connection manager\n");
            if (_used_intra_ports < _max_intra_ports){
                conn.port = this->get_used_ports_for(CONN_LOCAL);
                intraGroupConnections[conn.dest_lid].push_back(conn);
                _used_intra_ports++;
            }
            else
                tw_error(TW_LOC,"Attempting to add too many local connections per router - exceeding configuration value: %d",_max_intra_ports);
            break;
        case CONN_GLOBAL:
            // printf("R%d P%d: Global Add to %d\n", _source_id_global, conn.rail_or_planar_id, conn.dest_gid);

            if (_manType == MAN_TERMINAL)
                tw_error(TW_LOC, "Attempting to add global connections to a terminal connection manager\n");
            if(_used_inter_ports < _max_inter_ports) {
                conn.port = _max_intra_ports + this->get_used_ports_for(CONN_GLOBAL);
                globalConnections[conn.dest_gid].push_back(conn);
                _used_inter_ports++;
            }
            else
                tw_error(TW_LOC,"Attempting to add too many global connections per router - exceeding configuration value: %d",_max_inter_ports);
            break;
        case CONN_TERMINAL:
            if (_manType == MAN_TERMINAL)
                tw_error(TW_LOC, "Attempting to add terminal connections to a terminal connection manager\n");
            if(_used_terminal_ports < _max_terminal_ports){
                conn.port = _max_intra_ports + _max_inter_ports + this->get_used_ports_for(CONN_TERMINAL);
                conn.dest_group_id = _source_group;
                terminalConnections[conn.dest_gid].push_back(conn);
                _used_terminal_ports++;
            }
            else
                tw_error(TW_LOC,"Attempting to add too many terminal connections per router - exceeding configuration value: %d",_max_terminal_ports);
            break;
        case CONN_INJECTION:
            if(_used_injection_ports < _max_injection_ports){
                injectionConnections[conn.dest_gid].push_back(conn);
                _used_injection_ports++;
            }
            break;
        default:
            assert(false);
    }

    _portMap[conn.port] = conn;
    return conn.port;
}

void ConnectionManager::add_group_group_connection_information(map<pair<int,int>, vector<Connection*> > group_group_connections)
{
    map<pair<int, int>, vector<Connection*> >::iterator map_it = group_group_connections.begin();
    for(; map_it != group_group_connections.end(); map_it++)
    {
        pair<int,int> pair_key = map_it->first;
        int src_grp = pair_key.first;
        int dest_grp = pair_key.second;

        if(map_it->second.size() > 0)
        {
            add_as_if_set_int(dest_grp, _group_group_connection_map[src_grp]);
            add_as_if_set_int(src_grp, _group_group_connection_map[dest_grp]);
        }

        vector<Connection*>::iterator vec_it = map_it->second.begin();
        for(; vec_it != map_it->second.end(); vec_it++)
        {
            if((*vec_it)->is_failed == false)
            {
                add_as_if_set_int(dest_grp, _group_group_connection_map_nofail[src_grp]);
                add_as_if_set_int(src_grp, _group_group_connection_map_nofail[dest_grp]);
                break;
            }
        }
    }
}

void ConnectionManager::set_routed_connections_to_groups(map<int, vector<Connection> > conn_map)
{
    _routed_connections_to_group_map = conn_map;
}

vector< Connection > ConnectionManager::get_routed_connections_to_group(int group_id, bool get_next_hop, bool include_failed)
{
    vector< Connection > conns;
    vector< Connection >::iterator it;
    for(it = _routed_connections_to_group_map[group_id].begin(); it != _routed_connections_to_group_map[group_id].end(); it++)
    {

        if (get_next_hop) //then verify that we have a direct connection to the src_lid of this routed conn
        {
            vector< Connection > local_conns = get_connections_to_gid(it->src_gid, CONN_LOCAL, include_failed);
            conns.insert(conns.end(), local_conns.begin(), local_conns.end());
        }
        else
        {
            if(!include_failed)
            {
                if(it->is_failed == false)
                    conns.push_back(*it);
            }
            else
                conns.push_back(*it);
        }
    }
    return conns;
}

vector< Connection > ConnectionManager::get_routed_connections_to_group(int group_id, bool get_next_hop)
{
    return get_routed_connections_to_group(group_id, get_next_hop, false);
}

vector< int > ConnectionManager::get_router_gids_with_global_to_group(int group_id)
{
    return get_router_gids_with_global_to_group(group_id, false);
}

vector< int > ConnectionManager::get_router_gids_with_global_to_group(int group_id, bool include_failed)
{
    try {
        if (include_failed)
            return _routed_router_gids_to_group_map.at(group_id);
        else
            return _routed_router_gids_to_group_map_nofail.at(group_id);
    } catch (exception e) {
        return vector<int>();
    }
}

// vector< Connection > ConnectionManager::get_routed_connections_in_group(int dest_lid, bool include_failed)
// {

// }

// vector< Connection > ConnectionManager::get_routed_connections_in_group(int dest_lid)
// {
//     return get_routed_connections_in_group(dest_lid, false);
// }

vector< int > ConnectionManager::get_groups_that_connect_to_group(int dest_group, bool include_failed)
{
    try{
        if (include_failed)
            return _group_group_connection_map.at(dest_group);
        else
            return _group_group_connection_map_nofail.at(dest_group);
    } catch (exception e) {
        return vector<int>();
    }

}

vector< int > ConnectionManager::get_groups_that_connect_to_group(int dest_group)
{
    return get_groups_that_connect_to_group(dest_group, false);
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
        empty_conn.port = -1;
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
        else if (type == CONN_INJECTION)
            the_map = injectionConnections;
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
        else if (type == CONN_INJECTION)
            the_map = injectionConnections_nofail;
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
    if (is_connected_to_by_type(dest_global_id, CONN_INJECTION, include_failed))
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
        break;
        case CONN_GLOBAL:
            return _used_inter_ports - (_failed_inter_ports * account_for_failed);
        break;
        case CONN_TERMINAL:
            return _used_terminal_ports - (_failed_terminal_ports * account_for_failed);
        break;
        case CONN_INJECTION:
            return _used_injection_ports - (_failed_injection_ports * account_for_failed);
        break;
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
                case CONN_INJECTION:
                        return injectionConnections.at(dest_gid);
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
                case CONN_INJECTION:
                        return injectionConnections_nofail.at(dest_gid);
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
    for ( int enum_int = CONN_LOCAL; enum_int != CONN_INJECTION + 1; enum_int++ )
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
            case CONN_INJECTION:
                theMap = injectionConnections;
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

    for(it = injectionConnections.begin(); it != injectionConnections.end(); it++)
    {
        int id = it->first;
        vector<Connection> conns_to_id = it->second;
        vector< Connection >::iterator vec_it;
        for(vec_it = conns_to_id.begin(); vec_it != conns_to_id.end(); vec_it++)
        {
            if(vec_it->is_failed == 0)
                injectionConnections_nofail[id].push_back(*vec_it);
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
    if(_manType == MAN_ROUTER)
        printf("Connections for Router: %d ---------------------------------------\n",_source_id_global);
    else
        printf("Connections for Terminal: %d -------------------------------------\n",_source_id_global);

    int ports_printed = 0;
    map<int,Connection>::iterator it = _portMap.begin();
    for(; it != _portMap.end(); it++)
    {
        if ( (ports_printed == 0) && (_used_intra_ports > 0) )
        {
            printf(" -- Intra-Group Connections -- \n");
            printf("  Port  |  Dest_ID  |  Group  |  Link Type  |  Rail ID  |  Fail Status\n");
        }
        if ( (ports_printed == _used_intra_ports) && (_used_inter_ports > 0) )
        {
            printf(" -- Inter-Group Connections -- \n");
            printf("  Port  |  Dest_ID  |  Group  |  Link Type  |  Rail ID  |  Fail Status\n");
        }
        if ( (ports_printed == _used_intra_ports + _used_inter_ports) && (_used_terminal_ports > 0) )
        {
            printf(" -- Terminal Connections -- \n");
            printf("  Port  |  Dest_ID  |  Group  |  Link Type  |  Rail ID  |  Fail Status\n");
        }
        if ( (ports_printed == _used_intra_ports + _used_inter_ports + _used_terminal_ports) && (_used_injection_ports > 0) )
        {
            printf(" -- Injection Connections -- \n");
            printf("  Port  |  Dest_ID  |  Group  |  Link Type  |  Rail ID  |  Fail Status\n");
        }

        int port_num = it->first;
        int group_id = it->second.dest_group_id;

        int id,gid;
        if( get_port_type(port_num) == CONN_LOCAL ) {
            id = it->second.dest_lid;
            gid = it->second.dest_gid;
            printf("  %d   ->   (%d,%d)        :  %d     -  LOCAL  -   %d     -  %d\n", port_num, id, gid, group_id,it->second.rail_or_planar_id,it->second.is_failed);

        } 
        else if (get_port_type(port_num) == CONN_GLOBAL) {
            id = it->second.dest_gid;
            printf("  %d   ->   %d        :  %d     -  GLOBAL    -   %d   -     %d\n", port_num, id, group_id,it->second.rail_or_planar_id,it->second.is_failed);
        }
        else if (get_port_type(port_num) == CONN_TERMINAL) {
            id = it->second.dest_gid;
            printf("  %d   ->   %d        :  %d     -  TERMINAL   -    %d    -     %d\n", port_num, id, group_id,it->second.rail_or_planar_id,it->second.is_failed);
        }
        else if(get_port_type(port_num) == CONN_INJECTION) {
            id = it->second.dest_gid;
            printf("  %d   ->   %d        :  %d     -  INJECTION   -    %d    -     %d\n", port_num, id, group_id,it->second.rail_or_planar_id,it->second.is_failed);
        }
            
        ports_printed++;
    }
}
