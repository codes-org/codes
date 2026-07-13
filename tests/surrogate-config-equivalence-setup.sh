# Sourced (not executed) by equivalence-run.sh inside each run dir.
#
# Generates BOTH the surrogate ping-pong config formats -- the legacy .conf and its
# YAML twin -- from their shared templates via envsubst, so example-ping-pong-
# surrogate-config-equivalence can run one against the other and confirm they drive
# the model to identical committed work. The same env is exported for both, so the
# two configs differ only in format. A single PACKET_LATENCY_TRACE_PATH is fine
# because each run executes in its own subdir, so the two runs' trace output cannot
# collide. $bindir is exported by run-test.sh.
export PACKET_SIZE=1024
export CHUNK_SIZE=1024
export NETWORK_TREATMENT=freeze
export PREDICTOR_TYPE=average
export PACKET_LATENCY_TRACE_PATH=packet-latency-surrogate/
export IGNORE_UNTIL=0.0
export SWITCH_TIMESTAMPS='".08e6", ".1e6", ".2e6", ".6e6", ".7e6", ".9e6", "1.0e6", "1.3e6", "1.6e6", "1.7e6", "1.9e6", "2.0e6", "2.3e6", "2.6e6", "2.7e6", "2.9e6", "3.0e6", "3.3e6", "3.6e6", "3.7e6", "3.9e6", "4.0e6", "4.3e6", "4.6e6", "4.7e6", "4.9e6", "5.0e6", "9.8e6"'
cat "$bindir/doc/example/tutorial-ping-pong-surrogate.template.conf.in" | envsubst > tutorial-ping-pong-surrogate.conf
cat "$bindir/doc/example/tutorial-ping-pong-surrogate.template.yaml.in" | envsubst > tutorial-ping-pong-surrogate.yaml
