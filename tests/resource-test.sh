#!/bin/bash

if [[ -z $srcdir ]] ; then
    echo srcdir variable not set
    exit 1
fi

if [ -z $GENERATED_USING_CMAKE ]; then
    bindir=.
fi

"$bindir"/tests/resource-test --sync=1 --codes-config="$srcdir"/tests/conf/buffer_test.conf
