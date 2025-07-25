#!/bin/bash

if [[ -z $srcdir ]] ; then
    echo srcdir variable not set
    exit 1
fi

if [ -z $GENERATED_USING_CMAKE ]; then
    bindir=.
fi

mpirun -np 1 "$bindir"/tests/map-ctx-test "$srcdir"/tests/conf/map-ctx-test.conf
