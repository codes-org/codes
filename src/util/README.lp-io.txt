LP-IO
-----

LP-IO is a simple API for writing data collectively from a ROSS simulation.
It is best suited for relatively compact final statistics from each LP 
rather than for ongoing event logging.  

API
---

lp_io_prepare(): call before tw_run as a collective operation.  This 
prepares a directory to hold the simulation results.  If the
LP_IO_UNIQ_SUFFIX flag is specified, the lp-io will append a unique
identifier to the specified directory name based on the rank 0 pid and
the current unix time.

lp_io_write(): call at any time during the simulation itself (i.e. in an
event handler or in the lp finalize() function).  This is not a collective
call.  The caller must specify a string identifier (which will become a file
within the output directory) as well as the buffer and size to write.

NOTES on identifiers:
- Each LP can use as many identifiers as it wants to.
- Identifiers do not have to be coordinated across LPs.  For example, some
  LP types may use "lpdata1" and others may use "lpdata2", while others
  don't write data at all.
- Each identifier can be written to as many times as needed.  It just
  appends additional data on each write call.

lp_io_flush(): call after tw_run as a collective operation.  This will
aggregate all data written by the simulation with lp_io_write() calls and
store data in the output directory using collective write operations.

Example:
--------
See lp-io-test.  

To run sequentially: "./lp-io-test --sync=1"

To run in parallel: "mpiexec -n 8 ./lp-io-test --sync=2"

This example will generate two output files.  The output files are in text
format so that you can view them with "cat" or a text editor.

Limitations:
---------
- The code allocates a copy of all memory buffers.
- The aggregation could be optimized further.  Right now it includes a O(N)
  step to construct a global list of identifiers used by the model.
- There is a fixed (but arbitrary) limit on identifier string length and on 
  the total number of identifiers used by the simulation.  This was done to
  simplify the implementation rather than to address any particular resource
  limitation.
