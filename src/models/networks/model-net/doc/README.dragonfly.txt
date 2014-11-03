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
broken into smaller packet chunks (default size: 32 bytes) for transportation
over the network.  ROSS dragonfly model supports three different forms of
routing: minimal: packet is sent directly from the source group to destination
group over the single global channel connecting the source and destination
groups.  non-minimal: packet is first sent to an intermediate group and then to
the destination group. This type of routing helps to load balance the network
traffic under some traffic patterns which congest the single global channel
connecting the two groups.  adaptive routing: a congestion sensing algorithm is
used to choose the minimal or non-minimal path for the packet.

ROSS models are made up of a collection of logical processes (LPs).  Each LP
models a distinct component of the system. LPs interact with one another
through events in the form of timestamped messages.In the dragonfly model, each
LP represents an individual router or node and each time-stamped message
represents a packet sent to/from a node/router.

2- Configuring ROSS dragonfly network model
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

The first section, MODELNET_GRP specified the number of LPs and the layout of LPs. In the above
case, there are 264 repetitions of 4 server LPs, 4 dragonfly network node LPs and 1 dragonfly
router LP, which makes a total of 264 routers, 1056 network nodes and 1056 servers in the network.
The second section, PARAMS uses 'num_routers' for the dragonfly topology lay out and setsup the
connections between routers, network nodes and the servers.

Some other dragonfly specific parameters in the PARAMS section are

- num_vcs: number of virtual channels connecting a router-router, node-router (default set to 1)
- local_vc_size: Number of packet chunks (default: 32 bytes) that can fit in the channel connecting routers
within the same group.
- chunk_size: A full-sized packet of 'packet_size' is divided into smaller packet chunks for transporation
(default set to 64 bytes).
- global_vc_size: Number of packet chunks (default: 32 bytes) that can fit in the global channel connecting
two groups with each other.
- cn_vc_size: Number of packet chunks (default: 32 bytes) that can fit in the channel connecting the network 
node with its router.
- local_bandwidth: bandwidth of the channels connecting the routers within the same group. 
- global_bandwidth: bandwidth of the global channels connecting routers of two different groups. Note than 
each router has 'h' number of global channels connected to it where a=2p=2h in our configuration.
- cn_bandwidth: bandwidth of the channel connecing the compute node with the router.
** All the above bandwidth parameters are in Gigabytes/sec.
- routing: the routing algorithm can be minimal, nonminimal or adaptive.


3- Running ROSS dragonfly network model
- To run the dragonfly network model with the model-net test program, the following options are available

ROSS serial mode:

./tests/modelnet-test --sync=1 -- tests/conf/modelnet-test-dragonfly.conf

ROSS conservative mode:

mpirun -np 8 tests/modelnet-test --sync=2 -- tests/conf/modelnet-test-dragonfly.conf

ROSS optimistic mode:

mpirun -np 8 tests/modelnet-test --sync=3 -- tests/conf/modelnet-test-dragonfly.conf

4- Performance optimization tips for ROSS dragonfly model
- For large-scale dragonfly runs, the model has significant speedup in optimistic mode than the conservative mode.
- If 
