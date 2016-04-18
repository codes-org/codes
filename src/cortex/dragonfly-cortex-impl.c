#include "codes/cortex/dragonfly-cortex-api.h"
#include "codes/codes.h"
#include "codes/configuration.h"
#include "codes/codes-jobmap.h"

static int num_routers;
static int num_cns_per_router;
static int num_glinks_per_router;
static int num_groups;
static int total_terminals;
static double local_bw;
static double global_bw;
static double cn_bw;
static int total_routers;

struct codes_jobmap_ctx *jobmap_ctx;
struct codes_jobmap_params_list jobmap_p;

cortex_dfly_topo * topo;

typedef struct cortex_router_s* cortex_router;

struct cortex_router_s {
	uint16_t router_id;		/* id of the router in its group */
	uint16_t group_id;		/* id of the group to which this router belongs */
	int32_t* local_neighbors;  /* other routers connected directly to this router within a group (this is irrespective of the job_id) */
    int32_t* global_neighbors; /* other routers connected directly to this router via global channels (this is irrespective of the job_id)*/
};

/* router structs */
static cortex_router routers;

/******** Helper functions for setting local and global channel connections
 *******/

void setup_local_channels()
{
    for(int r = 0; r < total_routers; r++ )
    {
       int my_group_id = routers[r].group_id;

        /* Set up local channel connectivity */
       int n = 0;
       int inc_rts = 0;
       while(n < num_routers)
       {
           int nbr_router_id = my_group_id * num_routers + n;
           assert(nbr_router_id >= 0 && nbr_router_id < total_routers);

           if(nbr_router_id != routers[r].router_id)
           {
                routers[r].local_neighbors[inc_rts] = nbr_router_id;
                inc_rts++;
           }
            /* Skip my router ID*/
            n++;
       }
       assert(inc_rts < num_routers);
    }
}

void setup_global_channels()
{
    for(int r = 0; r < total_routers; r++)
    {
       /* Now setup global channel connectivity */
       int first = routers[r].router_id % num_routers;
       for(int g = 0; g < num_cns_per_router; g++)
       {
            int target_grp = first;
            if(target_grp == routers[r].group_id)
            {
                target_grp = num_groups - 1;
            }
            int my_pos = routers[r].group_id % num_routers;
            if(routers[r].group_id == num_groups - 1)
            {
                my_pos = target_grp % num_routers;
            }
            int g_indx = target_grp * num_routers + my_pos;
            assert(g_indx >=0 && g_indx < total_routers);

            routers[r].global_neighbors[g] = g_indx;
            first += num_routers;
       }
    }
}
/***************** Cortex Topology functions *************/
// This function takes the dragonfly config file and populates the cortex
// topology information. It also pre-populates the router connections.  
int cortex_dfly_topology_init()
{
    topo = malloc(sizeof(cortex_dfly_topo));

    int rc = configuration_get_value_int(&config, "PARAMS", "num_routers", NULL,
                        &num_routers);
    if(rc)
    {
        fprintf(stderr, "\n Number of routers not specified in the config file ");
        return CORTEX_DFLY_TOPO_NOT_SET;
    }
    
    rc = configuration_get_value_double(&config, "PARAMS", "global_bandwidth", NULL, &global_bw);
    if(rc)
    {
        fprintf(stderr, "\n Global channel bandwidth not specified in the config file ");
        return CORTEX_DFLY_TOPO_NOT_SET;
    }

    rc = configuration_get_value_double(&config, "PARAMS", "local_bandwidth", NULL, &local_bw); 
    if(rc)
    {
        fprintf(stderr, "\n Local channel bandwidth not specified in the config file ");
        return CORTEX_DFLY_TOPO_NOT_SET;
    }
    
    rc = configuration_get_value_double(&config, "PARAMS", "cn_bandwidth", NULL, &cn_bw); 
    if(rc)
    {
        fprintf(stderr, "\n Compute node channel bandwidth not specified in the config file ");
        return CORTEX_DFLY_TOPO_NOT_SET;
    }

    num_cns_per_router = num_routers / 2;
    num_glinks_per_router = num_routers / 2;
    num_groups = num_routers * (num_routers/2) + 1;
    total_routers = (num_groups * num_routers);
    total_terminals = total_routers * num_cns_per_router;

    topo->cn_per_router = num_cns_per_router;
    topo->glink_per_router = num_cns_per_router;
    topo->routers_per_group = num_routers;
    topo->local_bandwidth = local_bw;
    topo->global_bandwidth = global_bw;
    topo->cn_bandwidth = cn_bw;
    topo->num_groups = num_groups;

    /* Here we want to populate the router data structures */
    routers = malloc(total_routers * sizeof(struct cortex_router_s));
    
    /* First setup the router and group IDs */
    for(int r = 0; r < total_routers; r++)
    {
       routers[r].router_id = r;
       routers[r].group_id = r / num_routers;
       routers[r].local_neighbors = malloc(sizeof(cortex_router) * (num_routers - 1));
       routers[r].global_neighbors = malloc(sizeof(cortex_router) * num_cns_per_router);
    }

    /* Local channel connectivity */
    setup_local_channels();

    /* global channel connectivity */
    setup_global_channels();

    return CORTEX_DFLY_OK;
}

