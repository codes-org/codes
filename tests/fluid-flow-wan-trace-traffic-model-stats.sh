#!/bin/bash
# Test: ROSS instrumentation model-level stats for the fluid-flow WAN model.
#
# Runs model-net-fluid-flow-wan-trace-traffic with the ROSS instrumentation layer
# enabled and checks that the binary stats output is produced, is parseable by the
# reference parser (scripts/parse-ross-model-stats.py), carries records for both LP
# types, and is internally consistent (per-port egress sums to the aggregate egress,
# shared-buffer occupancy never exceeds capacity, deltas are non-negative).
#
# It also checks that turning instrumentation on does not change the model's own
# committed event count -- the instrumentation callbacks must not touch scheduling
# or RNG use.
#
# Sequential runs collect virtual-time samples only (GVT-mode sampling needs a
# protocol that computes intermediate GVTs); optimistic runs collect everything,
# which also exercises the rollback path of the virtual-time sample callbacks.
#
# Format spec: doc/model-stats-binary-format.md
set -euo pipefail

synch="${1:?synch mode required: 1 or 3}"
np="${2:?MPI rank count required}"
case_name="${3:-fluid-flow-wan-trace-traffic-model-stats-synch${synch}}"
mpi_exec="${4:-mpirun}"
mpi_np_flag="${5:--np}"

if [[ -z "${bindir:-}" || -z "${srcdir:-}" ]]; then
    echo "bindir/srcdir are not set; run through tests/run-test.sh"
    exit 1
fi

binary="$bindir/src/model-net-fluid-flow-wan-trace-traffic"
base_yaml="$bindir/doc/example/fluid-flow-wan-trace-traffic.yaml"
topology="$bindir/doc/example/fluid-flow-wan-topology.yaml"
trace="$bindir/doc/example/fluid-flow-wan-trace-traffic.csv"
[[ -f "$topology" ]] || topology="$srcdir/doc/example/fluid-flow-wan-topology.yaml"
[[ -f "$trace" ]] || trace="$srcdir/doc/example/fluid-flow-wan-trace-traffic.csv"
parser="$srcdir/scripts/parse-ross-model-stats.py"

[[ -x "$binary" ]] || { echo "missing executable: $binary"; exit 1; }
[[ -f "$base_yaml" ]] || { echo "missing generated config: $base_yaml"; exit 1; }
[[ -f "$topology" ]] || { echo "missing topology file"; exit 1; }
[[ -f "$trace" ]] || { echo "missing trace file"; exit 1; }
[[ -f "$parser" ]] || { echo "missing reference parser: $parser"; exit 1; }

rm -rf "$case_name"
mkdir -p "$case_name/logs"
cp "$topology" "$case_name/fluid-flow-wan-topology.yaml"
cp "$trace" "$case_name/fluid-flow-wan-trace-traffic.csv"
cp "$base_yaml" "$case_name/fluid-flow-wan-trace-traffic.yaml"

# The example config runs num_send_intervals + num_drain_intervals = 16 intervals of
# 1 s, i.e. it finishes at 1.6e10 ns. --vt-samp-end is REQUIRED: it defaults to
# g_tw_ts_end, which the model sets to a year of virtual time, so the analysis LPs
# would otherwise keep scheduling sample events long after the traffic ends.
stats_flags=(--vt-interval=1e9 --vt-samp-end=1.7e10 --stats-path=model-stats-out)
if [[ "$synch" == "1" ]]; then
    stats_flags=(--model-stats=3 "${stats_flags[@]}")
else
    stats_flags=(--model-stats=4 --event-trace=1 --num-gvt=4 --rt-interval=1 "${stats_flags[@]}")
fi

# baseline: same run without any instrumentation flags
if ! (
    cd "$case_name"
    "$mpi_exec" "$mpi_np_flag" "$np" "$binary" --sync="$synch" \
        -- fluid-flow-wan-trace-traffic.yaml \
        > baseline-output.txt 2> baseline-output-error.txt
); then
    echo "fluid-flow-wan baseline run failed"
    cat "$case_name/baseline-output.txt" || true
    cat "$case_name/baseline-output-error.txt" || true
    exit 1
fi

if ! (
    cd "$case_name"
    "$mpi_exec" "$mpi_np_flag" "$np" "$binary" --sync="$synch" "${stats_flags[@]}" \
        -- fluid-flow-wan-trace-traffic.yaml \
        > model-output.txt 2> model-output-error.txt
); then
    echo "fluid-flow-wan instrumented run failed"
    cat "$case_name/model-output.txt" || true
    cat "$case_name/model-output-error.txt" || true
    exit 1
fi

out="$case_name/model-output.txt"
grep "fluid-flow-wan config: workload=trace-traffic" "$out"
grep "Net Events Processed" "$out"

# Instrumentation must not perturb the simulation. Analysis LPs add their own event
# stream, so the instrumented run reports several Net Events Processed values; the
# model's own count has to be one of them.
baseline_events="$(awk '$1 == "Net" && $2 == "Events" && $3 == "Processed" {print $NF}' \
    "$case_name/baseline-output.txt")"
if [[ ! "$baseline_events" =~ ^[0-9]+$ ]]; then
    echo "could not read a single Net Events Processed value from the baseline run" >&2
    exit 1
fi
if ! awk -v want="$baseline_events" \
    '$1 == "Net" && $2 == "Events" && $3 == "Processed" && $NF == want { found = 1 }
     END { exit !found }' "$out"; then
    echo "instrumented run did not process $baseline_events model events" >&2
    exit 1
fi

if [[ ! -s "$case_name/model-stats-out/ross-stats-analysis-lps.bin" ]]; then
    echo "model-stats-out/ross-stats-analysis-lps.bin missing or empty" >&2
    exit 1
