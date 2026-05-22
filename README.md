# CODES Discrete-event Simulation Framework

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

- **MPI**: MPICH for parallel execution (OpenMPI is not supported by Union, a dependency)
- **CMake**: Version 3.12 or higher
- **ROSS**: Rensselaer Optimistic Simulation System (handled by script)
- **C/C++ compiler**: GCC or Clang with C++11 support

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
  -DCMAKE_C_COMPILER=mpicc \
  -DCMAKE_CXX_COMPILER=mpicxx \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON

# 4. Build and test
make -j
ctest
```

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
