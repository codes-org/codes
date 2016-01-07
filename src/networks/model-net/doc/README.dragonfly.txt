*** README file for dragonfly network model ***
This file describes the setup and configuration for the ROSS dragonfly network model.

1- Model of the dragonfly network topology

The dragonfly is a hierarchical topology having several groups connected by
all-to-all links. Each router has “p” number of compute nodes connected to it,
and there are “a” routers in each group. Routers within a group are connected
via local channels. Each router has “h” global channels, which are intergroup
connections through which routers in a group connect to routers in other
groups. Thus, the radix of each router is k = a + p + h − 1. For load balancing
the network traffic, the recommended dragonfly configuration is a = 2p = 2h.
Overall, the total number of groups “g” in the network is g = a ∗ h+1. Since
each group has “p” nodes and “a” routers, the total number of nodes ’N’ in the
network is determined by N = p ∗ a ∗ g.

Our ROSS dragonfly model uses the configuration a=2p=2h for modeling the
dragonfly topology. Full-sized network packets (default size: 512 bytes) are
broken into smaller flits (default size: 32 bytes) for transportation
over the network.  ROSS dragonfly model supports four different forms of
routing: minimal: packet is sent directly from the source group to destination
group over the single global channel connecting the source and destination
groups.  non-minimal: packet is first sent to a randomly selected intermediate
group and then to the destination group. This type of routing helps to load
balance the network traffic under some traffic patterns which congest the
single global channel connecting the two groups.  adaptive routing: a
congestion sensing algorithm is used to choose the minimal or non-minimal path
for the packet. progressive adaptive routing: decision to take minimal route is
re-assessed as long as the packet stays in the source group.

A credit-based flow control system is used to maintain congestion control in
the dragonfly. In credit-based flow control, the upstream node/routers keep a
count of free buffer slots in the downstream nodes/routers.  If buffer space is
not available for the next channel, the flit is placed in a pending queue until
a credit arrives for that channel. 

When using non-minimal or adaptive routing, each flit is forwarded to a random
global channel due to which the flits may arrive out-of-order at the receiving
destination node LP. Therefore, we keep a count of the flits arriving at the
destination dragonfly node LP and once all flits of a message arrive, an event
is invoked at the corresponding model-net LP, which notifies the higher level
MPI simulation layer about message arrival. 

ROSS models are made up of a collection of logical processes (LPs).  Each LP
models a distinct component of the system. LPs interact with one another
through events in the form of timestamped messages.In the dragonfly model, each
LP represents an individual router or node and each time-stamped message
represents a packet sent to/from a node/router.

2- Configuring CODES dragonfly network model
CODES dragonfly network model can be configured using the dragonfly config file (currently
located in codes-net/tests/conf). To adjust the network size, configure the MODELNET_GRP
section of the config file as well as the 'num_routers' parameter in the PARAMS section in
the config file. For example, for a dragonfly configuration of a=8, p=4, h=4, we have 33 groups
(g=a*h+1=8*4+1), 264 routers (total_routers=a*g=4*33=264), 1056 network nodes (total_nodes=
p*a*g=8*4*33). This configuration can be specified in the config file in the following way

MODELNET_GRP
{
	repetitions="264";
	server="4";
	modelnet_dragonfly="4";
	dragonfly_router="1";
} 
PARAMS
{
	....
	num_routers="8";
	....
}

The first section, MODELNET_GRP specified the number of LPs and the layout of
LPs. In the above case, there are 264 repetitions of 4 server LPs, 4 dragonfly
network node LPs and 1 dragonfly router LP, which makes a total of 264 routers,
1056 network nodes and 1056 servers in the network.  The second section, PARAMS
uses 'num_routers' for the dragonfly topology lay out and setsup the
connections between routers, network nodes and the servers. The num_routers in
PARAMS controls the topology layout that should match with the
modelnet_dragonfly and dragonfly_router in the first group (MODELNET_GRP).
According to Kim, Dally's a=2p=2h configuration (as given above in Section 1),
in this case num_routers=8 means that each dragonfly group has 8 routers, 4
terminals and each router has 4 global channels. This makes the total number of
dragonfly groups as g = 8 * 4 + 1 = 33 and number of network nodes as N = 4 * 8
* 33 = 1064. A correct dragonfly config file must specify the same number of
network nodes and routers in the MODELNET_GRP section. If there is a mismatch
between num_routers in PARAMS and MODELNET_GRP values then an error message is
displayed.

Some other dragonfly specific parameters in the PARAMS section are