fi

stats_files=("$case_name/model-stats-out/ross-stats-analysis-lps.bin")
if [[ "$synch" != "1" ]]; then
    for f in ross-stats-model.bin ross-stats-evtrace.bin; do
        if [[ ! -s "$case_name/model-stats-out/$f" ]]; then
            echo "model-stats-out/$f missing or empty" >&2
            exit 1
        fi
    done
    stats_files+=("$case_name/model-stats-out/ross-stats-model.bin")
fi

# Parse with the reference parser (soft-skip if python3 is unavailable; python3 is
# not a hard test dependency of this repo)
if ! command -v python3 > /dev/null ; then
    echo "python3 not found; skipping reference-parser check"
    exit 0
fi

if ! python3 "$parser" "${stats_files[@]}" \
        --csv-prefix "$case_name/model-stats" 2> "$case_name/parser-summary.txt"; then
    cat "$case_name/parser-summary.txt" >&2
    echo "reference parser failed on the fluid-flow-wan stats output" >&2
    exit 1
fi
cat "$case_name/parser-summary.txt"

grep 'unknown payloads: 0' "$case_name/parser-summary.txt" > /dev/null || {
    echo "parser hit unknown payload sizes (payload/format drift?)" >&2
    exit 1
}

# at least one record of each LP type (csv has a header line)
for lptype in ffw-terminal ffw-switch ; do
    csv="$case_name/model-stats-${lptype}.csv"
    [[ -s "$csv" ]] || { echo "no ${lptype} records decoded" >&2; exit 1; }
    nrec=$(( $(wc -l < "$csv") - 1 ))
    if [[ $nrec -lt 1 ]] ; then
        echo "no ${lptype} records decoded" >&2
        exit 1
    fi
done

# Content sanity: per-port egress must sum to the aggregate egress, shared-buffer
# occupancy must stay within capacity, and delta fields must be non-negative.
# Non-negativity is only asserted for virtual-time rows: those roll back exactly (the
# sample callback stashes its pre-sample snapshot in the payload), whereas a GVT or
# real-time sample can be taken ahead of the commit point, so a later rollback can
# make the next GVT/RT delta of the same LP briefly negative -- see
# doc/model-stats-binary-format.md.
python3 - "$case_name/model-stats-ffw-switch.csv" "$case_name/model-stats-ffw-terminal.csv" <<'PYEOF'
import csv
import sys

switch_csv, terminal_csv = sys.argv[1], sys.argv[2]
errors = []

with open(switch_csv, newline="") as f:
    switch_rows = list(csv.DictReader(f))
ports = sum(1 for k in switch_rows[0] if k.startswith("port_sent_mbit_p"))
if ports < 1:
    errors.append("switch records carry no per-port arrays")
totals = {}
for row in switch_rows:
    sent = float(row["sent_mbit"])
    per_port = sum(float(row[f"port_sent_mbit_p{i}"]) for i in range(ports))
    if abs(sent - per_port) > 1e-6 * max(1.0, abs(sent)):
        errors.append(f"switch {row['switch_id']} @{row['ts']}: "
                      f"sent_mbit {sent} != per-port sum {per_port}")
    occupancy = float(row["shared_buffer_occupancy_mbit"])
    capacity = float(row["shared_buffer_capacity_mbit"])
    if capacity <= 0.0:
        errors.append(f"switch {row['switch_id']}: non-positive buffer capacity")
    if occupancy > capacity + 1e-6:
        errors.append(f"switch {row['switch_id']} @{row['ts']}: "
                      f"occupancy {occupancy} exceeds capacity {capacity}")
    if int(row["num_ports"]) < 1 or int(row["num_ports"]) > ports:
        errors.append(f"switch {row['switch_id']}: implausible num_ports {row['num_ports']}")
    if row["stats_type"] == "vt":
        for field in ("sent_mbit", "delivered_local_mbit", "dropped_mbit"):
            if float(row[field]) < -1e-9:
                errors.append(f"switch {row['switch_id']} @{row['ts']}: "
                              f"negative delta {field}={row[field]}")
    key = (row["stats_type"], row["switch_id"])
    totals[key] = totals.get(key, 0.0) + sent

with open(terminal_csv, newline="") as f:
    terminal_rows = list(csv.DictReader(f))
generated = {}
for row in terminal_rows:
    if row["stats_type"] == "vt":
        for field in ("generated_mbit", "sent_to_switch_mbit", "received_mbit",
                      "pause_time_ns"):
            if float(row[field]) < -1e-9:
                errors.append(f"terminal {row['terminal_id']} @{row['ts']}: "
                              f"negative delta {field}={row[field]}")
    if float(row["source_backlog_mbit"]) < -1e-9:
        errors.append(f"terminal {row['terminal_id']} @{row['ts']}: negative backlog")
    key = (row["stats_type"], row["terminal_id"])
    generated[key] = generated.get(key, 0.0) + float(row["generated_mbit"])

# the trace offers 168 Gbit in total (see doc/example/fluid-flow-wan-trace-traffic.csv);
# require the virtual-time samples to have observed traffic on at least one terminal
# and at least one switch, so an all-zero payload cannot pass this test
if not any(v > 0.0 for (mode, _), v in generated.items() if mode == "vt"):
    errors.append("no virtual-time terminal sample recorded any generated volume")
if not any(v > 0.0 for (mode, _), v in totals.items() if mode == "vt"):
    errors.append("no virtual-time switch sample recorded any egress volume")

if errors:
    for e in errors[:20]:
        print(f"error: {e}", file=sys.stderr)
    sys.exit(1)
print(f"content checks passed: {len(switch_rows)} switch rows, "
      f"{len(terminal_rows)} terminal rows, {ports} ports per switch record")
PYEOF
