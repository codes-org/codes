Simplenet
---------

Model overview:
---------------

Simplenet is a module that implements a network module for use as a
component within model net.

The simplenet model is a basic cost model based on the startup cost
(latency) of the network and the bandwidth (cost per byte transferred)
of the network.  Each NIC is assumed to have full-duplex transmit and
receive capability.  Messages begin transmission when the sender's output
queue is available.  From the sender's perspective, the transmission is
complete as soon as enough virtual time has elapsed to account for the
startup and bandwidth costs of the message.  However, the message is
not delivered for processing on the receiver until the receiver's input
queue is also available.  The network fabric itself is not modeled and is
instead treated as if it has infinite buffering ability and no internal
routing overhead.

Simplenet supports optimistic mode and reverse computation.

Modularization concepts:
------------------------

The simplenet LPs are intended to be used in conjunction with other LPs that
implement a higher level protocol.  For examples, they may be used in
conjunction with LPs that model file servers in a parallel file system.

Each simplenet LP represents a NIC.  In terms of LP mapping, each simplenet
LP should also use an LP ID that is a fixed offset from the "node" that it
belongs to.  Note that the offset is specified to the simplenet module at
event creation time, which means that multiple "node" LPs can share the same
simplenet LP by using different offsets.

In terms of event transmission, simplenet can be thought of as a wrapper
around other events.  It tunnels the upper level events through the network
and delivers them to a remote LP, consuming some amount of virtual time
along the way according to the modeled network characteristics.  The upper
level model does not have access to the simplenet LPs, the simplenet message
struct, or any other internal parameters.  It is intended to abstract away
network details from some higher level model.
