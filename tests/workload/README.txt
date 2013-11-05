1- To run the test use:

mpirun -np 4 ./codes-workload-test --sync=2 codes-workload-test.conf

2- To modify the codes-workload-test.conf file, change the 'workload_type' parameters. If using bgp_io_workload type, change the io_kernel_meta_path and bgp_config_file parameters according to your local path.

3- Currently, the bgp_io_workload type only loads the workload file. TODO: read the operations inside the workload file one by one, load the appropriate parameters and display them correctly.
