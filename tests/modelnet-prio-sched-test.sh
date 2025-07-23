#!/bin/bash

if [ -z $GENERATED_USING_CMAKE ]; then
    bindir=.
fi

mpirun -np 1 "$bindir"/tests/modelnet-prio-sched-test --sync=1 -- \
    $srcdir/tests/conf/modelnet-prio-sched-test.conf
err=$?
if [[ $err -ne 0 ]]; then
    exit $err
fi

mpirun -np 2 "$bindir"/tests/modelnet-prio-sched-test --sync=3 -- \
    $srcdir/tests/conf/modelnet-prio-sched-test.conf
err=$?
if [[ $err -ne 0 ]]; then
    exit $err
fi
