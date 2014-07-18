#!/bin/bash

tst=$srcdir/tests
set -e
tests/mapping_test --sync=1 --codes-config=$tst/conf/mapping_test.conf | \
    grep TEST > mapping_test.out

diff $tst/expected/mapping_test.out mapping_test.out
err=$?

if [ "$err" -eq 0 ]; then
    rm mapping_test.out
fi
