/* turning on track lp will generate a lot of output messages */
#include <ross.h>
#include <inttypes.h>

#include "codes/codes-workload.h"
#include "codes/codes.h"

#ifdef ENABLE_CORTEX
#include <cortex/topology.h>
#endif

#include "codes/model-net.h"
/* TODO: Replace NUM_NODES and num_routers once we have functions to fetch
 * topology node and router counts. Right now it is hard coded for Theta. */
extern struct cortex_topology dragonfly_custom_cortex_topology;

int main(int argc, char** argv) {

    (void)argv;
    (void)argc;

#ifndef ENABLE_CORTEX
    printf("\n Cortex needs to be enabled in order to run the test. See <https://xgitlab.cels.anl.gov/codes/codes/wikis/codes-cortex-install> ");
    return -1;
#else
  if(argc < 2)
  {
    printf("\n Usage: %s dragonfly-config-file ", argv[0]);
    return -1;
  }
  int* net_ids;
  int num_nets;
  
  MPI_Init(&argc,&argv);

  printf("\n %s ", argv[1]);
  configuration_load(argv[1], MPI_COMM_WORLD, &config);
   
  model_net_register();
  net_ids = model_net_configure(&num_nets);

  /* The topo argument is NULL since we are using global variables */

  /* TODO: Replace NUM_NODES and num_routers once we have functions to fetch
   * topology node and router counts. Right now it is hard coded for Theta. */

  void * topo_arg = NULL;
  double local_bandwidth, global_bandwidth, cn_bandwidth;
  int num_router_cols, num_router_rows, num_cns;

  configuration_get_value_int(&config, "PARAMS", "num_router_rows", NULL, &num_router_rows);
  configuration_get_value_int(&config, "PARAMS", "num_cns_per_router", NULL, &num_cns);
  configuration_get_value_int(&config, "PARAMS", "num_router_cols", NULL, &num_router_cols);

  configuration_get_value_double(&config, "PARAMS", "local_bandwidth", NULL, &local_bandwidth);
  configuration_get_value_double(&config, "PARAMS", "global_bandwidth", NULL, &global_bandwidth);
  configuration_get_value_double(&config, "PARAMS", "cn_bandwidth", NULL, &cn_bandwidth);

  int num_routers =  dragonfly_custom_cortex_topology.get_number_of_routers(topo_arg);
  int total_cns = dragonfly_custom_cortex_topology.get_number_of_compute_nodes(topo_arg);

  printf("\n Aggregate number of routers %d number of cns %d ", num_routers, total_cns);
  /* First check for the same rows */
  for(int i = 0; i < num_router_cols - 1; i++)
  {
    double router_bw = dragonfly_custom_cortex_topology.get_router_link_bandwidth(topo_arg, i, i+1);
    assert(router_bw == local_bandwidth); 
  }

  /* Now check for black links, connections in the same column. */
  for(int i = 0; i < num_router_rows - 1; i++)
  {
    int src = i * num_router_cols;
    int dest = (i + 1) * num_router_cols;  
    double router_bw = dragonfly_custom_cortex_topology.get_router_link_bandwidth(topo_arg, src, dest);
    assert(router_bw == local_bandwidth);
  }

  /* Now check for a router in a different row and column, should not have a
   * connection in between them */
  for(int i = 0; i < num_router_rows - 1; i++)
  {
    int src = i * num_router_cols;
    int dest = (i + 1) * num_router_cols + 2;  
    double router_bw = dragonfly_custom_cortex_topology.get_router_link_bandwidth(topo_arg, src, dest);
    assert(router_bw == 0.0);
  }

  /* For global connections, check routers 0-3 connections to groups 1 and 5 */
  for(int i = 0; i < 4; i++)
  {
    int dest = i + (num_router_rows * num_router_cols); 
    double router_bw = dragonfly_custom_cortex_topology.get_router_link_bandwidth(topo_arg, i, dest);
    assert(router_bw == global_bandwidth);

    dest = i + (5 * num_router_rows * num_router_cols);
    router_bw = dragonfly_custom_cortex_topology.get_router_link_bandwidth(topo_arg, i, dest);
    assert(router_bw == global_bandwidth);
  }

  /* For the first four compute nodes , get their compute node bandwidth now */
  for(int i = 0; i < 4; i++)
  {
    double cn_bw = dragonfly_custom_cortex_topology.get_compute_node_bandwidth(topo_arg, i);
    assert(cn_bw == cn_bandwidth);

    cn_bw = dragonfly_custom_cortex_topology.get_compute_node_bandwidth(topo_arg, -1);
    assert(cn_bw == -1.0);

    cn_bw = dragonfly_custom_cortex_topology.get_compute_node_bandwidth(topo_arg, 3800);
    assert(cn_bw == -1.0);

  }

  int grp1_offset = num_router_cols * num_router_rows;
  /* Check connections for the first router row */
  for(int i = grp1_offset; i < grp1_offset + num_router_cols; i++)
  {
      int num_neighbors = dragonfly_custom_cortex_topology.get_router_neighbor_count(topo_arg, i);
      assert(num_neighbors == 22);

      router_id_t * routers = malloc(num_neighbors * sizeof(router_id_t));
      dragonfly_custom_cortex_topology.get_router_neighbor_list(topo_arg, i, routers);

      if(i == grp1_offset)
      {
        for(int j = 0; j < num_neighbors; j++)
          printf("\n Router id %d ", routers[j]);

      }
        int32_t * location = malloc(2 * sizeof(int32_t));
        dragonfly_custom_cortex_topology.get_router_location(topo_arg, i, location, sizeof(location));

        assert(location[0] == 1 && location[1] == (i - grp1_offset));

        free(location);

        for(int k = 0; k < num_cns; k++)
        {
            cn_id_t cn_id = i * num_cns + k;
            int32_t * loc_cn = malloc(3 * sizeof(int32_t));
            dragonfly_custom_cortex_topology.get_compute_node_location(topo_arg, cn_id, loc_cn, sizeof(loc_cn));
            //printf("\n location[0] %d location[1] %d location[2] %d ", location[0], location[1], location[2]);
            assert(location[0] == 1 && location[1] == (i - grp1_offset) && location[2] == k);
            free(loc_cn);

            router_id_t rid = dragonfly_custom_cortex_topology.get_router_from_compute_node(topo_arg, cn_id);
            assert(rid == i);
        }

        int cn_count = dragonfly_custom_cortex_topology.get_router_compute_node_count(topo_arg, i);
        cn_id_t * cn_ids = malloc(sizeof(cn_id_t) * cn_count);
        dragonfly_custom_cortex_topology.get_router_compute_node_list(topo_arg, i, cn_ids);

        for(int k = 0; k < cn_count; k++)
        {
           cn_id_t cn_id = i * num_cns + k;
           assert(cn_ids[k] == cn_id);
        }
  }
  MPI_Finalize();
#endif
}