- local_vc_size: Bytes (default: 8 KiB) that can fit in the channel connecting routers
within the same group.
- chunk_size: A full-sized packet of 'packet_size' is divided into smaller
  flits for transporation (default set to 64 bytes).
- global_vc_size: Bytes (default: 16 KiB) that can fit in the global channel connecting
two groups with each other.
- cn_vc_size: Bytes (default: 8 KiB) that can fit in the channel connecting the network 
node with its router.
- local_bandwidth: bandwidth of the channels in GiB/sec connecting the routers within the same group. 
- global_bandwidth: bandwidth of the global channels in GiB/sec connecting
  routers of two different groups. Note that each router has 'h' number of global channels 
  connected to it where a=2p=2h in our configuration.
- cn_bandwidth: bandwidth of the channel connecing the compute node with the router.
** All the above bandwidth parameters are in Gigabytes/sec.
- routing: the routing algorithm can be minimal, nonminimal, adaptive or
  prog-adaptive.

3- Running ROSS dragonfly network model
- To run the dragonfly network model with the model-net test program, the following options are available

ROSS serial mode:

./tests/modelnet-test --sync=1 -- tests/conf/modelnet-test-dragonfly.conf

ROSS conservative mode:

mpirun -np 8 tests/modelnet-test --sync=2 -- tests/conf/modelnet-test-dragonfly.conf

ROSS optimistic mode:

mpirun -np 8 tests/modelnet-test --sync=3 -- tests/conf/modelnet-test-dragonfly.conf

4- Running dragonfly model with DUMPI application traces

- codes-base needs to be configured with DUMPI. See
  codes-base/doc/GETTING_STARTED on how to configure codes-base with DUMPI

5- Performance optimization tips for ROSS dragonfly model
- For large-scale dragonfly runs, the model has significant speedup in optimistic mode than the conservative mode.
- For running large-scale synthetic traffic workloads, see 
  codes-net/src/models/network-workloads/README_synthetic.txt

- Design forward network traces are available at:
  http://portal.nersc.gov/project/CAL/designforward.htm

  For illustration purposes, we use the AMG network trace with 27 MPI processes
  available for download at:

  http://portal.nersc.gov/project/CAL/doe-miniapps-mpi-traces/AMG/df_AMG_n27_dumpi.tar.gz

- Note on trace reading - the input file prefix to the dumpi workload generator
  should be everything up to the rank number. E.g., if the dumpi files are of the
  form "dumpi-YYYY.MM.DD.HH.MM.SS-XXXX.bin", then the input should be
  "dumpi-YYYY.MM.DD.HH.MM.SS-"

- Example dragonfly model config file with network traces can be found at:
  src/models/network-workloads/conf/modelnet-mpi-test-dragonfly.conf

  The routing algorithms can be set as adaptive, nonminimal, minimal and
  prog-adaptive.

- Running CODES dragonfly model with AMG 27 rank trace in optimistic mode:
  mpirun -np 4 ./src/models/network-workloads/model-net-mpi-replay --sync=3
  --batch=2 --disable_compute=1 --workload_type="dumpi"
  --workload_file=../../df_traces/AMG/df_AMG_n27_dumpi/dumpi-2014.03.03.14.55.00-
  --num_net_traces=27 --
  ../src/models/network-workloads/conf/modelnet-mpi-test-dragonfly.conf 

  [batch is ROSS specific parameter that specifies the number of iterations the
  simulation must process before checking the top event scheduling loop for
  anti-messages. A smaller batch size comes with fewer rollbacks. The GVT
  synchronization is done after every batch*gvt-interval epochs (gvt-interval
  is 16 by default).
  
  num_net_traces is the number of MPI processes to be simulated from the trace
  file. With the torus and dragonfly networks, the number of simulated network
  nodes may not exactly match the number of MPI processes. This is because the
  simulated network nodes increase in specific increments for e.g. the number
  of routers in a dragonfly define the network size and the number of
  dimensions, dimension length defines the network nodes in the torus. Due to
  this mismatch, we must ensure that the network nodes in the config file are
  equal to or greater than the MPI processes to be simulated from the trace.
  
  disable_compute is an optional parameter which if set, will make the
  simulation disregard the compute times from the MPI traces. ]


- Running CODES dragonfly model with AMG application trace, 27 ranks in serial
  mode: 

  ./src/models/network-workloads/model-net-mpi-replay --sync=1
  --workload_type="dumpi"
  --workload_file=../../df_traces/AMG/df_AMG_n27_dumpi/dumpi-2014.03.03.14.55.00-
  --num_net_traces=27 --disable_compute=1 --
  ../src/models/network-workloads/conf/modelnet-mpi-test-dragonfly.conf


