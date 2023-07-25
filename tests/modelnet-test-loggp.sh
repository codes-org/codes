#!/bin/bash

if [ -z $GENERATED_USING_CMAKE ]; then
    bindir=.
fi

"$bindir"/tests/modelnet-test --sync=1 -- "$srcdir"/tests/conf/modelnet-test-loggp.conf
