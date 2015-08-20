#!/bin/bash

if [ -z $srcdir ]; then
    echo srcdir variable not set.
    exit 1
fi

src/workload/codes-workload-dump --type darshan_io_workload --d-log $srcdir/tests/workload/example.darshan.gz --d-aggregator-cnt 1
