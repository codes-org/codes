#!/bin/bash

if [ -z $GENERATED_USING_CMAKE ]; then
    bindir=.
fi

"$bindir"/tests/lp-io-test --sync=1
