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

----------------- RUNNING CODES MPI SIMULATION LAYER -----------------------
6- Download and untar the DUMPI AMG application trace for 1728 MPI ranks using the following download link:

wget http://portal.nersc.gov/project/CAL/doe-miniapps-mpi-traces/AMG/df_AMG_n1728_dumpi.tar.gz

8- Configure model-net config file (For this example config file is available at
src/network-workloads/conf/modelnet-mpi-test-dfly-amg-1728.conf)

9- Run the DUMPI trace replay simulation on top of model-net using:
   (/dumpi-2014-04-05.22.12.17.37- is the prefix of the DUMPI trace file. 
   We skip the last 4 digit prefix of the DUMPI trace files).

   ./src/network-workloads//model-net-mpi-replay --sync=1 
   --num_net_traces=1728 --workload_file=/path/to/dumpi/trace/directory/dumpi-2014.03.03.15.09.03-  
   --workload_type="dumpi" --lp-io-dir=amg-1728-trace --lp-io-use-suffix=1 
   -- ../src/network-workloads/conf/modelnet-mpi-test-dfly-amg-1728.conf 

  The simulation runs in ROSS serial, conservative and optimistic modes.

   Note: Dragonfly and torus networks may have more number of nodes in the network than the number network traces (Some network nodes will only pass messages and they will not end up loading the traces). Thats why --num_net_traces argument is used to specify exact number of traces available in the DUMPI directory if there is a mis-match between number of network nodes and traces.

10- Running the simulation in optimistic mode 
    
    mpirun -np 4 ./src/network-workloads//model-net-mpi-replay
    --batch=32 --gvt-interval=128 --sync=3
    --num_net_traces=13824 --workload_type=dumpi --lp-io-dir=amg_1728-trace
    --lp-io-use-suffix=1
    --workload_file=/projects/radix-io/mubarak/df_traces/directory/dumpi-2014.03.03.15.09.03- 
    -- src/network-workloads//conf/modelnet-mpi-test-dfly-amg-1728.conf 

---------------- Running Test Program (needs update) --------------------------
11- Run the test program for codes-nw-workload using. 

mpirun -np 4 ./src/models/mpi-trace-replay/model-net-dumpi-traces-dump --sync=3 --workload_type=dumpi --workload_file=/home/mubarm/df_traces/df_AMG_n27_dumpi/dumpi-2014.03.03.14.55.00- -- ../src/models/mpi-trace-replay/conf/modelnet-mpi-test.conf

The program shows the number of sends, receives, collectives and wait operations in the DUMPI trace log.

Note: If using a different DUMPI trace file, make sure to update the modelnet-mpi-test.conf file in the config directory.

