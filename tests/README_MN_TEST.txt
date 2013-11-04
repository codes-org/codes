Test programs can be run with the "make check" make target.  All programs
will be executed in sequential mode.

You can also run the test program manually in parallel (conservative or
optimistic) mode as follows:

mpiexec -n 4 tests/modelnet-test --sync=2 (optional --nkp=n) tests/modelnet-test.conf
    <or>
mpiexec -n 4 tests/modelnet-test --sync=3 (optional --nkp=n) tests/modelnet-test.conf

- To run the modelnet test with the simplenet network, torus and dragonfly network plugins use tests/modelnet-test.conf (default setting),  tests/modelnet-test-torus.conf and tests/modelnet-tests-dragonfly.conf respectivel
