#!/bin/bash

if [ -z $srcdir ]; then
         echo srcdir variable not set.
              exit 1
 fi

src/network-workloads/model-net-synthetic --sync=1 --num_messages=1 -- $srcdir/src/network-workloads/conf/modelnet-synthetic-dragonfly.conf 



