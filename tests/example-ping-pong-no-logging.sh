#!/bin/bash
# Test: checking simulation runs without any problem when "packet latency path" is not given

if [[ -z $bindir ]] ; then
    echo bindir variable not set
    exit 1
fi

# Configuring surrogate instance
export PACKET_SIZE=4096
export CHUNK_SIZE=4096
export PACKET_LATENCY_TRACE_PATH=
cat "$bindir/doc/example"/tutorial.template.conf.in | envsubst > tutorial-ping-pong.conf

# Running simulation twice with the same parameters

mpirun -np 1 "$bindir/doc/example/tutorial-synthetic-ping-pong" --sync=1 \
    --num_messages=10 --payload_sz=4096 \
    -- tutorial-ping-pong.conf \
    > model-output-1.txt 2> model-output-1-error.txt
err=$?
[[ $err -ne 0 ]] && exit $err

# Checking that there is actual output
grep 'Net Events Processed' model-output-1.txt