int cortex_dfly_topology_finalize()
{
    free(topo);
    
    for(int i = 0; i < total_routers; i++)
    {
        free(routers[i].local_neighbors);
        free(routers[i].global_neighbors);
    }
    free(routers);
    return CORTEX_DFLY_OK;
}

int cortex_dfly_topology_get(cortex_dfly_topo *topology)
{
    if(topo == NULL)
        return CORTEX_DFLY_TOPO_NOT_SET;

    memcpy(topology, topo, sizeof(cortex_dfly_topo));

    return CORTEX_DFLY_OK;
}

// This function takes the job allocation file and populates the
// job map context
int cortex_dfly_set_jobmap(char * allocation_file)
{
    assert(allocation_file);
    jobmap_p.alloc_file = allocation_file;
    jobmap_ctx = codes_jobmap_configure(CODES_JOBMAP_LIST, &jobmap_p);
    
    return CORTEX_DFLY_OK;
}

int cortex_dfly_location_get(const job_id_t job_id, const rank_t rank, terminal_id_t* terminal_id)
{
    struct codes_jobmap_id jid;
    jid.job = job_id;
    jid.rank = rank;
    terminal_id_t global_rank = codes_jobmap_to_global_id(jid, jobmap_ctx);

    /* checks in case an incorrect rank is passed in */
    if(global_rank < 0 || global_rank >= total_terminals)    
        return CORTEX_DFLY_ID_INVALID;

    *terminal_id = global_rank;
    return CORTEX_DFLY_OK;
}

int cortex_dfly_location_get_all(const job_id_t job_id, uint32_t count, terminal_id_t* terminal_ids)
{
    struct codes_jobmap_id jid;
    terminal_id_t global_rank;

    for(int i = 0; i < count; i++)
    {
        jid.job = job_id;
        jid.rank = i;
        global_rank = codes_jobmap_to_global_id(jid, jobmap_ctx);

        if(global_rank < 0 || global_rank >= total_terminals)
            return CORTEX_DFLY_ID_INVALID;

        terminal_ids[i] = global_rank;
    }
    return CORTEX_DFLY_OK;
}

/********* retreiving the information on the job **********/
int cortex_dfly_job_get_terminal_from_rank(const job_id_t job_id, const rank_t rank, terminal_id_t* terminal_id)
{
    struct codes_jobmap_id jid;
    jid.job = job_id;
    jid.rank = rank;
    
    int global_rank = codes_jobmap_to_global_id(jid, jobmap_ctx);
    if(global_rank < 0 || global_rank > total_terminals)
        return CORTEX_DFLY_ID_INVALID;

    *terminal_id = global_rank;
    return CORTEX_DFLY_OK;
}

int cortex_dfly_terminal_get_job_and_rank(const terminal_id_t terminal_id, job_id_t* job_id, rank_t* rank)
{
    if(terminal_id < 0 || terminal_id > total_terminals) 
        return CORTEX_DFLY_ID_INVALID;

    struct codes_jobmap_id jid;
    jid = codes_jobmap_to_local_id(terminal_id, jobmap_ctx);

    *job_id = jid.job;
    *rank = jid.rank;

    return CORTEX_DFLY_OK;
}

