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
export NETWORK_SURR_ON=0
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
  > model-output.txt 2> model-output-error.txt

err=$?

# Setting milc json back
mv "$tmpdir/milc_skeleton.json" "$SWM_DATAROOTDIR/milc_skeleton.json"
mv "$tmpdir/conceptual.json" "$UNION_DATAROOTDIR/conceptual.json"
rmdir "$tmpdir"

[[ $err -ne 0 ]] && exit $err

# Checking that there is actual output
grep 'Net Events Processed' model-output.txt
err=$?
[[ $err -ne 0 ]] && exit $err

# Checking both milc and jacobi ran
grep 'MILC: Iteration 119/120' model-output.txt
err=$?
[[ $err -ne 0 ]] && exit $err

grep 'Jacobi3D: Completed 39 iterations' model-output.txt
err=$?
[[ $err -ne 0 ]] && exit $err

grep 'App 0: All non-synthetic workloads have completed' model-output.txt
err=$?
[[ $err -ne 0 ]] && exit $err

# it transitioned into surrogacy
grep -e 'application iteration surrogate mode at GVT [0-9]* time' model-output.txt
err=$?
[[ $err -ne 0 ]] && exit $err

# it transitioned back to high-fidelity
grep -e 'application iteration mode at GVT [0-9]* time' model-output.txt
err=$?
[[ $err -ne 0 ]] && exit $err

exit 0
