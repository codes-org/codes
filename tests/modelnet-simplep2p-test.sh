#!/bin/bash

if [[ -z $srcdir ]] ; then
    echo srcdir variable not set
    exit 1
fi

if [ -z $GENERATED_USING_CMAKE ]; then
    bindir=.
fi

mpirun -np 1 "$bindir"/tests/modelnet-simplep2p-test --sync=1 -- "$srcdir"/tests/conf/modelnet-test-simplep2p.conf
