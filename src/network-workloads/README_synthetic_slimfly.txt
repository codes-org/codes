************ Synthetic traffic with slim fly network model **********
- traffic patterns supported: uniform random, and worst-case traffic.
	- (1) Uniform random traffic: sends messages to a randomly selected destination
      node. This traffic pattern is uniformly distributed throughout the
      network and gives a better performance with minimal routing as compared
      to non-minimal or adaptive routing.
	- (2) Worst-case traffic: simulates an application that is communicating in a
	  manner that fully saturates links in the network and thus creates a bottleneck 
	  for minimal routing. In this workload, each compute node in a router, R1, will 
	  communicate to a node within a paired router that is the maximum two hops away.
	  Another pair of routers that share the same middle link with the previous pair 
	  of routers will be established to fully saturate that center link. This setup 
	  of network communication puts a worst-case burden on the link between routers 
	  2 and 3 as 4p nodes are creating 2p data flows. With all nodes paired in this 
	  configuration, congestion quickly builds up for all nodes in the system and 
	  limits maximum throughput to 1/2p.

HOW TO RUN:

ROSS optimistic mode:
mpirun -n 4 src/network-workloads/model-net-synthetic-slimfly --sync=3 --traffic=1 
--load=0.95 -- ../../jenkins/codes/src/network-workloads/conf/modelnet-synthetic-slimfly-min.conf

ROSS serial mode:

./src/network-workloads/model-net-synthetic-slimfly --sync=1 --traffic=1 
--load=0.95 -- ../../jenkins/codes/src/network-workloads/conf/modelnet-synthetic-slimfly-min.conf

options:

load: percentage of link bandwidth each compute node is to utilize. Each node will 
generate packets at a rate that will maintain the given load's link utilization.

traffic: 1 for uniform random traffic, 2 for worst-case traffic.

*slimfly-results-log.txt: Has information on each slim fly execution including, 
model size, LPs, PEs, latency, efficiency and run time. Results are appended after
each execution.
