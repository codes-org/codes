# Copyright 2017 - Neil McGlohon
# mcglon@rpi.edu

import sys
from enum import Enum
import struct
import numpy as np
argv = sys.argv


class TopSize(Enum):
    SMALL = 0
    MEDIUM = 1
    LARGE = 2

def main():
    if(len(argv) < 8):
        print("Correct usage:  python %s <num_group> <num_spine> <num_leaf> <topology_size> <redundant_global_cons_per> <intra-file> <inter-file>" % sys.argv[0])
        exit(0)

    groups = int(argv[1])
    num_spine_routers = int(argv[2])
    num_leaf_routers = int(argv[3])
    topology_size = TopSize(int(argv[4]))
    redundant_global_cons_per = int(argv[5])
    intra = open(argv[6], "wb")
    inter = open(argv[7], "wb")

    writeIntra(num_spine_routers, num_leaf_routers, intra)
    writeInter(num_spine_routers, num_leaf_routers, topology_size, groups, redundant_global_cons_per, inter)

    intra.close()
    inter.close()


def getRouterGID(localID, groupNumber, routers_per_group):
    return(localID + (groupNumber*routers_per_group))


def writeIntra(num_spine_routers,num_leaf_routers,fd):
    total_routers = num_spine_routers + num_leaf_routers

    A = np.zeros((total_routers,total_routers))

    #for each spine router, connect it to all leaf routers in group
    for si in range(num_spine_routers):
        for li in range(num_spine_routers,total_routers):
            A[si][li] = 1
            fd.write(struct.pack("2i",si,li))
            print("INTRA %d %d"%(si,li))

    #for each leaf router, connect it to all spine routers in group
    for li in range(num_spine_routers,total_routers):
        for si in range(num_spine_routers):
            A[li][si] = 1
            fd.write(struct.pack("2i",li,si))
            print("INTRA %d %d"%(li,si))

def writeInter(num_spine_routers,num_leaf_routers, topology_size, num_groups, redundant_global_cons_per,fd):
    total_routers_per_group = num_spine_routers + num_leaf_routers
    global_total_routers = total_routers_per_group * num_groups
    global_cons_per = redundant_global_cons_per + 1

    if (topology_size is TopSize.SMALL) or (topology_size is TopSize.MEDIUM):
        #Every spine is connected to every other group

        if (topology_size is TopSize.MEDIUM) and (redundant_global_cons_per > 0):
            print("Error: redundant connections incompatible with Medium topology")
            exit(0)

        for source_gi in range(num_groups):
            for si in range(num_leaf_routers, total_routers_per_group):
                source_id = getRouterGID(si,source_gi,total_routers_per_group)

                for dest_gi in range(num_groups):
                    if source_gi != dest_gi:
                        dest_id = getRouterGID(si, dest_gi,total_routers_per_group)
                        for i in range(global_cons_per):
                            fd.write(struct.pack("2i",source_id,dest_id))
                            print("INTER %d %d srcg %d destg %d"%(source_id,dest_id,source_gi,dest_gi))

    elif topology_size is TopSize.LARGE:
        #Each group is connected to every other group via single connection on individual spines
        ind_radix = 2 * num_leaf_routers #TODO don't assume that the radix is half down half up
        ind_up_radix = int(ind_radix/2)
        num_other_groups = num_groups - 1

        if(num_other_groups%num_spine_routers != 0):
            print("ERROR: Assymetrical - num_other_groups\%num_spine_routers != 0") #TODO Consider allowing such a setting?
            exit(0)

        if(num_other_groups != (num_spine_routers**2)):
            print("ERROR: Assymetrical - num_other_groups != num_spine_routers^2")
            exit(0)

        if num_other_groups > (num_spine_routers * ind_radix/2):
            print("Error: Invalid topology - num groups exceeds group upward radix")
            exit(0)

        for source_gi in range(num_groups): #for each group i
            other_groups = [i for i in range(num_groups) if i != source_gi ] #list of group ids not including self

            for si in range(num_spine_routers): #for each local spine router
                source_id = getRouterGID(si,source_gi,total_routers_per_group) #get the global ID of local spine si

                for i in range(ind_up_radix): #Make a connection for each upward connection that the router has
                    gi = i + si*ind_up_radix
                    dest_id = getRouterGID(si,other_groups[gi],total_routers_per_group) #get global ID of destination router (has same local id as source)

                    fd.write(struct.pack("2i",source_id,dest_id))
                    print("INTER %d %d srcg %d destg %d"%(source_id,dest_id,source_gi,other_groups[gi]))

    else:
        print("Error: Invalid topology size given. Please use {0 | 1 | 2}")
        exit(0)


if __name__ == '__main__':
    main()
