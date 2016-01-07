"SimpleP2P"
----------

Model overview:
---------------

SimpleP2P is an extension of simplenet to allow for arbitrary point-to-point
capacities.

SimpleP2P has nearly the same duplex queued transmission semantics as
simplenet, though with a few important differences. First, point-to-point
latencies and bandwidths are different. Network links can also have different
ingress and egress latencies/bandwidths. Second, each unique link is given its
own queue that do not interfere with the others. The use case for this model is
a set of sites with internal networks, with each site communicating via
simplep2p; we assume the internal network is capable of servicing all
outgoing/incoming WAN transmissions at their full capacities.

Additional configuration is needed to initialize the link latencies/capacities.
In the codes-configuration file, the variables "net_latency_ns_file" and
"net_bw_mbps_file" must be set under the PARAMS group. They point (in a path
relative to the configuration file) to configurations for the latency and
bandwidth costs, respectively.

Each of the latency/bandwidth configuration files have the same format, based
on a square matrix of directed point to point capacities. Given N modelnet LPs,
it has the format:

1:1 1:2 ... 1:N
2:1 2:2 ... 2:N
      ...
N:1 N:2 ... N:N

where x:y is a pair of latency or bandwidth between components x and y.
An individual entry in this matrix is specified as "a,b" where 'a' is the egress 
latency/bandwidth of the link and 'b' is the ingress latency/bandwidth of the link.
Whitespace is ignored, but linebreaks are not, and delimit rows of the matrix.
The relative simplep2p identifiers 1..N are assigned to simplep2p LPs in the
order of their appearance in the codes-configuration file. It is expected that
all i:i entries are 0 - modelnet currently doesn't handle self messages.

Support in the code is also available for triangular matrices of the format:

1:2 1:3   ...   1:N
    2:3 2:4 ... 2:N
        ...
                N-1:N

However, this option is currently disabled (the configuration code path has not
been expanded to allow specifying the option). The option will be enabled
into the configuration routines if deemed necessary.

Caveats:
--------

The model-net statistics are slightly more complex than in other model-net
implementations because there are in essence multiple queues per simplep2p
instance. In particular, the "send time" and "recv time" stats are computed as
ranges of time in which the simplep2p LP is actively sending/receiving data
from *any* link. Hence, simple bandwidth calculations (send bytes / send time)
may not be accurate due to skew in message issuance.

Having more than one category in simplep2p will cause the times in the
derived "all" category to be off. "all" is currently implemented as the sum of
the various categories, which doesn't work when times in certain categories may
overlap.
