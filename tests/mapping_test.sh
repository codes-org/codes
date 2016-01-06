#!/bin/bash

tst=$srcdir/tests
set -e
tests/mapping_test --sync=1 --codes-config=$tst/conf/mapping_test.conf \
    2> mapping_test.err \
    1| grep TEST > mapping_test.out

diff $tst/expected/mapping_test.out mapping_test.out
err=$?

if [ -s mapping_test.err ] ; then
    echo ERROR: see mapping_test.err
    exit 1
fi

if [ "$err" -eq 0 ]; then
    rm mapping_test.out mapping_test.err
fi

exit $err
