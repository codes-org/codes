Contributions of external (non-Argonne) collaborators:

Nikhil Jain, Abhinav Bhatele (LLNL)
    - Improvements in credit-based flow control of CODES dragonfly and torus network models.
    - Addition of direct scheme for setting up dragonfly network topology.
    - Network configuration setup for custom dragonfly model.
    - Topology generations scripts for custom dragonfly model.
    - Bug fix for virtual channel deadlocks in custom dragonfly model.
    - Bug reporter for CODES network models.
    - Fat tree network setup and adaptive routing.
    - Pending: Merging Express mesh model to master.

Jens Domke (U. of Dresden)
    - Static routing in fat tree network model including ground work for
      dumping the topology and reading the routing tables.

Xu Yang (IIT)
    - Added support for running multiple application workloads with CODES MPI
      Simulation layer, along with supporting scripts and utilities.

Noah Wolfe (RPI):
    - Added a slim fly network model based on the topology proposed by Besta,
      Hoefler et al.
    - Added a fat tree network model that supports full and pruned fat tree
      network.
    - Added a multi-rail implementation for the fat tree networks (pending).
    - Bug reporter for CODES network models.

Caitlin Ross (RPI):
    - Added instrumentation so that network models can report sampled
      statistics over virtual time (pending).
    - Bug reporter for CODES models.
