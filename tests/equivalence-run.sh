#!/bin/bash
#
# Generic staged runner for CODES tests.
#
# Runs one or more commands, each in its own run-N/ subdir, and applies a set of
# checks. Drives both single-run smoke tests and multi-run equivalence tests:
#   - reproducibility   (same command run N times -> result must not vary),
#   - seq-vs-optimistic  (same args, differing --sync -> committed work matches),
#   - single-run smoke   (one run; pass on clean exit + marker present).
#
# It is driven from CMake by codes_add_run_test() / codes_add_equivalence_test();
# it is not meant to be hand-edited per test.
#
# Usage:
#   equivalence-run.sh [--marker <line>] [--require <line>]... [--setup <script>] \
#       [--lp-io <dir>] @@ <cmd for run 1...> [ @@ <cmd for run 2...> ... ]
#
# '@@' separates runs; a run's own '--' (ROSS opts | .conf) stays intact within
# its segment. Each run executes in run-N/ so runs writing to fixed relative
# output paths don't collide. --setup names a script that is *sourced inside each
# run's subdir* before that run, so it may export environment variables and/or
# generate config files local to the run (referenced by bare name in the cmd).
#
# Checks:
#   --marker <line>   the line must appear in every run's output; and, when there
#                     are 2+ runs, the marker lines are diffed across runs (they
#                     must be identical). Omit to skip the marker check/diff.
#   --require <line>  the line must appear in every run's output (presence only,
#                     never diffed). May be given multiple times.
#   --lp-io <dir>     each run wrote per-LP output to run-N/<dir> (via the model's
#                     --lp-io-dir); compare the identifier-file set and a sorted
#                     diff of each file across runs. The per-LP config-equivalence
#                     check (old .conf vs new .yaml). Needs 2+ runs.

set -u

marker=""
setup=""
lpio=""
requires=()

# Leading options, up to the first run separator.
while [[ $# -gt 0 && "$1" != "@@" ]]; do
    case "$1" in
        --marker)  marker="$2";        shift 2 ;;
        --require) requires+=("$2");    shift 2 ;;
        --setup)   setup="$2";          shift 2 ;;
        --lp-io)   lpio="$2";           shift 2 ;;
        *) echo "equivalence-run.sh: unknown option '$1'" >&2; exit 2 ;;
    esac
done
[[ "${1:-}" == "@@" ]] && shift

run_idx=0
cmd=()
outputs=()

# Show what a run actually printed: the tail of each stream file given. On
# failure this is called with both streams (tw_error() and most model error
# paths write to *stdout*; stderr usually has only MPI_Abort boilerplate); on
# success with just stdout, so even a passing test's log shows the end-of-run
# stats and a reader can confirm the simulation actually ran.
dump_run_output() {
    local f
    for f in "$@"; do
        [[ -s "$f" ]] || continue
        echo "---- tail of ${f} ----" >&2
        # 80 lines reaches back past ROSS's end-of-run stats block, so the
        # 'Net Events Processed' marker line is visible in a passing run.
        tail -n 80 "$f" >&2
        echo "---- end ${f} ----" >&2
    done
}

run_one() {
    run_idx=$((run_idx + 1))
    local dir="run-${run_idx}"
    mkdir -p "$dir"
    local out="${dir}/model-output.txt"
    local errf="${dir}/model-output-error.txt"

    echo "+ run ${run_idx}: ${cmd[*]}" >&2
    # setup is sourced *inside* the run dir so generated confs / trace output are
    # local to this run (isolating otherwise-colliding fixed relative paths).
    (
        cd "$dir" || exit 1
        if [[ -n "$setup" ]]; then
            # shellcheck disable=SC1090
            source "$setup"
        fi
        "${cmd[@]}"
    ) > "$out" 2> "$errf"
    local rc=$?
    if [[ $rc -ne 0 ]]; then
        echo "equivalence-run.sh: run ${run_idx} exited with $rc" >&2
        dump_run_output "$out" "$errf"
        exit $rc
    fi
    if [[ -n "$marker" ]] && ! grep -q "$marker" "$out"; then
        echo "equivalence-run.sh: run ${run_idx} produced no '$marker' line" \
             "(did the simulation produce output?)" >&2
        dump_run_output "$out" "$errf"
        exit 1
    fi
    local req
    for req in "${requires[@]:-}"; do
        [[ -z "$req" ]] && continue
        if ! grep -q "$req" "$out"; then
            echo "equivalence-run.sh: run ${run_idx} missing required line '$req'" >&2
            dump_run_output "$out" "$errf"
            exit 1
        fi
    done
    dump_run_output "$out"
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

# Cross-run comparisons need >= 2 runs; a single run is a smoke test whose
# per-run checks already ran above.
if [[ ${#outputs[@]} -lt 2 ]]; then
    exit 0
fi

n=${#outputs[@]}
status=0

# Marker comparison: the marked line must be identical across all runs.
if [[ -n "$marker" ]]; then
    ref="${outputs[0]}"
    for out in "${outputs[@]:1}"; do
        if ! diff <(grep "$marker" "$ref") <(grep "$marker" "$out") >&2; then
            echo "equivalence-run.sh: MISMATCH on '$marker' between ${ref} and ${out}" \
                 "-- the runs are not equivalent" >&2
            status=1
        fi
    done
fi

# lp-io comparison: each run wrote per-LP output to run-N/<lpio>. Compare the
# set of identifier files and a *sorted* diff of each (per-LP line order is not
# significant). Two equivalent configs produce identical lp-io output.
if [[ -n "$lpio" ]]; then
    refdir="run-1/$lpio"
    if [[ ! -d "$refdir" ]]; then
        echo "equivalence-run.sh: lp-io dir '$refdir' missing (did the run pass --lp-io-dir=$lpio?)" >&2
        exit 1
    fi
    for ((i = 2; i <= n; i++)); do
        d="run-${i}/$lpio"
        if [[ ! -d "$d" ]]; then
            echo "equivalence-run.sh: lp-io dir '$d' missing" >&2; status=1; continue
        fi
        if ! diff <(cd "$refdir" && ls | sort) <(cd "$d" && ls | sort) >&2; then
            echo "equivalence-run.sh: lp-io identifier-file set differs (run-1 vs run-${i})" >&2
            status=1; continue
        fi
        for f in "$refdir"/*; do
            [[ -f "$f" ]] || continue
            id=$(basename "$f")
            if ! diff <(sort "$refdir/$id") <(sort "$d/$id") >&2; then
                echo "equivalence-run.sh: lp-io MISMATCH on '$id' (run-1 vs run-${i})" \
                     "-- the configs are not equivalent" >&2
                status=1
            fi
        done
    done
fi

exit $status
