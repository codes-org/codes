#!/bin/bash
set -euo pipefail

synch="${1:?synch mode required: 1 or 3}"
np="${2:?MPI rank count required}"
case_name="${3:-fluid-flow-wan-trace-traffic-synch${synch}}"
mpi_exec="${4:-mpirun}"
mpi_np_flag="${5:--np}"

if [[ -z "${bindir:-}" || -z "${srcdir:-}" ]]; then
    echo "bindir/srcdir are not set; run through tests/run-test.sh"
    exit 1
fi

binary="$bindir/src/model-net-fluid-flow-wan-trace-traffic"
base_yaml="$bindir/doc/example/fluid-flow-wan-trace-traffic.yaml"
topology="$bindir/doc/example/fluid-flow-wan-topology.yaml"
trace="$bindir/doc/example/fluid-flow-wan-trace-traffic.csv"
[[ -f "$topology" ]] || topology="$srcdir/doc/example/fluid-flow-wan-topology.yaml"
[[ -f "$trace" ]] || trace="$srcdir/doc/example/fluid-flow-wan-trace-traffic.csv"

[[ -x "$binary" ]] || { echo "missing executable: $binary"; exit 1; }
[[ -f "$base_yaml" ]] || { echo "missing generated config: $base_yaml"; exit 1; }
[[ -f "$topology" ]] || { echo "missing topology file"; exit 1; }
[[ -f "$trace" ]] || { echo "missing trace file"; exit 1; }

rm -rf "$case_name"
mkdir -p "$case_name/logs"
cp "$topology" "$case_name/fluid-flow-wan-topology.yaml"
cp "$trace" "$case_name/fluid-flow-wan-trace-traffic.csv"
cp "$base_yaml" "$case_name/fluid-flow-wan-trace-traffic.yaml"

if ! (
    cd "$case_name"
    "$mpi_exec" "$mpi_np_flag" "$np" "$binary" --sync="$synch" -- fluid-flow-wan-trace-traffic.yaml \
        > model-output.txt 2> model-output-error.txt
); then
    echo "fluid-flow-wan trace-traffic model run failed"
    cat "$case_name/model-output.txt" || true
    cat "$case_name/model-output-error.txt" || true
    exit 1
fi

out="$case_name/model-output.txt"
grep "fluid-flow-wan config: workload=trace-traffic" "$out"
grep "trace_records=" "$out"
grep "Net Events Processed" "$out"
grep -Eq "source_backlog_(mbit|gbit)=" "$out"
grep -q ',trace_offer,' "$case_name/logs/terminal-events.csv"
grep -q ',send,' "$case_name/logs/terminal-events.csv"

if [[ "$synch" == "1" ]]; then
    grep "csv_logs=buffered-forward" "$out"
else
    grep "csv_logs=buffered-commit" "$out"
fi

for csv in terminal-events.csv switch-events.csv flowlet-events.csv; do
    [[ -s "$case_name/logs/$csv" ]] || { echo "missing or empty CSV log: $csv"; exit 1; }
done
