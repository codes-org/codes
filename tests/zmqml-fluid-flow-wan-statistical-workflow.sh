#!/bin/bash
set -euo pipefail

if [[ -z "${bindir:-}" || -z "${srcdir:-}" ]]; then
    echo "bindir/srcdir are not set"
    exit 1
fi

endpoint="${ZMQML_ENDPOINT:-tcp://localhost:5555}"
artifacts="$PWD/zmqml-fluid-flow-wan-statistical-artifacts"
rm -rf "$artifacts"
mkdir -p "$artifacts"/{pdes,statistical}

binary="$bindir/src/model-net-fluid-flow-wan"
base_yaml="$bindir/doc/example/fluid-flow-wan.yaml"
topology="$bindir/doc/example/fluid-flow-wan-topology.yaml"
[[ -f "$topology" ]] || topology="$srcdir/doc/example/fluid-flow-wan-topology.yaml"

make_case() {
    local mode="$1"
    local workdir="$artifacts/$mode"
    cp "$base_yaml" "$workdir/fluid-flow-wan.yaml"
    cp "$topology" "$workdir/fluid-flow-wan-topology.yaml"
    sed -i -E "s/^([[:space:]]*)egress_model:[[:space:]].*/\1egress_model: $mode/" \
        "$workdir/fluid-flow-wan.yaml"
    sed -i -E "s/^([[:space:]]*)debug_prints:[[:space:]].*/\1debug_prints: 1/" \
        "$workdir/fluid-flow-wan.yaml"
    mkdir -p "$workdir/logs"
}

run_case() {
    local mode="$1"
    local workdir="$artifacts/$mode"
    (
        cd "$workdir"
        ZMQML_ENDPOINT="$endpoint" \
        mpirun -np 1 "$binary" --sync=1 -- fluid-flow-wan.yaml
    ) > "$artifacts/$mode.out" 2> "$artifacts/$mode.err"
    grep 'Net Events Processed' "$artifacts/$mode.out"
    grep "egress_model=$mode" "$artifacts/$mode.out"
    test -s "$workdir/logs/terminal-events.csv"
    test -s "$workdir/logs/switch-events.csv"
    test -s "$workdir/logs/flowlet-events.csv"
}

make_case pdes
run_case pdes

python3 "$srcdir/src/surrogate/zmqml/zmqmlctl.py" \
    --endpoint "$endpoint" --family fluid-flow-wan --backend statistical status \
    > "$artifacts/statistical-status.json"
grep '"status": "done"' "$artifacts/statistical-status.json"
grep '"training_required": "0"' "$artifacts/statistical-status.json"

make_case statistical
run_case statistical
grep '\[fluid-flow-wan statistical egress\]' "$artifacts/statistical.err"

pdes_events=$(awk '/Net Events Processed/{value=$NF} END{print value}' "$artifacts/pdes.out")
statistical_events=$(awk '/Net Events Processed/{value=$NF} END{print value}' \
    "$artifacts/statistical.out")
[[ -n "$pdes_events" && "$pdes_events" == "$statistical_events" ]]

for csv in terminal-events.csv switch-events.csv flowlet-events.csv; do
    {
        head -n 1 "$artifacts/pdes/logs/$csv"
        tail -n +2 "$artifacts/pdes/logs/$csv" | sort
    } > "$artifacts/pdes-$csv.sorted"
    {
        head -n 1 "$artifacts/statistical/logs/$csv"
        tail -n +2 "$artifacts/statistical/logs/$csv" | sort
    } > "$artifacts/statistical-$csv.sorted"
    cmp "$artifacts/pdes-$csv.sorted" "$artifacts/statistical-$csv.sorted"
done

python3 "$srcdir/src/surrogate/zmqml/zmqmlctl.py" \
    --endpoint "$endpoint" --family fluid-flow-wan --backend statistical status \
    > "$artifacts/statistical-status-after.json"
grep '"model_count":' "$artifacts/statistical-status-after.json"

echo "fluid-flow-wan pure-PDES and statistical egress workflow passed"
