# Reproducing results of PADS23 paper

This document contains the instructions to follow in order to compile, run the experiments
and generate the figures and table that appears on the paper: Hybrid PDES Simulation of
HPC Networks Using Zombie Packets, by Cruz-Camacho et. al 2023.

The artifacts associated with this submission are:

- The PDES simulator [ROSS](https://github.com/ross-org/ross) (Licensed under the
    BSD-3-clause licence)
- The HPC network simulator [CODES](https://github.com/codes-org/codes) (Licensed under
    the BSD-3-clause licence)

All models included with the simulators are licensed under the same licence, namely
BSD-3-clause.

A copy of these artifacts are available via [Zenodo](https://about.zenodo.org) with
[doi:10.5281/zenodo.7879224](https://doi.org/10.5281/zenodo.7879224). Zenodo's policies on
long-time storage and availability of the artificats can be found in:
<https://about.zenodo.org/policies/>.

The code has been tested in two systems: a 20-core IBM Power9 processor (using 9 of its
cores), and an Intel core i7 vPro 8th Gen (a change in the number of available cores/slots
was needed in `experiments` as the processor does not have 9 available
cores).

## Build

To compile CODES (and ROSS), you need a CMake, and a C and C++ MPI-aware compiler.

We have succesfully compiled CODES in a system with a XLC_r compiler (version 16.1.1) and
the Spectrum MPI (version 10.4) library, and in a x64 system with GCC (12.2.1) and Open
MPI (4.1.5).

We assume that all commands are executed under base CODES directory:

```bash
cd path-to-this/CODES
```

First compile ROSS:

```bash
mkdir ROSS/build
pushd ROSS/build
cmake .. -DROSS_BUILD_MODELS=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DCMAKE_INSTALL_PREFIX="$(pwd -P)/bin" -DCMAKE_CXX_COMPILER=mpicxx \
    -DCMAKE_C_COMPILER=mpicc -DCMAKE_BUILD_TYPE=Debug
make
make install
popd
```

Then compile CODES:

```bash
mkdir build
pushd build
cmake .. -DCMAKE_PREFIX_PATH="$PWD/../ROSS/build/bin" \
    -DCMAKE_CXX_COMPILER=mpicxx -DCMAKE_C_COMPILER=mpicc \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_BUILD_TYPE=Debug \
    -DBUILD_TESTING=ON -DCMAKE_INSTALL_PREFIX="$(pwd -P)/bin"
make
# make install is NOT necessary
popd
```

## Run and generate figures/tables

The experiments, figure generation and table generation are contained in the script
`reproduce.sh`. The script calls the bash scripts in `experiments` which run the CODES
binary. If there is a need to change any parameter on the experiments (eg, number of
cores), these files are the place to do so.

Python 3 is needed to generate the figures. The Python libraries: NumPy and matplotlib are
also required. (Tested on Python 3.10, NumPy 1.24.2 and Matplotlib 3.7.1.) An additional
external tool is `wc`, which is used to count the total number of lines/packets in the
simulation. (Tested on GNU `wc` versions 8.3 and 9.2.)

To run the script simply:

```bash
cd scripts/reproducibility-pads23/
bash -x reproduce.sh
```

The total runtime for the script is dependent on machine resources. A runtime of 30
minutes has been reported for a system running on Intel i9-12900K (16 cores, 5.20 GHz),
while for smaller systems, like Intel i7-8650U (4 cores, 4.2 GHz), the runtime has been of
around 2 to 4 hours. The experiments take up to 3 GBs of space in disk. If CODES was
compiled in a folder other than the one suggested (`build/`), you must change the variable
`CODES_BUILD_DIR` in the script.

### Results

The figures can be found in the directory `figures` and the table results in the file text
`results/sumarized-table.txt`
