*** README file for slim fly network model ***
This file describes the setup and configuration for the ROSS slim fly network model.

1- Model of the slim fly network topology

Introduced by Besta and Hoefler [1], the slim fly is a hierarchical topology having 
many groups of interconnected routers constructed following McKay, Miller, Siran 
(MMS) graphs. Routers are grouped into two subgraphs, each containing "q" groups with
"q" routers per group. Unlike the dragonfly network, the slimfly does not have
fully connected router groups. Intragroup connections are determined by equations
(1) and (2) (equation one for routers in subgraph 0 and equation 2 for routers in
subgraph 1). No connections exist between router groups within the same subgraph, 
resulting in a bipartite graph between subgraphs. Each router has "h" global connections,
which connect to routers in the opposing subgraph and are governed by equation (3). 
Each router has “p” number of compute nodes connected to it. The theoretical upper 
bound for bisection bandwidth for the Slim Fly 
topology is achieved when p = floor(k/2).

router(0, x, y) is connected to (0, x, y') iff y-y' in X  (1)
router(1, m, c) is connected to (1, m, c') iff c-c' in X' (2)
router(0, x, y) is connected to (1, m, c)  iff y = mx+c   (3)

Example visual of structural layout of routers in subgraph zero for use with the 
construction equations (1)-(3) (subgraph,x,y):
(0,0,0) (0,1,0) (0,2,0) (0,3,0) (0,4,0) 
(0,0,0) (0,1,1) (0,2,1) (0,3,1) (0,4,1)
(0,0,0) (0,1,2) (0,2,2) (0,3,2) (0,4,2)
(0,0,0) (0,1,3) (0,2,3) (0,3,3) (0,4,3)
(0,0,0) (0,1,4) (0,2,4) (0,3,4) (0,4,4)

Full-sized network packets (default size: 512 bytes) are
broken into smaller flits (default size: 32 bytes) for transportation
over the network. ROSS slim fly model supports three different forms of
routing: minimal: packet is sent directly from the source router to destination
router using minimal (2-hop) path. non-minimal: packet is first routed (minimally) 
to a randomly selected intermediate group and then routed (minimally) to the 
destination group. This type of routing helps to load
balance the network traffic under some traffic patterns which congest the
minimal 2-hop path. adaptive routing: a congestion sensing algorithm is used to 
choose the minimal or non-minimal path for the packet by checking the output buffer
occupancy for the src router.

A credit-based flow control system is used to maintain congestion control in
the slim fly. In credit-based flow control, the upstream node/routers keep a
count of free buffer slots in the downstream nodes/routers.  If buffer space is
not available for the next channel, the flit is placed in a pending queue until
a credit arrives for that channel.

When using non-minimal or adaptive routing, each flit is forwarded to a random
global channel due to which the flits may arrive out-of-order at the receiving
destination node LP. Therefore, we keep a count of the flits arriving at the
destination slim fly node LP and once all flits of a message arrive, an event
is invoked at the corresponding model-net LP, which notifies the higher level
MPI simulation layer about message arrival. 

ROSS models are made up of a collection of logical processes (LPs).  Each LP
models a distinct component of the system. LPs interact with one another
through events in the form of timestamped messages. In the slim fly model, each
LP represents an individual router or node and each time-stamped message
represents a packet sent to/from a node/router.

2- Configuring CODES silmfly network model
CODES slim fly network model can be configured using the slim fly config file (currently
located in codes/tests/conf). To adjust the network size, configure the MODELNET_GRP
section of the config file as well as the 'num_routers' parameter in the PARAMS section in
the config file. For example, for a slim fly configuration of q=5, p=3, we have 5 routers per 
group, 5 groups per subgraph, 50 total routers (Nr = 2*q^2 = 2*5^2), 150 total compute nodes
(Nn = Nr*p = 50*3). Generator sets, used to construct the local router group connections must 
must also be set in the config file. Generator sets for tested/verified slim fly configurations
are provided below. For other slim fly configurations, the user is refered to [1] for constructing 
generator sets. This configuration can be specified in the config file in the following way

MODELNET_GRP
{
	repetitions="50";
	server="3";
	modelnet_slimfly="3";
	slimfly_router="1";
}

PARAMS
{
	....
	num_routers="5";
	generator_set_X=("1","4");
	generator_set_X_prime=("2","3");
	....
}

The first section, MODELNET_GRP specified the number of LPs and the layout of
LPs. In the above case, there are 50 repetitions of 3 server LPs, 3 slim fly
network node LPs and 1  slim fly router LP, which makes a total of 50 routers,
150 network nodes and 150 servers in the network.  The second section, PARAMS
uses 'num_routers' for the  slim fly topology lay out and sets up the
connections between routers, network nodes and the servers. The num_routers in
PARAMS controls the topology layout that should match with the
modelnet_slimfly and slimfly_router in the first group (MODELNET_GRP).
In this case num_routers=5 means that each slim fly group has 5 routers. This 
makes the total number of slim fly groups as g = 5*5 = 25. A correct slim fly 
config file must specify the same number of network nodes and routers in the 
MODELNET_GRP section. If there is a mismatch between num_routers in PARAMS and 
MODELNET_GRP values then an error message is displayed.

Some other slim fly specific parameters in the PARAMS section are

- local_vc_size: Bytes (default: 256 KiB) that can fit in the channel connecting routers
  within the same group.
- chunk_size: A full-sized packet of 'packet_size' is divided into smaller
  flits for transporation (default set to 64 bytes).
- global_vc_size: Bytes (default: 256 KiB) that can fit in the global channel connecting
  two routers in the two different subgraphs with each other.
- cn_vc_size: Bytes (default: 8 KiB) that can fit in the channel connecting the network 
  node with its router.
- local_bandwidth: bandwidth of the channels in GiB/sec connecting the routers within the same group. 
- global_bandwidth: bandwidth of the global channels in GiB/sec connecting
  routers of two different groups. Note that each router has 'h' number of global channels 
  connected to it.
- cn_bandwidth: bandwidth of the channel connecing the compute node with the router.
** All the above bandwidth parameters are in Gigabytes/sec.
- routing: the routing algorithm can be minimal, nonminimal or adaptive.

3- Running ROSS slim fly network model
- To run the slim fly network model with the model-net test program, the following options are available

ROSS serial mode:

./tests/modelnet-test --sync=1 -- tests/conf/modelnet-test-slimfly.conf

ROSS conservative mode:

mpirun -np 8 tests/modelnet-test --sync=2 -- tests/conf/modelnet-test-slimfly.conf

ROSS optimistic mode:

mpirun -np 8 tests/modelnet-test --sync=3 -- tests/conf/modelnet-test-slimfly.conf

4- Performance optimization tips for ROSS slim fly model
- For large-scale slim fly runs, the model has significant speedup in optimistic mode than the conservative mode.
- For running large-scale synthetic traffic workloads, see 
  codes-net/src/network-workloads/README_synthetic_slimfly.txt

5- Miscellaneous
Description of symbols used:
q: Prime power
p: Compute nodes/terminals connected to a router
Nr: Total routers in network (Nr = 2 * q^2)
Nn: Total nodes in network (Nn = Nr * p)
g: groups (g = q * q)
k': Router network radix
k: Router radix (k = k' + p)
h: global channels (h = q)
l: local channels (l = k' - h)
X: subgraph 0 generator set (used to compute local intragroup connections)
X': subgraph 1 generator set (used to compute local intragroup connections)


Tested/Verified Configurations:
	q=5, X={1,4}, X'={2,3}
	q=13, X={1,10,9,12,3,4}, X'={6,8,2,7,5,11}
	q=29, X={1,6,7,13,20,4,24,28,23,22,16,9,25,5}, X'={8,19,27,17,15,3,18,21,10,2,12,14,26,11}
	q=37, X={1,25,33,11,16,30,10,28,34,36,12,4,26,21,7,27,9,3,
   		  X'={32,23,20,19,31,35,24,8,15,5,14,17,18,6,2,13,29,22}

The user is referred to [1] for detailed instructions on creating different MMS/slim fly 
graphs/layouts.

[1] M. Besta and T. Hoefler. Slim Fly: A Cost Effective Low-Diameter Network Topology.
    Nov. 2014. Proceedings of the International Conference on High Performance Computing,
    Networking, Storage and Analysis (SC14).
