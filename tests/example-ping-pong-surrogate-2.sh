#!/bin/bash
# Test: checking whether surrogate mode behaves the same as high-fidelity
# Should take at most 1 minute to run

if [[ -z $bindir ]] ; then
    echo bindir variable not set
    exit 1
fi

# Configuring surrogate instance
export PACKET_SIZE=128
export CHUNK_SIZE=64
export NETWORK_TREATMENT=freeze
export PREDICTOR_TYPE=average
export PACKET_LATENCY_TRACE_PATH=packet-latency-surrogate/
export IGNORE_UNTIL=0.0
export SWITCH_TIMESTAMPS='".08e6", ".1e6", ".2e6", ".6e6", ".7e6", ".9e6", "1.0e6", "1.3e6", "1.6e6", "1.7e6", "1.9e6", "2.0e6", "2.3e6", "2.6e6", "2.7e6", "2.9e6", "3.0e6", "3.3e6", "3.6e6", "3.7e6", "3.9e6", "4.0e6", "4.3e6", "4.6e6", "4.7e6", "4.9e6", "5.0e6", "9.8e6"'
cat "$bindir/doc/example"/tutorial-surrogate.template.conf.in | envsubst > tutorial-ping-pong-surrogate.conf

export PACKET_LATENCY_TRACE_PATH=packet-latency-highdef/
cat "$bindir/doc/example"/tutorial.template.conf.in | envsubst > tutorial-ping-pong.conf

# Running simulation twice with the same parameters

mpirun -np 1 "$bindir/doc/example/tutorial-synthetic-ping-pong" --sync=1 \
    --num_messages=10 --payload_sz=16320 \
    -- tutorial-ping-pong.conf \
    > model-output-1.txt 2> model-output-1-error.txt
err=$?
[[ $err -ne 0 ]] && exit $err

mpirun -np 1 "$bindir/doc/example/tutorial-synthetic-ping-pong" --sync=1 \
    --num_messages=10 --payload_sz=16320 \
    -- tutorial-ping-pong-surrogate.conf \
    > model-output-2.txt 2> model-output-2-error.txt
err=$?
[[ $err -ne 0 ]] && exit $err

# Checking that there is actual output
grep 'Net Events Processed' model-output-1.txt
err=$?
[[ $err -ne 0 ]] && exit $err

# Checking that the surrogate switched properly
grep 'Switch completed' model-output-2.txt
err=$?
[[ $err -ne 0 ]] && exit $err

# This checks for the number of events processed. If they are different, then
# the simulation is not deterministic (so this should fail!). As always, just
# a unit test
to_remove_from_output=' sent [0-9]* bytes in [0-9.]* seconds'
diff <(grep "Sever LPID:" model-output-1.txt | sed "s/${to_remove_from_output}//") \
     <(grep "Sever LPID:" model-output-2.txt | sed "s/${to_remove_from_output}//")
err=$?
if [[ $err -ne 0 ]]; then
    >&2 echo "The surrogate gave different results from high-fidelity"
    exit $err
fi

# This checks for an equal number of packets transmitted
diff <(packet-latency-surrogate/*.txt | wc -l) <(packet-latency-highdef/*.txt | wc -l)
err=$?
if [[ $err -ne 0 ]]; then
    >&2 echo "The surrogate gave different results from high-fidelity on the number of packets transmitted"
    exit $err
fi
