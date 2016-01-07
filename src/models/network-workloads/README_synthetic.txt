************ Synthetic traffic with dragonfly network model **********
- traffic patterns supported: uniform random, nearest group and nearest neighbor traffic.
	- Uniform random traffic: sends messages to a randomly selected destination
      node. This traffic pattern is uniformly distributed throughout the
      network and gives a better performance with minimal routing as compared
      to non-minimal or adaptive routing.
	- Nearest group traffic: with minimal routing, it sends traffic to the single global channel connecting two groups (it congests the network when using minimal routing).
    This pattern performs better with non-minimal and adaptive routing
    algorithms.
	- Nearest neighbor traffic: it sends traffic to the next node, potentially connected to the same router. 
HOW TO RUN:

ROSS optimistic mode:
mpirun -np 4 ./src/models/network-workloads/model-net-synthetic --sync=3
--traffic=3 --lp-io-dir=mn_synthetic --lp-io-use-suffix=1  --arrival_time=100.0
-- ../src/models/network-workloads/conf/modelnet-synthetic-dragonfly.conf

ROSS serial mode:

./src/models/network-workloads/model-net-synthetic --sync=1 --traffic=3
--lp-io-dir=mn_synthetic --lp-io-use-suffix=1  --arrival_time=100.0 --
../src/models/network-workloads/conf/modelnet-synthetic-dragonfly.conf

options:

arrival_time: inter-arrival time between the messages. Smaller inter-arrival
time means messages will arrive more frequently (smaller inter-arrival time can
cause congestion in the network).

num_msgs: number of messages generated per terminal. Each message has a size of
2048 bytes. By default, 20 messages per terminal are generated. 

traffic: 1 for uniform random traffic, 2 for nearest group traffic and 3 for nearest neighbor traffic.

lp-io-dir: generates network traffic information on dragonfly terminals and
routers. Here is information on individual files:

*dragonfly-router-stats: Has information on how much time each link of a router
spent with its buffer space full. With this information, we can know which
links of a router had more congestion than the others.

*dragonfly-msg-stats: has overall network traffic information i.e. how long the
terminal links connected to the routers were congested, amount of data received
by each terminal, time spent in receiving the data, number of packets finished, 
average number of hops traversed by the receiving packets.

lp-io-use-suffix: Having a suffix ensures that the data files go to a unique
directory every time. 


