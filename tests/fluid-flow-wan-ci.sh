#!/bin/bash
set -euo pipefail

synch="${1:?synch mode required: 1 or 3}"
np="${2:?MPI rank count required}"
case_name="${3:-fluid-flow-wan-synch${synch}}"
mpi_exec="${4:-mpirun}"
mpi_np_flag="${5:--np}"

if [[ -z "${bindir:-}" || -z "${srcdir:-}" ]]; then
    echo "bindir/srcdir are not set; run through tests/run-test.sh"
    exit 1
fi

binary="$bindir/src/model-net-fluid-flow-wan"
base_conf="$bindir/doc/example/fluid-flow-wan.conf"
topology="$bindir/doc/example/fluid-flow-wan-topology.yaml"
[[ -f "$topology" ]] || topology="$srcdir/doc/example/fluid-flow-wan-topology.yaml"

[[ -x "$binary" ]] || { echo "missing executable: $binary"; exit 1; }
[[ -f "$base_conf" ]] || { echo "missing generated config: $base_conf"; exit 1; }
[[ -f "$topology" ]] || { echo "missing topology file"; exit 1; }

rm -rf "$case_name"
mkdir -p "$case_name/logs"
cp "$topology" "$case_name/fluid-flow-wan-topology.yaml"

sed \
    -e 's|topology_yaml_file="[^"]*";|topology_yaml_file="fluid-flow-wan-topology.yaml";|' \
    -e 's|terminal_log_path="[^"]*";|terminal_log_path="logs/terminal-events.csv";|' \
    -e 's|switch_log_path="[^"]*";|switch_log_path="logs/switch-events.csv";|' \
    -e 's|flowlet_log_path="[^"]*";|flowlet_log_path="logs/flowlet-events.csv";|' \
    -e 's|switch_training_log_path="[^"]*";|switch_training_log_path="logs/switch-training.csv";|' \
    "$base_conf" > "$case_name/fluid-flow-wan.conf"

if ! (
    cd "$case_name"
    "$mpi_exec" "$mpi_np_flag" "$np" "$binary" --sync="$synch" -- fluid-flow-wan.conf \
        > model-output.txt 2> model-output-error.txt
); then
    echo "fluid-flow-wan model run failed"
    cat "$case_name/model-output.txt" || true
    cat "$case_name/model-output-error.txt" || true
    exit 1
fi

out="$case_name/model-output.txt"
grep "fluid-flow-wan config:" "$out"
grep "Net Events Processed" "$out"
grep -Eq "source_backlog_(mbit|gbit)=" "$out"
grep -Eq "rate_updates_received=[1-9][0-9]*" "$out"
grep -Eq "rate_cache_entries=[1-9][0-9]*" "$out"
grep -q ',generate_flow,' "$case_name/logs/terminal-events.csv"
grep -q ',send,' "$case_name/logs/terminal-events.csv"

if [[ "$synch" == "1" ]]; then
    grep "csv_logs=buffered-forward" "$out"
else
    grep "csv_logs=buffered-commit" "$out"
fi

for csv in terminal-events.csv switch-events.csv flowlet-events.csv switch-training.csv; do
    [[ -s "$case_name/logs/$csv" ]] || { echo "missing or empty CSV log: $csv"; exit 1; }
done
