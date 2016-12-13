#!/bin/bash

if [ -z $srcdir ]; then
         echo srcdir variable not set.
              exit 1
 fi

source $srcdir/tests/download-traces.sh

src/network-workloads/model-net-synthetic --sync=1 --num_messages=1 -- $srcdir/src/network-workloads/conf/modelnet-synthetic-dragonfly.conf 

src/network-workloads/model-net-mpi-replay --sync=1 --num_net_traces=27 --workload_file=/tmp/df_AMG_n27_dumpi/dumpi-2014.03.03.14.55.00- --workload_type="dumpi" -- $srcdir/src/network-workloads/conf/modelnet-mpi-test-dfly-amg-216.conf 


