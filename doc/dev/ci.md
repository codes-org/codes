# CODES Continuous Integration

This documents the CI that runs on every push and pull request to `master`, and —
most importantly for day-to-day work — **how and when to bump the pinned ROSS
commit** that CODES builds against.

All workflows live in [`.github/workflows/`](../../.github/workflows).

## Workflows at a glance

| Workflow | File | Triggers | What it does |
| --- | --- | --- | --- |
| **Build** | `build.yml` | push/PR to `master`, nightly `0 7 * * *` | Builds ROSS + CODES and runs `ctest` across an OS × MPI × compiler matrix, plus a coverage job, ASan/UBSan sanitizer lanes, and a heavy-deps `full` job. |
| **Format check** | `format.yml` | push/PR to `master` | Runs `clang-format-20 --dry-run --Werror` over the C/C++ tree. Any drift fails the PR. Reformat with `clang-format-20 -i <file>`. |
| **Full CI image** | `full-ci-image.yml` | weekly `0 8 * * 1`, manual, and on changes to `ci/full/Dockerfile` | Builds and publishes `ghcr.io/codes-org/codes-ci-full` (the heavy-dependency image the `build.yml` `full` job runs inside). Not run on every PR. |

### `build.yml` jobs

- **`build`** — the core matrix. `{ubuntu-22.04, ubuntu-24.04, macos-14} × {mpich, openmpi} × {gcc, clang}`, pruned to 8 legs (macOS only ships Apple clang, so its "gcc" leg is dropped; ubuntu-22.04 carries the older glibc/gcc-11 toolchain and skips clang since the gcc-vs-clang signal comes from 24.04). Heavy optional deps are OFF so every leg runs on a stock runner with no custom image. The macOS leg is the safety net for Mac-specific link/include breakage.
- **`coverage`** — single config (ubuntu-24.04 / gcc / MPICH). Builds CODES with `--coverage`, runs the suite, and uploads to Codecov. Informational only — it never gates a PR, and the upload is a no-op until the `CODECOV_TOKEN` repo secret is set.
- **`sanitizers`** — AddressSanitizer (`asan`) and UndefinedBehaviorSanitizer (`ubsan`) as two independent matrix legs, single config (ubuntu-24.04 / gcc / MPICH). Both gate PRs; only CODES is instrumented (ROSS is built without sanitizers). See [Sanitizer lanes](#sanitizer-lanes) below.
- **`full`** — every optional subsystem ON (SWM, UNION, DUMPI, Torch, ZeroMQ). Runs inside `ghcr.io/codes-org/codes-ci-full` so the slow dependency compiles happen once in that image; this job only builds ROSS + the zmqml requester + CODES.

Each leg builds ROSS fresh from source (see the pinning section below), installs it,
then configures CODES against that install via `find_package(ROSS CONFIG)`
(`-DCMAKE_PREFIX_PATH=<ross-install>`). MPI is auto-discovered with
`find_package(MPI)` — the matrix never sets `CC=mpicc`; it only changes which
packages get installed.

## The ROSS pin (reciprocal pinning)

CODES and ROSS pin each other so that a breaking change in either repo's
consumer-facing surface is caught in the other's CI rather than after a merge:

- **CODES pins ROSS** via the `ROSS_REF` workflow `env:` value at the top of
  `build.yml`. Every job checks out `ROSS-org/ROSS` at that commit.
- **ROSS pins CODES** via `CODES_REF` in ROSS's
  `.github/workflows/codes-contract.yml` (the "CODES contract test"). It builds
  CODES at that SHA against a freshly-installed ROSS.

Each side catches consumer-API regressions in the other.

### Nightly drift detection

The `build.yml` nightly run (`cron: "0 7 * * *"`) **ignores `ROSS_REF` and builds
against `ROSS-org/ROSS@master`**:

```yaml
ref: ${{ github.event_name == 'schedule' && 'master' || env.ROSS_REF }}
```

So a green PR/push CI (pinned) alongside a **red nightly** means ROSS master has
moved in a way that breaks CODES — i.e. the pin is overdue for a bump, or a real
incompatibility landed in ROSS that needs handling on the CODES side. Pin drift
surfaces within a day instead of at the next manual bump.

### When to bump `ROSS_REF`

Bump it when CODES needs to track ROSS forward:

- CODES wants a ROSS feature, bug fix, or API change that has landed on ROSS master.
- The nightly drift job has gone red and the fix is to move CODES onto the newer ROSS.
- Routinely, to keep the pin from drifting far behind master.

### How to bump it

1. Edit the single line in `build.yml`:

   ```yaml
   env:
     ROSS_REF: <new ROSS commit SHA on ROSS-org/ROSS master>
   ```

   Use a full commit SHA (not a branch or tag) so the build is reproducible. Pin
   to a commit that is on ROSS `master`.
2. Open it as its own PR (or fold it into the PR that needs the newer ROSS). The
   matrix builds CODES against the new pin — **all legs must pass before merging.**
   If a leg fails, the new ROSS is incompatible: either fix CODES in the same PR or
   hold the bump until ROSS is fixed.
3. If a bump turns out to break `master` after merge, **roll back by reverting the
   one-line change** — it has no other moving parts.

### Bumping the other direction (CODES pin in ROSS)

When a CODES change needs to be visible to ROSS's contract test, bump `CODES_REF`
in ROSS's `codes-contract.yml` the same way (full SHA on `codes-org/codes`
master). That edit lives in the **ROSS** repo, not here.

