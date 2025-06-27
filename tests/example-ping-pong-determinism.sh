#!/bin/bash

if [[ -z $bindir ]] ; then
    echo bindir variable not set
    exit 1
fi

# Running simulation twice with the same parameters

mpirun -np 3 "$bindir/doc/example/tutorial-synthetic-ping-pong" --sync=3 \
    --num_messages=10 --payload_sz=8192 \
    -- "$bindir/doc/example/tutorial-ping-pong.conf" \
    > model-output-1.txt 2> model-output-1-error.txt
err=$?
[[ $err -ne 0 ]] && exit $err

mpirun -np 3 "$bindir/doc/example/tutorial-synthetic-ping-pong" --sync=3 \
    --num_messages=10 --payload_sz=8192 \
    -- "$bindir/doc/example/tutorial-ping-pong.conf" \
    > model-output-2.txt 2> model-output-2-error.txt
err=$?
[[ $err -ne 0 ]] && exit $err

# Checking that there is actual output
grep 'Net Events Processed' model-output-1.txt
err=$?
[[ $err -ne 0 ]] && exit $err

# This checks for the number of events processed. If they are different, then
# the simulation is not deterministic (so this should fail!). As always, just
# a unit test
diff <(grep 'Net Events Processed' model-output-1.txt) \
    <(grep 'Net Events Processed' model-output-2.txt)
err=$?
if [[ $err -ne 0 ]]; then
    >&2 echo "The number of net events processed does not coincide, ie," \
        "the simulation is not deterministic"
    exit $err
fi
