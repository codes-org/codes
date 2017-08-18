#!/bin/bash

if [ -z $srcdir ]; then
    echo srcdir variable not set.
    exit 1
fi

src/workload/codes-workload-dump --num-ranks 4 --type darshan_io_workload --d-log $srcdir/tests/workload/example.darshan --d-aggregator-cnt 1
