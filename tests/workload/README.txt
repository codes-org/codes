=========================
== codes-workload-test ==
=========================

To run the test use:

mpirun -np 4 ./codes-workload-test --sync=2 codes-workload-test.conf

===============================
== codes-workload-mpi-replay ==
===============================

To run this test use:

mpirun -np NUM_PROCS ./codes-workload-mpi-replay --conf <conf_file_path> --test-dir <test_directory>

- <conf_file_path> is the path of the configuration file to use for this workload replay
    - this configuration file format is described at the end of this README
- <test_directory> is the directory to replay the actual i/o operations in
    - TODO: the replay tool does not currently replay workloads that do not create the files they
            do i/o to (you can generate verbose i/o output, but not actually replay the i/o)

The following options may be specified on the command line:

-noop : do not perform i/o
-v : verbose output of i/o details/parameters

NOTE: NUM_PROCS need not match the number of ranks in the actual workload. I.E. if the workload is a
      darshan log with 1024 ranks, and NUM_PROCS is set to 8, i/o events will just be generated for
      ranks 0-7. This should not cause any runtime errors.

==============================
== codes-workload-test.conf ==
==============================

This file contains the sample configuration for each of the 3 (current) workload generators: test
workload generator, i/o language workload generator, and the darshan i/o workload_generator.
These configuration files are passed as arguments to both the codes-workload-test and the
codes-workload-mpi-replay programs. A summary of each workloads parameters is given below (NOTE:
these parameters should be updated each time a new paramter is added to each workload):

1.) test workload generator:

PARAMS
{
    workload_type = "test";
}

- workload_type is just the name of this generator ("test")

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

2.) i/o language workload generator:

PARAMS
{
    workload_type = "iolang_workload";
    io_kernel_meta_path = "/path/to/io/kernel/meta.txt";
    rank_count = "8";
}

- workload_type is just the name of this generator ("iolang_workload")
- io_kernel_meta_path is the path to the i/o kernel meta file
- rank_count is the number of ranks to generate i/o for
    - this needs to match the range of ranks given in the io_kernel_meta_file
      (unless -1 is used in the meta file, in which case the given number of
      ranks will be used)

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

3.) darshan i/o workload generator:

PARAMS
{
    workload_type = "darshan_io_workload";
    log_file_path = "/path/to/darshan/log";
    aggregator_count = "18";
}

- workload_type is just the name of this generator ("darshan_io_workload")
- log_file_path is the path to the darshan log to generate i/o for
- aggregator_count is the number of collective aggregators to use for collective file i/o generation
    - NOTE: this value may have a strong impact on the generated i/o pattern
            - i.e. if it is not known, it may be very difficult to reproduce the original pattern
    - NOTE: this value can be set to anything if the workload is mostly independently opened files
