#!/bin/bash
set -euo pipefail

scheduler="${1:?scheduler required: fifo or round_robin}"
synch="${2:?synch mode required: 1 or 3}"
np="${3:?MPI rank count required}"
case_name="${4:-fluid-flow-wan-${scheduler}-synch${synch}}"
mpi_exec="${5:-mpirun}"
mpi_np_flag="${6:--np}"

if [[ "$scheduler" != "fifo" && "$scheduler" != "round_robin" ]]; then
    echo "unsupported scheduler: $scheduler"
    exit 1
fi

if [[ -z "${bindir:-}" ]]; then
    echo "bindir is not set; this script should be run through tests/run-test.sh"
    exit 1
fi

if [[ -z "${srcdir:-}" ]]; then
    echo "srcdir is not set; this script should be run through tests/run-test.sh"
    exit 1
fi

binary="$bindir/src/model-net-fluid-flow-wan"
base_conf="$bindir/doc/example/fluid-flow-wan.conf"

if [[ ! -x "$binary" ]]; then
    echo "missing executable: $binary"
    exit 1
fi

if [[ ! -f "$base_conf" ]]; then
    echo "missing generated config: $base_conf"
    exit 1
fi

topology="$bindir/doc/example/fluid-flow-wan-topology.yaml"
if [[ ! -f "$topology" ]]; then
    topology="$srcdir/doc/example/fluid-flow-wan-topology.yaml"
fi

if [[ ! -f "$topology" ]]; then
    echo "missing topology file"
    exit 1
fi

rm -rf "$case_name"
mkdir -p "$case_name/logs"
cp "$topology" "$case_name/fluid-flow-wan-topology.yaml"

python3 - "$base_conf" "$case_name/fluid-flow-wan.conf" "$scheduler" <<'PY'
from pathlib import Path
import re
import sys

base_conf = Path(sys.argv[1])
out_conf = Path(sys.argv[2])
scheduler = sys.argv[3]

text = base_conf.read_text()

def replace_one(pattern, replacement, label):
    global text
    text, n = re.subn(pattern, replacement, text, count=1)
    if n != 1:
        raise SystemExit(f"could not replace {label}")

replace_one(r'switch_scheduler="[^"]*";',
            f'switch_scheduler="{scheduler}";',
            "switch_scheduler")

replace_one(r'topology_yaml_file="[^"]*";',
            'topology_yaml_file="fluid-flow-wan-topology.yaml";',
            "topology_yaml_file")

replace_one(r'terminal_log_path="[^"]*";',
            'terminal_log_path="logs/terminal-events.csv";',
            "terminal_log_path")

replace_one(r'switch_log_path="[^"]*";',
            'switch_log_path="logs/switch-events.csv";',
            "switch_log_path")

replace_one(r'flowlet_log_path="[^"]*";',
            'flowlet_log_path="logs/flowlet-events.csv";',
            "flowlet_log_path")

replace_one(r'switch_training_log_path="[^"]*";',
            'switch_training_log_path="logs/switch-training.csv";',
            "switch_training_log_path")

out_conf.write_text(text)
PY

if ! (
    cd "$case_name"
    "$mpi_exec" "$mpi_np_flag" "$np" "$binary" --sync="$synch" -- fluid-flow-wan.conf \
        > model-output.txt 2> model-output-error.txt
); then
    echo "fluid-flow-wan model run failed"
    echo "--- stdout ---"
    cat "$case_name/model-output.txt" || true
    echo "--- stderr ---"
    cat "$case_name/model-output-error.txt" || true
    exit 1
fi

out="$case_name/model-output.txt"

grep "fluid-flow-wan config:" "$out"
grep "switch_scheduler=$scheduler" "$out"
grep "Net Events Processed" "$out"

if [[ "$synch" == "1" ]]; then
    grep "csv_logs=buffered-forward" "$out"
else
    grep "csv_logs=buffered-commit" "$out"
fi

for csv in \
    "$case_name/logs/terminal-events.csv" \
    "$case_name/logs/switch-events.csv" \
    "$case_name/logs/flowlet-events.csv" \
    "$case_name/logs/switch-training.csv"
do
    if [[ ! -s "$csv" ]]; then
        echo "missing or empty CSV log: $csv"
        exit 1
    fi
done
