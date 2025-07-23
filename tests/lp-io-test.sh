#!/bin/bash

if [ -z $GENERATED_USING_CMAKE ]; then
    bindir=.
fi

mpirun -np 1 "$bindir"/tests/lp-io-test --sync=1
