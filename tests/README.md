# CODES tests

Everything CTest runs lives here. There are two distinct kinds of test in this
directory, and they are built and run differently:

| Kind | Runs under | Registered by | Asserts |
| --- | --- | --- | --- |
| **Unit tests** | the process directly (serial, **no MPI**) | `add_test` on a GoogleTest binary | in-process `EXPECT_*`/`ASSERT_*` on a single component |
| **Model / integration tests** | `mpiexec` (1–N ranks) | the `codes_add_*` helpers (or a legacy shell script) | clean exit + a marker line, determinism, or per-LP `lp-io` equivalence |

Run the whole suite the same way regardless:

```bash
ctest --preset debug                 # from a configured build; add --output-on-failure to see logs
ctest -R codes-config-compiler-test  # a single test by name / regex
ctest -N                             # list every registered test without running
```

Both kinds only build when `BUILD_TESTING` is `ON` (the `debug` preset sets it;
`ctest --preset debug` is what CI runs).

---

## Unit tests (GoogleTest)

Unit tests exercise one component in-process, with no simulator run and no MPI.
They are the right tool for a parser, a data-structure invariant, a mapping
function — anything you can call directly and check the return value of. Model
behavior that only emerges from a full simulation belongs in the model tests
below, not here.

### Framework

[GoogleTest](https://google.github.io/googletest/) is **vendored** under
[`thirdparty/googletest/`](../thirdparty/googletest/) (see
[`thirdparty/UPDATING.md`](../thirdparty/UPDATING.md) for how it's updated). It
is built only when `BUILD_TESTING` is on, via the thirdparty dispatcher, and
exposes the standard `GTest::gtest` / `GTest::gtest_main` targets. Nothing needs
to be installed on the machine.

### Conventions

- **Location:** the `.cxx` source sits directly in `tests/`.
- **Naming:** name the file `<area>-test.cxx` and give the target the same stem
  (e.g. `codes-config-compiler-test.cxx` → target `codes-config-compiler-test`).
  Use `.cxx`, never `.C` — see
  [coding conventions](../doc/dev/conventions.md#file-extensions).
- **Serial only:** unit tests do **not** launch under MPI. Do not call
  `MPI_Init`; if a component genuinely needs a communicator, it belongs in a
  model test.
- **Link `GTest::gtest_main`** so you don't write your own `main()`, and link
  `codes` when the test calls into the library. A component deliberately built
  ROSS/MPI-free can instead be compiled straight into the test binary with no
  `codes` link at all — the config-compiler test does this (see below).

### Adding one

A unit test is a single `.cxx` file holding one or more `TEST()` cases and no
`main()` of its own — linking `GTest::gtest_main` supplies one. The minimal
shape is just:

```cpp
#include <gtest/gtest.h>

TEST(MyComponent, DoesTheThing) {
    EXPECT_EQ(1 + 1, 2);
}
```

Register it in [`CMakeLists.txt`](CMakeLists.txt) with three lines — link
`GTest::gtest_main`, adding `codes` when the test calls into the library:

```cmake
add_executable(my-component-test my-component-test.cxx)
target_link_libraries(my-component-test PRIVATE GTest::gtest_main)  # + codes when needed
add_test(NAME my-component-test COMMAND my-component-test)
```

The real consumer,
[`codes-config-compiler-test.cxx`](codes-config-compiler-test.cxx) (target
`codes-config-compiler-test`), takes a different path: it does **not** link
`codes`. The YAML config-compiler core
(`src/modelconfig/config_compiler.cxx`) is ROSS/MPI-free by design, so the test
compiles that source directly into the binary together with the ryml
amalgamation TU and asserts on the returned `compiled_config` — no simulator, no
MPI. Reach for this compile-the-component-directly pattern when a component is
deliberately self-contained; otherwise the link-`codes` recipe above is the
default.

That one binary is assembled from three per-area source files —
[`codes-config-compiler-test.cxx`](codes-config-compiler-test.cxx) (the core
`ConfigCompiler` suite), [`codes-unit-convert-test.cxx`](codes-unit-convert-test.cxx)
(unit parsing/conversion), and
[`codes-config-workload-test.cxx`](codes-config-workload-test.cxx) (the
`simulation:` / `workload:` / `jobs:` config surface). GoogleTest registers
tests across translation units, so all three compile into the single
`codes-config-compiler-test` target and `--gtest_filter` still sees every suite;
fixtures used by more than one of them live in
[`config-compiler-test-util.h`](config-compiler-test-util.h). Split a large
gtest source this way once it spans several independent areas.

The test binary is a normal GoogleTest executable, so it takes the usual flags
directly (`./codes-config-compiler-test --gtest_filter=ConfigCompiler.* --gtest_repeat=10`)
when you want to run a subset outside CTest.

---

## Model & integration tests

These launch a CODES model under `mpiexec` and check its output. Adding one is a
single call to a helper in [`CMakeLists.txt`](CMakeLists.txt); the helpers'
header comments there are the authoritative reference for their arguments. In
brief:

- **`codes_add_run_test`** — run a binary once; pass on clean exit (optionally
  require a `MARKER` line, and/or generate the config in a `SETUP` script). The
  workhorse for single-run smoke tests.
- **`codes_add_equivalence_test`** — run a model two-or-more times and assert a
  marker line (default `Net Events Processed`) is identical across runs. Used
  for reproducibility/determinism checks.
- **`codes_add_lpio_equivalence_test`** — run a model once per config, each with
  its own `--lp-io-dir`, and diff the per-LP `lp-io` output. The canonical way
  to prove two configs are behaviorally equivalent — it is what proves a new
  YAML config byte-identical per-LP to the `.conf` it replaces, now that the
  YAML front-end has landed: the suite registers many `.conf`-vs-`.yaml` pairs
  (simplenet, simplep2p, loggp, torus, express-mesh, slimfly, the dragonfly
  variants, lsm). `CONFIG_FLAG` handles binaries that take the config as a ROSS
  option (e.g. `--conf=`) instead of the trailing `-- <config>` positional. Not
  every `.conf` gets a twin: `tests/workload/codes-workload-test.conf` is
  intentionally `.conf`-only — a PARAMS-only key/value bag with no `LPGROUPS`
  block, a shape the YAML format deliberately does not express (every YAML form
  defines a topology).
- **`codes_add_config_equivalence_test`** — the fallback to the lp-io helper for
  the cases a per-LP diff can't handle: a binary that emits no `lp-io` at all
  (e.g. `resource-test`), or one whose `lp-io` isn't reproducible run to run
  (e.g. `fattree`'s switch stats, alongside a wall-clock-derived `sim_log.txt`).
  It runs the two configs and compares a marker line (default `Net Events
  Processed`) instead of diffing `lp-io`. Like the lp-io helper it supports
  `CONFIG_FLAG`; like `codes_add_equivalence_test` it also supports `SETUP`
  (generate both config twins per run) and `REQUIRE` (a line that must appear in
  every run). Prefer the lp-io helper wherever the model's `lp-io` is
  reproducible — a per-LP diff is a strictly stronger check.
- **`codes_add_config_tree_test`** — assert a `.conf` and its YAML twin compile
  to the **same config tree**, through the production loader + `cf_equal_report`.
  It runs **no model**, so it is far cheaper than the behavioral equivalence
  tests and checks *every* section/key/value rather than a marker or lp-io side
  effect — catching defaulting/derivation drift a short run can mask. Use it
  **alongside** the lp-io / marker equivalence test for a twin, not instead of
  it: this proves the two trees are identical, those prove the model behaves
  identically.

A handful of tests remain as per-scenario **shell scripts** (the
`test-shell-files` list in `CMakeLists.txt`) because they do custom output
processing the helpers don't cover yet — `mapping_test.sh` (golden diff against
[`expected/mapping_test.out`](expected/)), the `example-ping-pong-surrogate-*`
scripts (sed-normalized per-LP stdout diffs), and the `USE_UNION`
`union-workload-test-surrogate*` set (labeled `nightly`, so PR/push CI skips
them and only the scheduled full-lane build runs them).

### Asserting a run aborts (the expected-failure pattern)

The `codes_add_*` helpers all expect a **clean exit**, so none of them can
express "this config *should* be rejected." A test that needs to prove a run
correctly aborts instead uses a raw `add_test` running the binary, plus
`set_tests_properties(... PROPERTIES PASS_REGULAR_EXPRESSION "<diagnostic>")`:
CTest passes the test when the output **matches the regex** and, crucially,
**ignores the process exit code entirely** — so an intentional abort still
passes.

Two tests on this branch use it: `config-tree-divergence` feeds
`config-tree-equivalence-test` two configs that are *not* twins and matches
`cf_equal_report`'s divergence text (`values differ` / `only in ... tree`), and
`modelnet-synthetic-rejects-multi-job-config` runs a multi-job `jobs:` config
the synthetic mains cannot execute and matches the guard's abort message.

Reach for this whenever the thing under test is a **rejection** — a config that
must be refused, a run that must abort with a clear diagnostic — which the
clean-exit helpers cannot express. The one caveat: because
`PASS_REGULAR_EXPRESSION` overrides exit-code checking outright, the regex must
be specific enough to match **only** the intended diagnostic; a loose pattern
would let any output containing those words — including an unrelated failure —
count as a pass.

---

## What CI catches — and what it doesn't

`ctest --output-on-failure` is a real gate, but be clear about what a green run
actually proves, so a passing suite isn't mistaken for full behavioral coverage:

- **Unit tests** assert exactly what their `EXPECT_*`/`ASSERT_*` lines say — the
  YAML config compiler is covered this way (`codes-config-compiler-test`).
- **Single-run model tests** (`codes_add_run_test`) assert only a **clean exit**
  (plus a non-empty `ross.csv`, and a `MARKER` line when one is given). A
  numeric-output regression that still exits 0 will pass. These catch crashes,
  aborts, and config-parse failures — not wrong-but-plausible results.
- **Determinism tests** (`codes_add_equivalence_test`) catch *non-reproducibility*
  — a run that differs from itself — but not a change that shifts every run's
  result the same way.
- **Per-LP equivalence** (`codes_add_lpio_equivalence_test`) is the strongest
  check, but it only covers models that emit `lp-io`. It backs the
  `.conf`-vs-`.yaml` checks for simplenet, simplep2p, loggp, torus,
  express-mesh, slimfly, the dragonfly variants, and both lsm configs — diffing
  every per-LP file. A model whose only observable output is **stdout** is out
  of its reach.
- **Marker-based config equivalence** (`codes_add_config_equivalence_test`) is
  the fallback where a per-LP diff can't be used: `resource` emits no `lp-io`,
  and `fattree`'s binary drops a wall-clock-derived `sim_log.txt` into its
  `lp-io` dir (the switch stats themselves *are* reproducible; that file isn't).
  Both compare the `Net Events Processed` marker instead — weaker than a per-LP
  diff, used only where the stronger check doesn't apply.
- **Config-tree equivalence** (`codes_add_config_tree_test`) is a **static**
  check: it loads a `.conf` and its YAML twin through the production loader and
  asserts the compiled trees are identical (every section/key/value), running no
  model. It catches defaulting/derivation drift a short behavioral run can mask,
  but proves only that the two *configs* agree — not that the model behaves
  correctly — so it runs **alongside** the behavioral checks above, never in
  their place.

### The stdout-only coverage gap

The canonical per-LP mechanism compares `lp-io` files. Models whose behavior is
only visible on **stdout** are not covered by it, and fall back to the fragile
approaches this suite is trying to retire:

- `example-ping-pong-surrogate-{1,2,3}.sh` scrape per-LP numbers out of stdout
  with `grep`/`sed` and diff the normalized text. This breaks whenever a log
  line's format changes, and only covers the fields the script happens to grep.
- `mapping_test.sh` diffs full stdout against a committed golden file
  (`expected/mapping_test.out`) — sensitive to any incidental log change.

The same stdout-only limit is why the tutorial ping-pong's `.conf`-vs-`.yaml`
checks (`example-ping-pong-config-equivalence` and
`example-ping-pong-surrogate-config-equivalence`) are marker-based
(`codes_add_config_equivalence_test`) rather than per-LP `lp-io` diffs:
`tutorial-synthetic-ping-pong` writes its per-LP results only to stdout, so the
`Net Events Processed` marker is the strongest thing available to compare the
two config formats on.

Closing this gap means teaching the surrogate path (and other stdout-only
models) to emit `lp-io`, then migrating these scripts onto
`codes_add_lpio_equivalence_test`. Until then, treat "the surrogate scripts
pass" as "it ran and printed roughly the right thing," not "its per-LP numbers
are verified."
