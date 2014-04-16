"Simplewan"
----------

Model overview:
---------------

Simplewan is an extension of simplenet to allow for arbitrary point-to-point
capacities.

Simplewan has nearly the same duplex queued transmission semantics as
simplenet, though with a few important differences. First, point-to-point
latencies and bandwidths are different. Second, each unique link is given its
own queue that do not interfere with the others. The use case for this model is
a set of sites with internal networks, with each site communicating via
simplewan; we assume the internal network is capable of servicing all
outgoing/incoming WAN transmissions at their full capacities.

Additional configuration is needed to initialize the link latencies/capacities.
In the codes-configuration file, the variables "net_startup_ns_file" and
"net_bw_mbps_file" must be set under the PARAMS group. They point (in a path
relative to the configuration file) to configurations for the startup and
bandwidth costs, respectively.

Each of the latency/bandwidth configuration files have the same format, based
on a triangular matrix. Given N modelnet LPs, it has the format:

1:2 1:3   ...   1:N
    2:3 2:4 ... 2:N
        ...
                N-1:N

where x:y is the latency or bandwidth between components x and y. Whitespace is
ignored, but linebreaks are not, and delimit rows of the matrix. The relative
simplewan identifiers 1..N are assigned to simplewan LPs in the order of
their appearance in the codes-configuration file. In the future, a full NxN
matrix may be used to facilitate asymmetric link capacities if the need arises.
