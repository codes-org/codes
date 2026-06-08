# CODES Rewrite Roadmap

**Status:** Planning

This document is the canonical plan for the staged rewrite of CODES. It captures
*why* the rewrite is happening, the sequence of phases (in dependency/risk order),
the decisions made so far (with rationale), and the open questions still to settle.
It is meant to be read top-to-bottom once, then used as a reference and updated as
phases complete.

The rewrite is deliberately **waved**: each phase keeps the simulator working,
leans on equivalence tests for safety, and keeps legacy paths alive in parallel
until their replacements are proven.

---

## 1. Background & motivation

CODES is built on ROSS (a C-based optimistic parallel discrete-event simulator).
Because ROSS is C, CODES started in C, with C++ being inconsistently bolted on in places.
There's two main structural problems that make it difficult to not only add new models,
but to maintain the code:

1. **Copy-and-edit modeling.** To write a new model, the established workflow is:
   find the most similar existing model, copy its file, and edit the copy. The same
   logic then lives in many files, so a single bug must be fixed in every copy.
2. **Inconsistent, ad-hoc C++.** Where C++ exists, it is uneven in style and rarely
   uses composition/abstraction to remove the duplication above.

Concrete evidence (paths/lines indicative, as of this writing):

- **Synthetic traffic drivers** — `src/network-workloads/model-net-synthetic.c`,
  `-fattree.c`, `-slimfly.c`, `-dragonfly-all.c` are near-identical. Each
  re-implements send/recv counters, `issue_event()`, RNG + reverse-RNG handling,
  the kickoff event, and its own `main()`. They differ mostly in the *traffic
  pattern* (destination selection).
- **Network models** — `dragonfly.c` (~3,800 lines), `fattree.c`, `torus.c`,
  `slimfly.c` each duplicate chunking math, per-network message-list types and
  their init/delete, buffer/credit/virtual-channel accounting, statistics, and
  bandwidth conversion. `bytes_to_ns` already exists in
  `src/networks/model-net/common-net.c` yet is re-copied into individual models.
- **simplep2p.c** even comments a helper as *"more or less copied from
  model_net_find_stats"*.

**Goal:** restructure CODES so shared logic lives in one place (kill the
copy-and-edit duplication), modern C++ is used consistently, and adding a new model
means writing only the parts that are genuinely new — done in safe, verifiable
waves inside the existing CODES repository.

---

## 2. How we sequence the work

Each candidate piece of work is ranked by four questions:

1. **Does it gate other work?** Foundational changes (build, configuration, core
   APIs, the ROSS interface) land before things built on top, so we don't refactor
   twice.
2. **Is it on the SBIR critical path?** The WAN / `simplep2p` + surrogate work has
   deadlines; anything blocking it moves up.
3. **Blast radius / risk.** Touching core (mapping, the message union, ROSS
   integration) destabilizes everything; isolated changes are safe anytime.
4. **Reversibility.** Cheap-to-undo experiments go early to learn; hard-to-undo
   commitments (a new config format, a language-wide C→C++ move) need more upfront
   agreement.

Two practices apply throughout:

- **Parallel legacy paths.** Replacements run alongside what they replace (e.g. the
  old `.conf` format) for a couple of releases, with a clear deprecation window.
- **Verify by equivalence.** Every migration is checked by comparing outputs: old
  vs. new config, sequential vs. optimistic runs, and refactored vs. original model.
