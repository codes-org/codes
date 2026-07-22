#!/bin/bash
set -euo pipefail

case_root="${1:-fluid-flow-wan-trace-traffic-seq-opt-equivalence}"
mpi_exec="${2:-mpirun}"
mpi_np_flag="${3:--np}"

if [[ -z "${bindir:-}" || -z "${srcdir:-}" ]]; then
    echo "bindir/srcdir are not set; run through tests/run-test.sh"
    exit 1
fi

smoke_test="$srcdir/tests/fluid-flow-wan-trace-traffic-ci.sh"
[[ -x "$smoke_test" ]] || { echo "missing executable test script: $smoke_test"; exit 1; }

seq_case="${case_root}-sequential"
opt_case="${case_root}-optimistic"
comparison_dir="${case_root}-comparison"

rm -rf "$seq_case" "$opt_case" "$comparison_dir"
mkdir -p "$comparison_dir"

"$smoke_test" 1 1 "$seq_case" "$mpi_exec" "$mpi_np_flag"
"$smoke_test" 3 2 "$opt_case" "$mpi_exec" "$mpi_np_flag"

seq_output="$seq_case/model-output.txt"
opt_output="$opt_case/model-output.txt"

extract_net_events() {
    local output="$1"
    local count value
    count="$(awk '$1 == "Net" && $2 == "Events" && $3 == "Processed" {count++} END {print count + 0}' "$output")"
    value="$(awk '$1 == "Net" && $2 == "Events" && $3 == "Processed" {print $NF}' "$output")"
    if [[ "$count" != "1" || ! "$value" =~ ^[0-9]+$ ]]; then
        echo "expected exactly one numeric Net Events Processed value in $output" >&2
        return 1
    fi
    printf '%s\n' "$value"
}

canonicalize_csv() {
    local input="$1" output="$2" header row_count
    [[ -s "$input" ]] || { echo "missing or empty committed CSV log: $input"; return 1; }
    IFS= read -r header < "$input" || { echo "could not read CSV header from $input"; return 1; }
    header="${header%$'\r'}"
    {
        printf '%s\n' "$header"
        tail -n +2 "$input" | tr -d '\r' | LC_ALL=C sort
    } > "$output"
    row_count="$(wc -l < "$output")"
    (( row_count >= 2 )) || { echo "committed CSV log has no data rows: $input"; return 1; }
}

for csv in terminal-events.csv switch-events.csv flowlet-events.csv; do
    canonicalize_csv "$seq_case/logs/$csv" "$comparison_dir/sequential-$csv"
    canonicalize_csv "$opt_case/logs/$csv" "$comparison_dir/optimistic-$csv"
    if ! diff -u "$comparison_dir/sequential-$csv" "$comparison_dir/optimistic-$csv"; then
        echo "sequential and optimistic committed CSV logs differ: $csv"
        exit 1
    fi
done

seq_net_events="$(extract_net_events "$seq_output")"
opt_net_events="$(extract_net_events "$opt_output")"
[[ "$seq_net_events" == "$opt_net_events" ]] || {
    echo "Net Events Processed differs: sequential=$seq_net_events optimistic=$opt_net_events"
    exit 1
}

rolled_back="$(awk '$1 == "Events" && $2 == "Rolled" && $3 == "Back" {print $NF}' "$opt_output" | tail -n 1)"
rollbacks="$(awk '$1 == "Total" && $2 == "Roll" && $3 == "Backs" {print $NF}' "$opt_output" | tail -n 1)"
[[ "$rolled_back" =~ ^[0-9]+$ ]] && (( rolled_back > 0 )) || {
    echo "optimistic run did not exercise event rollback: Events Rolled Back=${rolled_back:-missing}"
    exit 1
}
[[ "$rollbacks" =~ ^[0-9]+$ ]] && (( rollbacks > 0 )) || {
    echo "optimistic run did not exercise rollback handling: Total Roll Backs=${rollbacks:-missing}"
    exit 1
}

echo "fluid-flow-wan trace-traffic sequential/optimistic committed CSV logs match"
echo "Net Events Processed=$seq_net_events"
echo "Events Rolled Back=$rolled_back"
echo "Total Roll Backs=$rollbacks"
