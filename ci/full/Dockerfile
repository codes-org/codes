# Heavy-dependency image for the CODES "full" CI job.
#
# Built on amd64 in CI and pushed to GHCR; the `full` job runs IN this image and
# only builds ROSS + CODES, so the slow dependency compiles happen once here
# instead of on every PR.
#
# Base: ubuntu 22.04 (jammy) — it ships python2 (needed by conceptual/UNION, a
# later stage) AND cmake 3.22 (>= CODES's 3.17 minimum). noble/24.04 drops
# python2.
#
# Built up one dependency at a time (each its own cached layer):
#   - MPICH + build toolchain + argobots + SWM
#   - python2 + conceptual + UNION + ZeroMQ (the director-client path)
#   - SST-DUMPI + LibTorch (CPU)
#   - ZMQML server Python runtime (pyzmq/numpy/pandas/scikit-learn) for the
#     zmqml-hybrid workflow (.github/workflows/zmqml-hybrid.yml)
# For the build.yml `full` job, Torch and DUMPI are build-only checks: no trace
# data / no model ships, so that job just verifies CODES compiles + links with
# USE_DUMPI / USE_TORCH on. The zmqml-hybrid workflow reuses this image and also
# runs the Python ML server at test time (Torch is its model engine), which is
# why the ZMQML server runtime deps below are baked in here.
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# Optional apt mirror override. The default arm64 mirror (ports.ubuntu.com) is
# unreliable for large .debs, which makes LOCAL arm64 builds flaky; pass e.g.
# --build-arg APT_MIRROR=http://ftp.fau.de/ubuntu-ports to use a faster one.
# Left empty in CI, where the amd64 image builds from the fast archive.ubuntu.com.
ARG APT_MIRROR=

