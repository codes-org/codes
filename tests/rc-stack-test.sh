#!/bin/bash

if [[ -z $srcdir ]] ; then
    echo srcdir variable not set
    exit 1
fi

if [ -z $GENERATED_USING_CMAKE ]; then
    bindir=.
fi

"$bindir"/tests/modelnet-simplep2p-test
