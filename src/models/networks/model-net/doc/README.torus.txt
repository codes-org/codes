*** README file for torus network model ***

1- Model of the torus network topology 

CODES torus model uses realistic design parameters of a torus network, and we
have validated our simulation results against the existing Blue Gene torus
architecture.  Similar to the Blue Gene architecture, CODES torus model uses a
bubble escape virtual channel to prevent deadlocks.  Following the
specifications of the BG/Q 5D torus network, by default the CODES torus model
has packets with a maximum size of 512 bytes, in which each packet is broken
into flits of 32 bytes for transportation over the network.  Our torus network
model uses dimension-order routing to route packets. In this form of routing,
the radix-k digits of the destination are used to direct network packets, one
dimension at a time.

For more details about the torus network and its design, please see 

Blue Gene/L torus interconnection network by Adiga, Blumrich et al.

The torus network model has a two LP types, a model-net LP and a torus node LP.
The high-level messages are passed on to the model-net LP by either the MPI
simulation layer or a synthetic workload generator.  These messages are
scheduled on to the underlying torus node LP in the form of network packets,
which are then further broken into flits.  Each torus node LP is connected to
its neighbors via channels having a fixed buffer capacity.  Similar to the
dragonfly node LP, the torus network model also uses a credit-based flow
control to regulate network traffic.  Whenever a flit arrives at the torus
network node, a hop delay based on the router speed and port bandwidth is added
in order to simulate the processing time of the flit.  Once all flits of a
packet arrive at the destination torus node LP, they are forwarded to the
receiving model-net layer LP, which notifies the higher level (MPI simulation
layer or synthetic workload generator) layer about message arrival. 

2- Configuring CODES torus network model
A simple config file for the CODES torus model can be found in codes-net/tests/conf.
The configuration parameters are as follows:
* n_dims - the number of torus dimensions.
* dim_length - the length of each torus dimension. For example, "4,2,2,2"
* describes a
  four-dimensional, 4x2x2x2 torus.
  * link_bandwidth - the bandwidth available per torus link (specified in
  * GiB/sec).
  * buffer_size - the buffer size available at each torus node. Buffer size is
  * measured
  in number of flits or chunks (flit/chunk size is configurable using
  chunk_size parameter).
  * num_vc - number of virtual channels (currently unused - all traffic goes
    through a single channel).
  * chunk_size - element size per transfer, specified in bytes.
  * Messages/packets are sent in
      individual chunks. This is typically a small number (e.g., 32 bytes).

3- Running torus model test program
- To run the torus network model with the modelnet-test program, the following
  options are available

ROSS serial mode: 

./tests/modelnet-test --sync=1 -- tests/conf/modelnet-test-torus.conf

ROSS optimistic mode:

mpirun -np 4 ./tests/modelnet-test --sync=3 -- tests/conf/modelnet-test-torus.conf

4- Running torus model with DUMPI application traces

- codes-base needs to be configured with DUMPI. See
  codes-base/doc/GETTING_STARTED on how to configure codes-base with DUMPI

- Design forward network traces are available at:
  http://portal.nersc.gov/project/CAL/designforward.htm

  For illustration purposes, we use the AMG network trace with 27 MPI processes
  available for download at:

  http://portal.nersc.gov/project/CAL/doe-miniapps-mpi-traces/AMG/df_AMG_n27_dumpi.tar.gz

- Note on trace reading - the input file prefix to the dumpi workload generator
  should be everything up to the rank number. E.g., if the dumpi files are of the
  form "dumpi-YYYY.MM.DD.HH.MM.SS-XXXX.bin", then the input should be
  "dumpi-YYYY.MM.DD.HH.MM.SS-"

- Example torus model config file with network traces can be found at:
  src/models/network-workloads/conf/modelnet-mpi-test-torus.conf

- Running CODES torus model with AMG 27 rank trace in optimistic mode:
  mpirun -np 4 ./src/models/network-workloads/model-net-mpi-replay --sync=3
  --batch=2 --workload_type="dumpi" --num_net_traces=27 --disable_compute=1 
  --workload_file=../../df_traces/AMG/df_AMG_n27_dumpi/dumpi-2014.03.03.14.55.00-
  -- ../src/models/network-workloads/conf/modelnet-mpi-test-torus.conf
  
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

 
- Running CODES torus model with AMG application trace, 27 ranks in serial
  mode:

  ./src/models/network-workloads/model-net-mpi-replay --sync=1
  --workload_type=dumpi --disable_compute=1 
  --workload_file=../../df_traces/AMG/df_AMG_n27_dumpi/dumpi-2014.03.03.14.55.00-
  --num_net_traces=27 --
  ../src/models/network-workloads/conf/modelnet-mpi-test-torus.conf 

- At small scale (usually up to a thousand simulated MPI processes), ROSS
  serial mode has better performance than the optimistic mode. The benefits of
  using optimistic mode usually show up for larger scale runs where the
  simulation gets too big to run sequentially.

