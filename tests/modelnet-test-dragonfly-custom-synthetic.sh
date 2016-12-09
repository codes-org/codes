#!/bin/bash

if [ -z $srcdir ]; then
         echo srcdir variable not set.
              exit 1
 fi

src/network-workloads/model-net-synthetic-custom-dfly --sync=1 --num_messages=1 -- src/network-workloads/conf/dragonfly-custom/modelnet-test-dragonfly-theta.conf 



