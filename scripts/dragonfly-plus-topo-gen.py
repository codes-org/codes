# Copyright 2017 - Neil McGlohon
# mcglon@rpi.edu

import sys
from enum import Enum
import struct
import numpy as np
argv = sys.argv

if sys.version_info[0] < 3:
    raise Exception("Python 3 or a more recent version is required.")



class TopSize(Enum):
    SMALL = 0
    MEDIUM = 1
    LARGE = 2

def main():
    if(len(argv) < 8):
        raise Exception("Correct usage:  python %s <num_group> <num_spine> <num_leaf> <topology_size> <redundant_global_cons_per> <intra-file> <inter-file>" % sys.argv[0])

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

    A = np.zeros((total_routers,total_routers)) #intra adjacency matrix in case you want one

    #for each leaf router, connect it to all spine routers in group
    for li in range(num_leaf_routers):
        for si in range(num_leaf_routers,total_routers):
            A[li][si] = 1
            fd.write(struct.pack("2i",li,si))
            print("INTRA %d %d"%(li,si))

    #for each spine router, connect it to all leaf routers in group
    for si in range(num_leaf_routers,total_routers):
        for li in range(num_leaf_routers):
            A[si][li] = 1
            fd.write(struct.pack("2i",si,li))
            print("INTRA %d %d"%(si,li))

def writeInter(num_spine_routers,num_leaf_routers, topology_size, num_groups, redundant_global_cons_per,fd):
    total_routers_per_group = num_spine_routers + num_leaf_routers
    global_total_routers = total_routers_per_group * num_groups
    global_cons_per = redundant_global_cons_per + 1

    Ag = np.zeros((global_total_routers,global_total_routers))

    if (topology_size is TopSize.SMALL) or (topology_size is TopSize.MEDIUM):
        #Every spine is connected to every other group

        if (topology_size is TopSize.MEDIUM) and (redundant_global_cons_per > 0):
            raise Exception("Error: redundant connections incompatible with Medium topology")

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
            raise Exception("ERROR: Assymetrical - num_other_groups\%num_spine_routers != 0") #TODO Consider allowing such a setting?

        if(num_other_groups != (num_spine_routers**2)):
            raise Exception("ERROR: Assymetrical - num_other_groups != num_spine_routers^2")

        if num_other_groups != (num_spine_routers * ind_up_radix):
            raise Exception("Error: Invalid topology - num groups exceeds group upward radix")

        if ind_up_radix > num_groups:
            raise Exception("ERROR: The number of global connections per spine router exceeds the number of groups. Not Large Topology!")

        if ind_up_radix != num_spine_routers:
            raise Exception("ERROR: the upward radix must equal the number of spine routers")

        interlinks = []

        spine_router_ids = [i for i in range(global_total_routers) if (i % total_routers_per_group) >= num_leaf_routers]
        all_groups = [i for i in range(num_groups)]
        for i,source_id in enumerate(spine_router_ids):
            source_group_id = int(source_id / total_routers_per_group)
            spine_local_id = source_id % total_routers_per_group
            xth_spine = spine_local_id - num_leaf_routers #if we were only counting spine routers, this means that I am the xth spine where x is this value

            other_groups = [j for j in range(num_groups) if j != source_group_id] #ids of groups not the source group
            my_other_groups = [] #the specific groups that this spine router will connect to
            for ii in range(ind_up_radix):
                index = (source_group_id+1 + ii + xth_spine*ind_up_radix) % num_groups
                my_other_groups.append(all_groups[index])

            dest_spinal_offset = (num_spine_routers-1) - xth_spine #which xth spine will the spine router connect to in the dest group.
            for dest_group_id in my_other_groups:
                dest_group_offset = dest_group_id * total_routers_per_group #starting gid for routers in dest group
                dest_id = dest_group_offset + dest_spinal_offset + num_leaf_routers #gid of destination router

                Ag[source_id,dest_id] = 1
                interlinks.append((source_id, dest_id, source_group_id, dest_group_id))


        interlinks.sort(key=lambda x: x[0])

        for link in interlinks:
            fd.write(struct.pack("2i",link[0], link[1]))
            print("INTER %d %d srcg %d destg %d" % (link[0], link[1], link[2], link[3]) )

        for row in Ag:
            row_sum = sum(row)
            if row_sum > ind_up_radix:
                raise Exception("Error: connectivity exceeds radix!")

        for col in Ag.T:
            col_sum = sum(col)
            if col_sum > ind_up_radix:
                raise Exception("Error: Connectivity exceeds radix!")

    else:
        raise Exception("Error: Invalid topology size given. Please use {0 | 1 | 2}")


if __name__ == '__main__':
    main()
