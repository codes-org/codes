#!/bin/bash

if [ -z $srcdir ]; then
    echo srcdir variable not set.
    exit 1
fi

tests/modelnet-test --sync=1 -- $srcdir/tests/modelnet-test-dragonfly.conf
