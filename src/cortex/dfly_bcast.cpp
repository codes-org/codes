#include <vector>
#include <map>
#include <set>
#include <iostream>
#include <algorithm>
#include <mpi.h>
#include "stdint.h"
#include "stdlib.h"
#include "codes/cortex/dragonfly-cortex-api.h"
#include "codes/codes.h"
#include "codes/configuration.h"
#include "codes/cortex/dfly_bcast.h"

struct placement_info {

	rank_t		rank;
	terminal_id_t	terminal;
	router_id_t	router;
	group_id_t 	group;

	bool operator==(const placement_info& other) const {
		return (rank == other.rank) && (terminal == other.terminal)
		       && (router == other.router) && (group == other.group);
	}
};

typedef std::vector<rank_t> rank_vector_t;
typedef std::map<router_id_t, rank_vector_t> router_map_t;
typedef std::map<group_id_t, router_map_t> group_map_t;
typedef std::pair<rank_t, rank_t> couple_t;

/**
 * This function prints the steps of a tree-based broadcast across the processes of
 * the ranks array. The root of the broadcast is ranks[0].
 * input ranks : array of ranks participating in the broadcast.
 */
static int dfly_bcast_tree(job_id_t jid, const std::vector<rank_t>& ranks, int size, const comm_handler* comm, void* uarg) {
	for(int i=0; i<ranks.size(); i++) {
		int mask = 0x1;
		while(mask < ranks.size()) {
			if(i & mask) break;
			mask = mask << 1;
		}
		mask = mask >> 1;
		while(mask > 0) {
			if(i + mask < ranks.size()) {
				if(comm->my_rank == ranks[i]) {
					comm->do_send(jid,comm->my_rank, size ,ranks[i+mask],0,uarg);
				} else if(comm->my_rank == ranks[i+mask]) {
					comm->do_recv(jid,comm->my_rank, size ,ranks[i],0,uarg);
				}
			}
			mask = mask >> 1;
		}
	}
	return 0;
}

/** 
 * This function builds the topology structure. 
 * input job_id : the job id
 * input ranks : the list of ranks involved
 * output topo : the topology
 * The topology is a map associating group_ids involved in the job with router_maps.
 * A router_map is a map associating router_ids involved in the job with a vector of ranks handled by this router.
 */
static int dfly_build_topology(group_map_t& topo, job_id_t job_id, const std::vector<rank_t>& ranks) {
	
	topo.clear();
	for(int i=0; i<ranks.size(); i++) {
		terminal_id_t t;
		router_id_t   r;
		group_id_t    g;
		cortex_dfly_job_get_terminal_from_rank(job_id, ranks[i], &t);
		cortex_dfly_get_router_from_terminal(t,&r);
		cortex_dfly_get_group_from_router(r,&g);
		topo[g][r].push_back(ranks[i]);
	}

	return 0;
}

/** 
 * This function builds a team for group g based on the given topology.
 * output team : vector of ranks belonging to the group and composing the team
 * input topo : topology structure
 * input job_id : id of the job
 * input root : placement information for the root process
 * input g : group for which to build a team
 * The root rank is necessarily a team member if the group contains it.
 */
static int dfly_build_team(std::vector<rank_t>& team, 
			   group_map_t& topo, job_id_t job_id, 
			   const placement_info& root, group_id_t g) {

	// clear the team
	team.clear();
	// the considered group must have some processes of the job in it
	if(topo.count(g) == 0) return 0;

	// get the routers in this group that have processes of this job
	const router_map_t &rmap = topo[g];

	// for all routers ...
	router_map_t::const_iterator r = rmap.begin();
	for(; r != rmap.end(); r++) {
		// if router's id is the root router, insert the root rank of the bcast
		if(r->first == root.router) {
			team.insert(team.begin(),root.rank);
		} else {
			team.push_back(r->second[0]);
		}
	}
	return 0;
}

/**
 * This function builds the set of couples. A couple is a pair of
 * ranks such that the first element is in the root group and the second element
 * is in the remote group.
 * output couples : array of couples built
 * input topo : topology structure
 * input job_id : id of the job
 * input root : placement of the root process.
 */
