The loggp.c file implements a model-net method that models network cost
based on the loggp analytical model presented in this paper:

    Albert Alexandrov, Mihai F. Ionescu, Klaus E. Schauser, and
    Chris Scheiman.  LogGP: incorporating long messages into the LogP
    model—one step closer towards a realistic model for parallel
    computation. In Proceedings of the seventh annual ACM symposium on
    Parallel algorithms and architectures (SPAA '95). ACM, New York,
    NY, USA, 95-105.

It uses model parameters obtained with the netgauge tool described in this
paper:

    Torsten Hoefler, Torsten Mehlan, Andrew Lumsdaine, and Wolfgang Rehm.
    Netgauge: a network performance measurement framework. In Proceedings
    of the Third international conference on High Performance Computing
    and Communications (HPCC'07).

The model can be validated using the model found in
tests/modelnet-p2p-bw.c.  It implements a synchronous point to
point ping-pong benchmark in a manner similar to the mpptest tool.
modelnet-p2p-bw sweeps over a range of message sizes, with 1000 iterations
at each message size.  It should work fine in optimistic mode, though
there isn't much reason to run it in parallel because it generally only
uses 4 ROSS LPs.  It prints final results to stdout in the exact same
data format as mpptest.

The loggp modelnet method reads in output produced by Torsten's netgauge
tool and uses it as a lookup table to calculate delays for network messages.
You can find an example of netgauge output from the Tukey system at ANL
(using MVAPICH over InfiniBand) in tests/ng-mpi-tukey.dat.

A few notes on the implementation:

- The logp family of analytical models generally assume a set of fixed
  parameters will be used as input, but netgauge actually calculates them
  independently over a range of message sizes.  For better model agreement
  we are using the range of values produced by netgauge as a
  lookup table for model parameters at different sizes rather than trying
  using single fixed values.

- netgauge also produces "overhead" values (o_r and o_s for receiver and
  sender, referred to simply as o in the literature). These represent the
  amount of CPU time used by the receiver and sender to process messages,
  and in the loggp paper it is used directly in calculating communication
  time.  If you read the netgauge paper, though, they are assuming that
  these values *overlap* with the fabric latency and bandwidth costs on
  modern networks, and as a result if you are using netgauge parameters
  you should omit the o_r and o_s values when calculating network time or
  your numbers will be way off.  They would be useful if you were tracking
  the amount of time that a processor core has available for computation,
  which we are not doing yet. Another side note is
  that these overhead values are quite high for large messages on Tukey,
  presumably because the MPI implementation is polling during
  transmission.

- The loggp model-net method does not introduce any random noise in the
  communication cost; it uses fixed parameters for each message size.
  It does use a random number generator to introduce minor variations in
  transfer time between hosts and network cards as part of the model-net
  framework, but this variance is not significant.

- The loggp model-net method uses the same technique as simplenet for
  handling network card contention.  It tracks the state of input and output
  queues in terms of when they will next be available for communication and
  adjusts those times as new messages are scheduled for communication.  The
  model assumes infinite buffering in the network switch fabric.
