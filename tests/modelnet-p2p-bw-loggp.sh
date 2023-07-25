#!/bin/bash

if [ -z $GENERATED_USING_CMAKE ]; then
    bindir=.
fi

"$bindir"/tests/modelnet-p2p-bw --sync=1 -- "$srcdir"/tests/conf/modelnet-p2p-bw-loggp.conf
