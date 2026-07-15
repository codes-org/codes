#!/bin/bash
# Test: ROSS instrumentation model-level stats, all sampling modes at once
# (--model-stats=4 = GVT + real-time + virtual-time).
# Checks the virtual-time (analysis LP) path produces a parseable
# ross-stats-analysis-lps.bin alongside ross-stats-model.bin. Real-time records
# are wall-clock dependent (a fast run may emit few or none), so their presence
# is not asserted -- only that the run and the files stay well-formed.
# Format spec: doc/model-stats-binary-format.md

if [[ -z $bindir ]] ; then
    echo bindir variable not set
    exit 1
fi

export PACKET_SIZE=4096
export CHUNK_SIZE=64
export PACKET_LATENCY_TRACE_PATH=packet-latency-highdef/
cat "$bindir/doc/example"/tutorial-ping-pong.template.conf.in | envsubst > tutorial-ping-pong.conf

# NOTE: --vt-samp-end is required here: it defaults to g_tw_ts_end, which the
# tutorial sets to 24h of virtual time, so without it the analysis LPs would
# keep self-scheduling sample events long after the ping pong traffic ends.
# 2e6 ns with 1e5 ns intervals covers the active phase with ~20 sample points.
mpirun -np 3 "$bindir/doc/example/tutorial-synthetic-ping-pong" --sync=3 \
    --num_messages=10 --payload_sz=8192 \
    --model-stats=4 --num-gvt=100 --rt-interval=1 \
    --vt-interval=1e5 --vt-samp-end=2e6 \
    --stats-path=model-stats-out \
    -- tutorial-ping-pong.conf \
    > model-output.txt 2> model-output-error.txt
err=$?
[[ $err -ne 0 ]] && exit $err

grep 'Net Events Processed' model-output.txt
err=$?
[[ $err -ne 0 ]] && exit $err

# Both output files exist and are non-empty
for f in ross-stats-model.bin ross-stats-analysis-lps.bin ; do
    if [[ ! -s model-stats-out/$f ]] ; then
        >&2 echo "model-stats-out/$f missing or empty"
        exit 1
    fi
done

if command -v python3 > /dev/null ; then
    scriptdir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    python3 "$scriptdir/../scripts/parse-ross-model-stats.py" \
        model-stats-out/ross-stats-model.bin \
        model-stats-out/ross-stats-analysis-lps.bin \
        --csv-prefix model-stats 2> parser-summary.txt
    err=$?
    if [[ $err -ne 0 ]] ; then
        >&2 cat parser-summary.txt
        >&2 echo "reference parser failed"
        exit $err
    fi
    cat parser-summary.txt

    grep 'unknown payloads: 0' parser-summary.txt
    err=$?
    if [[ $err -ne 0 ]] ; then
        >&2 echo "parser hit unknown payload sizes (payload/format drift?)"
        exit 1
    fi

    # the virtual-time path produced records (first CSV column is stats_type)
    if ! cut -d, -f1 model-stats-svr.csv | grep -q '^vt$' ; then
        >&2 echo "no virtual-time svr records decoded"
        exit 1
    fi
else
    echo "python3 not found; skipping reference-parser check"
fi
