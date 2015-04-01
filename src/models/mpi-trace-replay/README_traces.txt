1- Download, build and install the DUMPI software according to the 
   instructions available at:
   http://sst.sandia.gov/about_dumpi.html

   Configure dumpi with the following parameters:

   ../configure --enable-test --disable-shared --prefix=/home/mubarm/dumpi/dumpi/install CC=mpicc CXX=mpicxx

2- Configure codes-base with DUMPI. Make sure the CC environment variable
   refers to a MPI compiler

   ./configure --with-ross=/path/to/ross/install --with-dumpi=/path/to/dumpi/install
	 --prefix=/path/to/codes-base/install CC=mpicc 

3- Build codes-base (See codes-base INSTALL for instructions on building codes-base with dumpi)

    make clean && make && make install

4- Configure and build codes-net (See INSTALL for instructions on building codes-net).

5- Download and untar the design forward DUMPI traces from URL

   http://portal.nersc.gov/project/CAL/designforward.htm

----------------- RUNNING CODES NETWORK WORKLOAD TEST PROGRAM -----------------------
6- Download and untar the DUMPI AMG application trace for 27 MPI ranks using the following download link:

wget http://portal.nersc.gov/project/CAL/doe-miniapps-mpi-traces/AMG/df_AMG_n27_dumpi.tar.gz

7- Run the test program for codes-nw-workload using. 

mpirun -np 4 ./src/models/mpi-trace-replay/model-net-dumpi-traces-dump --sync=3 --workload_type=dumpi --workload_file=/home/mubarm/df_traces/df_AMG_n27_dumpi/dumpi-2014.03.03.14.55.00- -- ../src/models/mpi-trace-replay/conf/modelnet-mpi-test.conf

The program shows the number of sends, receives, collectives and wait operations in the DUMPI trace log.

Note: If using a different DUMPI trace file, make sure to update the modelnet-mpi-test.conf file in the config directory.

----------------- RUNNING MODEL-NET WITH CODES NW WORKLOADS -----------------------------
8- Configure model-net using its config file (Example .conf files available at src/models/mpi-trace-replay/)
   Make sure the number of nw-lp and model-net LP are the same in the config file.

9- From the main source directory of codes-net, run the DUMPI trace replay simulation on top of
   model-net using (/dumpi-2014-04-05.22.12.17.37- is the prefix of the all DUMPI trace files. 
   We skip the last 4 digit prefix of the DUMPI trace file names).

   ./src/models/mpi-trace-replay/model-net-mpi-wrklds --sync=1 --workload_file=/path/to/dumpi/trace/directory/dumpi-2014-04-05.22.12.17.37- - --workload_type="dumpi" -- src/models/mpi-trace-replay/conf/modelnet-mpi-test.conf 

  The simulation runs in ROSS serial, conservative and optimistic modes.

10- Some example runs with small-scale traces

(i) AMG 8 MPI tasks http://portal.nersc.gov/project/CAL/designforward.htm#AMG

   ** Torus network model
   mpirun -np 4 ./src/models/mpi-trace-replay/model-net-mpi-wrklds --sync=3 --extramem=962144 --workload_file=/home/mubarm/dumpi/df_AMG_n27_dumpi/dumpi-2014.03.03.14.12.46- --workload_type="dumpi" --batch=2 --gvt-interval=2 --num_net_traces=27 -- tests/conf/modelnet-mpi-test-torus.conf

  ** Simplenet network model

  mpirun -np 8 ./src/models/mpi-trace-replay/model-net-mpi-wrklds --sync=3 --workload_file=/home/mubarm/dumpi/df_AMG_n27_dumpi/dumpi-2014.03.03.14.12.46- --workload_type="dumpi" --batch=2 --gvt-interval=2 -- tests/conf/modelnet-mpi-test.conf

  ** Dragonfly network model
   mpirun -np 8 ./src/models/mpi-trace-replay/model-net-mpi-wrklds --sync=3 --extramem=2962144 --workload_file=/home/mubarm/dumpi/df_AMG_n27_dumpi/dumpi-2014.03.03.14.12.46- --workload_type="dumpi" --batch=2 --gvt-interval=2 --num_net_traces=27 -- src/models/mpi-trace-replay//conf/modelnet-mpi-test-dragonfly.conf
  
   Note: Dragonfly and torus networks may have more number of nodes in the network than the number network traces (Some network nodes will only pass messages and they will not end up loading the traces). Thats why --num_net_traces argument is used to specify exact number of traces available in the DUMPI directory if there is a mis-match between number of network nodes and traces.

(ii) Crystal router 10 MPI tasks http://portal.nersc.gov/project/CAL/designforward.htm#CrystalRouter

  ** Simple-net network model 
  mpirun -np 10 ./src/models/mpi-trace-replay/model-net-mpi-wrklds --sync=3 --extramem=185536 --workload_file=/home/mubarm/dumpi/cry_router/dumpi--2014.04.23.12.08.27- --workload_type="dumpi" -- src/models/mpi-trace-replay/conf/modelnet-mpi-test-cry-router.conf

(iii) MiniFE 18 MPI tasks http://portal.nersc.gov/project/CAL/designforward.htm#MiniFE

** Simple-net network model
  mpirun -np 18 ./src/models/mpi-trace-replay/model-net-mpi-wrklds --sync=3 --extramem=6185536 --workload_file=/home/mubarm/dumpi/dumpi_data_18/dumpi-2014.04.22.12.17.37- --workload_type="dumpi" -- src/models/mpi-trace-replay/conf/modelnet-mpi-test-mini-fe.conf 
