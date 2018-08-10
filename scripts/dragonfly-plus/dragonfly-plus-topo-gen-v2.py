# Copyright 2018 - Neil McGlohon
# mcglon@rpi.edu
# An Object Oriented approach to topology generation - Allows for a logical generation that may be more complicated than v1 which used assumptions
# In hindsight this was a lot more complicated than I intended. It was looking to solve a complex problem that turned out to be invalid from the beginning.

### USAGE ###
# Correct usage: python3 script.py <num_groups> <num_spine_pg> <num_leaf_pg> <router_radix> <num_terminal_per_leaf> <intra-file> <inter-file>
###       ###

import sys
from enum import Enum
import struct
import numpy as np
argv = sys.argv

class Loudness(Enum):
    DEBUG = 0 #prints all output
    EXTRA_LOUD = 1 #prints every single connection too
    LOUD = 2 #prints group connections and other similarly verbose things
    STANDARD = 3 #prints what step its working on
    QUIET = 4 #print no output

global DRYRUN
global LOUDNESS
global SHOW_ADJACENCY
global NO_OUTPUT_FILE

LOUDNESS = Loudness.STANDARD
DRYRUN = 0
SHOW_ADJACENCY = 0
NO_OUTPUT_FILE = 0

global desired_num_gc_bg


if sys.version_info[0] < 3:
    raise Exception("Python 3 or a more recent version is required.")

class ConnType(Enum):
    LOCAL = 0
    GLOBAL = 1

class RouterType(Enum):
    LEAF = 0
    SPINE = 1

class AdjacencyType(Enum):
    LOCAL_LOCAL = 0 # size = (num_routers_pg, num_routers_pg)
    LOCAL_ONLY_GID = 1 # size = (total_routers, total_routers) but only the submatrix for the group is filled in. i.e. local connections only by gid
    GLOBAL_ONLY = 2 # size = (total_routers, total_routers) but only the global connecitons are included
    ALL_CONNS = 3 # size = (total_routers, total_routers) with global connections and local connections. Actual FULL adjacency matrix


def log(mystr, loudness):
    global LOUDNESS
    if loudness.value >= LOUDNESS.value:
        print(mystr)

def sortBySource(conn): #returns the key by which connections should be sorted (source id, local or global)
    if conn.connType is ConnType.LOCAL:
        return conn.src_router.local_id
    else:
        return conn.src_router.gid

def sortByDest(conn):
    if conn.connType is ConnType.LOCAL:
        return conn.dest_router.local_id
    else:
        return conn.dest_router.gid

def convertLocalIDtoGID(router_lid, router_group_num, routers_per_group):
    return router_lid + router_group_num * routers_per_group

def convertGIDtoLocalID(router_gid, routers_per_group):
    return router_gid % routers_per_group

