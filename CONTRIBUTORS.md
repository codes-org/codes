Contributors to date (in chronological order, with current affiliations)
- Ning Liu, Rensselaer Polytechnic Institute
- Jason Cope, Argonne National Labs
- Philip Carns, Argonne National Labs
- Misbah Mubarak, Argonne National Labs
- Shane Snyder, Argonne National Labs
- Jonathan P. Jenkins, Argonne National Labs
- Noah Wolfe, RPI
- Nikhil Jain, Lawrence Livermore National Labs
- Giorgis Georgakoudis, Lawrence Livermore Labs
- Matthieu Dorier, Argonne National Labs
- Caitlin Ross, RPI
- Xu Yang, Illinois Institute of Tech.
- Jens Domke, Tokyo Institute of Tech.
- Xin Wang, IIT
- Neil McGlohon, Rensselaer Polytechnic Institute
- Elsa Gonsiorowski, Rensselaer Polytechnic Institute
- Justin M. Wozniak, Argonne National Laboratory
- Robert B. Ross, Argonne National Laboratory
- Lee Savoie, Univ. of Arizona 

Contributions (listed in chronological order):

Misbah Mubarak (ANL)
    - Introduced 1-D dragonfly and enhanced torus network model.
    - Added quality of service in dragonfly and megafly network models.
    - Added MPI simulation layer to simulate MPI operations.
    - Updated and merged burst buffer storage model with 2-D dragonfly.
    - Added and validated 2-D dragonfly network model.
    - Added multiple workload sources including MPI communication, Scalable
      Workload Models, DUMPI communication traces.
    - Added online simulation capability with Argobots and SWMs.
    - Instrumented the network models to report time-stepped series statistics. 
    - Bug fixes for network, storage and workload models with CODES.

Neil McGlohon (RPI)
    - Introduced Megafly network model.
    - Merged 1-D dragonfly and 2-D dragonfly network models.
    - Updated adaptive routing in megafly and 1-D dragonfly network models. 

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

John Jenkins 
    - Introduced storage models in a separate codes-storage-repo.
    - Enhanced the codes-mapping APIs to map advanced combinations on PEs.
    - Bug fixing with network models.
    - Bug fixing with MPI simulation layer.
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

Neil McGlohon (RPI):
    - Added dragonfly plus network model
