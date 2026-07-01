# Sourced (not executed) by equivalence-run.sh inside the run dir.
#
# Generates the high-fidelity ping-pong config with an EMPTY packet-latency
# trace path, so example-ping-pong-no-logging can verify the model runs cleanly
# when that path is not given. $bindir is exported by run-test.sh.
export PACKET_SIZE=4096
export CHUNK_SIZE=4096
export PACKET_LATENCY_TRACE_PATH=
cat "$bindir/doc/example/tutorial-ping-pong.template.conf.in" | envsubst > tutorial-ping-pong.conf
