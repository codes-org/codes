# CODES Discrete-event Simulation Framework

[![Build](https://github.com/codes-org/codes/actions/workflows/build.yml/badge.svg)](https://github.com/codes-org/codes/actions/workflows/build.yml)

A high-performance discrete-event simulation framework for modeling HPC system architectures, network fabrics, and storage systems. Built on top of ROSS (Rensselaer Optimistic Simulation System) for massively parallel simulation capabilities.

## Quick Start

The easiest way to build CODES is using our automated compilation script that handles all dependencies and configurations.

1. **Download the compilation script** [click here](https://raw.githubusercontent.com/codes-org/codes/master/CODES-compile-instructions.sh) or:

   ```bash
   wget https://raw.githubusercontent.com/codes-org/codes/master/CODES-compile-instructions.sh
   ```

2. **Edit and Run the script**:
   ```bash
   bash ./CODES-compile-instructions.sh
   ```

The script will create a new directory with all dependencies and CODES compiled and ready to use.

## Prerequisites

- **MPI**: MPICH or OpenMPI (UNION, an optional workload dependency, requires MPICH)
- **CMake**: Version 3.17 or higher
- **ROSS**: Rensselaer Optimistic Simulation System (handled by script)
- **C/C++ compiler**: GCC or Clang with C++17 support

Optional dependencies (automatically handled by script if enabled):
- **UNION**: For advanced workload generation
- **SWM**: For structured workload modeling
- **Argobots**: Threading library for enhanced performance
- **PyTorch**: For ML model integration (if enabled)

## Manual Installation

For advanced users who prefer manual installation:

```bash
# 1. Build and install ROSS first
git clone https://github.com/ross-org/ROSS.git
cd ROSS && mkdir build && cd build
cmake .. -DCMAKE_INSTALL_PREFIX=$HOME/ross
make -j && make install
cd ../..

# 2. Clone and build CODES
git clone https://github.com/codes-org/codes.git
cd codes && mkdir build && cd build

# 3. Configure with CMake
cmake .. \
  -DCMAKE_PREFIX_PATH=$HOME/ross \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON

# 4. Build and test
make -j
ctest
```

MPI is auto-discovered via `find_package(MPI)` — do **not** set `CC=mpicc` or
`-DCMAKE_C_COMPILER=mpicc`. Just make sure an MPI implementation is installed and
its wrapper (`mpicc`) is on your `PATH` (e.g. `module load mpich`). For a
non-standard install, hint with `-DMPI_HOME=/path/to/mpi`.

### Locating optional dependencies

The optional pkg-config dependencies (SWM, UNION, argobots) are found with
`pkg_check_modules`. There are two supported ways to point at copies installed
outside the default search paths:

**1. `CMAKE_PREFIX_PATH` (recommended)** — add each install *prefix*; the same
variable that locates ROSS. `pkg_check_modules` searches `<prefix>/lib/pkgconfig`
(and `share/pkgconfig`, `lib64/...`) under it automatically. Use this when the
dependency is installed normally (its `.pc` lives under `<prefix>/lib/pkgconfig`):

```bash
cmake .. \
  -DCMAKE_PREFIX_PATH="$HOME/ross;/opt/swm;/opt/union;/opt/argobots" \
  -DCODES_USE_UNION=ON
```

**2. `PKG_CONFIG_PATH` (environment)** — point directly at the *directories
containing the `.pc` files*. Use this when a dependency's `.pc` is **not** under a
standard `<prefix>/lib/pkgconfig` — e.g. an un-installed in-tree build like SWM's
`build/maint/swm.pc`, which `CMAKE_PREFIX_PATH` cannot reach:

```bash
export PKG_CONFIG_PATH="/path/to/swm/build/maint:/path/to/argobots/build/maint:$PKG_CONFIG_PATH"
cmake .. -DCODES_USE_SWM=ON
```

(`CODES-compile-instructions.sh` uses this form, since it builds the deps in
place.)

> **Deprecated:** the per-dependency `SWM_PKG_CONFIG_PATH`,
> `UNION_PKG_CONFIG_PATH`, and `ARGOBOTS_PKG_CONFIG_PATH` cache variables still
> work for one release cycle (each is prepended to `PKG_CONFIG_PATH`) but emit a
> deprecation warning. Migrate to `CMAKE_PREFIX_PATH` (or `PKG_CONFIG_PATH` for
> non-standard `.pc` locations); they will be removed.

## Building with CMake presets

`CMakePresets.json` ships ready-to-use build configurations so you don't have to
remember the flags above. Presets require CMake ≥ 3.21.

First make ROSS discoverable. Point `ROSS_ROOT` at the prefix you installed ROSS
into (or set `CMAKE_PREFIX_PATH`, or `module load ross` on HPC):

```bash
export ROSS_ROOT=$HOME/ross
```

Then configure, build, and test by preset name — no `-S`/`-B`/`-G` flags needed:

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Available configure / build / test presets:

| Preset | Description |
| --- | --- |
| `debug` | Debug build; optional deps (DUMPI/SWM/UNION/Torch/ZeroMQ) auto-detected |
| `release` | Optimized build |
| `relwithdebinfo` | Optimized build with debug symbols |
| `coverage` | Debug + `--coverage` instrumentation (mirrors the coverage CI job) |
| `full-ci` | All optional deps forced on, with dep paths hardcoded to the `ghcr.io/codes-org/codes-ci-full` image (`/opt/<dep>`). Meant to run inside that image; CI-only |
| `full-local-debug` | Debug build, all optional deps forced on, with dep paths taken from environment variables (see below). Use this to build everything locally |
| `full-local-release` | Same as `full-local-debug` but an optimized build |

Build trees land in `build/<preset>/` and installs in `install/<preset>/` (both
git-ignored). Override any cache variable inline, e.g. to force a dependency off:

```bash
cmake --preset debug -DCODES_USE_ZEROMQ=OFF
```

### Building with all heavy dependencies locally

The `full-local-debug` / `full-local-release` presets force every optional
subsystem on (SWM, UNION, DUMPI, Torch, ZeroMQ) and read each dependency's
location from an environment variable, so you can point them at wherever the deps
are installed on your machine — no `/opt` assumption:

| Variable | Set to the install prefix of |
| --- | --- |
| `CODES_SWM_DIR` | SWM (expects `$CODES_SWM_DIR/lib/pkgconfig`) |
| `CODES_UNION_DIR` | UNION (expects `$CODES_UNION_DIR/lib/pkgconfig`) |
| `CODES_ARGOBOTS_DIR` | Argobots (expects `$CODES_ARGOBOTS_DIR/lib/pkgconfig`) |
| `CODES_DUMPI_DIR` | DUMPI (the dir containing its `include/` and `lib/`) |

Torch is discovered separately via `Torch_DIR`, the same as in CI:

```bash
export ROSS_ROOT=$HOME/ross
export CODES_SWM_DIR=$HOME/deps/swm
export CODES_UNION_DIR=$HOME/deps/union
export CODES_ARGOBOTS_DIR=$HOME/deps/argobots
export CODES_DUMPI_DIR=$HOME/deps/dumpi
export Torch_DIR="$(python3 -c 'import torch; print(torch.utils.cmake_prefix_path)')/Torch"

cmake --preset full-local-release
cmake --build --preset full-local-release
ctest --preset full-local-release
```

Because every `CODES_USE_<dep>` is `ON`, an unset or wrong path is a hard error at
configure time (`CODES_USE_<dep>=ON but <dep> could not be found/enabled.`) rather
than a silently-skipped dependency. If you only want a subset, use the plain
`debug` / `release` presets — those auto-detect each dep and skip what's missing.

For personal tweaks (a fixed ROSS path, extra dependency paths, a different
generator), create a `CMakeUserPresets.json` that `inherits` from these — it is
git-ignored and never committed.

## Testing

Check your installation with:

```bash
# Run all tests
cd codes/build && ctest

# Run specific tests
ctest -R modelnet-test-dragonfly
ctest -R union-workload-test-surrogate

# Keep test output for inspection
DONT_DELETE_TEST_DIR=1 ctest -R your-test-name
```

All tests pass to date of writing, including those that require UNION support. Tests verify:

- Network model correctness and determinism
- Workload generation and replay accuracy
- Multi-fidelity simulation switching
- Parallel execution and reverse computation
- Configuration file parsing and LP setup

## Basic Usage

Running a CODES experiment is tricky due to the large amount of compontents that have to be correctly configured. Please use the [experiments repo](https://github.com/CODES-org/experiments) for examples of simulation you can run.

If you have used the compilation script from above (quick start) run the following (in the folder that contains `CODES-compile-instructions.sh`):

```bash
git clone https://github.com/CODES-org/experiments
```

To run an experiment do:

```bash
cd experiments
bash run-experiment.sh path-to-experiment/script.sh
```

A folder will be created under `path-to-experiment/results` containing the result of running the experiment.

## Features

CODES provides comprehensive simulation capabilities for:

### Network Topologies
- **Dragonfly**: High-radix interconnect with adaptive routing (most up to date)
- **Torus**: Multi-dimensional torus networks
- **Fat-tree**: Hierarchical tree topologies
- **Express Mesh**: Enhanced mesh networks
- **Simple P2P**: Point-to-point networks

### Workload Generation
- **SWM and UNION**: Workload generation
- **MPI trace replay**: Support for DUMPI traces
- **Synthetic patterns**: Uniform random, nearest neighbor, and custom patterns

### Multi-fidelity Simulation
- **Network surrogate models**: Switch between high-fidelity and surrogate modes
- **Application surrogate models**: Accelerate application-level simulation
- **Adaptive directors**: Intelligent switching between simulation modes

## Contributing

For naming, file extensions, header guards, and how LP state must be constructed,
see the [coding conventions](doc/dev/conventions.md). Formatting is machine-checked
(below).

### Code formatting

This repo uses [clang-format](https://clang.llvm.org/docs/ClangFormat.html) to keep C/C++ style consistent. The rules live in [`.clang-format`](.clang-format) at the repo root; configure your editor to pick it up:

- **VS Code:** install the C/C++ extension and enable `editor.formatOnSave` for C/C++ (or use Command Palette → "Format Document").
- **CLion / IntelliJ:** uses `.clang-format` by default.
- **Vim:** see [vim-clang-format](https://github.com/rhysd/vim-clang-format), or run `:!clang-format -i %`.
- **Emacs:** see [clang-format.el](https://clang.llvm.org/docs/ClangFormat.html#emacs-integration).

To reformat a file manually: `clang-format -i path/to/file.c`. CI runs `clang-format --dry-run --Werror` on every PR and rejects any drift, so PRs with unformatted code don't merge.
Note: The CI uses clang-format major release version 20, so you should format your files with that version.

### Determinism

Before contributing please run the full test suite. Some tests verify our determinism guarantees (every simulation should be reproducible), i.e, the number of net events processed between two runs in parallel mode should be the same. We want to keep our determinism guarantees forever. Non-deterministic simulations are often the result of faulty reverse handlers, which have caused serious bug failures and hundreds of hours of debugging.

If you find yourself with a model that is not deterministic (two runs with the same initial configuration produce different numbers of net events), then you can check for errors in the reverse handlers via the ROSS feature: reverse handlers check. For this, run your model with `--synch=6`. Make sure that all LPs in the simulation (ie, routers, terminals and others) have implemented proper reversibility checks (defined in a struct of type `crv_checkpointer`).

## License

See LICENSE file for licensing information.

## Credits

Developed by Argonne National Laboratory and Rensselaer Polytechnic Institute, with collaborations from UC Davis and Lawrence Livermore National Laboratory.

## About CODES

Discrete event driven simulation of HPC system architectures and subsystems has emerged as a productive and cost-effective means to evaluating potential HPC designs, along with capabilities for executing simulations of extreme scale systems. The goal of the CODES project is to use highly parallel simulation to explore the design of exascale storage/network architectures and distributed data-intensive science facilities.

Our simulations build upon the Rensselaer Optimistic Simulation System (ROSS), a discrete event simulation framework that allows simulations to be run in parallel, decreasing the simulation run time of massive simulations to hours. We are using ROSS to explore topics including large-scale storage systems, I/O workloads, HPC network fabrics, distributed science systems, and data-intensive computation environments.

The CODES project is a collaboration between the Mathematics and Computer Science department at Argonne National Laboratory and Rensselaer Polytechnic Institute. We collaborate with researchers at University of California at Davis to come up with novel methods for analysis and visualizations of large-scale event driven simulations. We also collaborate with Lawrence Livermore National Laboratory for modeling HPC interconnect systems.

## About this README

Claude helped us in templating this doc. Any typos are our own and after the fact.
