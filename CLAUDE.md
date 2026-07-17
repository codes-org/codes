# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

CODES is a parallel discrete-event simulation framework for HPC networks, workloads, and
storage, built on ROSS (a PDES engine with optimistic synchronization). ROSS is an
**external dependency**, not vendored: CMake finds it via `find_package(ROSS CONFIG)`, so
an installed ROSS must be reachable through `ROSS_ROOT` or `CMAKE_PREFIX_PATH`. CI pins a
specific ROSS commit (`ROSS_REF`) reciprocally with a CODES pin in ROSS — see
`doc/dev/ci.md` before bumping either.

## Build

Presets (CMake ≥ 3.21, Ninja) are the standard path; build trees land in
`build/<preset>/`:

```bash
export ROSS_ROOT=$HOME/ross        # or CMAKE_PREFIX_PATH
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Committed presets: `debug`, `release`, `relwithdebinfo`, `coverage`, `asan`, `ubsan`,
`full-ci`, `full-local-{debug,release}` (the `full-*` ones force all heavy optional deps —
SWM/UNION/DUMPI/Torch/ZeroMQ — on; plain `debug`/`release` auto-detect and skip missing
ones). A git-ignored `CMakeUserPresets.json` may add user presets inheriting these (e.g.
ones pointing `CMAKE_PREFIX_PATH` at a sibling ROSS checkout's install tree).

Do not set `CC=mpicc` — MPI is found via `find_package(MPI)`.

## Tests

```bash
ctest --preset debug                     # whole suite (what CI runs)
ctest --test-dir build/debug -R <regex>  # subset by name, e.g. -R example-ping-pong
ctest -N                                 # list without running
DONT_DELETE_TEST_DIR=1 ctest -R <name>   # keep the staged run dir for inspection
./build/debug/tests/codes-config-compiler-test --gtest_filter=ConfigCompiler.*  # gtest direct
```

Two kinds of test live in `tests/` (see `tests/README.md`, which is authoritative):

- **Unit tests**: GoogleTest (vendored in `thirdparty/googletest/`), serial, no MPI,
  `<area>-test.cxx` + 3-line CMake registration.
- **Model/integration tests**: run a model under `mpiexec` via helper functions in
  `tests/CMakeLists.txt` — `codes_add_run_test` (smoke: clean exit + marker line),
  `codes_add_equivalence_test` (determinism: identical `Net Events Processed` across
  runs), `codes_add_lpio_equivalence_test` (strongest: per-LP `lp-io` diff between two
  configs; proves `.conf`-vs-`.yaml` twins), `codes_add_config_equivalence_test`
  (marker-based fallback when a model emits no reproducible lp-io). Prefer the helpers;
  only custom-assertion tests go in the `test-shell-files` shell-script list (those
  scripts get `$bindir` and generate configs from `*.template.conf.in` via `envsubst`).
- Long UNION workload tests carry the `nightly` label; PR CI excludes them.

## Formatting (CI-enforced)

CI runs **clang-format 20** with `--dry-run --Werror` over every `*.c/*.h/*.cpp/*.hpp/*.cxx`
under `src codes doc tests` — whole files, not just diffs. Different clang-format major
versions produce subtly different output, so use major version 20 (check
`clang-format --version`; stop and say so if it isn't 20 rather than formatting anyway).

**Commit protocol — apply this to every commit that touches C/C++ files:**

1. Before committing, run `clang-format -i` on the touched `.c/.h/.cpp/.hpp/.cxx` files
   and verify they pass the CI check:
   ```bash
   clang-format --dry-run --Werror <touched files>
   ```
2. Include the formatting in the same commit as the change. Never create a separate
   "apply clang-format" / "fix formatting" commit — every commit must pass the format
   check on its own.
3. Never hand-format to satisfy the check; the formatter is the source of truth.

An opt-in repo hook runs the same check at commit time — enable once per clone with
`git config core.hooksPath scripts/git-hooks`.

Other conventions (`doc/dev/conventions.md`): `snake_case` everywhere, `.cxx` for C++
(never `.C`), `#ifndef CODES_..._H` guards (no `#pragma once`), Doxygen `/** */` on public
headers added incrementally (a header needs a `/** @file */` block or its symbol comments
are silently dropped). Commit messages use a lowercase area prefix (`tests:`, `fix:`,
`dragonfly-dally:`, `doc:`).

## Running a model

Model binaries take ROSS options, then `--`, then the config file:

```bash
mpirun -np 3 build/debug/doc/example/tutorial-synthetic-ping-pong \
    --sync=3 --num_messages=100 -- build/debug/doc/example/tutorial-ping-pong.conf