## The heavy-deps image

The `full` job runs inside `ghcr.io/codes-org/codes-ci-full`, built from
`ci/full/Dockerfile` and published by `full-ci-image.yml`. The image bakes in
MPICH, cmake, ninja, flex/bison, and each heavy dependency (SWM, UNION, Argobots,
DUMPI, Torch, ZeroMQ) under `/opt/<dep>`, so the `full` job only compiles
ROSS + the zmqml requester + CODES.

- The package is **public**, so fork-PR runs can pull it anonymously.
- It is tagged `:latest` and `:<sha>`. If a Dockerfile bump regresses the image, the
  `full` job can pin a specific `:<sha>` tag instead of `:latest`.
- **Bootstrap:** the image must exist in GHCR before the `full` job can run — trigger
  `full-ci-image.yml` once via *Actions → Full CI image → Run workflow* to publish
  `:latest`.

## Sanitizer lanes

The `sanitizers` job builds CODES under AddressSanitizer (`asan`) and
UndefinedBehaviorSanitizer (`ubsan`) as two independent matrix legs, on a single
ubuntu-24.04 / gcc / MPICH config. They're kept separate (not one combined build)
so each reports and gates on its own, and MPICH is used because it's far quieter
under the sanitizers than OpenMPI. Select a lane with `cmake --preset asan|ubsan`,
which sets the `CODES_SANITIZER` cache variable; **only CODES targets are
instrumented** — ROSS is a prebuilt imported target and is not reinstrumented.

- **Both lanes gate.** UBSan aborts on any UB (`-fno-sanitize-recover` is baked
  into the preset); ASan aborts on the first memory error. A finding fails the job
  and blocks merge.
- **ASan builds at `-O1`** (the `asan` preset overrides Debug's `-O0`). At `-O0`
  the suite took ~1 h on CI — the six ping-pong tests ran 10–15 min each. `-O1` brings the
  full suite to ~5 min. UBSan stays `-O0`.
- **Leak detection is off** (`detect_leaks=0`), so the lane delivers
  heap-overflow / use-after-free / stack-overflow detection without MPICH and
  teardown leak noise.
- **`log_path`** writes each sanitizer report to a file that's uploaded as an
  artifact on failure — the test harnesses redirect per-rank stderr, so otherwise
  a bare abort code would be all CI shows.

Run a lane locally with `export ROSS_ROOT=<ross-install>` then
`cmake --preset ubsan && cmake --build --preset ubsan && ctest --preset ubsan`
(same for `asan`).

### Deferred follow-ups

Lower-priority improvements intentionally left out of the initial lanes — pick
them up later:

1. **Enable LeakSanitizer.** Flip `detect_leaks=1` and add an LSan suppression
   file for MPICH internals and CODES's known unfreed globals. Expect the lane to
   go red until the suppressions are tuned, so land it as its own focused change,
   not folded into unrelated work.

If ASan runtime ever balloons again, the fallback is to run a fast test subset on
PRs and the full suite on the nightly `schedule` (the cron already exists); at
~5 min the full suite is currently fine to gate as-is.

## Note for workflow edits

Changes to `.github/workflows/*.yml` only run in a PR's CI when they're pushed to the
**upstream `codes-org/codes`** repo (or a branch there). A workflow edit pushed only
to a personal fork won't be exercised by the upstream PR's checks.
