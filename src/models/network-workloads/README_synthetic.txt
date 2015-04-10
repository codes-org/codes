************ Synthetic traffic with dragonfly network model **********
- traffic patterns supported: uniform random, nearest neighbor traffic.
	- Uniform random traffic: sends messages to a randomly selected destination node. It is uniformly distributed throughout the network and gives a better performance with minimal routing and therefore higher simulation event rate.
	- Nearest group traffic: with minimal routing, it sends traffic to the single global channel connecting two groups (it congests the network when using minimal routing). May have a low simulation event rate because of the congestion being simulated.
	- Nearest neighbor traffic: it sends traffic to the next node, potentially connected to the same router. 
HOW TO RUN:

ROSS optimistic mode:
mpirun -np 4 ./src/models/mpi-trace-replay/model-net-synthetic --sync=3 --traffic=2 --arrival_time=500.0  --extramem=200000 ../tests/conf/modelnet-synthetic-dragonfly.conf

ROSS serial mode:
./src/models/mpi-trace-replay/model-net-synthetic --sync=1 --traffic=2 --arrival_time=500.0  --extramem=200000 ../tests/conf/modelnet-synthetic-dragonfly.conf 

options:

arrival_time: inter-arrival time between the messages. Smaller inter-arrival time means messages will arrive more frequently (smaller inter-arrival time can cause congestion in the network and may overflow the network buffers).
traffic: 1 for uniform random traffic, 2 for nearest group traffic and 3 for nearest neighbor traffic.
