#!/bin/bash

tests/modelnet-prio-sched-test --sync=1 -- \
    $srcdir/tests/conf/modelnet-prio-sched-test.conf
err=$?
if [[ $err -ne 0 ]]; then
    exit $err
fi

mpirun -np 2 tests/modelnet-prio-sched-test --sync=3 -- \
    $srcdir/tests/conf/modelnet-prio-sched-test.conf
err=$?
if [[ $err -ne 0 ]]; then
    exit $err
fi
