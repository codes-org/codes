#!/bin/bash

if [[ -z $bindir ]] ; then
    echo bindir variable not set
    exit 1
fi

if [[ -z $UNION_DATAROOTDIR ]] ; then
    echo UNION_DATAROOTDIR variable not set
    exit 1
fi

if [[ -z $SWM_DATAROOTDIR ]] ; then
    echo SWM_DATAROOTDIR variable not set
    exit 1
fi

np=3

expfolder="$PWD"
export CONFIGS_PATH="$srcdir/tests/conf/union-milc-jacobi-workload"

# Backing up and copying milc json!
tmpdir="$(TMPDIR="$PWD" mktemp -d)"
mv "$SWM_DATAROOTDIR/milc_skeleton.json" "$tmpdir/milc_skeleton.json"
cp "$CONFIGS_PATH/milc_skeleton.json" "$SWM_DATAROOTDIR/milc_skeleton.json"
mv "$UNION_DATAROOTDIR/conceptual.json" "$tmpdir/conceptual.json"
cp "$CONFIGS_PATH/conceptual.json" "$UNION_DATAROOTDIR/conceptual.json"

# Copying configuration files to keep as documentation
cp "$CONFIGS_PATH/milc_skeleton.json" "$expfolder"
cp "$CONFIGS_PATH/conceptual.json" "$expfolder"
cp "$CONFIGS_PATH/jacobi_MILC.workload.conf" "$expfolder"
cp "$CONFIGS_PATH/rand_node0-1d-72-jacobi_MILC.alloc.conf" "$expfolder"

# CODES config file
export CHUNK_SIZE=4096
export PATH_TO_CONNECTIONS="$CONFIGS_PATH"
export NETWORK_SURR_ON=1
export NETWORK_MODE=freeze
export APP_SURR_ON=1
export APP_DIRECTOR_MODE=every-n-nanoseconds
#export APP_DIRECTOR_MODE=every-n-gvt
export EVERY_N_GVT=500
export EVERY_NSECS=1e6
envsubst < "$bindir/tests/conf/union-milc-jacobi-workload/dfdally-72-par.conf.in" > "$expfolder/dfdally-72-par.conf"

# running simulation
cons_lookahead=200
opt_lookahead=600

export PATH_TO_CODES_BUILD="$bindir"

mkdir run-1
pushd run-1

mpirun -np $np "$PATH_TO_CODES_BUILD"/src/model-net-mpi-replay \
  --synch=3 \
  --batch=4 --gvt-interval=256 \
  --cons-lookahead=$cons_lookahead \
  --max-opt-lookahead=$opt_lookahead \
  --workload_type=conc-online \
  --lp-io-dir=lp-io-dir \
  --workload_conf_file="$expfolder"/jacobi_MILC.workload.conf \
  --alloc_file="$expfolder"/rand_node0-1d-72-jacobi_MILC.alloc.conf \
  -- "$expfolder/dfdally-72-par.conf" \
  > model-output-1.txt 2> model-output-1-error.txt

err=$?
[[ $err -ne 0 ]] && exit $err

popd

mkdir run-2
pushd run-2

mpirun -np $np "$PATH_TO_CODES_BUILD"/src/model-net-mpi-replay \
  --synch=3 \
  --batch=4 --gvt-interval=256 \
  --cons-lookahead=$cons_lookahead \
  --max-opt-lookahead=$opt_lookahead \
  --workload_type=conc-online \
  --lp-io-dir=lp-io-dir \
  --workload_conf_file="$expfolder"/jacobi_MILC.workload.conf \
  --alloc_file="$expfolder"/rand_node0-1d-72-jacobi_MILC.alloc.conf \
  -- "$expfolder/dfdally-72-par.conf" \
  > model-output-2.txt 2> model-output-2-error.txt

err=$?

popd

# Setting milc json back
mv "$tmpdir/milc_skeleton.json" "$SWM_DATAROOTDIR/milc_skeleton.json"
mv "$tmpdir/conceptual.json" "$UNION_DATAROOTDIR/conceptual.json"
rmdir "$tmpdir"

[[ $err -ne 0 ]] && exit $err

# Checking that there is actual output
grep 'Net Events Processed' run-1/model-output-1.txt
err=$?
[[ $err -ne 0 ]] && exit $err

diff <(grep 'Net Events Processed' run-1/model-output-1.txt) \
    <(grep 'Net Events Processed' run-2/model-output-2.txt)
err=$?
if [[ $err -ne 0 ]]; then
    >&2 echo "The number of net events processed does not coincide, ie," \
        "the simulation is not deterministic"
    exit $err
fi

exit 0
