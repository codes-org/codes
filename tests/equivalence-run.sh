#!/bin/bash
#
# Generic equivalence runner for CODES tests.
#
# Runs two (or more) commands, greps a marker line from each run's stdout, and
# asserts the marked lines are identical across every run. Used for both:
#   - reproducibility  (same command run N times -> result must not vary), and
#   - seq-vs-optimistic (same args, differing --sync -> committed work must match).
#
# It is driven entirely from CMake by codes_add_equivalence_test(); it is not
# meant to be hand-edited per test. Each run executes in its own run-N/ subdir
# so that runs writing to fixed relative output paths don't collide.
#
# Usage:
#   equivalence-run.sh --marker "Net Events Processed" [--setup <script>] \
#       @@ <cmd for run 1...> @@ <cmd for run 2...> [ @@ <run 3...> ]
#
# '@@' separates runs. A run's own '--' (ROSS opts | .conf) stays intact within
# its segment. --setup names a script that is *sourced* before any run, so it
# may export environment variables and/or generate config files in the sandbox.

set -u

marker="Net Events Processed"
setup=""

# Leading options, up to the first run separator.
while [[ $# -gt 0 && "$1" != "@@" ]]; do
    case "$1" in
        --marker) marker="$2"; shift 2 ;;
        --setup)  setup="$2";  shift 2 ;;
        *) echo "equivalence-run.sh: unknown option '$1'" >&2; exit 2 ;;
    esac
done
[[ "${1:-}" == "@@" ]] && shift

# Sourced (not executed) so exported vars and generated confs are visible to runs.
if [[ -n "$setup" ]]; then
    # shellcheck disable=SC1090
    source "$setup"
fi

run_idx=0
cmd=()
outputs=()

run_one() {
    run_idx=$((run_idx + 1))
    local dir="run-${run_idx}"
    mkdir -p "$dir"
    local out="${dir}/model-output.txt"
    local errf="${dir}/model-output-error.txt"

    echo "+ run ${run_idx}: ${cmd[*]}" >&2
    ( cd "$dir" && "${cmd[@]}" ) > "$out" 2> "$errf"
    local rc=$?
    if [[ $rc -ne 0 ]]; then
        echo "equivalence-run.sh: run ${run_idx} exited with $rc" >&2
        cat "$errf" >&2
        exit $rc
    fi
    if ! grep -q "$marker" "$out"; then
        echo "equivalence-run.sh: run ${run_idx} produced no '$marker' line" \
             "(did the simulation produce output?)" >&2
        exit 1
    fi
    outputs+=("$out")
    cmd=()
}

for tok in "$@"; do
    if [[ "$tok" == "@@" ]]; then
        run_one
    else
        cmd+=("$tok")
    fi
done
[[ ${#cmd[@]} -gt 0 ]] && run_one

if [[ ${#outputs[@]} -lt 2 ]]; then
    echo "equivalence-run.sh: need at least 2 runs to compare, got ${#outputs[@]}" >&2
    exit 2
fi

# Compare every run's marker line against the first.
ref="${outputs[0]}"
status=0
for out in "${outputs[@]:1}"; do
    if ! diff <(grep "$marker" "$ref") <(grep "$marker" "$out") >&2; then
        echo "equivalence-run.sh: MISMATCH on '$marker' between ${ref} and ${out}" \
             "-- the runs are not equivalent" >&2
        status=1
    fi
done
exit $status