- **Existing prototype code.** Some C++ prototypes already exist in the `digital-twin`
  repo (branch `initial-code`) — a config-driven driver (`Orchestrator`), a RapidYAML
  `ConfigParser`, YAML/DOT example configs, and a `Mapper`. These are pulled into CODES
  **incrementally, in the phase that consumes each** (the config pieces in Phase 3, the
  `Mapper`'s connectivity *idea* harvested during the Wave 4 WAN-model work, §8), not
  all at once.

---

## 3. Hard constraints from ROSS (shape every phase)

These are non-negotiable properties of the engine and drive much of the design,
especially the C++ class work (Phase 4):

1. **ROSS owns LP state and event messages as fixed-size POD blobs** of `state_sz`
   bytes (`tw_lptype.state_sz`); it does **not** run C++ constructors on them.
   → A polymorphic LP object with a vtable pointer embedded in the state blob is a
   footgun (and interacts badly with the `crv_checkpointer` state-compare
   machinery in `model-net-lp.c`).
2. **ROSS dispatches via C function pointers** (`init_f`/`event_f`/`revent_f`/…).
   → C++ must expose C-compatible callbacks (a trampoline).
3. **Optimistic PDES ⇒ reverse computation.** Every forward event handler needs an
   exact undo. `tw_bf` bits record which branches were taken; every `tw_rand_*`
   draw needs a matching `tw_rand_reverse_unif`; non-recomputable state is saved
   into the message and restored in reverse. This is the single biggest source of
   fragile, duplicated code.
4. **Messages are fixed-size** (`model_net_wrap_msg` is a union of every network's
   message type). Undo data must fit in the message or live in `rc_stack` (heap,
   GVT-tracked). No unbounded per-event journals.

---

## 4. Phase overview

| # | Phase | Gates? | Risk | Summary |
|---|-------|:---:|:---:|---------|
| 1a | Safety net: CI & equivalence | ✅ | low | CI on MRs (GitHub Actions), pinned-clone ROSS built in-CI, equivalence harness, C++17 baseline, unit-test framework |
| 1b | Build hygiene | – | low | CMake modernization (target-based, find_package ROSS, tri-state deps), co-location reorg, portable presets, heavy-deps images, install/export — must **not** block Phase 3; may run in parallel with it |
| 2 | Shared C++ foundation | ✅ | low | Conventions + automated style enforcement (clang-format/clang-tidy in CI) — one idiom for config + classes |
| 3 | YAML config front-end | ◐ | med | Friendly YAML "config compiler" → **existing** mapping (narrow `ConfigVTable` seam); Cytoscape topology + explicit units + custom components; old `.conf` in parallel |
| 4 | C++ class application | – | med | Hybrid base+composition LP classes; Wave 1 synthetic (incl. ROSS trampoline) → Wave 2 `simplep2p` → Wave 3 others → Wave 4 **new WAN model** (routing+congestion) + the explicit-connectivity workstream (§8) |
| 5 | Far future | – | – | Decouple the message union, modernize workload API, surrogate integration cleanup, broader C→C++, performance baseline |

A cross-cutting **config-driven driver** (the `Orchestrator`, see §11) runs through
all phases as the single entry point.

---

## 5. Phase 1 — Build, packaging & CI

**Goal:** a correctly-engineered CMake build, shareable presets, a pinned ROSS, and
a CI safety net strong enough to protect every later phase. This phase also does a
deliberate **CMake cleanup** — the current build works but uses dated, non-idiomatic
patterns (global flag appends, an `ENV{PKG_CONFIG_PATH}` hack, a legacy MPI module,
no off-switch for found deps) that should be fixed before more is layered on.

**Phase 1 is split in two** — by the §2 criteria, much of this phase's scope
doesn't actually gate anything downstream, and the SBIR-critical Phase 3 shouldn't
queue behind build hygiene:

- **1a — the safety net (gates Phase 3):** CI on every MR (Workstream 4), the pinned
  ROSS (Workstream 3), the equivalence harness (Workstream 5), the unit-test framework
  (Workstream 6), and the C++17 baseline (a one-line change, listed with Workstream 1).
- **1b — build hygiene (gates nothing on the critical path):** the CMake modernization
  including the co-location reorg (Workstream 1), portable presets (Workstream 2),
  tri-state optional deps, install/export, and the heavy-deps/python2 images. Valuable,
  but it may run **in parallel with (or after) Phase 3** rather than in front of it.

**Decisions made**

- **Repo structure:** the rewrite happens **entirely in the CODES repo** (§2).
- **Optional-dependency hygiene:** Every optional feature
  gets a user-facing `CODES_USE_<X>` cache variable with values **AUTO / ON / OFF**:
  - `AUTO` (default): probe; enable if found, silently disable if not.
  - `ON`: probe; **hard error** if not found (so a `full`/CI build fails loudly
    instead of silently producing a lesser binary).
  - `OFF`: don't probe at all.

  Keep the user-facing string (`CODES_USE_SWM`) separate from the internal resolved
  boolean (`USE_SWM`) the build already uses, so downstream code is untouched.
  Respect dependency chains (UNION ⇒ SWM ⇒ argobots). Applies to SWM, UNION, DUMPI,
  RECORDER, TORCH (and the stubbed DARSHAN). Today only TORCH is close to this
  (and its `ON` silently downgrades); SWM/UNION/DUMPI can't be turned off when found.
- **Consume ROSS correctly via `find_package(ROSS CONFIG)`.** ROSS now ships a
  proper CMake package (`ross/cmake/ROSSConfig.cmake.in` → `ROSSTargets.cmake`,
  installed at `<prefix>/lib/cmake/ROSS/`). CODES links the imported ROSS target and
  **drops** the current pkg-config probe **and** the `set(ENV{PKG_CONFIG_PATH} …)`
  hack.
- **Drop DAMARIS *build* support, keep the code.** ROSS master recently removed the
  damaris build path; CODES follows. Remove `DAMARIS_PKG_CONFIG_PATH` and the
  commented DAMARIS block; leave the `#ifdef USE_RDAMARIS` code paths in place (never
  defined) pending the eventual RISA rewrite.

**Workstream 1 (1b) — CMake modernization (the cleanup)**

- **Co-location reorg first.** Execute the big-bang layout move (§6) right after 1a's
  CI lands and before the rest of this workstream, so the modern CMake below is written once
  against the final `codes/<subsystem>/` tree (and harvested/new C++ lands there from
  day one). Purely mechanical (`git mv` + include rewrite); forwarding-header shims for
  downstream; time it for low branch activity.
- **Target-based everything.** Replace global `include_directories`,
  string-appended `CMAKE_C_FLAGS`, and `add_definitions` with `target_link_libraries`
  / `target_include_directories` / `target_compile_definitions` /
  `target_compile_options` with correct PUBLIC/PRIVATE/INTERFACE scoping.
- **ROSS** via `find_package(ROSS CONFIG REQUIRED)` + imported target (above).
- **MPI** via `find_package(MPI REQUIRED)` + `MPI::MPI_C` / `MPI::MPI_CXX` imported
  targets; delete the legacy BLT `src/cmake/SetupMPI.cmake` and the manual include/lib
  plumbing.
- **Optional deps** linked as imported targets (`PkgConfig::SWM`, …) on the `codes`
  target, gated by the tri-state resolver — not global flag appends.
- **Install + export** a `codesConfig.cmake` (imported `codes::codes` target) so
  in-repo consumers (driver, tests) and any external user link CODES the modern way.
- **C++17 baseline** set unconditionally (drop the Torch-only bump), with a quick
  compile-clean check of the existing "C with classes" code at 17.
- **Housekeeping:** settle one CMake minimum version — **≥3.23** (needed for
  `FILE_SET HEADERS`). Use generator
  expressions for build-type logic (e.g. the test gate) instead of string compares;
  purge dead autotools `.gitignore` entries and add `install/`, `test/` artifacts.

**Workstream 2 (1b) — Portable presets**

- Committed `CMakePresets.json` with environment-independent base presets: `debug`
  (tests + RC verifier), `release`, `core` (heavy optional deps OFF), `full` (all ON,
  strict). Dependency *locations* come from `$env{…}`.
- Personal/machine paths live in a git-ignored **`CMakeUserPresets.json`** that
  `inherits` a base preset.

**Workstream 3 (1a) — ROSS pinning (decided: pinned clone, built in CI)**

Pin via a **pinned clone**: each CI job clones ROSS at
a fixed commit/tag, builds + installs it, and CODES consumes it via `find_package`.
ROSS is **built in CI, not baked into an image** — it's quick to build and it's the
pin that changes most often, so baking it would force an image rebuild on every bump.
The **ROSS pin is a workflow variable**; bumping it is a one-line reviewed change with
no image rebuild. Cache the built ROSS keyed on its commit so unchanged pins skip the
rebuild.

**Workstream 4 (1a; full-matrix images are 1b) — CI (decided: GitHub Actions; ROSS in-CI, heavy deps imaged)**

- Host: **GitHub Actions**. The **whole build matrix runs on every MR/PR** (and on
  push to master) — debug/release × feature sets.
- **Docker images bake most deps** (SWM/UNION/argobots/
  conceptual + Torch), rebuilt rarely (only when those pins change). ROSS is built
  in-CI on top (Workstream 3).
- **Core** job needs **no custom image** — a stock runner with apt MPI/cmake/ninja +
  the in-CI ROSS build covers Phase 3 and Phase 4 waves 1–2. The **full** job uses the
  heavy-deps image (plus the same in-CI ROSS build). Derive the full image from
  `CODES-compile-instructions.sh`.
- **python2 is confined to the UNION feature** and lives only in the full image:
  conceptual (which UNION depends on) needs python2 *at image-build time* for code
  generation; SWM-only online and everything else do **not**. Contain
  python2 in the image (e.g. `ubuntu:20.04` base, or build 2.7.18 from source) — it
  isn't needed at simulation runtime.

**Workstream 5 (1a) — Equivalence / golden-output harness (decided)**

Extend the existing determinism tests (`example-ping-pong-determinism.sh`) into the
equivalence checks the migration leans on:
- seq (`--sync=1`) vs. optimistic (`--sync=3`) — proves reverse computation (Phase 4),
- old `.conf` vs. new YAML — proves the config migration (Phase 3),
- refactored vs. original model — proves Phase 4 refactors.

Two layers of check:
- **Aggregate determinism** — diff `Net Events Processed` (robust, format-stable).
  Keep this for **all** models as a cheap fast-path.
- **Per-LP result equivalence** — diff **`lp-io` output** (structured, per-LP,
  sortable), normalized and sorted. Use it **now wherever a model supports `lp-io`**.

Do **not** scrape fragile per-LP **stdout** stats. Models that only print stats today
get **no per-LP equivalence coverage for now** (they still get the aggregate check);
when such a model is rewritten in Phase 4, add a small deterministic **results digest**
to it — the long-term canonical form. So: `lp-io` now → results digest as models are
rewritten; stdout-only models are an explicit, documented coverage gap until then.

Never diff raw `ross.csv` for seq-vs-optimistic — its engine counters (rollbacks, etc.)
legitimately differ. Prefer equivalence (self-comparison) over stored golden files;
keep a few golden snapshots only as a both-paths-drift backstop.

**Workstream 6 (1a) — Unit-test framework**

Equivalence testing protects *refactors*; it cannot exercise *new* code's error paths.
Pick a C++ unit-test framework (Catch2 vs GoogleTest — settle at implementation time,
§13) and wire it into CTest + the 1a CI jobs. First real consumer: the Phase 3 config
compiler's validation / derivation / unit-conversion logic — especially the negative
paths (rejecting redundant shape values, bad units, hand-written derived values;
contract §12), which equivalence against a golden `.conf` can never reach.

---

## 6. Phase 2 — Shared C++ foundation

**Goal:** one C++ idiom used by *both* the config/orchestrator subsystem and the LP
classes, so the two bodies of C++ don't drift apart.

**Why now (pulled up from the class work):** the config subsystem pulled in during
Phase 3 is already C++ (RapidYAML parser, `Orchestrator`, `Mapper`). If the LP classes
introduce a *different* C++ style, we end up maintaining two idioms. Establishing the
foundation here lets both reuse it. (C++17 itself is set in Phase 1.)

**Decisions made**

- **Rich, placement-new'd LP state.** The `make_lptype<T>()` trampoline (§9.2)
  placement-news the state object in `init` and runs its destructor in `final`, so the
  state blob *is* a properly-constructed C++ object (STL members allowed) — fixing the
  current pattern (e.g. `congestion-controller.C` *assigns* `s->output_ports =
  set<int>()` into never-constructed, zeroed memory: technically UB). Safe because
  CODES uses reverse computation (in-place, no state copy); the one path that copies
  state is ROSS's opt-in `SEQUENTIAL_ROLLBACK_CHECK` mode (`crv_checkpointer`, in
  `ross/core/check-revent/crv-state.h`), for which we supply a **C++-aware
  checkpointer** (save = copy-construct, clean = destruct, check = `==`/field-compare)
  instead of the default whole-struct byte copy. **Messages stay POD always** (ROSS
  memcpy's events).
- **Naming: `snake_case`**, matching existing CODES C style (not the CamelCase of the
  prototype config code) — so the **harvested `Orchestrator`/`ConfigParser`/… get
  renamed to snake_case** when pulled in (Phase 3).
- **File extensions: `.cxx` for C++ sources** (rename the current `.C` files — which
  also removes the `.C`/`.c` collision on case-insensitive macOS filesystems; update
  the source lists in `src/CMakeLists.txt`). `.c` for C sources; `.h` headers with
  `extern "C"` guards where C-includable (as today).
- **Layout: fully co-located, big-bang (decided).** Every header sits beside its
  source in one `codes/`-rooted tree (`codes/<subsystem>/<file>.{h,cxx,c}`), so
  includes become `#include "codes/<subsystem>/…"`; public headers are designated for
  install via CMake `target_sources(FILE_SET HEADERS)` (needs CMake ≥3.23 → the Phase 1
  minimum). Done **all at once**, not incrementally — a layout move is mechanical and
  CI-verifiable, and one clean break beats a long two-conventions-at-once period that
  smears churn across releases. Executed **in Phase 1b, after 1a's CI is up and before
  the target-based CMake rewrite** (so new CMake is written once against the final tree and
  all new/harvested C++ lands in the final layout). Guardrails: keep the move **purely
  mechanical** (`git mv` + include rewrite + CMake path updates — *no* logic changes,
  designed target tree decided first); ship **forwarding-header shims** at old
  `codes/foo.h` paths (deprecation `#warning`) for a release or two; and **time it for
  low in-flight-branch activity** (it touches nearly every file — a rebase grenade
  otherwise). It's also the moment to define CODES's **public API surface**.

**Work**

- **Automated style enforcement** — `clang-format` enforced in CI plus a starter
  `clang-tidy` profile. The diagnosed root cause of the codebase's drift (§1) is
  rotating contributors, and a conventions doc alone doesn't survive them — CI gates
  do. This is the enforcement arm of the conventions doc below.
- **Conventions doc** — captures the naming/layout/extension rules above; the
  **reverse-computation discipline** guide is stubbed now and filled in Phase 4 when
  the RC helpers exist.

The **Layer-0 ROSS trampoline** (§9.2) and the C++-aware `crv_checkpointer` are
*designed* by the decisions above but **implemented in Phase 4 Wave 1, against their
first real consumer** — the same "primitives born from real consumers" principle as
the Layer-1 helpers (§9.2). Building them cold, ahead of any LP, risks an API that
gets redesigned on first contact.

---

## 7. Phase 3 — YAML configuration front-end (the "config compiler")

**Goal:** introduce a **user-friendly YAML configuration** (the basis for NetMaestro
and wide-area networks) that feeds the **existing `codes_mapping`** — **without**
rewriting mapping yet. The wire-format contract for NetMaestro's editor lives in a
companion doc, [new-config-format.md](new-config-format.md); this section is the
design rationale and the implementation plan.

### 7.1 Why a new format (not just a nicer `.conf`)

The current `.conf` (`LPGROUPS` + `PARAMS` + `@annotation`) is HPC-oriented and
**hostile to non-PDES experts**. Configuration today is scattered across **four**
places:

- `.conf` `PARAMS` (e.g. `packet_size`, bandwidths),
- separate files (simplep2p's two **latency/bandwidth matrices**;
  dragonfly's intra/inter-group connection files),
- ROSS **command-line options** (synthetic's `traffic`, `num_messages`,
  `arrival_time` — `model-net-synthetic.c:156`),
- `#define`s in source (`PAYLOAD_SZ` is `#define 2048` in
  `model-net-synthetic.c:20`).

It also leaks implementation details into the user's file — the worst offender is
`message_size`, which is the **ROSS event-blob size** (`codes_mapping.c:577` →
`tw_define_lps`), not a simulated message size at all. A user should never have to
know implementation details to fill out a config file.

### 7.2 The lesson from the abandoned prototype (why this is now low-risk)

The earlier prototype (`digital-twin`, branch `initial-code`) changed **the format and the mapping at the same
time** — it went straight to a graph-based `Mapper` that *replaced* `codes_mapping`,
then spent itself trying to re-derive `codes_mapping`'s
`(group, repetition, lp_type, annotation, offset)` math from a flat DOT graph. The
scars: `offset = 0` hardcoded with FIXMEs, annotations dropped entirely, the
GROUP_RATIO routing path commented out, restricted to a single network type. So
**feeding the existing `codes_mapping` and deferring the connectivity harvest to the
Wave 4 workstream (§8) is the correction**.

### 7.3 Approach (decided): a config compiler feeding the existing pipeline

Treat the front-end as a **config compiler**. The friendly YAML is the *source*; it
**compiles down** to the full, explicit internal configuration that `codes_mapping`
and every model already consume — filling in defaults, *deriving* implementation
details, and expanding compact descriptions into the group/rep structure. Downstream
is untouched.

- **Narrow seam.** The whole legacy query surface — `configuration_get_lpgroups`
  (which builds `lpconf`, which `codes_mapping` reads) **and** every model's
  `configuration_get_value_*("PARAMS", …, anno)` — funnels through one abstract
  interface, `struct ConfigVTable` (`codes/configfile.h:42`); the `.conf` text parser
  is just one implementation of it. The compiler's *output* plugs in at that seam (a
  programmatically-built config tree / a YAML-backed `ConfigVTable`), so
  **`codes_mapping` and the models need no changes**.
- **Two seam edges, bridged (decided).** The seam delivers *strings*; two of the
  target models do work *behind* it that the YAML must respect:
  - **simplep2p's matrices:** the model receives the matrix file **paths** through the
    seam (`configuration_get_value_relpath`, `simplep2p.c:871`) and `fread`s the files
    itself. So initially the YAML simply **references the existing matrix files by
    path** — the same treatment the parametric fabrics give their `connections:`
    binaries (contract §5.5). The friendly per-edge `bandwidth`/`latency` form can
    later be compiled down to *emitted* matrix files without touching simplep2p;
    whether simplep2p is modernized at all is revisited once the WAN model exists
    (it may not be worth it then).
  - **The synthetic drivers' parameters** (`traffic`, `num_messages`, `arrival_time`)
    are ROSS **command-line options** (`model-net-synthetic.c:156`), not config —
    initially they stay CLI. Eventually they
    become settable either way, with **CLI taking precedence** over the config file —
    a parameter sweep shouldn't require near-identical config copies that differ in
    one knob. To keep that reproducible, every run **dumps the fully-resolved config
    including CLI overrides**, so a result is always traceable to one complete
    configuration.
- **Keep `.conf` in parallel** for a deprecation window (length TBD, §13).
- **Verify by equivalence.** Because the YAML is intentionally *not* a 1:1 mirror of
  `.conf`, verify two ways: (a) compile the YAML, dump the resolved config tree, and
  `cf_equal()` it (`configfile.h:106`) against a golden `.conf` — this checks the
  defaulting/derivation directly; and (b) **behavioral** equivalence (Net Events /
  `lp-io`) via the Phase 1 harness.

### 7.4 The friendly schema

The schema design is settled around one principle: **a knob stays user-facing only
if it is a physical property of the modeled system or a deliberate experimental
variable; anything that is an artifact of how CODES is implemented is derived or
defaulted.** Concretely:

- **Derived (never written; a hand value would be wrong):** `message_size` (from the
  models' message-union size), `modelnet_order` (from the models present),
  `pe_mem_factor`, repetition/group counts (from the topology).
- **Parameter with a default (always overridable; prominence varies):**
  - *prominent* (physical/experimental): bandwidth & latency, packet/payload size,
    network scale & shape, traffic pattern, workload intensity, end time, and
    **routing** (defaulted but front-and-center — it is a studied variable).
  - *advanced but reachable*: `chunk_size`, VC/buffer sizes, scheduler, low-level
    timing/queue internals.
- **Output / instrumentation is dual-owned and a pass-through.** NetMaestro owns
  results/visualization in its flow; a direct-run user configures collection
  themselves. The schema must be able to **enable `lp-io`** (the Phase 1 equivalence
  harness depends on it). Phase 3 does **not** unify CODES-direct stats vs. the ROSS
  instrumentation-layer callbacks — that standardization is explicitly deferred
  (§10); the output section is a thin pass-through for now.

- **Topology = Cytoscape elements; one parser.** Topology is expressed in
  [Cytoscape.js][cyto] element form (nodes/edges with `data`). Because JSON ⊂ YAML,
  RapidYAML reads both an editor-exported `.json` and a hand-written `.yaml` — **no
  DOT, no `libcgraph`, no matrix files.** Physical quantities are **unit-bearing**
  (`"100Gbps"`, `"5ms"`), with a documented default unit when written bare; the
  compiler converts. Per-edge `bandwidth`/`latency` **replace the
  simplep2p matrices** (`simplep2p.c:315` `parse_mat`); the compiler builds the link
  table from edges.
- **Custom components + per-node overrides (this is NetMaestro's model).** A node
  references a **custom component** (`ComponentConfig`: a model + configured params),
  and may override individual params per node. This *is* the modern replacement for
  `@annotation` (see below).
- **Topology source is a first-class choice.** *Enumerated* (Cytoscape elements —
  hand-drawn, editor-exported, irregular WAN) is implemented in Phase 3.
  *Parametric* (a `fabric` block of shape params — regular HPC fabrics like dragonfly,
  fattree, torus, slimfly) is now **specified** as a hand-authored source
  (new-config-format.md §5.5): the compiler compiles the fabric's shape + per-class
  links + routing down to the PARAMS the existing models read. Crucially, **connectivity
  generation stays where it is** — internally-generated models (torus/fattree/slimfly/
  regular dragonfly) generate as today; the file-enumerated dragonflies (custom/dally/
  plus) keep their current external Python generators + binary `intra`/`inter` files,
  referenced by path. A **shared generator → elements utility** (so NetMaestro can
  *visualize* HPC fabrics and feed the sim from one source) is **future, not
  precluded** — deferred because HPC is not the SBIR focus, and the parametric schema
  is designed to accept it later unchanged. A generator's *output* is the same
  enumerated elements. The very-large WAN generated source remains reserved.

[cyto]: https://js.cytoscape.org/#notation/elements-json

### 7.5 Annotations are subsumed by custom components

The legacy `@annotation` mechanism (canonical example:
`doc/example_heterogeneous/example.conf`) exists to run **one model type with
different parameter sets in different regions of one simulation** — e.g. a `foo`
cluster `simplenet` at 10 Gb/s and a `bar` cluster `simplenet` at 15 Gb/s, the
annotation string selecting which `PARAMS` apply. A **custom component is exactly
this, named and defaulted** (`@foo` ⇒ component `foo_net`). So:

- Annotations are **not a user-facing concept** in the new format — nobody writes `@`.
- The compiler may **generate annotations internally** (one per custom component that
  shares a model) to drive the *existing, annotation-aware* `codes_mapping` for
  heterogeneous configs. The legacy annotation machinery thus becomes an **asset**.
- Phase 3's single-network target configs share no model across components, so the
  compiler emits **zero** annotations at first; they appear only with multi-network
  composition (`TopologyGroup`s / sites), which is deferred to future multi-network
  work (the `groups:` seam stays reserved — contract §5.4).

### 7.6 Harvest & scope

- **Harvest from `digital-twin` (`initial-code`):** the RapidYAML config parser and
  the `Orchestrator` entry point (renamed to `snake_case`, §6). **Decouple the
  `Orchestrator` from the prototype `Mapper`** — it must drive `codes_mapping_setup()`
  (the compiled config feeding the existing mapping), *not* install ROSS `CUSTOM`
  mapping via the prototype `Mapper`. The prototype `Mapper` and its DOT/`node_ids`
  data model are **left behind**; the Wave 4 connectivity workstream harvests their
  connectivity *idea*, not the code (§8).
- **Scope narrowly:** target `simplep2p` / `simplenet` / synthetic, verified by
  equivalence — not the entire legacy API on day one.
- **Unit tests for the compiler** (on the Workstream 6 framework): the validation /
  derivation / unit-conversion rules, especially the negative paths (contract §12) —
  they are not reachable by equivalence against a golden `.conf`.

---

## 8. Explicit connectivity — a Wave 4 workstream

**Goal:** give mapping a first-class, queryable model of **connectivity** (who is
connected to whom) for irregular topologies, **without** discarding the proven
`(group, repetition, lp_type, annotation, offset)` semantics the existing
`codes_mapping` is built on. This is an **extension of mapping, not a wholesale
replacement** of `codes_mapping`.

**This is no longer a standalone phase (decided).** It was originally sequenced as its
own phase between the config work and the classes, but its only real consumer is the
Wave 4 WAN model (§9.3): Waves 1–2 need nothing from it — the ESnet 2-site path runs
entirely off the Phase 3 compiled link table — and the HPC models keep deriving their
own neighbors. So the connectivity service is built **inside Wave 4, against the WAN
model's actual queries** — the same "primitives born from real consumers" principle as
the trampoline (§6, §9.2) — and built **incrementally**: the SBIR-minimum queries
first, the rest time-pending. The §8.5 guardrails still apply; see there for how the
service stays verifiable now that it lands in the same wave as its consumer.

### 8.1 What actually motivates this workstream

It is **not** the new format. Phase 3 already makes the YAML run by compiling down to
the existing `codes_mapping` at the `ConfigVTable` seam (§7.3), so the format needs no
mapper changes at all. The one capability `codes_mapping` genuinely lacks is **explicit
connectivity**: its entire API (`codes/codes_mapping.h`) is projections of the
group/rep/type/anno/offset coordinate system, and **nothing in it answers "who is node
A connected to."** Today connectivity is *implicit* — each network model re-derives its
own neighbors from topology PARAMS (dragonfly's connection files, torus's algorithmic
neighbors, simplep2p's latency/bandwidth matrix). A hand-drawn WAN (Cytoscape elements,
§7.4) has no algorithm to derive neighbors from — the graph *is* the source of truth —
so something must carry "A ↔ B" as first-class data. That gap is the whole reason this
workstream exists.

### 8.2 Harvest the idea, not the code

The prototype `Mapper` (`digital-twin`, `initial-code`) proved out exactly this one
thing: an explicit `Node`/edge graph with `GetDestinationLPId` /
`GetDestinationLPCount`. **That connectivity model is what we keep.** But the prototype
gets there by **throwing away** group/rep/annotation/offset — global ids are
DOT-traversal order, `offset` is just an index into a `NodeNames` list, annotations
don't exist, and it is built on `graphviz/cgraph`. That is precisely the §7.2 failure
mode (offset=0 FIXMEs, annotations dropped, GROUP_RATIO commented out, single network
type), and it is now also obsolete: Phase 3 chose Cytoscape elements over DOT, so the
connectivity comes from the Cytoscape graph, **not** `libcgraph`.

So we **do not** mature the prototype `Mapper` into a drop-in `codes_mapping`
replacement. Doing so would reintroduce those regressions *and* pay the
highest-blast-radius cost in the codebase (every LP and model calls `codes_mapping_*`;
a different API means rewriting every call site) in order to *lose* proven semantics.

### 8.3 Decision: extend, scoped narrow-first

Keep `codes_mapping`'s API and its group/rep/annotation/offset semantics intact; **add
an explicit-connectivity layer** (the prototype's `Node`/edge model + `GetDestination*`
queries) **fed from the Cytoscape graph**, alongside the existing mapping. Regular
models keep deriving neighbors as they do; irregular WAN models get to *ask* the mapper
for theirs.

- **Narrow (do first):** an additive connectivity service **beside** `codes_mapping`,
  consumed by the new WAN model (Wave 4) and grown query-by-query as that model needs
  them. Tiny blast radius, directly serves the SBIR FABRIC/WAN path, and can be built
  and verified behind the Phase 1 equivalence tests on a 2-site config.
- **Broad (defer):** actually routing *all* models through one new mapper and retiring
  `codes_mapping`. Only worth it if a model genuinely needs it — and if it ever
  happens, group/rep/annotation are **re-implemented** on the new mapper, not discarded.
  This is the original "replace `codes_mapping`" idea, demoted to optional/future.

### 8.4 Parity, not new capability

The prototype's `MappingSetup` also does engine setup — `pe_mem_factor`, RNG-offset
seeding, `tw_define_lps`, the `g_tw_nRNG_per_lp` bumps, and installing ROSS `CUSTOM`
mapping. `codes_mapping_setup` already does all of this; it is parity work that has to
live somewhere, **not** a reason to switch mappers.

### 8.5 Guardrails

Behind the Phase 1 equivalence tests; the old mapping stays available throughout;
verify connectivity queries against a known small topology before any model depends on
them. That verification step is an **explicit early Wave 4 task** — since the service
and its consumer now land in the same wave, it is what keeps WAN-model bugs and
connectivity bugs distinguishable.

---

## 9. Phase 4 — C++ class application (the LP class design)

This is the most fully-planned phase. It removes the copy-and-edit duplication in
the LPs themselves. It comes after the config work because the LP classes consume
configuration, and doing it earlier would mean building against an API that's about
to change.

### 9.1 Chosen approach

- **Variation → Hybrid: thin concrete base + composition.** A base/framework class
  owns the shared LP lifecycle and ROSS glue; the parts that actually differ
  (routing, topology, traffic pattern) are injected as **stateless strategy
  singletons referenced by pointer**. This is the natural C++ formalization of a
  pattern CODES already uses — `struct model_net_method` is effectively a strategy
  vtable. LP state objects stay **non-virtual / POD-friendly** (no vtable in the
  blob); only the shared strategy singletons are polymorphic. Templates/CRTP are
  held in reserve as a surgical optimization for a single proven-hot tiny function
  (per-hop routing) *if* profiling ever demands it — not a default. (Rationale: the
  dispatch cost equals the C function-pointer call ROSS already pays; the real cost
  of CRTP is readability/onboarding for rotating contributors.)

- **Reverse computation → targeted helpers, not full automation.** Keep reverse
  handlers hand-written but *safe by construction*:
  1. **`ReversibleRng`** — wraps the RNG, counts draws, auto-reverses them
     (eliminates hand-counted `tw_rand_reverse_unif`).
  2. **RC verification harness** — debug-only snapshot → forward → reverse →
     assert-equal per event, built on the existing `crv_checkpointer` seams; zero
     release cost. *This is what makes hand-written reverse handlers safe.*
  3. **`RcStack`** — RAII wrapper over the existing `rc_stack`.
  4. **`RcJournal`** (typed field save/restore in the message) — **prototype only**,
     adopted selectively where it clearly wins (bounded by fixed message size).
  Explicitly **avoided:** mandating the journal everywhere, and switching to full
  state-saving rollback (both trade away the performance reverse computation buys).

### 9.2 Class architecture

**Layer 0 — ROSS adapter (trampoline).** `make_lptype<T>()` + callback trampolines
(header-only): generates a `tw_lptype` whose C function pointers cast `void* sv` →
`T*` and call fixed methods; does **placement-new in init** and **destructor in
final** so C++ members in the state object are correctly constructed/destroyed. LP
classes are concrete and `final` (no vtable in state).

**Layer 1 — Reusable primitives**  born from real
consumers:
- `ReversibleRng`, `RcStack`, `RcVerifier`, `RcJournal` (prototype) — the RC bundle.
- `WorkloadSource` — thin C++ wrapper over the `codes_workload_method` vtable.
- `NetworkStats` — wraps `mn_stats` + `model_net_find_stats` with `record_send/recv`
  and RC twins (consumed from Wave 2).
- `Bandwidth` — one home for `bytes_to_ns` (GiB/s) and `rate_to_ns` (MiB/s)
  (Wave 2).
- *(Wave 2/3)* `Chunker`/`Packet`, `MessageQueue<Msg>`, `VirtualChannelSet`/
  `BufferPool`, `LinkScheduler` (idle-time tracking).

**Layer 2 — LP role bases** (concrete; hold strategies):
- `WorkloadDrivenLp` *(Wave 1)* — base for app/traffic LPs: lifecycle, counters,
  `issue_next_event` (+RC), completion tracking, RC scaffolding; holds a
  `TrafficPattern` (synthetic) or `WorkloadSource` (replay) strategy.
- `TrafficPattern` *(Wave 1)* — strategy `choose_destination(...)` (+RC twin);
  impls `UniformRandom`, `NearestGroup`, `NearestNeighbor`.
- `NetworkModelLp` + `RoutingStrategy` + `Topology` *(Wave 2/3)* — endpoint/router
  lifecycle with injected routing + topology.

### 9.3 Waves (risk order)

- **Wave 1 — Synthetic app LPs.** Safe seam: application LPs (`nw-lp`) that only call
  `model_net_event`, no network internals touched. Proves the trampoline, the hybrid
  base+strategy, the RC bundle, and the C++17 build end-to-end on a small target.
- **Wave 2 — `simplep2p`** (prioritized). Refactor onto Layer-1 network primitives
  (`NetworkStats`, `Bandwidth`, `LinkScheduler` idle-tracking; its `*_saved` fields are
  the first `RcJournal` candidate), validated against the current model's output as a
  golden test. Designs those primitives against a real consumer. Now consumes the new
  YAML. **simplep2p's role:** it is an abstract direct-link delay model — *no runtime
  routing*; it delivers source→dest in one hop using a pre-set per-pair
  latency/bandwidth table (proven in `simplep2p.c:670`, `handle_msg_start_event`: it
  reads the direct `(src,dest)` cell and aborts if absent — it never transits an
  intermediate node). That is **exactly enough for the SBIR's actual deliverable — the
  ESnet testbed (2 sites, one router ≈ a point-to-point link)** — and it
  bootstraps the config compiler + the C++ architecture. It is **not** a general WAN
  model (see Wave 4).
- **Wave 3 — remaining network models one at a time** (dragonfly/fattree/torus/
  slimfly) as time allows, onto shared primitives + `RoutingStrategy`. Biggest
  line-count payoff; lower current priority.
- **Wave 4 — a new WAN model (routing + congestion)** — the real SBIR payoff for
  FABRIC and other WANs. Beyond the ESnet testbed, simplep2p's static per-pair pipes
  cannot model routing, congestion, loss, or programmable behavior — the things WAN
  research is about (and pre-computing an all-pairs matrix over a full network is both
  impractical and scientifically frozen). **This is the concrete justification for the
  whole refactor:** the WAN model is built by *composing* the Phase 4 primitives
  (`RoutingStrategy`, `Topology`, `VirtualChannelSet`/`BufferPool`, `LinkScheduler`)
  instead of copy-pasting a 3,800-line model. CODES already does routed,
  congestion-aware simulation at scale (the HPC fabric models), so the engine machinery
  is proven; the genuinely new part is the **congestion/transport** model — the
  Internet is lossy with end-to-end TCP-like control, vs HPC's lossless credit-based
  backpressure. Sequenced **after** Waves 1–3 harden the primitives. Wave 4 also
  carries the **explicit-connectivity workstream** (§8): the mapper-side service is
  designed against this model's actual queries and built incrementally alongside it,
  with the §8.5 verify-against-a-known-topology step done early. A staged
  simple→sophisticated congestion ladder, each rung useful on its own:
  1. **per-link queueing, open-loop** — packets route hop-by-hop and queue at busy
     links → delay under load. Reuses the existing per-port queue/buffer machinery and
     reverse-computes cleanly (local state). Already far beyond simplep2p. *(queueing
     congestion, not yet transport dynamics)*
  2. **finite buffers + drop** (drop-tail / simple AQM) — models loss, the defining
     Internet behavior HPC lacks. Still open-loop.
  3. **closed-loop transport** (TCP-like AIMD/CUBIC, RTT, retransmit) — real flow
     dynamics (fairness, sawtooth, incast). The big new piece; **no HPC analog**.
  4. **programmable routing/protocols** for FABRIC-style custom behavior.
  Rungs 1–2 ride on the Phase 4 primitives; rung 3 is where the new modeling
  concentrates. Open design questions in §13.

### 9.4 Wave 1 concrete steps

1. **Build:** confirm `CMAKE_CXX_STANDARD 17`; add new C++ sources to the `codes`
   library in `src/CMakeLists.txt`.
2. **Layer 0:** `codes/cpp/ross_lp.h` — `make_lptype<T>()` + trampolines +
   placement-new/dtor lifecycle (designed by the Phase 2 decisions, implemented here
   with its first consumer), plus the C++-aware `crv_checkpointer`.
3. **Layer 1 (lean):** `codes/cpp/reversible_rng.h`, `codes/cpp/rc_verifier.h`.
   (`RcStack`/`NetworkStats`/`Bandwidth` deferred to Wave 2 — synthetic LPs use
   plain int counters.)
4. **Layer 2:** `codes/cpp/workload_driven_lp.h` + `traffic_pattern.h` with the
   three patterns. Capture the real variation: destination selection + the small
   topology queries each synthetic file hardcodes (e.g. the dragonfly `num_nodes`
   formula).
5. **Run under the config-driven driver** (`Orchestrator`, *not* a new `main()`):
   register the new C++ LP types with the registry; let the driver instantiate them
   + their `TrafficPattern` from config. Retire the per-model `main()` boilerplate.
6. **Pilot then fold in:** port `model-net-synthetic.c` (dragonfly) first; verify;
   then re-express `-fattree`/`-slimfly`/`-dragonfly-all` as configurations of the
   same code. Keep existing executable names so downstream scripts keep working.

### 9.5 Reuse (do not reinvent)

`model_net_event` / `model_net_event_rc2`; `codes_local_latency` /
`codes_local_latency_reverse`; `codes_mapping_*`; `configuration_*`; `rc_stack` +
`crv_checkpointer` seams; `mn_stats` + `model_net_find_stats`; `bytes_to_ns`
(`common-net.c`); `codes_workload_*` vtable.

### 9.6 Verification

1. Build with C++17; the synthetic executables still link.
2. Golden output: run `tests/modelnet-test-{dragonfly,fattree,slimfly}-synthetic.sh`
   before vs. after; diff per-server send/recv counts and MiB/s.
3. RC correctness: `--sync=1` vs. `--sync=3` must match.
4. RC drift: enable the `RcVerifier` in a debug build on a small run.
5. The headline win: 4 near-duplicate `.c` files collapse to one shared
   implementation + small strategy/config files.

---

## 10. Phase 5 — Far future

- Decouple the `model_net_wrap_msg` union (adding a network currently edits a central
  union and sizes every event to the largest member).
- Modernize the workload API (`codes_workload_method`).
- Surrogate / ML integration **cleanup** (`zmqml`, `torch-jit`). *Note:* the surrogate's
  *config* home is not far-future — it is already reserved in the new format and firms
  up near-term on the SBIR path as the surrogate productionizes (contract §7); only the
  broader integration cleanup lives here.
- **Establish a performance baseline + tracking** (e.g. events/sec on fixed configs,
  recorded in CI). Deliberately deferred: no baseline exists today to regress against,
  and the equivalence harness checks outputs, not speed. Capture before/after numbers
  once the Wave 2/3 refactors of hot LP code begin in earnest.
- Broader, deliberate C→C++ migration of remaining subsystems.
- **Standardize data collection** onto a single mechanism — CODES-direct stats
  (`lp-io`, `counting_*`, sample files) and the ROSS instrumentation-layer callbacks
  are duplicative today (called out in Phase 3 as out of scope there).
- **Model self-description / manifest** so a third-party model (in a separate repo)
  can advertise its `ComponentType`, parameters (name, type, default, unit,
  user-facing?), and connectivity for NetMaestro to **ingest into its catalog**. The
  parameter-metadata piece dovetails with the Phase 4 model framework — a model that
  declares its parameters serves both the config compiler *and* NetMaestro.

---

## 11. Cross-cutting: the config-driven driver

An earlier prototyping effort produced a single **config-driven driver**
(`Orchestrator`) that runs any simulation from config + an LP-type registry,
replacing the per-model `main()` file that used to be copied for every new
experiment. It is pulled into CODES with the config work (Phase 3) and retained as
**the** entry point. In Phase 3 it is **decoupled from the prototype `Mapper`** and
drives `codes_mapping_setup()` (the compiled config feeding the existing mapping);
harvesting the prototype `Mapper`'s connectivity model is deferred to the Wave 4
connectivity workstream (§8).

It is also the natural plug-in seam for the Phase 4 hybrid design: the driver
constructs an LP plus its injected strategy from the `model:` name + YAML properties.
So Wave 3's "knock off another network model" becomes "register another LP class with
the registry" — never a new driver file again.

---

## 12. Decision log

| Decision | Rationale |
|---|---|
| Rewrite **in the CODES repo** (not a separate one) | Single build, single source of truth; `digital-twin` was only ever a personal prototype to harvest from |
| Variation via **hybrid (thin base + composition)** | Matches existing `model_net_method` pattern; POD-friendly state; testable; readable for rotating contributors. CRTP held in reserve for proven hot paths only |
| RC bundle: **RNG wrapper + verify harness + RcStack** solid; **RcJournal** prototype | High-value/low-risk; verification makes hand-written reverse handlers safe; avoid perf-killing full state-saving |
| **Incremental** config adapter (YAML → existing mapping) | Mapping is highest-risk; decouple format adoption from the mapping rewrite; keep old `.conf` in parallel |
| Adapter **scoped narrowly** | Bridge only the configs the YAML examples target; verify by equivalence |
| First class wave = **synthetic LPs**; Wave 2 = **simplep2p** | Synthetic = safest proof of the architecture; simplep2p = clean self-contained model that also delivers the SBIR's ESnet-testbed artifact (2 sites ≈ point-to-point) |
| **simplep2p ≠ general WAN model; a new routing+congestion WAN model is Wave 4** | simplep2p has no runtime routing (direct per-pair pipes, `simplep2p.c:670`); fine for the ESnet testbed, but FABRIC/WAN research needs routing, congestion, loss — built by composing Phase 4 primitives. The WAN model is the refactor's concrete payoff |
| **C++17** baseline | Low risk; already used with Torch; enables modern idioms |
| Keep the **config-driven driver** | Eliminates per-model `main()` copying; is the plug-in seam for new LP classes |
| Optional deps use **tri-state style** (AUTO/ON/OFF; strict ON errors) | Reproducible configs; can disable a found dep; CI fails loudly when a requested dep is missing |
| Consume ROSS via **`find_package(ROSS CONFIG)`**; drop pkg-config + `ENV{PKG_CONFIG_PATH}` hack | ROSS now correctly exports a CMake package; target-based is the correct idiom |
| Phase 1 includes a **CMake modernization pass** (target-based, modern MPI, install/export) | Current build is dated/non-idiomatic; fix the foundation before layering on |
| **Drop DAMARIS build support, keep the code** | ROSS master removed the damaris build path; RISA rewrite is future — keep `#ifdef USE_RDAMARIS` code, remove only the build machinery |
| ROSS pinned via a **pinned clone built in CI** (not a submodule, not imaged) | ROSS is quick to build and the most-bumped pin — keep it a workflow variable so bumps need no image rebuild (cache by commit) |
| CI = **GitHub Actions**; ROSS in-CI, **heavy deps in Docker images**; full matrix on every MR | Separates the fast-moving pin (ROSS) from slow stable deps (imaged); core job needs no custom image; full builds stay cheap, no nightly tier |
| **python2 confined to the UNION feature** (full image, build-time only) | conceptual needs py2 for codegen; SWM-only online + everything else don't; UNION is optional/off the critical path |
| Equivalence diff: **`lp-io` now → results digest as models are rewritten**; skip fragile stdout | `lp-io` is structured/per-LP; stdout scraping is brittle; stdout-only models keep the aggregate event-count check until they get a digest |
| **Rich, placement-new'd LP state** (trampoline constructs/destructs; C++-aware `crv_checkpointer`) | Fixes current UB (assigning into unconstructed STL members); clean "state is the object" model; safe under reverse computation |
| C++ **snake_case** names, **`.cxx`** sources | Match existing CODES C style; `.cxx` avoids `.C`/`.c` clash on case-insensitive filesystems; harvested CamelCase config code gets renamed |
| **Fully co-located header+impl, big-bang reorg** (in Phase 1, after CI) | One clean break beats incremental smear; mechanical + CI-verifiable; mechanical-only + forwarding shims + timed for low branch activity; defines the public API surface |
| Phase 3 front-end is a **config compiler** (friendly YAML → resolved internal config → existing pipeline) | YAML intentionally omits implementation details; downstream `codes_mapping`/models unchanged; verify via compiled-tree `cf_equal` + behavioral equivalence |
| Plug in at the **`ConfigVTable` seam**; defer **connectivity** to the Wave 4 workstream | The prototype failed by changing format **and** mapping together; the seam keeps blast radius minimal and reuses the whole existing query pipeline |
| Explicit connectivity = **extend mapping** (harvest the prototype's graph idea), **not** replace `codes_mapping`; additive, narrow-first | The format already runs via Phase 3; the only real gap is explicit connectivity for irregular WANs. `codes_mapping` has no connectivity API; models derive neighbors themselves. Maturing the prototype wholesale would re-drop group/rep/annotation (§7.2) and force an every-call-site migration to *lose* proven semantics. An additive connectivity service beside `codes_mapping` keeps blast radius low; full replacement demoted to optional/future |
| The connectivity work is a **Wave 4 workstream, not a standalone phase** (the former mapping phase is dissolved) | Its only real consumer is the Wave 4 WAN model — Waves 1–2 run off the Phase 3 compiled link table and HPC models derive their own neighbors. Designing the query API against its real consumer ("born from real consumers") beats building it in a vacuum, and it can grow incrementally: SBIR-minimum first, rest time-pending |
| **Cytoscape** compatible topology for WANs (JSON ⊂ YAML, one parser), explicit units, per-edge bw/latency | Established format NetMaestro's editor exports natively; drops DOT/`libcgraph` from prototype and the error-prone latency/bandwidth matrices |
| **Custom components + per-node overrides**; users never write annotations | Matches NetMaestro's catalog→instance→place flow; subsumes `@annotation` (the compiler may emit annotations internally to drive the existing mapping) |
| Hide engine knobs (`message_size`, `modelnet_order`, `pe_mem_factor`, rep counts) as **derived**; everything else defaulted + overridable | Friendly to non-PDES users; e.g. `message_size` is the ROSS event-blob size, which should be an implementation detail and never a user concept |
| Phase 1 **split: 1a safety net** (CI, ROSS pin, equivalence, C++17, unit-test framework) **gates Phase 3; 1b build hygiene** (CMake modernization + reorg, presets, tri-state, images, install/export) doesn't | The SBIR-critical Phase 3 shouldn't queue behind build hygiene; by the §2 criteria most of 1b gates nothing — it may run in parallel with Phase 3 |
| **Unit-test framework in 1a**; compiler-validation tests are a Phase 3 deliverable | Equivalence protects refactors, not new code — the config compiler's negative paths (redundant shape, bad units, hand-written derived values) need real unit tests |
| **Automated style enforcement in Phase 2** (clang-format CI gate + clang-tidy starter profile) | The diagnosed root cause is style drift from rotating contributors; a conventions doc alone doesn't survive them — CI gates do |
| **Trampoline implemented in Wave 1**, not Phase 2 (design decisions stay in Phase 2) | Born from its first real consumer; built cold it risks redesign on first contact |
| simplep2p YAML initially **references the existing matrix files by path** | The seam delivers paths and the model `fread`s them itself (`simplep2p.c:871`); zero model changes. The per-edge form can later compile to *emitted* matrix files; whether to modernize simplep2p at all is revisited once the WAN model exists |
| Synthetic params stay **CLI-only initially → later config-or-CLI, CLI takes precedence**; every run dumps the resolved config incl. overrides | They're ROSS options today, not config; parameter sweeps shouldn't need near-identical config copies; the resolved-config dump keeps every result traceable |
| **Job placement (jobmap) ≠ LP mapping** — the format drives the existing `codes-jobmap` as-is | Placing a job's ranks on *simulated* nodes is a model concern; placing/routing LPs is an engine concern. Multi-job support doesn't wait on the connectivity/mapper work (contract §6.4) |
| **Performance baseline deferred** to far future (§10) | No baseline exists today to regress against; recorded as a decision, not an oversight |

---

## 13. Open questions

- **Config-format specifics** (Phase 3): largely settled — see §7 and
  [new-config-format.md](new-config-format.md).
  **Remaining:** the per-model canonical input sets; placement-policy set + jobmap
  binding; per-source workload schemas beyond `synthetic`; the future **shared generator
  → elements utility** (where it runs / what language) and NetMaestro HPC visualization;
  the **very-large** generated source; the wire-format micro-decisions in
  new-config-format.md §13.
- **Unit-test framework choice** — Catch2 vs GoogleTest (Phase 1a, Workstream 6).
- **Deprecation window** for the old `.conf` (how many releases?).