0 - Checkout, build, and install codes-base

    git clone git@git.mcs.anl.gov:radix/codes-base
    <see codes-base/README.txt>

1 - If this is the first time you are building codes-net, run

    ./prepare.sh

2- Configure codes-net. This can be done in the source directory or in a
   dedicated build directory if you prefer out-of-tree builds.  The CC
   environment variable must refer to an MPI compiler.

    ./configure --with-codes-base=/path/to/codes-base/install --prefix=/path/to/codes-net/install CC=mpicc

3 - Build codes-net

    make clean && make
    make install
    make tests

4 - (optional) run test programs

    make check