int cortex_dfly_job_get_group_count(const job_id_t job_id, uint16_t* num_groups)
{
    int ngroups = 0;
    job_id_t jid;
    rank_t rank_id;
    int prev_group_id = -1;
    int cur_group_id = 0;

    for(int i = 0; i < total_terminals; i++)
    {
        cur_group_id = i / (num_cns_per_router * num_routers);
        cortex_dfly_terminal_get_job_and_rank(i, &jid, &rank_id);

        if(job_id == jid && cur_group_id != prev_group_id)
        {
            ngroups++;
            prev_group_id = cur_group_id;
        }
    }
    *num_groups = ngroups;

    return CORTEX_DFLY_OK;
}

int cortex_dfly_job_get_group_list(const job_id_t job_id, group_id_t* group_ids)
{
    job_id_t jid;
    rank_t rank_id;
    
    int prev_group_id = -1;
    int cur_group_id = 0;
    int k = 0;

    for(int i = 0; i < total_terminals; i++)
    {
        cur_group_id = i / (num_cns_per_router * num_routers);
        cortex_dfly_terminal_get_job_and_rank(i, &jid, &rank_id);

        if(job_id == jid && cur_group_id != prev_group_id)
        {
            group_ids[k] = cur_group_id;
            prev_group_id = cur_group_id;
            k++;
        }

    }
    return CORTEX_DFLY_OK;
}

int cortex_dfly_job_get_router_count(const job_id_t job_id, const group_id_t group_id, uint16_t* router_count)
{
    job_id_t jid;
    rank_t rank_id;
   
    if(group_id < 0 || group_id >= num_groups)
        return CORTEX_DFLY_ID_INVALID;

    int prev_router_id = -1;
    int cur_router_id = 0;
    int rcount = 0;
    int start = group_id * num_routers * num_cns_per_router;
    int end = start + num_routers * num_cns_per_router - 1;

    for(int i = start; i <= end; i++)
    {
        cur_router_id = i / num_cns_per_router;
        cortex_dfly_terminal_get_job_and_rank(i, &jid, &rank_id);

        if(job_id == jid && cur_router_id != prev_router_id)
        {
            prev_router_id = cur_router_id;
            rcount++;
        }

    }
    *router_count = rcount;
    
    return CORTEX_DFLY_OK;
}

int cortex_dfly_job_get_router_list(const job_id_t job_id, const group_id_t group_id, router_id_t* router_ids)
{
    job_id_t jid;
    rank_t rank_id;

    if(group_id < 0 || group_id >= num_groups)
        return CORTEX_DFLY_ID_INVALID; 

    int prev_router_id = -1;
    int cur_router_id = 0;
    int k = 0;
    int start = group_id * num_routers * num_cns_per_router;
    int end = start + num_routers * num_cns_per_router - 1;

    for(int i = start; i <= end; i++)
    {
        cur_router_id = i / num_cns_per_router;
        cortex_dfly_terminal_get_job_and_rank(i, &jid, &rank_id);

        if(job_id == jid && cur_router_id != prev_router_id)
        {
            prev_router_id = cur_router_id;
            router_ids[k] = cur_router_id;
            k++;
        }

    }
    return CORTEX_DFLY_OK;
}

int cortex_dfly_job_get_terminal_count(const job_id_t job_id, const router_id_t router_id, uint16_t* terminal_count)
{
    job_id_t jid;
    rank_t rank_id;

    if(router_id < 0 || router_id > total_routers)
        return CORTEX_DFLY_ID_INVALID;

    int num_terms = 0;
    int start = router_id * num_cns_per_router;
    int end = start + (num_cns_per_router - 1);

    for(int i = start; i <= end; i++ )
    {
        cortex_dfly_terminal_get_job_and_rank(i, &jid, &rank_id);
        if(jid == job_id)
            num_terms++;
    }
    *terminal_count = num_terms;

    return CORTEX_DFLY_OK;
}

