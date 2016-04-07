1- Download, build and install the DUMPI software according to the 
   instructions available at:
   http://sst.sandia.gov/about_dumpi.html

   Configure dumpi with the following parameters:

   ../configure --enable-test --disable-shared --prefix=/home/mubarm/dumpi/dumpi/install CC=mpicc CXX=mpicxx

2- Configure codes with DUMPI. Make sure the CC environment variable
   refers to a MPI compiler

   ./configure PKG_CONFIG_PATH=$PATH --with-dumpi=/path/to/dumpi/install
	 --prefix=/path/to/codes-base/install CC=mpicc 

3- Build codes-base (See codes-base INSTALL for instructions on building codes-base with dumpi)

    make clean && make && make install

4- Configure and build codes-net (See INSTALL for instructions on building codes-net).

5- Download and untar the design forward DUMPI traces from URL

   http://portal.nersc.gov/project/CAL/designforward.htm

--- RUNNING CODES MPI SIMULATION LAYER (DEFAULT JOB ALLOCATION, SINGLE WORKLOAD)-------------
6- Download and untar the DUMPI AMG application trace for 216 MPI ranks using the following download link:

wget
http://portal.nersc.gov/project/CAL/doe-miniapps-mpi-traces/AMG/df_AMG_n216_dumpi.tar.gz

8- Configure model-net config file (For this example config file is available at
src/network-workloads/conf/modelnet-mpi-test-dfly-amg-216.conf)

9- Run the DUMPI trace replay simulation on top of model-net using:
   (/dumpi-2014.03.03.14.55.23- is the prefix of the DUMPI trace file. 
   We skip the last 4 digit prefix of the DUMPI trace files).

   ./src/network-workloads//model-net-mpi-replay --sync=1 
   --num_net_traces=216 --workload_file=/path/to/dumpi/trace/directory/dumpi-2014.03.03.15.09.03-  
   --workload_type="dumpi" --lp-io-dir=amg-216-trace --lp-io-use-suffix=1 
   -- ../src/network-workloads/conf/modelnet-mpi-test-dfly-amg-216.conf 

  The simulation runs in ROSS serial, conservative and optimistic modes.

   Note: Dragonfly and torus networks may have more number of nodes in the
   network than the number network traces (Some network nodes will only pass
   messages and they will not end up loading the traces). Thats why
   --num_net_traces argument is used to specify exact number of traces
   available in the DUMPI directory if there is a mis-match between number of
   network nodes and traces.

10- Running the simulation in optimistic mode 
    
    mpirun -np 4 ./src/network-workloads//model-net-mpi-replay --sync=3
    --num_net_traces=216 --workload_type="dumpi" --lp-io-dir=amg_216-trace
    --lp-io-use-suffix=1
    --workload_file=/projects/radix-io/mubarak/df_traces/directory/dumpi-2014.03.03.15.09.03- 
    -- src/network-workloads//conf/modelnet-mpi-test-dfly-amg-216.conf 

--- RUNNING MPI SIMULATION LAYER WITH MULTIPLE WORKLOADS --------
11- Generate job allocation file (random or contiguous) using python scripts.

Allocation options 

- Random allocation assigns a set of randomly selected network nodes to each
job. 
- Contiguous allocation assigns a set of contiguous network nodes to the
jobs. 

See codes/allocation_gen/README for instructions on how to generate job
allocation file using python. Example allocation files are in
src/network-workloads/conf/allocation-rand.conf, allocation-cont.conf.

12- Run the simulation with multiple job allocations

./src/network-workloads//model-net-mpi-replay --sync=1
--workload_conf_file=../src/network-workloads/workloads.conf
--alloc_file=../src/network-workloads/conf/allocation-rand.conf
--workload_type="dumpi" --
../src/network-workloads/conf/modelnet-mpi-test-dfly-amg-216.conf 

To run in optimistic mode:

mpirun -np 4 ./src/network-workloads//model-net-mpi-replay --sync=3
--workload_conf_file=../src/network-workloads/workloads.conf --alloc_file=allocation.conf
--workload_type="dumpi" --
../src/network-workloads/conf/modelnet-mpi-test-dfly-amg-216.conf

----- sampling and debugging options for MPI Simulation Layer ---- 

Runtime options can be used to enable time-stepped series data of simulation
with multiple workloads:

--enable_sampling = 1 [Enables sampling of network & workload statistics after a specific simulated interval.
Default sampling interval is 5 millisec and default sampling end time is 3
secs. These values can be adjusted at runtime using --sampling_interval and
--sampling_end_time options.]


--lp-io-dir-dir-name [Turns on end of simulation statistics for dragonfly network model]

--lp-io-use-suffix [Output to a unique directory ]

--enable_mpi_debug=1 prints the details of the MPI operations being simulated. Enabling
debug mode can display a lot of print statements!.

