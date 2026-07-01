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
artifacts="$expfolder/zmqml-iteration-artifacts"
mkdir -p "$artifacts"

train_workdir="$artifacts/train-run"
hybrid_workdir="$artifacts/hybrid-run"
mkdir -p "$train_workdir" "$hybrid_workdir"

cp "$bindir/doc/example/kb.dfdally-72-milc-small.workload.conf" "$expfolder/"
cp "$bindir/doc/example/kb.dfdally-72-milc-small.json" "$expfolder/"
cp "$bindir/doc/example/kb.dfdally-72-milc-small.alloc.conf" "$expfolder/"

make_conf() {
    local out="$1"
    local start_iter="$2"
    local end_iter="$3"
    local inferencing="$4"
    local surrogate="$5"
    local training="$6"

    START_ITER="$start_iter" \
    END_ITER="$end_iter" \
    RETRAIN_ENABLED=0 \
    RETRAIN_ITER=-1 \
    RETRAIN_SAVE_PATH="" \
    SECOND_SURROGATE_ENABLED=0 \
    SECOND_START_ITER=100000 \
    SECOND_END_ITER=100001 \
    INFERENCING_ENABLED="$inferencing" \
    SURROGATE_ENABLED="$surrogate" \
    TRAINING_ENABLED="$training" \
    DIRECTOR_DEBUG_PRINTS=1 \
    SHUTDOWN_ZMQML_SERVER_ON_FINALIZE=0 \
    PACKET_SIZE=4096 \
    CHUNK_SIZE=64 \
    envsubst < "$bindir/doc/example/kb.dfdally-72-zeromq-director.template.conf.in" > "$out"
}

python3 "$srcdir/src/surrogate/zmqml/zmqmlctl.py" \
    --endpoint "$endpoint" \
    --family iteration-time \
    status

# Phase 1: high-fidelity/director data collection. Inferencing is off, training
# records are sent to the external ZMQML server.
make_conf "$expfolder/iteration-train.conf" 0 10 0 1 1

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
        -- "$expfolder/iteration-train.conf"
) > "$artifacts/iteration-train.out" \
  2> "$artifacts/iteration-train.err"

grep 'Net Events Processed' "$artifacts/iteration-train.out"

python3 "$srcdir/src/surrogate/zmqml/zmqmlctl.py" \
    --endpoint "$endpoint" \
    --family iteration-time \
    train \
    > "$artifacts/iteration-train-model.json"

grep '"status": "done"' "$artifacts/iteration-train-model.json"

python3 "$srcdir/src/surrogate/zmqml/zmqmlctl.py" \
    --endpoint "$endpoint" \
    --family iteration-time \
    save "$artifacts/iteration-time-model.pt" \
    > "$artifacts/iteration-save-model.json"

test -s "$artifacts/iteration-time-model.pt"
grep '"status": "done"' "$artifacts/iteration-save-model.json"

python3 "$srcdir/src/surrogate/zmqml/zmqmlctl.py" \
    --endpoint "$endpoint" \
    --family iteration-time \
    load "$artifacts/iteration-time-model.pt" \
    > "$artifacts/iteration-load-model.json"

grep '"status": "done"' "$artifacts/iteration-load-model.json"

# Phase 2: hybrid inference. Training is off; inference is on.
make_conf "$expfolder/iteration-hybrid.conf" 3 8 1 1 0

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
        -- "$expfolder/iteration-hybrid.conf"
) > "$artifacts/iteration-hybrid.out" \
  2> "$artifacts/iteration-hybrid.err"

grep 'Net Events Processed' "$artifacts/iteration-hybrid.out"
grep -E '\[DIR\] iteration-time predictions director_id=[0-9]+ count=[1-9][0-9]* values=' "$artifacts/iteration-hybrid.err"

exit 0
