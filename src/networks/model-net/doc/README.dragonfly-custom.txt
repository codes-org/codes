------- Network Layout ---------
- The network layout for the custom dragonfly model is similar to Cray XC
  systems. Each group has X number of routers arranged in rows and columns.
  There are all-to-all connections among routers within the same row and same
  column. If a packet is sent to a router that is within the same group but
  lies on a different row and column, it will be first sent to an intermediate
  router that has a direct connection to the destination router.

- There can be multiple global channels between two groups as specified by the
  network configuration files. Currently, Edison has 12 global channels
  connecting any two groups in the network.

- For more details on the Cray dragonfly topology, see "Cray cascade: a
  scalable HPC system based on a Dragonfly network" by Greg Faanes et al.
  in Supercomputing 2012.

------- Network Setup ---------
- Edison Configuration: The network configuration file from Cray Edison system
  at NERSC can be used to setup the dragonfly network topology. The
  configuration has 5,760 nodes, 1440 routers and 15 groups. The instructions
  on how to geneate the network configuration files for Edison can be found at:
  codes/scripts/gen-cray-topo/README.txt

- Custom Configuration: The model can be configured with an arbitrary number of
  groups and number of routers within a group. For instructions on how to
  generate a custom network configuration, see
  codes/scripts/gen-cray-topo/README.txt

------- Routing Protocols ------
- Minimal: Within a group, a minimal route will traverse three intermediate
  hops at the maximum (a source router, an intermediate router if source and
  destination routers do not share the same row or column and the destination
  router). Across groups, a minimal route will choose the shortest possible
  connection to the destination group. A global minimal route will traverse a
  maximum of 6 router hops (including source and destination).

- Non-Minimal: Within a group, a non-minimal route will direct the packets to a
  randomly selected router first. A global non-minimal route will involve
  routing to a randomly selected intermediate router from the network first. A
  global non-minimal route can traverse up to 11 router hops (including source
  and destination).

- Local adaptive: Local adaptive routing takes a non-minimal route within a
  group if it detects congestion on the minimal route (queues on minimal port
  are used to detect congestion). 

- Global Adaptive: Global adaptive routing takes a global non-minimal route if
  it detects congestion on the minimal route (queues on minimal port are used
  to detect congestion).

- Progressive Adaptive: Progressive adaptive routing re-evaluates the decision
  to take a minimal or a non-minimal route as long as the packet stays in the 
  source group. If a non-minimal route is decided at some point in the source
  group, the decision is no more re-evaluated.

-------- Running simulations --------
- Synthetic Traffic Patterns: 

  [With custom dragonfly network having 6,400 network nodes, 1600 routers and
  20 groups. Each group has 80 routers arranged in a 20x4 matrix]

  ./bin/model-net-synthetic-custom-dfly --sync=1 --
  ../src/network-workloads/conf/dragonfly-custom/modelnet-test-dragonfly-custom.conf 

  [With theta dragonfly network having 3,456 compute nodes, 864 routers and 9
  groups. Each group has 96 routers arranged in a 6x16 matrix]
  
  mpirun -np 4 ./bin/model-net-synthetic-custom-dfly --sync=3 --
  ../src/network-workloads/conf/dragonfly-custom/modelnet-test-dragonfly-theta.conf

  [With edison dragonfly network having 5,702 network nodes, 1440 routers and 15
  groups. Each group has 96 routers arranged in 6x16 matrix.]

  mpirun -np 4 ./bin/model-net-synthetic-custom-dfly --sync=3 --
  ../src/network-workloads/conf/dragonfly-custom/modelnet-test-dragonfly-edison.conf

- Design Forward Network traces:

   [With Edison style dragonfly having 5,760 network nodes and small-scale Multigrid network trace having 125 ranks]

  ./bin/model-net-mpi-replay --sync=1 --disable_compute=1
  --workload_type="dumpi"
  --workload_file=../../../df_traces/Multigrid/MultiGrid_C_n125_dumpi/dumpi-2014.03.06.23.48.13-
  --num_net_traces=125 --
  ../src/network-workloads/conf/dragonfly-custom/modelnet-test-dragonfly-edison.conf

  [With Edison style dragonfly and AMG 1,728 application trace]

  mpirun -np 4 ./bin/model-net-mpi-replay --sync=3 --disable_compute=1
  --workload_type="dumpi"
  --workload_file=../../../df_traces/AMG/df_AMG_n1728_dumpi/dumpi-2014.03.03.14.55.50-
  --num_net_traces=1728 --
  ../src/network-workloads/dragonfly-custom/conf/modelnet-test-dragonfly-edison.conf

  [With theta style dragonfly and AMG 1,728 application trace]

  mpirun -np 4 ./bin/model-net-mpi-replay --sync=3 --disable_compute=1
  --workload_type="dumpi"
  --workload_file=../../../df_traces/AMG/df_AMG_n1728_dumpi/dumpi-2014.03.03.14.55.50-
  --num_net_traces=1728 --
  ../src/network-workloads/conf/dragonfly-custom/modelnet-test-dragonfly-
--------- Debugging Tips ------------
- Set DUMP_CONNECTIONS debugging option to see the detailed local and global
  channel connectivity of routers in src/networks/model-net/dragonfly-custom.C
