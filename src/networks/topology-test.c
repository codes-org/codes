/* turning on track lp will generate a lot of output messages */
#include <ross.h>
#include <inttypes.h>

#include "codes/codes-workload.h"
#include "codes/codes.h"
#include <cortex/topology.h>
#include "codes/model-net.h"

int main(int argc, char** argv) {
  int* net_ids;
  int num_nets;
  
  MPI_Init(&argc,&argv);

  printf("\n %s ", argv[1]);
  configuration_load(argv[1], MPI_COMM_WORLD, &config);
   
  model_net_register();
  net_ids = model_net_configure(&num_nets);

  /* The topo argument is NULL since we are using global variables */
  void * topo = NULL;

  get_router_link_bandwidth(topo, 0, 1);
  MPI_Finalize();
}
