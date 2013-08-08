Test programs can be run with the "make check" make target.  All programs
will be executed in sequential mode.

You can also run the test program manually in parallel (conservative or
optimistic) mode as follows:

mpiexec -n 4 tests/modelnet-test --sync=2 --num_servers=n (optional --nkp=n) tests/modelnet-test.conf
    <or>
mpiexec -n 4 tests/modelnet-test --sync=3 --num_servers=n (optional --nkp=n) tests/modelnet-test.conf

Note: The num_servers runtime argument should match the number of server repetitions specified in the modelnet-test config file.

(1)- To run the modelnet test with the simplenet network plugin, use tests/modelnet-test.conf (default setting).
(2)- To run the modelnet test with the torus network plugin, use tests/modelnet-test-torus.conf file.