int cortex_dfly_job_get_terminal_list(const job_id_t job_id, const router_id_t router_id, terminal_id_t* terminal_ids)
{
    job_id_t jid;
    rank_t rank_id;

    if(router_id < 0 || router_id > total_routers)
        return CORTEX_DFLY_ID_INVALID;

    int k = 0;
    int start = router_id * num_cns_per_router;
    int end = start + (num_cns_per_router - 1);

    for(int i = start; i <= end; i++ )
    {
        cortex_dfly_terminal_get_job_and_rank(i, &jid, &rank_id);
        if(jid == job_id)
        {
            terminal_ids[k] = i;
            k++;
        }
    }
    return CORTEX_DFLY_OK;
}

/**************** Retrieving the information on the topology **********/
int cortex_dfly_get_router_from_terminal(const terminal_id_t terminal_id, router_id_t *router_id)
{
    *router_id = terminal_id/num_cns_per_router;

    if(*router_id < 0 || *router_id >= total_routers)
        return CORTEX_DFLY_ID_INVALID;

    return CORTEX_DFLY_OK;
}

int cortex_dfly_get_group_from_router(const router_id_t router_id, group_id_t* group_id)
{
    if(router_id < 0 || router_id > total_routers)
        return CORTEX_DFLY_ID_INVALID;

    *group_id = routers[router_id].group_id;

    return CORTEX_DFLY_OK;
}

int cortex_dfly_get_terminals_from_router(const router_id_t router_id, terminal_id_t* min, terminal_id_t* max)
{
    if(router_id < 0 || router_id > total_routers)
        return CORTEX_DFLY_ID_INVALID;

    *min = router_id * num_cns_per_router;
    *max = *min + (num_cns_per_router - 1);

    return CORTEX_DFLY_OK;
}

int cortex_dfly_get_routers_from_group(const group_id_t group_id, router_id_t* min, router_id_t* max)
{
   if(group_id < 0 || group_id > num_groups)
       return CORTEX_DFLY_ID_INVALID;

   *min = group_id * num_routers;
   *max = *min + (num_routers - 1);
   
   return CORTEX_DFLY_OK;
}

/****************
Getter functions for dragonfly connectivity (irrespective of the job)
*********/
int cortex_dfly_get_local_router_list(const router_id_t router_id, router_id_t* local_router_ids)
{
    if(router_id < 0 || router_id > total_routers)
        return CORTEX_DFLY_ID_INVALID;

    struct cortex_router_s router = routers[router_id];
    memcpy(local_router_ids, router.local_neighbors, sizeof(router_id_t) * (num_routers - 1));
    
    return CORTEX_DFLY_OK;
}

int cortex_dfly_get_global_router_list(const router_id_t router_id, router_id_t* global_router_ids)
{
    if(router_id < 0 || router_id > total_routers)
        return CORTEX_DFLY_ID_INVALID;

    struct cortex_router_s router = routers[router_id];
    memcpy(global_router_ids, router.global_neighbors, sizeof(int32_t) * num_glinks_per_router);
    
    return CORTEX_DFLY_OK;
}

int cortex_dfly_get_group_link_count(const group_id_t g1, const group_id_t g2, uint32_t* count)
{

    /* For now there is a single global channel between the groups */
    *count = 1;

    return CORTEX_DFLY_OK;
}

int cortex_dfly_get_group_link_list(const group_id_t g1, const group_id_t g2, router_id_t* r1, router_id_t* r2)
{

    /* Search which router has connection to g2 */
    int dest_grp_id;
    router_id_t start = g1 * num_routers;
    router_id_t end = start + num_routers - 1;

    for(int i = start; i <= end; i++)
    {
        for(int g = 0; g < num_glinks_per_router; g++)
        {
           dest_grp_id = routers[i].global_neighbors[g] / num_routers;
           
           if(dest_grp_id == g2)
           {
                *r1 = i;
                *r2 = routers[i].global_neighbors[g];
                return CORTEX_DFLY_OK;
           }
        }
    }
    return CORTEX_DFLY_ID_INVALID;
}

/**************** Cortex-CODES job information functions ****************/
int cortex_dfly_get_num_jobs()
{
   return codes_jobmap_get_num_jobs(jobmap_ctx); 
}

int cortex_dfly_get_job_ranks(int job_id)
{
    return codes_jobmap_get_num_ranks(job_id, jobmap_ctx);
}
