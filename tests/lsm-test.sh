#!/bin/bash

if [ -z $srcdir ]; then
    echo srcdir variable not set.
    exit 1
fi

tests/lsm-test --sync=1 --conf=$srcdir/tests/conf/lsm-test.conf
