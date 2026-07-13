#!/bin/bash
set -euo pipefail

if [[ -z "${bindir:-}" ]]; then
    echo "bindir variable not set"
    exit 1
fi

if [[ -z "${srcdir:-}" ]]; then
    echo "srcdir variable not set"
    exit 1
fi

endpoint="${ZMQML_ENDPOINT:-tcp://localhost:5555}"
np="${ZMQML_TEST_NP:-1}"
mpi_exec="${ZMQML_MPIEXEC_EXECUTABLE:-mpirun}"
mpi_np_flag="${ZMQML_MPIEXEC_NUMPROC_FLAG:--np}"

expfolder="$PWD"
artifacts="$expfolder/zmqml-fluid-flow-wan-artifacts"
pure_workdir="$artifacts/pure-run"
hybrid_workdir="$artifacts/hybrid-run"
models="$artifacts/models"

rm -rf "$artifacts"
mkdir -p "$pure_workdir/logs" "$hybrid_workdir/logs" "$models"

cp "$bindir/doc/example/fluid-flow-wan-topology.yaml" \
    "$pure_workdir/fluid-flow-wan-topology.yaml"
cp "$bindir/doc/example/fluid-flow-wan-topology.yaml" \
    "$hybrid_workdir/fluid-flow-wan-topology.yaml"

sed \
    -e 's|topology_yaml_file="[^"]*";|topology_yaml_file="fluid-flow-wan-topology.yaml";|' \
    -e 's|switch_scheduler="[^"]*";|switch_scheduler="round_robin";|' \
    -e 's|terminal_log_path="[^"]*";|terminal_log_path="logs/terminal-events.csv";|' \
    -e 's|switch_log_path="[^"]*";|switch_log_path="logs/switch-events.csv";|' \
    -e 's|flowlet_log_path="[^"]*";|flowlet_log_path="logs/flowlet-events.csv";|' \
    -e 's|switch_training_log_path="[^"]*";|switch_training_log_path="logs/switch-training.csv";|' \
    "$bindir/doc/example/fluid-flow-wan.conf" \
    > "$pure_workdir/fluid-flow-wan.conf"

(
    cd "$pure_workdir"
    "$mpi_exec" "$mpi_np_flag" "$np" \
        "$bindir/src/model-net-fluid-flow-wan" \
        --synch=1 -- fluid-flow-wan.conf
) > "$artifacts/pure.out" 2> "$artifacts/pure.err"

grep 'Net Events Processed' "$artifacts/pure.out"
test -s "$pure_workdir/logs/switch-training.csv"

python3 "$srcdir/src/surrogate/zmqml/zmqmlctl.py" \
    --endpoint "$endpoint" \
    --family fluid-flow-wan \
    load-records "$pure_workdir/logs/switch-training.csv" \
    > "$artifacts/load-records.json"

grep '"status": "done"' "$artifacts/load-records.json"

python3 "$srcdir/src/surrogate/zmqml/zmqmlctl.py" \
    --endpoint "$endpoint" \
    --family fluid-flow-wan \
    train \
    > "$artifacts/train.json"

grep '"status": "done"' "$artifacts/train.json"

python3 - "$artifacts/train.json" <<'PY_TRAIN_CHECK'
import json
import sys
from pathlib import Path

data = json.loads(Path(sys.argv[1]).read_text())
trained = int(data.get("trained_models", "0"))
if trained != 3:
    raise SystemExit(f"expected one model for each of 3 switches, got: {data}")
print(f"fluid-flow-wan trained_models={trained}")
PY_TRAIN_CHECK

python3 "$srcdir/src/surrogate/zmqml/zmqmlctl.py" \
    --endpoint "$endpoint" \
    --family fluid-flow-wan \
    save "$models" \
    > "$artifacts/save.json"

grep '"status": "done"' "$artifacts/save.json"
for switch_id in 0 1 2; do
    test -s "$models/switch-${switch_id}.pt"
done

python3 "$srcdir/src/surrogate/zmqml/zmqmlctl.py" \
    --endpoint "$endpoint" \
    --family fluid-flow-wan \
    load "$models" \
    > "$artifacts/load-models.json"

grep '"status": "done"' "$artifacts/load-models.json"

sed \
    -e 's|topology_yaml_file="[^"]*";|topology_yaml_file="fluid-flow-wan-topology.yaml";|' \
    -e "s|director_endpoint=\"[^\"]*\";|director_endpoint=\"$endpoint\";|" \
    -e 's|terminal_log_path="[^"]*";|terminal_log_path="logs/terminal-events.csv";|' \
    -e 's|switch_log_path="[^"]*";|switch_log_path="logs/switch-events.csv";|' \
    -e 's|flowlet_log_path="[^"]*";|flowlet_log_path="logs/flowlet-events.csv";|' \
    -e 's|switch_training_log_path="[^"]*";|switch_training_log_path="logs/switch-decisions.csv";|' \
    "$bindir/doc/example/fluid-flow-director.conf" \
    > "$hybrid_workdir/fluid-flow-director.conf"

(
    cd "$hybrid_workdir"
    "$mpi_exec" "$mpi_np_flag" "$np" \
        "$bindir/src/model-net-fluid-flow-wan" \
        --synch=1 -- fluid-flow-director.conf
) > "$artifacts/hybrid.out" 2> "$artifacts/hybrid.err"

grep 'Net Events Processed' "$artifacts/hybrid.out"
grep 'switch_scheduler=ml' "$artifacts/hybrid.out"
test -s "$hybrid_workdir/logs/switch-decisions.csv"

python3 - "$hybrid_workdir/logs/switch-decisions.csv" <<'PY_HYBRID_CHECK'
import csv
import sys
from pathlib import Path

path = Path(sys.argv[1])
with path.open(newline="") as f:
    rows = list(csv.DictReader(f))

if not rows:
    raise SystemExit("hybrid switch decision log is empty")
if any(row.get("scheduler") != "ml" for row in rows):
    raise SystemExit("hybrid decision log contains a non-ML scheduler row")
if not any(row.get("used_fallback") == "0" for row in rows):
    raise SystemExit("every hybrid egress used fallback; ML inference was not exercised")

print(f"fluid-flow-wan hybrid decision rows={len(rows)}")
PY_HYBRID_CHECK

exit 0
