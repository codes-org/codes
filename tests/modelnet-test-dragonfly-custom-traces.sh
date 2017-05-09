#!/bin/bash

if [ -z $srcdir ]; then
     echo srcdir variable not set.
    exit 1
fi

mpirun -np 2 src/network-workloads/model-net-mpi-replay --sync=3 --num_net_traces=27 --disable_compute=1 --workload_file=/tmp/df_AMG_n27_dumpi/dumpi-2014.03.03.14.55.00- --workload_type="dumpi" -- src/network-workloads/conf/dragonfly-custom/modelnet-test-dragonfly-1728-nodes.conf
