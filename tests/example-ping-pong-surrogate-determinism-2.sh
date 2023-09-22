#!/bin/bash

if [[ -z $bindir ]] ; then
    echo bindir variable not set
    exit 1
fi

# Configuring surrogate instance
export PACKET_SIZE=1024
export CHUNK_SIZE=1024
export NETWORK_TREATMENT=freeze
export PREDICTOR_TYPE=average
export PACKET_LATENCY_TRACE_PATH=packet-latency-surrogate-1/
export IGNORE_UNTIL=0.0
export SWITCH_TIMESTAMPS='".08e6", ".1e6", ".2e6", ".6e6", ".7e6", ".9e6", "1.0e6", "1.3e6", "1.6e6", "1.7e6", "1.9e6", "2.0e6", "2.3e6", "2.6e6", "2.7e6", "2.9e6", "3.0e6", "3.3e6", "3.6e6", "3.7e6", "3.9e6", "4.0e6", "4.3e6", "4.6e6", "4.7e6", "4.9e6", "5.0e6", "9.8e6"'
cat "$bindir/doc/example"/tutorial-ping-pong-surrogate.template.conf.in | envsubst > tutorial-ping-pong-surrogate-1.conf

export PACKET_LATENCY_TRACE_PATH=packet-latency-surrogate-2/
cat "$bindir/doc/example"/tutorial-ping-pong-surrogate.template.conf.in | envsubst > tutorial-ping-pong-surrogate-2.conf

# Running simulation twice with the same parameters

mpirun -np 3 "$bindir/doc/example/tutorial-synthetic-ping-pong" --sync=3 \
    --num_messages=100 --payload_sz=8192 \
    -- tutorial-ping-pong-surrogate-1.conf \
    > model-output-1.txt 2> model-output-1-error.txt
err=$?
[[ $err -ne 0 ]] && exit $err

mpirun -np 3 "$bindir/doc/example/tutorial-synthetic-ping-pong" --sync=3 \
    --num_messages=100 --payload_sz=8192 \
    -- tutorial-ping-pong-surrogate-2.conf \
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