static int dfly_build_couples(std::vector<couple_t>& couples,
		group_map_t& topo, job_id_t job_id,
		const placement_info& root) {
	// build the team ranks used in the root group
	std::vector<rank_t> root_team_ranks_vec;
	dfly_build_team(root_team_ranks_vec, topo, job_id, root, root.group);
	// convert it into a set
	std::set<rank_t> root_team_ranks_set(root_team_ranks_vec.begin(), root_team_ranks_vec.end());

	// divide groups into different levels depending on whether they can be reached directly
	// from the root group or not
	std::set<group_id_t> distant_groups;
	// iterator over all non-root groups
	for(group_map_t::const_iterator g = topo.begin(); g != topo.end(); g++) {
		if(g->first == root.group) continue; // root group, do nothing

		// get a router r1 in g1 and r2 in root group that are connected		
		group_id_t g1 = g->first;
		router_id_t r1, r2;
		cortex_dfly_get_group_link_list(g1, root.group, &r1, &r2);

		// check if r2 is used by the job
		if(topo[root.group].count(r2) == 0) { // r2 is not used, the group g1 is distant
			// and will be handled later
			distant_groups.insert(g1);
			continue;
		}
		// here we know that r2 is used by the job, we take the first terminal of the job
		// that belongs to r2 for the pair,
		// unless the router contains the root rank in which case we choose it
		couple_t couple;
		couple.first = (r2 == root.router) ? root.rank : topo[root.group][r2][0];

		if(topo[g1].count(r1)) { // r1 in g1 is used by the job
			// take the first rank in g1 that belongs to the job
			couple.second = topo[g1][r1][0];
		} else { // r1 is not used by the job, we take the first rank in the first router used
			// in g1 instead
			couple.second = topo[g1].begin()->second[0]; // take the first router instead
		}
		// insert in couples
		couples.push_back(couple);
		// remove the rank used in root group from the root_team_ranks_set
		// so that it won't be used for distant groups unless it's really necessary
		root_team_ranks_set.erase(couple.first);
	}
	
	// for all distant groups, build a pair
	int i = 0; // index to do a round-robin in root_team_ranks_vec
		   // after we have emptied root_ream_ranks_set
	for(std::set<group_id_t>::iterator g = distant_groups.begin(); g != distant_groups.end(); g++) {
		group_id_t g1 = *g; // get the distant group's id
		router_id_t r1, r2; // find the routers that connect it to the root group
		cortex_dfly_get_group_link_list(g1, root.group, &r1, &r2);
		
		rank_t rkr, rk1;

		if(root_team_ranks_set.size() != 0) {
			rkr = *(root_team_ranks_set.begin());
			root_team_ranks_set.erase(rkr);
		} else {
			rkr = root_team_ranks_vec[i];
			i += 1;
			i %= root_team_ranks_vec.size();
		}
		
		if(topo[g1].count(r1)) { // r1 is in g1 
			rk1 = topo[g1][r1][0];
		} else {
			rk1 = topo[g1].begin()->second[0]; // take the first router instead
		}
		// insert in couples
		couples.push_back(couple_t(rkr,rk1));
	}

	return 0;
}

static int dfly_bcast_llf(job_id_t job_id, std::vector<rank_t> ranks_vec, int size, const comm_handler* comm, void* uarg) {
	rank_t root_rank = ranks_vec[0];
	// get the placement of the root process
	placement_info 	root;
	terminal_id_t 	root_terminal;
	router_id_t 	root_router;
	group_id_t 	root_group;
	cortex_dfly_job_get_terminal_from_rank(job_id,root_rank,&root_terminal);
	cortex_dfly_get_router_from_terminal(root_terminal,&root_router);
	cortex_dfly_get_group_from_router(root_router,&root_group);
	root.rank 	= root_rank;
	root.terminal 	= root_terminal;
	root.router 	= root_router;
	root.group 	= root_group;

	// build the topology
	group_map_t topo;
	dfly_build_topology(topo, job_id, ranks_vec);

	//print_topo(topo);
	// teams
	std::map<group_id_t, std::vector<rank_t> > teams;

	// get the groups involved and build the teams
	std::vector<group_id_t> groups(topo.size());
	group_map_t::iterator g = topo.begin();
	int i = 0;
	for(; g != topo.end(); g++, i++) {
		group_id_t g1 = g->first;
		groups[i] = g1;
		// build the team for group g1
		dfly_build_team(teams[g1], topo, job_id, root, g1);
	}

	// build couples
	std::vector<couple_t> couples;
	dfly_build_couples(couples,topo,job_id,root);

	// do a broadcast in the team of the root group
	std::vector<rank_t>& root_team = teams[root.group];
	// make sure to put the 
	dfly_bcast_tree(job_id, root_team, size, comm, uarg);

	// send in couples
	for(i=0; i<couples.size(); i++) {
		couple_t& c = couples[i];
		if(c.first == comm->my_rank) {
			comm->do_send(job_id, comm->my_rank, size, c.second, 0, uarg);
		} else if(c.second == comm->my_rank) {
			comm->do_recv(job_id, comm->my_rank, size, c.first, 0, uarg);
		}
	}

	// do a broadcast in the non-root teams
	// first we need to make sure that the first rank in a team is the end of a couple
	for(i=0; i<couples.size(); i++) {
		// locate the end of the couple
		couple_t& c = couples[i];
		terminal_id_t t;
		router_id_t r;
		group_id_t g;
		cortex_dfly_job_get_terminal_from_rank(job_id,c.second,&t);
		cortex_dfly_get_router_from_terminal(t,&r);
		cortex_dfly_get_group_from_router(r,&g);
		// get the team of its group
		std::vector<rank_t>& team = teams[g];
		if(team[0] != c.second) {
			std::vector<rank_t>::iterator it = std::find(team.begin(),team.end(),c.second);
			if(it == team.end()) {
				std::cerr << "A - Error here..." << std::endl;
			}
			*it = team[0];
			team[0] = c.second;
		}
		// do the actual broadcast
		dfly_bcast_tree(job_id,team,size,comm,uarg);
	}
	
	// do a broadcast in each router
	g = topo.begin();
	for(; g != topo.end(); g++) {
		// get an iterator over the routers in this group
		router_map_t::iterator r = g->second.begin();
		for(; r != g->second.end(); r++) {
			// get the router id
			router_id_t r_id = r->first;
			// get the list of ranks
			rank_vector_t& processes = r->second;
			// if this router contains the root rank
			if(r_id == root.router) {
				// find it
				rank_vector_t::iterator it = std::find(processes.begin(),processes.end(),root.rank);
				if(it == processes.end()) {
					std::cerr << "B - Error here..." << std::endl;
				}
				// exchange the first element
				*it = processes[0];
				processes[0] = root.rank;
			}
			// do a broadcast
			dfly_bcast_tree(job_id,processes,size,comm,uarg);
		}
	}

	return 0;
}

