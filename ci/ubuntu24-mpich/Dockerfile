# ubuntu-24.04 CI image with a source-built MPICH, for the build.yml jobs that
# run ctest with MPICH on ubuntu-24.04: the build matrix's 24.04 x mpich legs,
# the ASan/UBSan sanitizer lanes, and the coverage job.
#
# Why this image exists
# ---------------------
# Ubuntu 24.04's apt mpich (4.2.0-5build3) is built --with-device=ch4:ucx and is
# broken on the GitHub-hosted runners in TWO independent ways:
#   1. UCX probes the runner's InfiniBand-verbs device and tries the RDMA (rc)
#      transport, aborting MPI_Init with "ibv_create_srq() failed: Operation not
#      supported" -- every MPI-initializing test dies.
#   2. Even past that, mpiexec starts every rank as its own size-1 singleton, so
#      optimistic (--sync=3) tests silently run sequential.
# There is no fixed mpich in the noble apt archive. So we build MPICH from source
# with --with-device=ch4:ofi (libfabric, sockets/TCP -- no UCX, no verbs), which
# forms a real multi-rank world on the runners. The mpi-multirank-sanity ctest is
# the tripwire: it fails if the launcher ever regresses to singletons again.
#
# The base stays ubuntu:24.04 so these legs keep exercising the modern toolchain
# (gcc-13 / clang) they exist for -- only the MPI is swapped for our own build.
#
# Published as ghcr.io/codes-org/codes-ci-ubuntu24-mpich by
# .github/workflows/ubuntu24-mpich-image.yml. Like codes-ci-full, make the GHCR package
# PUBLIC after the first publish so build.yml (including fork PRs) can pull it
# anonymously.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# Toolchain + CODES build deps (cmake/ninja/pkg-config/flex/bison), BOTH compilers
# the matrix drives (gcc via build-essential + clang), lcov for the coverage job,
# gettext-base for the surrogate tests' envsubst config templating, and
# curl + gnupg for the codecov-action (it downloads its uploader with curl and
# signature-verifies it with gpg). These are all present on the stock GitHub
# runners but not in the bare base image, and the tools to fetch/build MPICH.
# Deliberately NO apt mpich -- we build our own below and put it ahead of PATH.
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential clang \
        cmake ninja-build pkg-config \
        flex bison \
        lcov \
        gettext-base \
        curl gnupg \
        git ca-certificates wget \
    && rm -rf /var/lib/apt/lists/*

# --- MPICH from source, ch4:ofi device (embedded libfabric, sockets/TCP). No UCX
#     and no verbs probing, reliable PMI wire-up -> mpiexec forms a real multi-
#     rank world on the CI runners. Installed to /opt/mpich and put on PATH so
#     find_package(MPI) discovers mpicc/mpicxx/mpiexec with no CODES/ROSS config
#     change. Bump MPICH_VERSION to update -- that's the whole maintenance story.
ARG MPICH_VERSION=4.2.3
RUN wget -q "https://www.mpich.org/static/downloads/${MPICH_VERSION}/mpich-${MPICH_VERSION}.tar.gz" \
    && tar xzf "mpich-${MPICH_VERSION}.tar.gz" \
    && cd "mpich-${MPICH_VERSION}" \
    && ./configure --prefix=/opt/mpich --with-device=ch4:ofi --disable-fortran \
    && make -j"$(nproc)" \
    && make install \
    && cd / \
    && rm -rf "mpich-${MPICH_VERSION}" "mpich-${MPICH_VERSION}.tar.gz"

# Put our mpich first so find_package(MPI) picks it over anything else.
ENV PATH=/opt/mpich/bin:$PATH
