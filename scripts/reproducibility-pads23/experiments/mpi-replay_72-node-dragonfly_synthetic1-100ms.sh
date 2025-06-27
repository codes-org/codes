#!/usr/bin/bash -x

np=3

# CONFIGURATION
# exported env variables are to be used by `envsubst` below
PATH_TO_CODES_BUILD="$1"
export PATH_TO_CODES_SRC="$2"
CONF_FILE_TEMPLATES="$3"
export CHUNK_SIZE=64

# configuration file for high-fidelity codes
export BUFFER_SNAPSHOTS='"1e6", "2e6", "3e6", "4e6", "5e6", "6e6", "7e6", "8e6", "9e6", "10e6", "11e6", "12e6", "13e6", "14e6", "15e6", "16e6", "17e6", "18e6", "19e6", "20e6", "21e6", "22e6", "23e6", "24e6", "25e6", "26e6", "27e6", "28e6", "29e6", "30e6", "31e6", "32e6", "33e6", "34e6", "35e6", "36e6", "37e6", "38e6", "39e6", "40e6", "41e6", "42e6", "43e6", "44e6", "45e6", "46e6", "47e6", "48e6", "49e6", "50e6", "51e6", "52e6", "53e6", "54e6", "55e6", "56e6", "57e6", "58e6", "59e6", "60e6", "61e6", "62e6", "63e6", "64e6", "65e6", "66e6", "67e6", "68e6", "69e6", "70e6", "71e6", "72e6", "73e6", "74e6", "75e6", "76e6", "77e6", "78e6", "79e6", "80e6", "81e6", "82e6", "83e6", "84e6", "85e6", "86e6", "87e6", "88e6", "89e6", "90e6", "91e6", "92e6", "93e6", "94e6", "95e6", "96e6", "97e6", "98e6", "99e6", "99.9e6"'
export PACKET_LATENCY_PATH='high-fidelity/packet-latency-trace'
cat "$CONF_FILE_TEMPLATES"/terminal-dragonfly-72-v5.conf.in | envsubst > terminal-dragonfly-72.conf

# configuration file for hybrid-lite and hybrid codes
#export BUFFER_SNAPSHOTS='"1e6", "2e6", "3e6", "4e6", "5e6", "6e6", "7e6", "8e6", "9e6", "10e6", "11e6", "12e6", "13e6", "14e6", "15e6", "16e6", "17e6", "18e6", "19e6", "91e6", "92e6", "93e6", "94e6", "95e6", "96e6", "97e6", "98e6", "99e6", "99.9e6"'
export IGNORE_UNTIL=10e6
export SWITCH_TIMESTAMPS='"20e6", "90e6"'
export NETWORK_TREATMENT=freeze
export PACKET_LATENCY_PATH='hybrid/packet-latency-trace'
cat "$CONF_FILE_TEMPLATES"/terminal-dragonfly-72-surrogate-v5.conf.in | envsubst > terminal-dragonfly-72-hybrid.conf

# configuration file for hybrid-lite
export NETWORK_TREATMENT=nothing
export PACKET_LATENCY_PATH='hybrid-lite/packet-latency-trace'
cat "$CONF_FILE_TEMPLATES"/terminal-dragonfly-72-surrogate-v5.conf.in | envsubst > terminal-dragonfly-72-hybrid-lite.conf

# yet more configuration files
cp "$CONF_FILE_TEMPLATES"/72-dragonfly-full.alloc .

# creating dirs
mkdir -p high-fidelity hybrid hybrid-lite

# RUNNING SIMULATION
period=480

# Creating custom/individual configuration files
work_alloc_file="72-dragonfly-period=${period}.synthetic.conf"
cat > "$work_alloc_file" <<END
72 synthetic1 0 ${period}
END

lookahead=200
# Note: --extramem is only required for simulations with a very short period as they generate many, many, many events (and they keep on accumulating)
extramem=10000

# RUNNING CODES
# Note: cons-lookahead is used as the offset to process packet latency events (the event is scheduled back to the sender, thus a smaller offset will force GVT more often; too large of an offset and the predictor will be behind significantly)
mpirun -np $np \
  "$PATH_TO_CODES_BUILD"/src/model-net-mpi-replay --synch=3 \
     --workload_type=online --workload_conf_file="$work_alloc_file" \
     --cons-lookahead=$lookahead --max-opt-lookahead=${lookahead%.*} --batch=4 --gvt-interval=256 \
     --alloc_file=72-dragonfly-full.alloc --end='100.001e6' \
     --extramem=$extramem --lp-io-dir=high-fidelity/codes-output \
     -- terminal-dragonfly-72.conf > high-fidelity/model-result.txt 2> high-fidelity/model-result.stderr.txt

# RUNNING CODES with SURROGATE MODEL
mpirun -np $np \
  "$PATH_TO_CODES_BUILD"/src/model-net-mpi-replay --synch=3 \
     --workload_type=online --workload_conf_file="$work_alloc_file" \
     --cons-lookahead=$lookahead --max-opt-lookahead=${lookahead%.*} --batch=4 --gvt-interval=256 \
     --alloc_file=72-dragonfly-full.alloc --end='100.001e6' \
     --extramem=$extramem --lp-io-dir=hybrid/codes-output \
     -- terminal-dragonfly-72-hybrid.conf > hybrid/model-result.txt 2> hybrid/model-result.stderr.txt

# SAME AS BEFORE BUT NONFREEZING
mpirun -np $np \
  "$PATH_TO_CODES_BUILD"/src/model-net-mpi-replay --synch=3 \
     --workload_type=online --workload_conf_file="$work_alloc_file" \
     --cons-lookahead=$lookahead --max-opt-lookahead=${lookahead%.*} --batch=4 --gvt-interval=256 \
     --alloc_file=72-dragonfly-full.alloc --end='100.001e6' \
     --extramem=$extramem --lp-io-dir=hybrid-lite/codes-output \
     -- terminal-dragonfly-72-hybrid-lite.conf > hybrid-lite/model-result.txt 2> hybrid-lite/model-result.stderr.txt
