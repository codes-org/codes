#!/bin/bash

if [[ -z $srcdir ]] ; then
    echo srcdir variable not set
    exit 1
fi

tests/map-ctx-test $srcdir/tests/conf/map-ctx-test.conf
