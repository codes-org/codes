CODES_SOURCE_DIR="$PWD/../.."
CODES_BUILD_DIR="$PWD/../../build"
EXP_SCRIPTS="$PWD/experiments"


# Running experiments
mkdir -p results/10ms results/100ms

pushd results/10ms
bash -x "$EXP_SCRIPTS"/mpi-replay_72-node-dragonfly_synthetic1-10ms.sh \
  "$CODES_BUILD_DIR" "$CODES_SOURCE_DIR" "$EXP_SCRIPTS/conf-files/"
popd

pushd results/100ms
bash -x "$EXP_SCRIPTS"/mpi-replay_72-node-dragonfly_synthetic1-100ms.sh \
  "$CODES_BUILD_DIR" "$CODES_SOURCE_DIR" "$EXP_SCRIPTS/conf-files/"
popd


# Generating figures
mkdir results/10ms/condensed results/100ms/condensed

for exp in {10,100}; do
  for kind in {high-fidelity,hybrid,hybrid-lite}; do
    python python-scripts/delay_in_window.py \
      --latencies-dir results/${exp}ms/$kind/packet-latency-trace \
      --output results/${exp}ms/condensed/packet_latency-$kind \
      --start 0.0 --end ${exp}e6
  done
done

mkdir figures

python python-scripts/plot-packet-latency.py pads23 \
  --latencies results/10ms/condensed \
  --output figures/packet_latency-10ms

python python-scripts/port-occupancy.py pads23 \
  --experiment-folder results/10ms --output figures/port-occupancy-10ms


# Generating table
python python-scripts/generate-table.py \
  --folder-10ms results/10ms --folder-100ms results/100ms \
  > results/sumarized-table.txt
