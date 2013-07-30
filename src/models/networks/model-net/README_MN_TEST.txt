- The test.conf file has the mapping of model-net and server LPs (currently 16 server LPs and 16 modelnet LPs)
- To run the program :
    ./modelnettest --sync=1 test.conf
    mpirun -np 4 ./modelnettest --sync=2 test.conf (The synchronization protocol is specified before the file name)
