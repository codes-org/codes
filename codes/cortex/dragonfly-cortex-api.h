#ifndef CORTEX_DRAGONFLY_TOPO_API_H
#define CORTEX_DRAGONFLY_TOPO_API_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* *********************************************************************************************
   Data Structures
 ********************************************************************************************* */

typedef int16_t job_id_t;
typedef int32_t  rank_t;
typedef int32_t terminal_id_t;
typedef int32_t router_id_t;
typedef int32_t group_id_t;

enum {
	CORTEX_DFLY_OK = 0,
	CORTEX_DFLY_TOPO_NOT_SET,
	CORTEX_DFLY_TOPO_INVALID,
	CORTEX_DFLY_ID_INVALID
};

/**
 * Topology information for a Dragonfly network.
 * cortex_dfly_topo gives information about the entire network (not on a per job basis).
 */
typedef struct {
    uint16_t cn_per_router; 	/* number of compute nodes (terminals) connected to a router */
    uint16_t routers_per_group; /* number of routers in a group */
    uint16_t num_groups;	/* number of groups */
    uint16_t glink_per_router;	/* number of global links per router */
    double local_bandwidth;	/* bandwidth of the router-router channels within a group */
    double global_bandwidth;	/* bandwidth of the inter-group router connections */
    double cn_bandwidth;	/* bandwidth of the compute node channels connected to routers */
} cortex_dfly_topo;

/**********************************************************************************************
 Retrieving the information on the topology
 ********************************************************************************************* */

/**
 * Get the router to which the terminal is attached.
 * input param: terminal_id
 * output param: router_id
 */
int cortex_dfly_get_router_from_terminal(const terminal_id_t terminal_id, router_id_t *router_id);

/**
 * Get the group to which the router belongs.
 * input param: router_id
 * output param: group_id
 */
int cortex_dfly_get_group_from_router(const router_id_t router_id, group_id_t* group_id);

/**
 * Get the lower and upper bounds of terminal ids within a router.
 * input param: router_id
 * output param: min
 * output param: max
 */
int cortex_dfly_get_terminals_from_router(const router_id_t router_id, terminal_id_t* min, terminal_id_t* max);

/**
 * Get the lower and upper bounds of router ids within a group.
 * input param: group_id
 * output param: min
 * output param: max
 */
int cortex_dfly_get_routers_from_group(const group_id_t group_id, router_id_t* min, router_id_t* max);

/**********************************************************************************************
 Retrieving the information on the job
 ********************************************************************************************* */

/**
 * MM: Redundant to cortex_dfly_location_get
 * Get the terminal to which the rank of a job is attached.
 * input param: job_id
 * input param: rank
 * output param: terminal_id
 */
int cortex_dfly_job_get_terminal_from_rank(const job_id_t job_id, const rank_t rank, terminal_id_t* terminal_id);

/**
 * Get the job id and rank for the given terminal, if such information exists.
 * input param: terminal_id
 * output param: job_id
 * output param: rank
 */
int cortex_dfly_terminal_get_job_and_rank(const terminal_id_t terminal_id, job_id_t* job_id, rank_t* rank);

/**
 * Get the number of groups that have ranks executing job with job_id.
 * input param: job_id
 * output param: num_groups
 */
int cortex_dfly_job_get_group_count(const job_id_t job_id, uint16_t* num_groups);

/**
 * Get the list of groups that have ranks executing job with job_id.
 * The array passed (group_ids) must be pre-allocated with enough space
 * (see cortex_dfly_job_get_group_count).
 * input param: job_id
 * output param: group_ids
 */
int cortex_dfly_job_get_group_list(const job_id_t job_id, group_id_t* group_ids);

/**
 * Get the number of routers of a given group to which ranks of a particular job (job_id) are connected.
 * input param: job_id
 * input param: group_id
 * output param: router_count
 **/
int cortex_dfly_job_get_router_count(const job_id_t job_id, const group_id_t group_id, uint16_t* router_count);

/**
 * Get the list of routers executing a particular job (job_id) in a group (group_id).
 * The array passed (router_ids) must be pre-allocated with enough space
 * (see cortex_dfly_job_get_router_count).
 * input param: job_id
 * input param: group_id
 * output_param: group_routers
 */
int cortex_dfly_job_get_router_list(const job_id_t job_id, const group_id_t group_id, router_id_t* router_ids);

/**
 * Get the number of terminals within a given router that have a process of the given job.
 * input param: job_id
 * input param: router_id
 * output param: terminal_count
 */
