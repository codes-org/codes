#!/usr/bin/bash -x

np=3

# CONFIGURATION
# exported env variables are to be used by `envsubst` below
PATH_TO_CODES_BUILD="$1"
export PATH_TO_CODES_SRC="$2"
CONF_FILE_TEMPLATES="$3"
export CHUNK_SIZE=64

# configuration file for high-fidelity codes
export BUFFER_SNAPSHOTS='"100e3", "200e3", "300e3", "400e3", "500e3", "600e3", "700e3", "800e3", "900e3", "1e6", "1.1e6", "1.2e6", "1.3e6", "1.4e6", "1.5e6", "1.6e6", "1.7e6", "1.8e6", "1.9e6", "2e6", "2.1e6", "2.2e6", "2.3e6", "2.4e6", "2.5e6", "2.6e6", "2.7e6", "2.8e6", "2.9e6", "3e6", "3.1e6", "3.2e6", "3.3e6", "3.4e6", "3.5e6", "3.6e6", "3.7e6", "3.8e6", "3.9e6", "4e6", "4.1e6", "4.2e6", "4.3e6", "4.4e6", "4.5e6", "4.6e6", "4.7e6", "4.8e6", "4.9e6", "5e6", "5.1e6", "5.2e6", "5.3e6", "5.4e6", "5.5e6", "5.6e6", "5.7e6", "5.8e6", "5.9e6", "6e6", "6.1e6", "6.2e6", "6.3e6", "6.4e6", "6.5e6", "6.6e6", "6.7e6", "6.8e6", "6.9e6", "7e6", "7.1e6", "7.2e6", "7.3e6", "7.4e6", "7.5e6", "7.6e6", "7.7e6", "7.8e6", "7.9e6", "8e6", "8.1e6", "8.2e6", "8.3e6", "8.4e6", "8.5e6", "8.6e6", "8.7e6", "8.8e6", "8.9e6", "9e6", "9.1e6", "9.2e6", "9.3e6", "9.4e6", "9.5e6", "9.6e6", "9.7e6", "9.8e6", "9.9e6", "9.990e6"'
export PACKET_LATENCY_PATH='high-fidelity/packet-latency-trace'
cat "$CONF_FILE_TEMPLATES"/terminal-dragonfly-72-v5.conf.in | envsubst > terminal-dragonfly-72.conf

# configuration file for hybrid-lite and hybrid codes
export IGNORE_UNTIL=2000e3
export SWITCH_TIMESTAMPS='"3000e3", "8000e3"'
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
     --alloc_file=72-dragonfly-full.alloc --end=10000.01e3 \
     --extramem=$extramem --lp-io-dir=high-fidelity/codes-output \
     -- terminal-dragonfly-72.conf > high-fidelity/model-result.txt 2> high-fidelity/model-result.stderr.txt

# RUNNING CODES with SURROGATE MODEL
mpirun -np $np \
  "$PATH_TO_CODES_BUILD"/src/model-net-mpi-replay --synch=3 \
     --workload_type=online --workload_conf_file="$work_alloc_file" \
     --cons-lookahead=$lookahead --max-opt-lookahead=${lookahead%.*} --batch=4 --gvt-interval=256 \
     --alloc_file=72-dragonfly-full.alloc --end=10000.01e3 \
     --extramem=$extramem --lp-io-dir=hybrid/codes-output \
     -- terminal-dragonfly-72-hybrid.conf > hybrid/model-result.txt 2> hybrid/model-result.stderr.txt

# SAME AS BEFORE BUT NONFREEZING
mpirun -np $np \
  "$PATH_TO_CODES_BUILD"/src/model-net-mpi-replay --synch=3 \
     --workload_type=online --workload_conf_file="$work_alloc_file" \
     --cons-lookahead=$lookahead --max-opt-lookahead=${lookahead%.*} --batch=4 --gvt-interval=256 \
     --alloc_file=72-dragonfly-full.alloc --end=10000.01e3 \
     --extramem=$extramem --lp-io-dir=hybrid-lite/codes-output \
     -- terminal-dragonfly-72-hybrid-lite.conf > hybrid-lite/model-result.txt 2> hybrid-lite/model-result.stderr.txt
