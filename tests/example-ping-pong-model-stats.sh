#!/bin/bash
# Test: ROSS instrumentation model-level stats, GVT mode (--model-stats=1).
# Runs the ping pong tutorial with GVT-based model-stats collection and checks
# that ross-stats-model.bin is produced, non-empty, and parseable by the
# reference parser (scripts/parse-ross-model-stats.py), with records for all
# three LP payloads (svr, dally terminal, dally router).
# Format spec: doc/model-stats-binary-format.md

if [[ -z $bindir ]] ; then
    echo bindir variable not set
    exit 1
fi

export PACKET_SIZE=4096
export CHUNK_SIZE=64
export PACKET_LATENCY_TRACE_PATH=packet-latency-highdef/
cat "$bindir/doc/example"/tutorial-ping-pong.template.conf.in | envsubst > tutorial-ping-pong.conf

# GVT-based sampling needs a synchronization protocol that computes intermediate
# GVTs, so run conservative/optimistic (sync=3), not sequential. This run computes
# a few thousand GVTs; sampling every 100th keeps the output around a megabyte
# (every GVT would be ~50x that) while still yielding dozens of samples per LP.
mpirun -np 3 "$bindir/doc/example/tutorial-synthetic-ping-pong" --sync=3 \
    --num_messages=10 --payload_sz=8192 \
    --model-stats=1 --num-gvt=100 --stats-path=model-stats-out \
    -- tutorial-ping-pong.conf \
    > model-output.txt 2> model-output-error.txt
err=$?
[[ $err -ne 0 ]] && exit $err

# Simulation completed
grep 'Net Events Processed' model-output.txt
err=$?
[[ $err -ne 0 ]] && exit $err

# Model stats file exists and is non-empty
if [[ ! -s model-stats-out/ross-stats-model.bin ]] ; then
    >&2 echo "model-stats-out/ross-stats-model.bin missing or empty"
    exit 1
fi

# Parse it with the reference parser (soft-skip if python3 is unavailable;
# python3 is not a hard test dependency of this repo)
if command -v python3 > /dev/null ; then
    scriptdir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    python3 "$scriptdir/../scripts/parse-ross-model-stats.py" \
        model-stats-out/ross-stats-model.bin \
        --csv-prefix model-stats 2> parser-summary.txt
    err=$?
    if [[ $err -ne 0 ]] ; then
        >&2 cat parser-summary.txt
        >&2 echo "reference parser failed on ross-stats-model.bin"
        exit $err
    fi
    cat parser-summary.txt

    # every payload size decoded (config dims match the parser defaults)
    grep 'unknown payloads: 0' parser-summary.txt
    err=$?
    if [[ $err -ne 0 ]] ; then
        >&2 echo "parser hit unknown payload sizes (payload/format drift?)"
        exit 1
    fi

    # at least one record of each LP type (csv has a header line)
    for lptype in svr terminal router ; do
        nrec=$(( $(wc -l < model-stats-${lptype}.csv) - 1 ))
        if [[ $nrec -lt 1 ]] ; then
            >&2 echo "no ${lptype} records in ross-stats-model.bin"
            exit 1
        fi
    done
else
    echo "python3 not found; skipping reference-parser check"
fi
