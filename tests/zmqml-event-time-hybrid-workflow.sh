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

expfolder="$PWD"
artifacts="$expfolder/zmqml-event-artifacts"
mkdir -p "$artifacts"

train_workdir="$artifacts/train-run"
hybrid_workdir="$artifacts/hybrid-run"
mkdir -p "$train_workdir" "$hybrid_workdir"

cp "$bindir/doc/example/kb.dfdally-72-milc-small.json" "$expfolder/"
cp "$bindir/doc/example/kb.dfdally-72-milc-small.alloc.conf" "$expfolder/"

# Keep the event-time CI test small. The stock 10-iteration MILC run
# processes millions of events and makes the hybrid event-time inference
# phase too large for a smoke test.
python3 - "$expfolder/kb.dfdally-72-milc-small.json" <<'PYJSON'
import json
import sys
from pathlib import Path

path = Path(sys.argv[1])
data = json.loads(path.read_text())
data["jobs"]["cfg"]["iteration_cnt"] = 4
path.write_text(json.dumps(data, indent=2) + "\n")
PYJSON

printf '72 %s/kb.dfdally-72-milc-small.json 1 0\n' "$expfolder" \
    > "$expfolder/kb.dfdally-72-milc-small.workload.conf"

make_event_conf() {
    local out="$1"
    local inferencing="$2"
    local training="$3"

    SURROGATE_BACKEND=dragonfly-dally \
    SURROGATE_FAMILY=event-time \
    START_ITER=1 \
    END_ITER=2 \
    RETRAIN_ENABLED=0 \
    RETRAIN_ITER=-1 \
    RETRAIN_SAVE_PATH="" \
    SECOND_SURROGATE_ENABLED=0 \
    SECOND_START_ITER=100000 \
    SECOND_END_ITER=100001 \
    INFERENCING_ENABLED="$inferencing" \
    SURROGATE_ENABLED=1 \
    TRAINING_ENABLED="$training" \
    DIRECTOR_DEBUG_PRINTS=1 \
    SHUTDOWN_ZMQML_SERVER_ON_FINALIZE=0 \
    envsubst < "$bindir/doc/example/kb.dfdally-72-event-time-director.template.conf.in" > "$out"
}

python3 "$srcdir/src/surrogate/zmqml/zmqmlctl.py" \
    --endpoint "$endpoint" \
    --family event-time \
    status

# Phase 1: high-fidelity event-time record collection.
make_event_conf "$expfolder/event-time-train.conf" 0 1

(
    cd "$train_workdir"
    ZMQML_ENDPOINT="$endpoint" \
    mpirun -np "$np" "$bindir/src/model-net-mpi-replay" \
        --synch=1 \
        --workload_type=swm-online \
        --disable_compute=0 \
        --payload_sz=4096 \
        --workload_conf_file="$expfolder/kb.dfdally-72-milc-small.workload.conf" \
        --alloc_file="$expfolder/kb.dfdally-72-milc-small.alloc.conf" \
        -- "$expfolder/event-time-train.conf"
) > "$artifacts/event-time-train.out" \
  2> "$artifacts/event-time-train.err"

grep 'Net Events Processed' "$artifacts/event-time-train.out"
grep -E '\[event-time records\].*send_to_zmq=1' "$artifacts/event-time-train.err"

python3 "$srcdir/src/surrogate/zmqml/zmqmlctl.py" \
    --endpoint "$endpoint" \
    --family event-time \
    train \
    > "$artifacts/event-time-train-model.json"

grep '"status": "done"' "$artifacts/event-time-train-model.json"

python3 "$srcdir/src/surrogate/zmqml/zmqmlctl.py" \
    --endpoint "$endpoint" \
    --family event-time \
    save "$artifacts/event-time-models" \
    > "$artifacts/event-time-save-model.json"

test -f "$artifacts/event-time-models/manifest.json"
grep '"status": "done"' "$artifacts/event-time-save-model.json"

python3 "$srcdir/src/surrogate/zmqml/zmqmlctl.py" \
    --endpoint "$endpoint" \
    --family event-time \
    load "$artifacts/event-time-models" \
    > "$artifacts/event-time-load-model.json"

grep '"status": "done"' "$artifacts/event-time-load-model.json"

# Phase 2: hybrid event-time inference.
make_event_conf "$expfolder/event-time-hybrid.conf" 1 0

(
    cd "$hybrid_workdir"
    ZMQML_ENDPOINT="$endpoint" \
    mpirun -np "$np" "$bindir/src/model-net-mpi-replay" \
        --synch=1 \
        --workload_type=swm-online \
        --disable_compute=0 \
        --payload_sz=4096 \
        --workload_conf_file="$expfolder/kb.dfdally-72-milc-small.workload.conf" \
        --alloc_file="$expfolder/kb.dfdally-72-milc-small.alloc.conf" \
        -- "$expfolder/event-time-hybrid.conf"
) > "$artifacts/event-time-hybrid.out" \
  2> "$artifacts/event-time-hybrid.err"

grep 'Net Events Processed' "$artifacts/event-time-hybrid.out"
grep -E '\[event-time surrogate\] active=1|\[event-time inference cache-(hit|miss)\]' "$artifacts/event-time-hybrid.err"

exit 0
