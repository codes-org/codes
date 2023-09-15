#!/bin/bash

if [ -z $GENERATED_USING_CMAKE ]; then
    bindir=.
fi

mpirun -np 1 "$bindir"/tests/modelnet-test --sync=1 -- "$srcdir"/tests/conf/modelnet-test-dragonfly.conf
