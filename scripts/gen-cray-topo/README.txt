** Generating inter and intra group files for Edison and Theta Interconnects **:

- Edison network config files:
    python edison.py link-edison.txt intra-edison inter-edison
    
    [intra-edison and inter-edison are the intra-group and inter-group network
    configuration files required by the simulation. The python script
    translates Edison's network configuration into a file format that can be fed
    into the simulation.]

- Theta network config files:

   python theta.py theta.interconnect intra-theta inter-theta

   [intra-theta and inter-theta are the intra and inter-group config files for
   dragonfly. ]

 ** Generating customizable dragonfly interconnects **:

mpicc connections_general.c -o connections_general

./connections_general g r c intra-file inter-file

--> g: number of groups in the network
--> r: number of router rows within a group
--> c: number of router columns within a group
--> intra-file: output files for intra-group connections
--> inter-file: output file for inter-group connections

- The scripts and code for translating existing topologies and generating
  cray-style dragonfly topologies have been contributed by Nikhil Jain, Abhinav
  Bhatele and Peer-Timo Breemer from LLNL.

- For details on cray XC dragonfly network topology, see the following paper:

@inproceedings{faanes2012cray,
    title={Cray cascade: a scalable HPC system based on a Dragonfly network},
    author={Faanes, Greg and Bataineh, Abdulla and Roweth, Duncan and Froese,
    Edwin and Alverson, Bob and Johnson, Tim and Kopnick, Joe and Higgins, Mike
    and Reinhard, James and others},
    booktitle={Proceedings of the International Conference on High
    Performance Computing, Networking, Storage and Analysis},
    pages={103},
    year={2012},
    organization={IEEE Computer Society Press}
}
