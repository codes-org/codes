#!/bin/bash

if [ -z $srcdir ]; then
         echo srcdir variable not set.
              exit 1
 fi

source $srcdir/tests/download-traces.sh

src/network-workloads/model-net-synthetic-slimfly --sync=1 -- $srcdir/src/network-workloads/conf/modelnet-synthetic-slimfly-min.conf 

mpirun -np 2 src/network-workloads/model-net-mpi-replay --disable_compute=1 --sync=3 --num_net_traces=27 --workload_file=/tmp/df_AMG_n27_dumpi/dumpi-2014.03.03.14.55.00- --workload_type="dumpi" -- $srcdir/src/network-workloads/conf/modelnet-mpi-test-slimfly-min.conf 