# Build toolchain, MPICH, autotools (argobots/SWM build with autotools), and the
# CODES build deps (cmake/ninja/pkg-config/flex/bison) so the `full` job can
# build ROSS + CODES inside this image with nothing extra to install.
RUN if [ -n "$APT_MIRROR" ]; then \
        sed -i "s|http://ports.ubuntu.com/ubuntu-ports|$APT_MIRROR|g; s|http://archive.ubuntu.com/ubuntu|$APT_MIRROR|g" /etc/apt/sources.list ; \
    fi \
    && printf 'Acquire::Retries "8";\nAcquire::http::Timeout "60";\nAcquire::https::Timeout "60";\n' \
        > /etc/apt/apt.conf.d/80-codes-ci \
    && apt-get update && apt-get install -y --no-install-recommends \
        build-essential ca-certificates git curl \
        autoconf automake libtool m4 \
        mpich libmpich-dev \
        cmake ninja-build pkg-config \
        flex bison \
    && rm -rf /var/lib/apt/lists/*

# --- argobots: userspace threading, required by SWM in CODES ---
RUN git clone --depth=1 https://github.com/pmodels/argobots /src/argobots \
    && cd /src/argobots \
    && ./autogen.sh \
    && mkdir build && cd build \
    && ../configure --disable-shared --prefix=/opt/argobots \
         CC=mpicc CXX=mpicxx CFLAGS=-g CXXFLAGS=-g \
    && make -j"$(nproc)" && make install

# SWM's workload models (nekbone, lammps, ...) use header-only Boost
# (property_tree, foreach, random, tuple). Use libboost-dev, NOT libboost-all-dev:
# the latter drags in libboost-mpi-dev -> a second MPI (OpenMPI), which then wins
# find_package(MPI) and whose mpirun refuses to run as root in the container.
RUN apt-get update && apt-get install -y --no-install-recommends \
        libboost-dev \
    && rm -rf /var/lib/apt/lists/*

# --- SWM: online communication workloads ---
RUN git clone --depth=1 --branch=v1.2 https://github.com/codes-org/swm-workloads /src/swm-workloads \
    && cd /src/swm-workloads/swm \
    && ./prepare.sh \
    && mkdir build && cd build \
    && ../configure --disable-shared --prefix=/opt/swm \
         CC=mpicc CXX=mpicxx CFLAGS=-g CXXFLAGS=-g \
    && make -j"$(nproc)" && make install

# --- python2 + texinfo (build-time only): conceptual/UNION need python2, and
#     conceptual's autotools build runs makeinfo (texinfo) for its docs. ---
RUN apt-get update && apt-get install -y --no-install-recommends \
        python2 texinfo \
    && rm -rf /var/lib/apt/lists/*

# --- conceptual (ncptl): UNION's workload-description compiler ---
RUN mkdir -p /src \
    && curl -L -o /tmp/conceptual.tar.gz \
        https://sourceforge.net/projects/conceptual/files/conceptual/1.5.1b/conceptual-1.5.1b.tar.gz \
    && tar -C /src -xzf /tmp/conceptual.tar.gz && rm /tmp/conceptual.tar.gz \
    && cd /src/conceptual-1.5.1b \
    && PYTHON=python2 ./configure --prefix=/opt/conceptual LIBS=-lm \
    && make -j"$(nproc)" && make install

# --- UNION: online workload combiner (needs conceptual + SWM). UNION's build
#     invokes `python` expecting python2, so shim it onto PATH for the make. ---
RUN git clone https://github.com/SPEAR-UIC/Union /src/Union \
    && cd /src/Union && git checkout 99b3df3 \
    && mkdir -p python-override && ln -sf /usr/bin/python2 python-override/python \
    && ./prepare.sh \
    && PYTHON=python2 ./configure --disable-shared \
         --with-conceptual=/opt/conceptual \
         --with-conceptual-src=/src/conceptual-1.5.1b \
         --prefix=/opt/union CC=mpicc CXX=mpicxx \
    && PATH="$PWD/python-override:$PATH" make -j"$(nproc)" && make install

# --- test-runtime tools: the union workload tests render their config templates
#     with envsubst (gettext-base). ---
RUN apt-get update && apt-get install -y --no-install-recommends \
        gettext-base \
    && rm -rf /var/lib/apt/lists/*

# --- ZeroMQ + RapidJSON: back the optional director-client / zmqml surrogate
#     (USE_ZEROMQ). On jammy libzmq3-dev ships zmq.h AND the zmq.hpp C++ binding;
#     rapidjson-dev is the header-only JSON parser the requester + director-client
#     use. Both are headers/libs only — the zmqml requester itself is built from
#     CODES source by CODES' own CMake (src/surrogate/zmqml) in the build job. ---
RUN apt-get update && apt-get install -y --no-install-recommends \
        libzmq3-dev rapidjson-dev \
    && rm -rf /var/lib/apt/lists/*

# --- SST-DUMPI: trace-based MPI workloads (USE_DUMPI). Build-only check in CI —
#     no trace data ships, so the `full` job only verifies CODES compiles + links
#     against it. CODES finds it via find_library(undumpi) under DUMPI_BUILD_PATH,
#     so install to /opt/dumpi (libundumpi.a in lib/, headers in include/dumpi/).
#     Clean the /src tree in the same layer so it doesn't bloat the image. ---
RUN git clone --depth=1 https://github.com/sstsimulator/sst-dumpi /src/sst-dumpi \
    && cd /src/sst-dumpi \
    && ./bootstrap.sh \
    && mkdir build && cd build \
    && ../configure --prefix=/opt/dumpi --enable-libdumpi \
         CC=mpicc CXX=mpicxx CFLAGS=-g CXXFLAGS=-g \
    && make -j"$(nproc)" && make install \
    && rm -rf /src/sst-dumpi

# --- LibTorch (CPU): the Torch-JIT packet-latency predictor (USE_TORCH).
#     Build-only check (no model ships). CODES discovers Torch via
#     find_package(Torch), which reads Torch_DIR; the build job sets
#       Torch_DIR=$(python3 -c 'import torch; print(torch.utils.cmake_prefix_path)')/Torch
#     exactly as CODES-compile-instructions.sh does — i.e. find_package(Torch) is
#     driven off the *pip* package's cmake_prefix_path, so we install the CPU wheel
#     rather than the standalone libtorch zip (which has no official aarch64 build).
#
#     Pin the install to PyTorch's CPU wheel index. It serves CPU-only wheels for
#     BOTH amd64 (CI) and aarch64 (local arm64) on cp310 (ubuntu 22.04's python),
#     so the image is CPU-only on either arch. Critically this avoids PyPI's
#     DEFAULT torch wheel, which on aarch64 now drags in multi-GB nvidia-cuda-*
#     deps (the unified Jetson/Grace build) that CODES neither needs nor — per
#     CODES-compile-instructions.sh's CUDA_HOME guard — accepts for a CPU build. ---
RUN apt-get update && apt-get install -y --no-install-recommends \
        python3 python3-pip \
    && rm -rf /var/lib/apt/lists/* \
    && pip3 install --no-cache-dir torch --index-url https://download.pytorch.org/whl/cpu

# --- ZMQML server Python runtime: the zmqml-hybrid CI workflow
#     (.github/workflows/zmqml-hybrid.yml) runs the Python ML server
#     (src/surrogate/zmqml/zmqmlserver.py) and control client (zmqmlctl.py) inside
#     this image. They need pyzmq for the ZMQ transport, numpy + pandas for record
#     handling, and scikit-learn for the packet-delay model's scaler; Torch (the
#     iteration-/event-time models) is already installed in the layer above.
#     Kept as the LAST layer so a future dep tweak is a cheap one-layer rebuild.
#     Torch (2.x CPU wheel) declares numpy optional and installs none of its own,
#     so this apt python3-numpy is the only numpy in the image and the one Torch
#     imports against — same as the workflow's runtime installs produce today. ---
RUN apt-get update && apt-get install -y --no-install-recommends \
        python3-zmq python3-numpy python3-pandas python3-sklearn \
    && rm -rf /var/lib/apt/lists/*