```

`--sync=1` sequential, `--sync=2` conservative, `--sync=3` optimistic, `--sync=6` reverse-
handler checks (see Determinism below). Configs come in two equivalent formats chosen by
extension: legacy `.conf` (LPGROUPS/PARAMS) and YAML/JSON (`doc/dev/yaml-config.md`); the
YAML front-end (`src/modelconfig/`, ryml vendored in `thirdparty/rapidyaml/`) compiles to
the same internal representation, and equivalence tests hold the twins together — **edit
both when changing a config**.

## Architecture

Everything is shaped by ROSS's optimistic PDES model. Models are LPs (logical processes)
defined by a `tw_lptype` of callbacks: `init`, forward event handler, **reverse event
handler**, optional `commit`, `final`, and a mapping function. Under `--sync=3` events
speculatively execute and may roll back; every state mutation in a forward handler must
have an exact inverse in the reverse handler (RNG calls reversed with
`tw_rand_reverse_unif`, values needed for reversal stashed in the message, conditional
work flagged via `tw_bf` bitfields). Getting this wrong is the classic CODES bug: it shows
up as non-determinism, not a crash.

**Determinism is a hard project guarantee**: two runs with the same config must process
the same number of net events. The equivalence tests enforce it; `--synch=6` +
`crv_checkpointer` state save/compare functions (e.g. `save_terminal_state` /
`check_terminal_state` in dragonfly models) exist to localize reverse-handler bugs. New
state fields must be threaded through those save/check functions or listed in their
exclusion comments.

Layout: `codes/` is the installed public header surface; `src/` builds the single
`libcodes` library; model drivers (executables) live in `src/network-workloads/`
(synthetic traffic + MPI trace replay via DUMPI/SWM/UNION) and `doc/example/` (tutorial
models, notably `tutorial-synthetic-ping-pong.c` — the commented teaching example).

A model driver's `main()` follows a fixed sequence: `tw_opt_add`/`tw_init` →
`configuration_load` → `model_net_register()` + `lp_type_register()` for the driver's own
LPs (+ `st_model_type_register()` for instrumentation callbacks) → `codes_mapping_setup()`
(instantiates LPs from the config's group/count layout — `src/util/codes_mapping.c` owns
the name→type→global-id mapping) → `model_net_configure` → `tw_run()` → finalize/report.

**model-net** (`src/networks/model-net/`) is the network abstraction. Each network model
(dragonfly.c, dragonfly-custom/-plus/-dally.cxx, fattree, slimfly, torus, express-mesh,
simplenet, simplep2p, loggp) implements a `model_net_method` vtable
(`codes/model-net-method.h`). Terminal/NIC LPs are wrapped by a base LP
(`core/model-net-lp.c`) that handles scheduling, queuing, and sampling hooks before
delegating to the method; router LPs are separate methods with their own LP types.
Application LPs inject traffic with `model_net_event()` and reverse it with
`model_net_event_rc2()`. LP counts and parameters (radix, num_rails, num_qos_levels…)
come from the config, so payload/state sizes are often only known at LP-init time.
dragonfly-dally is the most maintained variant.

**LP state rules** (root cause of past segfaults, see `doc/dev/conventions.md`): ROSS
hands LPs a zeroed blob and never runs C++ constructors — C++/STL members in state are
allowed only if placement-new'd in `init` and destructed in `final`; never assign a
container into never-constructed memory. Event/message structs must stay POD (ROSS
memcpys them). Growing a driver's message struct can overflow the config's
`message_size` PARAM — the run aborts with an explicit size error; bump the configs.

**Instrumentation** is two separate mechanisms: (1) CODES-native sampling
(`model_net_enable_sampling`, per-model `*_sample_file` PARAMS, model-owned output
formats), and (2) the ROSS instrumentation layer (`--model-stats=1..4` for GVT/real-time/
virtual-time sampling + `--event-trace`), driven by per-LP `st_model_types` callbacks.
The binary output format of (2) and a reference parser live in
`doc/model-stats-binary-format.md` and `scripts/parse-ross-model-stats.py`; registration
mechanics in `doc/codes-vis-readme.md`. Keep payload layouts, `mstat_sz`/
`sample_struct_sz` sizing, the format doc, and the parser in sync when touching any of
them.

**Surrogate subsystem** (`src/surrogate/`): multi-fidelity simulation that can freeze the
network model mid-run (`NETWORK_SURROGATE` config section) and replace packet latencies
with predictions (ML integration via Torch/ZeroMQ). The freeze path memsets terminal
state and selectively restores fields — any new terminal-state field must be considered
there (`dragonfly_dally_terminal_highdef_to_surrogate` and its unfreeze twin), or it
silently zeroes (or crashes, for pointers) while frozen.