static int dfly_bcast_glf(job_id_t job_id, std::vector<rank_t> ranks_vec, int size, const comm_handler* comm, void* uarg) {
	rank_t root_rank = ranks_vec[0];
	// get the placement of the root process
	placement_info  root;
	terminal_id_t   root_terminal;
	router_id_t     root_router;
	group_id_t      root_group;
	cortex_dfly_job_get_terminal_from_rank(job_id,root_rank,&root_terminal);
	cortex_dfly_get_router_from_terminal(root_terminal,&root_router);
	cortex_dfly_get_group_from_router(root_router,&root_group);
	root.rank       = root_rank;
	root.terminal   = root_terminal;
	root.router     = root_router;
	root.group      = root_group;

	// build the topology
	group_map_t topo;
	dfly_build_topology(topo, job_id, ranks_vec);

	// do a broadcast across groups
	std::vector<rank_t> group_ranks;
	group_ranks.push_back(root.rank);
	for(group_map_t::iterator g = topo.begin(); g != topo.end(); g++) {
		if(g->first == root.group) continue;
		group_ranks.push_back(g->second.begin()->second[0]);
	}
	dfly_bcast_tree(job_id,group_ranks,size,comm,uarg);

	// within each group, do a broadcast across routers
	for(group_map_t::iterator g = topo.begin(); g != topo.end(); g++) {
		std::vector<rank_t> router_ranks;
		if(g->first == root.group) {
			router_ranks.push_back(root.rank);
		}
		for(router_map_t::iterator r = g->second.begin(); r != g->second.end(); r++) {
			if(r->first == root.router) continue;
			router_ranks.push_back(r->second[0]);
		}
		dfly_bcast_tree(job_id,router_ranks,size,comm,uarg);
	}

	// within each router, do a broadcast across terminals
	for(group_map_t::iterator g = topo.begin(); g != topo.end(); g++) {
	for(router_map_t::iterator r = g->second.begin(); r != g->second.end(); r++) {
		std::vector<rank_t> terminal_ranks;
		if(r->first == root.router) {
			terminal_ranks.push_back(root.rank);
		}
		for(std::vector<rank_t>::iterator t = r->second.begin(); t != r->second.end(); t++) {
			if(*t == root.rank) continue;
			terminal_ranks.push_back(*t);
		}
		dfly_bcast_tree(job_id,terminal_ranks,size,comm,uarg);
	}
	}

	return 0;
}

extern "C" void dfly_bcast(bcast_type type, int app_id, int root, int nprocs, int size, const comm_handler* comm, void* uarg) 
{
	std::vector<rank_t> ranks(nprocs);
	for(int i=0; i<nprocs; i++) {
		ranks[i] = (i-root) % nprocs;
	}

	switch(type) {
	case DFLY_BCAST_TREE:
		dfly_bcast_tree(app_id, ranks, size, comm, uarg);
		break;
	case DFLY_BCAST_LLF:
		dfly_bcast_llf(app_id, ranks, size, comm, uarg);
		break;
	case DFLY_BCAST_GLF:
		dfly_bcast_glf(app_id, ranks, size, comm, uarg);
		break;
	}
}