class DragonflyPlusNetwork(object):
    def __init__(self,num_groups=5, num_spine_pg=2, num_leaf_pg=2, router_radix=4, num_hosts_per_leaf=2):
        log("New Dragonfly Plus Network", Loudness.STANDARD)
        self.num_groups = num_groups
        self.num_spine_pg = num_spine_pg
        self.num_leaf_pg = num_leaf_pg
        self.router_radix = router_radix

        self.num_global_links_per_spine = router_radix - num_leaf_pg
        self.num_global_links_pg = self.num_global_links_per_spine * num_spine_pg

        largest_num_groups = (router_radix//2)**2 + 1
        global desired_num_gc_bg
        self.desired_num_global_links_between_groups = desired_num_gc_bg
        self.num_global_links_between_groups = (largest_num_groups-1)//(num_groups-1)

        self.num_hosts_per_leaf = num_hosts_per_leaf #just for stats, not used in the actual topology export

        self.localConnectionsOutput = []
        self.globalConnectionsOutput = []
        self.groups = []

        self.precheckForErrors()
        if not DRYRUN:
            self.initializeGroups()
            self.generateLocalGroupConnections()
            self.generateGlobalGroupConnections()
            self.verifyTopology()

    def getSummary(self):
        outStr = "\nDragonfly Plus Network:\n"
        outStr += "\tNumber of Groups:          %d\n" % self.num_groups
        outStr += "\tRouter Radix:              %d\n" % self.router_radix
        outStr += "\tNumber Spine Per Group:    %d\n" % self.num_spine_pg
        outStr += "\tNumber Leaf Per Group:     %d\n" % self.num_leaf_pg
        outStr += "\tNumber Terminal Per Leaf:  %d\n" % self.num_hosts_per_leaf
        outStr += "\n"
        outStr += "\tNumber GC per Spine:       %d\n" % self.num_global_links_per_spine
        outStr += "\tNumber GC per Group:       %d\n" % (self.num_global_links_per_spine * self.num_spine_pg)
        outStr += "\tNumber GC between Groups:  %d\n" % (self.num_global_links_between_groups)
        outStr += "\n"
        outStr += "\tTotal Spine:               %d\n" % (self.num_spine_pg * self.num_groups)
        outStr += "\tTotal Leaf:                %d\n" % (self.num_leaf_pg * self.num_groups)
        outStr += "\tTotal Routers:             %d\n" % ((self.num_leaf_pg + self.num_spine_pg) * self.num_groups)
        outStr += "\tTotal Number Terminals:    %d\n" % (self.num_leaf_pg * self.num_hosts_per_leaf * self.num_groups)
        outStr += "\t"
        return outStr


    def initializeGroups(self):
        groups = []
        num_routers_pg = self.num_spine_pg + self.num_leaf_pg
        for new_group_id in range(self.num_groups):
            newGroup = Group(new_group_id, self.num_global_links_pg, self)

            for local_id in range(num_routers_pg):
                router_gid = local_id + (new_group_id * num_routers_pg)
                if local_id < self.num_leaf_pg:
                    intra_radix = self.num_spine_pg
                    inter_radix = 0
                    rt = RouterType.LEAF

                elif local_id >= self.num_leaf_pg:
                    intra_radix = self.num_leaf_pg
                    inter_radix = self.getNumGlobalConnsPerSpine()
                    rt = RouterType.SPINE

                newRouter = Router(router_gid, local_id, new_group_id, intra_radix, inter_radix, rt, self)
                newGroup.addRouter(newRouter)

            groups.append(newGroup)
            log("Added New Group. ID: %d, Size: %d" % ( new_group_id, len(newGroup.group_routers) ) , Loudness.LOUD)

        self.groups = groups

    def generateLocalGroupConnections(self):
        log("Dragonfly Plus Network: Generating Local Group Connections", Loudness.STANDARD)
        for group in self.groups:
            group.generateLocalConnections()

    def generateGlobalGroupConnections(self):
        log("Dragonfly Plus Network: Generating Global Group Connections", Loudness.STANDARD)
        for group in self.groups:
            other_groups = group.getOtherGroupIDsStartingAfterMe(self.num_groups)
            for gl in range(self.num_global_links_pg):
                other_group_id = other_groups[gl % len(other_groups)]

                group.addGlobalConnection(self.groups[other_group_id])

        for group in self.groups:
            group.assignGlobalConnectionsToRouters()

    def getNumGlobalConnsPerSpine(self):
        return self.num_global_links_pg // self.num_spine_pg

    def getTotalRouters(self):
        return self.num_groups * (self.num_spine_pg + self.num_leaf_pg)

    def getTotalNodes(self):
        return self.num_hosts_per_leaf * self.num_leaf_pg * self.num_groups

    def getAllSpineRouters(self):
        spine_rtrs = []
        for g in self.groups:
            for rtr in g.group_routers:
                if rtr.routerType is RouterType.SPINE:
                    spine_rtrs.append(rtr)
        return spine_rtrs

    def getAllLeafRouters(self):
        leaf_rtrs = []
        for g in self.groups:
            for rtr in g.group_routers:
                if rtr.routerType is RouterType.LEAF:
                    leaf_rtrs.append(rtr)
        return leaf_rtrs

    def getRouter(self,gid):
        router_group_num = gid // (self.num_spine_pg + self.num_leaf_pg)
        offset = gid % (self.num_spine_pg + self.num_leaf_pg)

        rtr = self.groups[router_group_num].group_routers[offset]
        assert(rtr.gid == gid)
        return rtr

    def precheckForErrors(self):
        if self.num_global_links_pg % self.desired_num_global_links_between_groups != 0:
            raise Exception("DragonflyPlusNetwork: Global Connection Fairness Violation. num_global_links_pg % num_global_links_between_groups != 0")
        if self.num_global_links_pg % self.num_spine_pg != 0: #are we fair?
            raise Exception("DragonflyPlusNetwork: Global Connection Fairness Violation. num_global_links_pg % num_spine_pg != 0!!!!")
        if self.num_leaf_pg > (self.router_radix - self.getNumGlobalConnsPerSpine()): #do we have enough ports left to connect to all the leafs in the group after connecting global conns?
            raise Exception("DragonflyPlusNetwork: Inadequate radix for number of global connections per spine router")

    def verifyTopology(self):
        A = self.getAdjacencyMatrix(AdjacencyType.ALL_CONNS)

        #verify symmetry. 1 <-> 2 == 2 <-> 1
        log("Verifying Symmetry...", Loudness.STANDARD)
        for i in range(A.shape[0]):
            for j in range(A.shape[1]):
                if A[i][j] != A[j][i]:
                    raise Exception("DragonflyPlusNetwork: Failed Verification: Topology not symmetric")

        #verify safe radix
        log("Verifying Radix Usage...", Loudness.STANDARD)
        for row in A:
            # log("%d > %d: %s" % (sum(row), self.router_radix, str(sum(row) > self.router_radix) ) )
            if sum(row) > self.router_radix:
                raise Exception("DragonflyPlusNetwork: Failed Verification: Router Radix Exceeded")

        #verify fairness
        log("Verifying Fairness...", Loudness.STANDARD)
        spine_rtrs = self.getAllSpineRouters()
        leaf_rtrs = self.getAllLeafRouters()

        #all spine routers have same local and global connections sizes
        loc_conns = len(spine_rtrs[0].local_connections)
        glob_conns = len(spine_rtrs[0].global_connections)
        for s in spine_rtrs:
            loc = len(s.local_connections)
            glob = len(s.global_connections)
            failed = False
            if loc is not loc_conns:
                failed = True
            if glob is not glob_conns:
                failed = True

        #all leaf routers have same local and global connections sizes
        #all leaf routers have 0 global connections
        loc_conns = len(leaf_rtrs[0].local_connections)
        glob_conns = 0
        for l in leaf_rtrs:
            loc = len(l.local_connections)
            glob = len(l.global_connections)
            failed = False
            if loc is not loc_conns:
                failed = True
            if glob is not glob_conns:
                failed = True

        log("Verifying Dragonfly Nature...", Loudness.STANDARD)


        if failed:
            raise Exception("DragonflyPlusNetwork: Failed Verification: Fairness")

        for g in self.groups:
            if len(set(g.groupConns)) != self.num_global_links_pg:
                raise Exception("DragonflyPlusNetwork: Not Enough Group Connections")

        log("Verifying Inter Group Connection Uniformity...", Loudness.STANDARD)
        num_gc_between_0_1 = len(self.groups[0].getConnectionsToGroup(1))
        for g in self.groups:
            other_groups = g.getOtherGroupIDsStartingAfterMe(self.num_groups)
            for other_group_id in other_groups:
                if len(g.getConnectionsToGroup(other_group_id)) != num_gc_between_0_1:
                    raise Exception("DragonflyPlusNetwork: Failed Verification: InterGroup Connection Uniformity")






    def commitConnection(self,conn, connType):
        if connType is ConnType.LOCAL:
            self.localConnectionsOutput.append(conn)
        else:
            self.globalConnectionsOutput.append(conn)

    def writeInterconnectionFile(self, inter_filename):
        log("\nWriting out InterConnection File '%s': " % inter_filename, Loudness.STANDARD)
        with open(inter_filename, "wb") as fd:
            for conn in sorted(self.globalConnectionsOutput,key=sortBySource):
                src_gid = conn.src_router.gid
                dest_gid = conn.dest_router.gid
                fd.write(struct.pack("2i",src_gid,dest_gid))

                log("INTER %d %d sgrp %d dgrp %d" % (src_gid, dest_gid, conn.src_router.group_id, conn.dest_router.group_id), Loudness.EXTRA_LOUD)

    def writeIntraconnectionFile(self, intra_filename):
        log("\nWriting out IntraConnection File '%s': " % intra_filename, Loudness.STANDARD)
        with open(intra_filename, "wb") as fd:
            for conn in sorted(self.localConnectionsOutput,key=sortBySource):
                if conn.src_router.group_id == 0:
                    src_id = conn.src_router.local_id
                    dest_id = conn.dest_router.local_id
                    fd.write(struct.pack("2i",src_id,dest_id))
                    log("INTRA %d %d" % (src_id, dest_id), Loudness.EXTRA_LOUD)

    def getAdjacencyMatrix(self, adj_type):
        conns = []
        if adj_type is AdjacencyType.LOCAL_LOCAL:
            conns.extend(self.localConnectionsOutput)
            size = self.num_spine_pg + self.num_leaf_pg
        elif adj_type is AdjacencyType.LOCAL_ONLY_GID:
            conns.extend(self.localConnectionsOutput)
            size = self.num_groups * (self.num_spine_pg + self.num_leaf_pg)
        elif adj_type is AdjacencyType.GLOBAL_ONLY:
            conns.extend(self.globalConnectionsOutput)
            size = self.num_groups * (self.num_spine_pg + self.num_leaf_pg)
        elif adj_type is AdjacencyType.ALL_CONNS:
            conns.extend(self.localConnectionsOutput)
            conns.extend(self.globalConnectionsOutput)
            size = self.num_groups * (self.num_spine_pg + self.num_leaf_pg)

        log("A = np.zeros((%d,%d))" % (size,size), Loudness.DEBUG)
        A = np.zeros((size,size))

        for conn in conns:
            if adj_type is AdjacencyType.LOCAL_LOCAL:
                if conn.src_router.group_id is 0:
                    A[conn.src_router.local_id][conn.dest_router.local_id] += 1
            else:
                A[conn.src_router.gid][conn.dest_router.gid] += 1

        return A


class Group(object):
    def __init__(self, group_id, group_global_radix, network):
        self.group_id = group_id
        self.group_routers = [] #list of routers in this group
        self.group_global_radix = group_global_radix #number of connections from this group to other groups
        self.used_radix = 0 #used number of connections from this group to other groups
        self.network = network

        self.groupConns = []


    def addRouter(self,router):
        self.group_routers.append(router)
        self.used_radix += router.inter_radix

    def addGlobalConnection(self, other_group):
        log("Group %d -> Group %d" % (self.group_id, other_group.group_id), Loudness.LOUD)
        self.groupConns.append(GroupConnection(self,other_group))

    def getSpineRouters(self):
        return [r for r in self.group_routers if r.routerType is RouterType.SPINE]

    def getLeafRouters(self):
        return [r for r in self.group_routers if r.routerType is RouterType.LEAF]

    def getOtherGroupIDsStartingAfterMe(self,num_groups):
        my_group_id = self.group_id
        all_group_ids = [i for i in range(num_groups) if i != my_group_id]
        return np.roll(all_group_ids, -1*my_group_id)

    def getConnectionsToGroup(self,other_group_id):
        return [conn for conn in self.groupConns if conn.dest_group.group_id == other_group_id]

    def generateLocalConnections(self):
        log("Group %d: generating local connections" % self.group_id, Loudness.LOUD)

        spine_routers = [rtr for rtr in self.group_routers if rtr.routerType == RouterType.SPINE]
        leaf_routers = [rtr for rtr in self.group_routers if rtr.routerType == RouterType.LEAF]

        for srtr in spine_routers:
            for lrtr in leaf_routers:
                srtr.connectTo(lrtr, ConnType.LOCAL)

    def assignGlobalConnectionsToRouters(self):
        log("Group %d: assigning global connections" % self.group_id, Loudness.LOUD)
        num_routers_pg = self.network.num_leaf_pg + self.network.num_spine_pg

        spine_routers = self.getSpineRouters()

        other_groups = self.getOtherGroupIDsStartingAfterMe(self.network.num_groups)
        for i,group_conn in enumerate(self.groupConns):
            src_router = spine_routers[int(i // self.network.num_global_links_per_spine)]
            dest_group = group_conn.dest_group
            dest_group_id = group_conn.dest_group.group_id
            local_spinal_id = src_router.local_id - self.network.num_leaf_pg #which spine is the source
            dest_spinal_id = self.network.num_spine_pg - local_spinal_id - 1
            dest_local_id = dest_group.getSpineRouters()[dest_spinal_id].local_id
            dest_global_id = convertLocalIDtoGID(dest_local_id, dest_group_id, num_routers_pg)
            dest_rtr = self.network.getRouter(dest_global_id)
            src_router.connectToOneWay(dest_rtr, ConnType.GLOBAL)


class Router(object):
    def __init__(self, self_gid, self_local_id, group_id, intra_radix, inter_radix, routerType, network):
        self.gid = self_gid
        self.local_id = self_local_id
        self.group_id = group_id
        self.intra_radix = intra_radix
        self.inter_radix = inter_radix
        self.routerType = routerType
        self.local_connections = []
        self.global_connections = []
        self.network = network

        log("New Router: GID: %d    LID: %d    Group %d" % (self.gid, self.local_id, self.group_id), Loudness.DEBUG)

        # local_spinal_id = self.local_id - self.network.num_leaf_pg #what is my ID in terms of num spine
        # other_groups = self.network.groups[group_id].getOtherGroupIDsStartingAfterMe(self.network.num_groups)
        # other_groups_i_connect_to_std = [g for i,g in enumerate(other_groups) if i]
        # other_groups_i_connect_to_start_index_std = (local_spinal_id * self.network.num_spine_pg) % len(other_groups)
        # other_groups_i_connect_to_start_index_addtl_gc = (other_groups_i_connect_to_start_index_std - (self.network.num_global_links_per_spine - self.network.num_spine_pg)) % len(other_groups)




    def connectTo(self, other_rtr, connType):
        if connType is ConnType.GLOBAL:
            assert(self.routerType == RouterType.SPINE)
            assert(other_rtr.routerType == RouterType.SPINE)
        conn = Connection(self,other_rtr,connType)
        oconn = Connection(other_rtr, self,connType)

        self.addConnection(conn, connType)
        other_rtr.addConnection(oconn, connType)

    def connectToOneWay(self, other_rtr, connType): #connects without connecting backward - for use if you know your loop will double count
        if connType is ConnType.GLOBAL:
            assert(self.routerType == RouterType.SPINE)
            assert(other_rtr.routerType == RouterType.SPINE)
        conn = Connection(self, other_rtr, connType)
        self.addConnection(conn, connType)

    def addConnection(self,conn, conntype):
        if conntype is ConnType.LOCAL:
            if len(self.local_connections) >= self.intra_radix:
                raise Exception("Router %d: Cannot add connection, exceeds intra_radix"% self.gid)
            self.local_connections.append(conn)
            log("New: INTRA %d %d" % (conn.src_router.local_id, conn.dest_router.local_id), Loudness.EXTRA_LOUD)

        elif conntype is ConnType.GLOBAL:
            if len(self.global_connections) >= self.inter_radix:
                raise Exception("Router %d: Cannot add connection, exceeds inter_radix"% self.gid)
            self.global_connections.append(conn)
            log("New: INTER %d %d  | Group: %d -> %d" % (conn.src_router.gid, conn.dest_router.gid, conn.src_router.group_id, conn.dest_router.group_id), Loudness.EXTRA_LOUD)
        else:
            raise Exception("Invalid Connection Type")

        self.network.commitConnection(conn, conntype)


class Connection(object):
    def __init__(self, src_router, dest_router, connType, shifted_by=0):
        self.src_router = src_router
        self.dest_router = dest_router
        self.connType = connType
        self.shifted_by = shifted_by

    def __getitem__(self, key):
        if type(key) is str:
            if key == "src":
                return self.src_router
            if key == "dest":
                return self.dest_router

        elif type(key) is int:
            if key is 0:
                return self.src_router
            elif key is 1:
                return self.dest_router
            else:
                raise IndexError("Connection: index out of range")

        else:
            raise KeyError("Connection: Invalid __getitem__() key")

class GroupConnection(object):
    def __init__(self, src_group, dest_group):
        self.src_group = src_group
        self.dest_group = dest_group

def parseOptionArguments():
    global DRYRUN
    global LOUDNESS
    global SHOW_ADJACENCY
    global NO_OUTPUT_FILE

    if "--debug" in argv:
        LOUDNESS = Loudness.DEBUG

    if "--extra-loud" in argv:
        LOUDNESS = Loudness.EXTRA_LOUD

    if "--loud" in argv:
        LOUDNESS = Loudness.LOUD

    if "--quiet" in argv:
        LOUDNESS = Loudness.QUIET

    if "--dry-run" in argv:
        DRYRUN = 1

    if "--no-output" in argv:
        NO_OUTPUT_FILE = 1

    if "--show-adjacency" in argv:
        SHOW_ADJACENCY = 1


#mainV3() assumes things about the network:
#   num_spine_pg == num_leaf_pg
#   term_per_leaf = num_spine_pg
#   num_router_pg = router_radix (half up half down)
#   total_gc_per_group = router_radix / 2
def mainV3():
    parseOptionArguments()

    if DRYRUN or NO_OUTPUT_FILE:
        if DRYRUN and NO_OUTPUT_FILE:
            option = '--dry-run'
        elif DRYRUN:
            option = '--dry-run'
        elif NO_OUTPUT_FILE:
            option = '--no-output'
        if(len(argv) < 4):
            raise Exception("Correct usage: python %s <router_radix> <num_gc_between_groups> %s" % (sys.argv[0], option))

    elif(len(argv) < 5):
        raise Exception("Correct usage:  python %s <router_radix> <num_gc_between_groups> <intra-file> <inter-file>" % sys.argv[0])

    router_radix = int(argv[1])
    num_gc_between_groups = int(argv[2])

    global desired_num_gc_bg
    desired_num_gc_bg = num_gc_between_groups

    largest_num_groups = (router_radix//2)**2 + 1
    num_groups = (largest_num_groups - 1) // num_gc_between_groups + 1

    num_router_pg = int(router_radix)
    num_spine_pg = int(num_router_pg / 2)
    num_leaf_pg = num_spine_pg
    term_per_leaf = num_spine_pg

    if not (DRYRUN or NO_OUTPUT_FILE):
        intra_filename = argv[3]
        inter_filename = argv[4]


    try:
        dfp_network = DragonflyPlusNetwork(num_groups, num_spine_pg, num_leaf_pg, router_radix, num_hosts_per_leaf=term_per_leaf)
    except Exception as err:
        print("ERROR: ",err)
        exit(1)
        

    if not DRYRUN:
        if not NO_OUTPUT_FILE:
            dfp_network.writeIntraconnectionFile(intra_filename)
            dfp_network.writeInterconnectionFile(inter_filename)

    if LOUDNESS is not Loudness.QUIET:
        print(dfp_network.getSummary())

    if SHOW_ADJACENCY == 1:
        print("\nPrinting Adjacency Matrix:")

        np.set_printoptions(linewidth=400,threshold=10000,edgeitems=200)
        A = dfp_network.getAdjacencyMatrix(AdjacencyType.ALL_CONNS)
        print(A.astype(int))


def mainV2():
    if(len(argv) < 8):
        raise Exception("Correct usage:  python %s <num_groups> <num_spine_pg> <num_leaf_pg> <router_radix> <terminals-per-leaf> <intra-file> <inter-file>" % sys.argv[0])

    num_groups = int(argv[1])
    num_spine_pg = int(argv[2])
    num_leaf_pg = int(argv[3])
    router_radix = int(argv[4])
    term_per_leaf = int(argv[5])
    intra_filename = argv[6]
    inter_filename = argv[7]

    parseOptionArguments()

    dfp_network = DragonflyPlusNetwork(num_groups, num_spine_pg, num_leaf_pg, router_radix, num_hosts_per_leaf=term_per_leaf)

    if not DRYRUN:
        dfp_network.writeIntraconnectionFile(intra_filename)
        dfp_network.writeInterconnectionFile(inter_filename)

    if LOUDNESS is not Loudness.QUIET:
        print("\nNOTE: THIS STILL CAN'T DO THE MED-LARGE TOPOLOGY RIGHT\n")

        print(dfp_network.getSummary())

    if SHOW_ADJACENCY == 1:
        print("\nPrinting Adjacency Matrix:")

        np.set_printoptions(linewidth=400,threshold=10000,edgeitems=200)
        A = dfp_network.getAdjacencyMatrix(AdjacencyType.ALL_CONNS)
        print(A.astype(int))

if __name__ == '__main__':
    mainV3()
