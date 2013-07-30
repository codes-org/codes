Test programs can be run with the "make check" make target.  All programs
will be executed in sequential mode.

You can also run the test program manually in parallel (conservative or
optimistic) mode as follows:

mpiexec -n 4 tests/modelnet-test --sync=2 tests/modelnet-test.conf
    <or>
mpiexec -n 4 tests/modelnet-test --sync=3 tests/modelnet-test.conf

