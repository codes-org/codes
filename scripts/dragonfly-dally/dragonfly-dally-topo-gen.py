# Copyright 2018 - Neil McGlohon
# mcglon@rpi.edu

# This was a quick script to generate a dally dragonfly topology for use with dragonfly custom model

import sys
from enum import Enum
import struct
import numpy as np
argv = sys.argv
DRYRUN = 0

if sys.version_info[0] < 3:
    raise Exception("Python 3 or a more recent version is required.")


class Params(object):
    def __init__(self, radix, num_conn_between_groups):
        self.router_radix = radix
        self.num_conn_between_groups = num_conn_between_groups

        self.num_routers_per_group = int((radix + 1)/2) #a = (radix + 1)/2
        self.num_hosts_per_router = int(self.num_routers_per_group // 2)
        self.num_gc_per_router = int(self.num_routers_per_group // 2)
        self.num_gc_per_group = self.num_gc_per_router * self.num_routers_per_group

        num_gc_per_group = self.num_gc_per_router * self.num_routers_per_group
        self.num_groups = int((num_gc_per_group / self.num_conn_between_groups)) + 1
        self.total_routers = self.num_routers_per_group * self.num_groups


    def getSummary(self):
            outStr = "\nDragonfly (Dally) Network:\n"
            outStr += "\tNumber of Groups:            %d\n" % self.num_groups
            outStr += "\tRouter Radix:                %d\n" % self.router_radix
            outStr += "\tNumber Routers Per Group:    %d\n" % self.num_routers_per_group
            outStr += "\tNumber Terminal Per Router:  %d\n" % self.num_hosts_per_router
            outStr += "\n"
            outStr += "\tNumber GC per Router:        %d\n" % self.num_gc_per_router
            outStr += "\tNumber GC per Group:         %d\n" % self.num_gc_per_group
            outStr += "\tNumber GC between Groups:    %d\n" % self.num_conn_between_groups
            outStr += "\n"
            outStr += "\tTotal Routers:               %d\n" % self.total_routers
            outStr += "\tTotal Number Terminals:      %d\n" % (self.num_routers_per_group * self.num_hosts_per_router * self.num_groups)
            outStr += "\t"
            return outStr

def main():
    global DRYRUN
    if "--dryrun" in argv:
        DRYRUN = 1

    if(len(argv) < 5):
        raise Exception("Correct usage:  python %s <router_radix> <num_conn_between_groups> <intra-file> <inter-file>" % sys.argv[0])

    router_radix = int(argv[1])
    num_conn_between_groups = int(argv[2])

    intra = open(argv[3], "wb")
    inter = open(argv[4], "wb")

    params = Params(router_radix, num_conn_between_groups)

    print(params.getSummary())

    if not DRYRUN:
        global A
        A = np.zeros((params.total_routers, params.total_routers)) #adjacency matrix for verification
        writeIntra(params, intra)
        writeInter(params, inter)

        # np.set_printoptions(linewidth=400,threshold=10000,edgeitems=200)
        # print(A.astype(int))
        verifyConnections(params)


    intra.close()
    inter.close()



def verifyConnections(params):
    print("Verifying Radix Usage...")
    global A
    for row in A:
        if sum(row) + params.num_hosts_per_router > params.router_radix:
            print("ERROR - ROUTER RADIX EXCEEDED")
            exit(1)

    for col in A.T:
        if sum(col) + params.num_hosts_per_router > params.router_radix:
            print("ERROR - ROUTER RADIX EXCEEDED")
            exit(1)

    print("Verifying Group Interconnection Counts...")
    global group_conns
    conn_dict = dict.fromkeys(group_conns,0)
    for gc in group_conns:
        conn_dict[gc] += 1

    for num_conns in conn_dict.values():
        if num_conns > params.num_conn_between_groups:
            print("ERROR - GROUP INTERCONNECTION COUNT EXCEEDED")
            exit(1)


def getRouterGID(localID, groupNumber, routers_per_group):
    return(localID + (groupNumber*routers_per_group))

def getOtherGroupIDsStartingAfterMe(my_group_id, num_groups):
    all_group_ids = [i for i in range(num_groups) if i != my_group_id]
    return np.roll(all_group_ids, -1*my_group_id)

def writeIntra(params,fd):
    #for each router, connect to all other routers in group
    for si in range(params.num_routers_per_group):
        for di in range(params.num_routers_per_group):
            if si is not di:
                fd.write(struct.pack("3i",si,di,0)) #we don't care about the 'color', set third int (color) to 0
                # print("INTRA %d %d"%(si,di))

                for gi in range(params.num_groups): #loop over all groups becasue the intra only iterates over a single group
                    src_gid = getRouterGID(si, gi, params.num_routers_per_group)
                    dest_gid = getRouterGID(di, gi, params.num_routers_per_group)
                    A[src_gid][dest_gid] += 1


def writeInter(params ,fd):
    global group_conns
    group_conns = []
    for gi in range(params.num_groups):
        other_groups = getOtherGroupIDsStartingAfterMe(gi, params.num_groups)
        for gl in range(params.num_gc_per_group):
            other_group_id = other_groups[gl % len(other_groups)]
            group_conns.append( (gi, other_group_id) )

    for i, group_conn in enumerate(group_conns):
        src_gi = group_conn[0]
        dest_gi = group_conn[1]

        src_rtr_local_id = int(i // params.num_gc_per_router) % params.num_routers_per_group
        dest_rtr_local_id = params.num_routers_per_group - src_rtr_local_id - 1

        src_rtr_gid = getRouterGID(src_rtr_local_id, src_gi, params.num_routers_per_group)
        dest_rtr_gid = getRouterGID(dest_rtr_local_id, dest_gi, params.num_routers_per_group)

        fd.write(struct.pack("2i",src_rtr_gid, dest_rtr_gid))
        # print("INTER %d %d"%(src_rtr_gid, dest_rtr_gid))

        A[src_rtr_gid,dest_rtr_gid] += 1

if __name__ == '__main__':
    main()
