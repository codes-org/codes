#!/bin/bash

if [[ -z $srcdir ]] ; then
    echo srcdir variable not set
    exit 1
fi

tests/jobmap-test $srcdir/tests/conf/jobmap-test-list.conf
