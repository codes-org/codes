#include "codes/cortex/dragonfly-cortex-api.h"
#include "codes/codes.h"
#include "codes/configuration.h"

/**** Caution: turning this on could generate a lot of output ********/
#define RANK_DBG 1
#define JOB_DBG 0

#define dprintf(_fmt, ...) \
    do {if (RANK_DBG) printf(_fmt, __VA_ARGS__);} while (0)

#define jprintf(_fmt, ...) \
    do {if (JOB_DBG) printf(_fmt, __VA_ARGS__);} while (0)
/* configuration file name */
static char conf_file_name[4096] = {'\0'};
static char alloc_file_name[4096] = {'\0'};

int main(int argc, char* argv[])
{
    MPI_Init(&argc, &argv);
    int rank, nprocs;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nprocs);

    assert(argv[1] && argv[2]);
    strcpy(conf_file_name, argv[1]);
    strcpy(alloc_file_name, argv[2]);

    dprintf("\n Loading config file %s and alloc file %s ", 
            conf_file_name,
            alloc_file_name);

    /* loading the CODES config file */
    configuration_load(conf_file_name, MPI_COMM_WORLD, &config);

    cortex_dfly_topology_init();

    /* Now populate the cortex data structure */
    cortex_dfly_topo topo;
    cortex_dfly_topology_get(&topo);

    /* Setup call for the job mapping */
    cortex_dfly_set_jobmap(alloc_file_name);

    int num_routers = topo.routers_per_group;
    int cn_per_router = topo.cn_per_router;
    int num_glinks_per_router = topo.glink_per_router;

    int num_jobs = cortex_dfly_get_num_jobs();

    int ret;
    int group_id;
    router_id_t src_rtr;
    router_id_t dest_rtr;
    terminal_id_t min_t, max_t;
            
    terminal_id_t terminal_id;
    router_id_t min_rtr, max_rtr;
    router_id_t router_id;
    job_id_t job_id;
    rank_t rank_id;
    
    router_id_t* local_router_ids = malloc(sizeof(router_id_t) * (num_routers - 1));
    router_id_t* global_router_ids = malloc(sizeof(router_id_t) * num_glinks_per_router);
    /* In the test config ile, there are two jobs, query their information */
    for(int jid = 0; jid < num_jobs; jid++)
    {
        int num_ranks_per_job = cortex_dfly_get_job_ranks(jid);

        for(uint32_t rank = 0; rank < num_ranks_per_job; rank++)
        {
            ret = cortex_dfly_location_get(jid, rank, &terminal_id);
            assert(ret == 0);
            
            /* Now get my router information */
            ret = cortex_dfly_get_router_from_terminal(terminal_id, &router_id);
            assert(ret == 0);

            /* Now get the local neighboring router list for my router */
            ret = cortex_dfly_get_local_router_list(router_id, local_router_ids);
            assert(ret == 0);

            /* get the global neighboring router list for my router */
            ret = cortex_dfly_get_global_router_list(router_id, global_router_ids);
            assert(ret == 0);

            /* Now get the group id for my router */
            ret = cortex_dfly_get_group_from_router(router_id, &group_id);
            assert(ret == 0);
            
            /* pick a randomly selected group and find out the router
             * connections */
            int rand_dst_grp = rand() % (topo.num_groups - 1);
            while(rand_dst_grp == group_id)
                rand_dst_grp = rand() % (topo.num_groups - 1);
            
            ret = cortex_dfly_get_group_link_list(group_id, rand_dst_grp, &src_rtr, &dest_rtr);
            assert(ret == 0);

            /* Now get range of terminals for this router */
            ret = cortex_dfly_get_terminals_from_router(router_id, &min_t, &max_t);
            assert(ret == 0);

            assert(min_t <= terminal_id <= max_t);
                
            ret = cortex_dfly_get_routers_from_group(group_id, &min_rtr, &max_rtr);
            assert(ret == 0 && min_rtr <= router_id <= max_rtr);

            ret = cortex_dfly_terminal_get_job_and_rank(terminal_id, &job_id, &rank_id);
            assert(ret == 0 && job_id == jid && rank_id == rank);

            dprintf("\n \n Rank %d Job ID %d Global Rank %d Router %d Group ID %d ", rank, jid, terminal_id, router_id, group_id);

            for(int i = 0; i < num_routers - 1; i++)
            {
                dprintf("\n Local neighbor %d ", local_router_ids[i]);
            }
            for(int i = 0; i < num_glinks_per_router; i++)
            {
                dprintf("\n Global neighbor %d ", global_router_ids[i]);
            }

            dprintf("\n Connection check: Group %d and Group %d routers %d and %d ", group_id,
                    rand_dst_grp,
                    src_rtr,
                    dest_rtr);
        }
        
        uint16_t g_count;
        uint16_t term_count;
        uint16_t router_count;
        
        ret = cortex_dfly_job_get_group_count(jid, &g_count);
        assert(ret == 0);

        jprintf("\n **** Group count for job %d is %d **** ", jid, g_count);
        group_id_t* group_ids = malloc(sizeof(group_id_t) * g_count);
        ret = cortex_dfly_job_get_group_list(jid, group_ids);
        assert(ret == 0);

        for(int g = 0; g < g_count; g++)
        {
            ret = cortex_dfly_job_get_router_count(jid, group_ids[g], &router_count);
            assert(ret == 0);
            router_id_t* router_ids = malloc(sizeof(router_id_t) * router_count);

            jprintf("\n \n Job id: %d Group id: %d Router count %d ", jid, group_ids[g], router_count);
            ret = cortex_dfly_job_get_router_list(jid, group_ids[g], router_ids);
            assert(ret == 0);

            for(int r = 0; r < router_count; r++)
            {
                ret = cortex_dfly_job_get_terminal_count(jid, router_ids[r], &term_count);
                assert(ret == 0);

                jprintf("\n Router ID %d Terminal count %d ", router_ids[r], term_count);
                
                terminal_id_t* terminal_ids = malloc(sizeof(terminal_id_t) * term_count);
                ret = cortex_dfly_job_get_terminal_list(jid, router_ids[r], terminal_ids);
                assert(ret == 0);

                for(int t = 0; t < term_count; t++)
                {
                   jprintf("\n Terminal id %d ", terminal_ids[t]); 
                }
                free(terminal_ids);
            }
            free(router_ids);
        }
        free(group_ids);
    }
    if(ret == 0)
        printf("\n Success! Turn on debug options in the dragonfly-cortex-test source to generate detailed output. See README for info.\n");
    cortex_dfly_topology_finalize();
    MPI_Finalize();
}