int cortex_dfly_job_get_terminal_count(const job_id_t job_id, const router_id_t router_id, uint16_t* terminal_count);

/**
 * Get the list of terminals within a given router that have a process of the given job.
 * The array passed (terminal_ids) must be pre-allocated with enough space
 * (see cortex_dfly_job_get_terminal_count).
 * input param: job_id
 * input param: router_id
 * output param: terminal_count
 */
int cortex_dfly_job_get_terminal_list(const job_id_t job_id, const router_id_t router_id, terminal_id_t* terminal_ids);

/**********************************************************************************************
 Getter functions for dragonfly connectivity (irrespective of the job)
 ********************************************************************************************* */
/* Note that the number of local and global channels can be extracted from the topology struct
 * at any time so we don't need getter functions to get number of local and global channels. */

/**
 * Get the list of routers connected to a particular router within a group (irrespective of the job).
 * The array passed (local_router_ids) must be pre-allocated with a size corresponding to routers_per_group-1
 * (from the topology structure).
 * input param: router_id
 * output param: local_router_ids
 */
int cortex_dfly_get_local_router_list(const router_id_t router_id, router_id_t* local_router_ids);

/**
 * Get the list of routers connected to a particular router across the group (irrespective of the job).
 * The array passed (global_router_ids) must be pre-allocated with a size corresponding to glink_per_router
 * (from the topology structure).
 * input param: router_id
 * output param: global_router_ids
 */
int cortex_dfly_get_global_router_list(const router_id_t router_id, router_id_t* global_router_ids);

/**
 * Get the number of global links between groups g1 and g2.
 * input param: g1
 * input param: g2
 * output param: count
 */
int cortex_dfly_get_group_link_count(const group_id_t g1, const group_id_t g2, uint32_t* count);

/**
 * Get the list of routers r1 in g1 and r2 in g2 such that r1[i] is connected to r2[i].
 * r1 and r2 mush be preallocated with a size corresponding to the count returned by
 * cortex_dfly_get_group_link_count.
 * input param: g1
 * input param: g2
 * output param: r1
 * output param: r2
 */
int cortex_dfly_get_group_link_list(const group_id_t g1, const group_id_t g2, router_id_t* r1, router_id_t* r2);

/**********************************************************************************************
   Setting the topology and process placement
 ********************************************************************************************* */

/**
 * Setup function: sets the topology information for CORTEX.
 * input param: topology
 */
int cortex_dfly_topology_init();

/**
 * finalize cortex topology. This function is used to free up the malloced
 * space */
int cortex_dfly_topology_finalize();
/**
 * Query topology: Get the information about the topology.
 * output param: topology
 */
int cortex_dfly_topology_get(cortex_dfly_topo *topology);

/**
 * MM: I don't think we would need this. The rank placement is already
 * done by the CODES job mapping API. 
 * Attaches a process of a given job to a given terminal.
 * input param: job_id
 * input param: rank
 * output param: terminal_id
 */
int cortex_dfly_location_set(const job_id_t job_id, const rank_t rank, const terminal_id_t terminal_id);

/**
 * Get the location of a process given its rank.
 * input param: job_id
 * input param: rank
 * output param: terminal_id
 */
int cortex_dfly_location_get(const job_id_t job_id, const rank_t rank, terminal_id_t* terminal_id);

/**
 * MM: Not needed, see previous note. 
 * Attaches all processes from 0 to count-1 to an array of terminal ids.
 * input param: job_id
 * input param: count
 * input param: terminal_ids
 */
int cortex_dfly_location_set_all(const job_id_t job_id, uint32_t count, const terminal_id_t* terminal_ids);

/**
 * Get location information for all processes.
 * The passed array (terminal_ids) must be allocated with a size of count elements.
 * input param: job_id
 * input param: count
 * output param: terminal_ids
 */
int cortex_dfly_location_get_all(const job_id_t job_id, uint32_t count, terminal_id_t* terminal_ids);

/******************* Additional functions for setting jobmap ********/
/**
 * Sets the jobmap information from the allocation file
 * This function uses codes jobmap API under the covers
 */
int cortex_dfly_set_jobmap(char * allocation_file);

/**
 * Get the number of jobs as specified from the allocation file
 * This function uses codes jobmap API under the covers
 */
int cortex_dfly_get_num_jobs();

/** 
 * Given the job ID, this gets the number of ranks assigned to a particular job
 * This function uses codes jobmap API under the covers
 */
int cortex_dfly_get_job_ranks(int job_id);

#ifdef __cplusplus
}
#endif

#endif
