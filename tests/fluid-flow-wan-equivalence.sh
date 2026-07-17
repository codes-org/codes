#!/bin/bash
set -euo pipefail

case_root="${1:-fluid-flow-wan-seq-opt-equivalence}"
mpi_exec="${2:-mpirun}"
mpi_np_flag="${3:--np}"

if [[ -z "${bindir:-}" || -z "${srcdir:-}" ]]; then
    echo "bindir/srcdir are not set; run through tests/run-test.sh"
    exit 1
fi

smoke_test="$srcdir/tests/fluid-flow-wan-ci.sh"
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

extract_committed_state() {
    local output="$1"
    local summary="$2"

    local switch_count
    local terminal_count
    local net_event_count
    switch_count="$(grep -c '^fluid-flow-wan gid=' "$output" || true)"
    terminal_count="$(grep -c '^fluid-flow-wan-terminal gid=' "$output" || true)"
    net_event_count="$(grep -c 'Net Events Processed' "$output" || true)"

    if (( switch_count == 0 || terminal_count == 0 || net_event_count != 1 )); then
        echo "incomplete committed-state output in $output"
        echo "switch summaries: $switch_count"
        echo "terminal summaries: $terminal_count"
        echo "Net Events Processed lines: $net_event_count"
        exit 1
    fi

    {
        grep -E '^fluid-flow-wan(-terminal)? gid=' "$output" | LC_ALL=C sort
        awk '/Net Events Processed/ {print "Net Events Processed=" $NF}' "$output"
    } > "$summary"
}

seq_summary="$comparison_dir/sequential-summary.txt"
opt_summary="$comparison_dir/optimistic-summary.txt"
extract_committed_state "$seq_output" "$seq_summary"
extract_committed_state "$opt_output" "$opt_summary"

if ! diff -u "$seq_summary" "$opt_summary"; then
    echo "sequential and optimistic committed fluid-flow-wan states differ"
    exit 1
fi

rolled_back="$(awk '$1 == "Events" && $2 == "Rolled" && $3 == "Back" {print $NF}' "$opt_output" | tail -n 1)"
rollbacks="$(awk '$1 == "Total" && $2 == "Roll" && $3 == "Backs" {print $NF}' "$opt_output" | tail -n 1)"

if [[ ! "$rolled_back" =~ ^[0-9]+$ ]] || (( rolled_back == 0 )); then
    echo "optimistic run did not exercise event rollback: Events Rolled Back=${rolled_back:-missing}"
    exit 1
fi

if [[ ! "$rollbacks" =~ ^[0-9]+$ ]] || (( rollbacks == 0 )); then
    echo "optimistic run did not exercise rollback handling: Total Roll Backs=${rollbacks:-missing}"
    exit 1
fi

echo "fluid-flow-wan sequential/optimistic committed state matches"
echo "Events Rolled Back=$rolled_back"
echo "Total Roll Backs=$rollbacks"
