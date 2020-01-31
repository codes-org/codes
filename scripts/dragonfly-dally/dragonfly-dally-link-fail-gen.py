# Copyright 2020 - Neil McGlohon
# mcglon@rpi.edu

import sys
from enum import Enum
import struct
import numpy as np
import random
import scipy.sparse as sps
import scipy.linalg as la
from scipy.sparse import csgraph
argv = sys.argv
DRYRUN = 0
SPECTRAL = 0
COMPONENTS = 0



class LinkType(Enum):
    INTRA = 1,
    INTER = 2


class Network(object):
    def __init__(self, params):
        self.link_list = []
        self.link_map = {}
        self.adj_map = {}
        self.num_intra_links = 0
        self.num_inter_links = 0

        self.num_intra_failed = 0
        self.num_inter_failed = 0

        self.params = params

    def add_link(self, src_gid, dest_gid, link_type):
        id_set = frozenset([src_gid, dest_gid, link_type])
        
        if id_set not in self.link_map:
            # print("New Link %d -> %d"%(src_gid,dest_gid))
            new_link = Link(src_gid, dest_gid, link_type)
            self.link_map[id_set] = [new_link]
            self.link_list.append(new_link)
        elif id_set in self.link_map:
            done = False
            for link in self.link_map[id_set]:
                if link.bi_link_added == False:
                    link.bi_link_added = True
                    done = True
                    # print("Complete %d <-> %d"%(src_gid,dest_gid))
                    break
                if not done:
                    # print("New Link %d -> %d"%(src_gid,dest_gid))
                    new_link = Link(src_gid, dest_gid, link_type)
                    self.link_map[id_set].append(new_link)
                    self.link_list.append(new_link)

        self.num_intra_links = len([link for link in self.link_list if link.link_type == LinkType.INTRA])
        self.num_inter_links = len([link for link in self.link_list if link.link_type == LinkType.INTER])

    def fail_links(self, link_type, percent_to_fail):
        links_to_consider = [link for link in self.link_list if link.link_type == link_type]    
        num_to_fail = int(len(links_to_consider)*percent_to_fail)

        links_to_fail = random.sample(links_to_consider, k=num_to_fail)

        for link in links_to_fail:
            link.is_failed = True
            if link_type is LinkType.INTRA:
                self.num_intra_failed += 1
            else:
                self.num_inter_failed += 1

    def get_adjacency_matrix(self, include_failed_links=False):
        A = np.zeros((self.params.total_routers, self.params.total_routers))
        for link in self.link_list:
            if not include_failed_links:
                if not link.is_failed:
                    A[link.src_gid,link.dest_gid] += 1
                    A[link.dest_gid,link.src_gid] += 1
            else:
                A[link.src_gid,link.dest_gid] += 1
                A[link.dest_gid,link.src_gid] += 1

        return A
    
    def calc_adjacency_map(self, include_failed_links=False):
        for link in self.link_list:
            if link.is_failed:
                continue

            if link.src_gid in self.adj_map:
                self.adj_map[link.src_gid].append(link.dest_gid)
            else:
                self.adj_map[link.src_gid] = [link.dest_gid]
            if link.dest_gid in self.adj_map:
                self.adj_map[link.dest_gid].append(link.src_gid)
            else:
                self.adj_map[link.dest_gid] = [link.src_gid]
        

    def get_laplacian(self, include_failed_links=False):
        A = self.get_adjacency_matrix(include_failed_links)        
        L = csgraph.laplacian(A)
        return L

    def dfs_recursive(self, gid, visited):
        visited[gid] = True

        for neighbor in self.adj_map[gid]:
            if visited[neighbor] == False:
                self.dfs_recursive(neighbor, visited)
    
    def is_connected(self):
        visited = [False] * (self.params.total_routers)
        self.calc_adjacency_map()
        try:
            self.dfs_recursive(0, visited)
        except:
            return False

        return all(visited)

    def __str__(self):
        the_str = ""
        for src_id in range(self.params.total_routers):
            for dest_id in range(self.params.total_routers):
                id_set = frozenset([src_id, dest_id, LinkType.INTRA])
                if id_set in self.link_map:
                    for link in self.link_map[id_set]:
                        the_str += "%d -> %d    %s,  Failed=%s\n"%(src_id, dest_id, str(link.link_type), str(link.is_failed))

        for src_id in range(self.params.total_routers):
            for dest_id in range(self.params.total_routers):
                id_set = frozenset([src_id, dest_id, LinkType.INTER])
                if id_set in self.link_map:
                    for link in self.link_map[id_set]:
                        the_str += "%d -> %d    %s,  Failed=%s\n"%(src_id, dest_id, str(link.link_type), str(link.is_failed))

        
        return the_str

class Params(object):
    def __init__(self, radix, num_conn_between_groups, failure_mode):
        self.router_radix = radix
        self.num_conn_between_groups = num_conn_between_groups

        self.num_routers_per_group = int((radix + 1)/2) #a = (radix + 1)/2
        self.num_hosts_per_router = int(self.num_routers_per_group // 2)
        self.num_gc_per_router = int(self.num_routers_per_group // 2)
        self.num_gc_per_group = self.num_gc_per_router * self.num_routers_per_group

        num_gc_per_group = self.num_gc_per_router * self.num_routers_per_group
        self.num_groups = int((num_gc_per_group / self.num_conn_between_groups)) + 1
        self.total_routers = self.num_routers_per_group * self.num_groups
        
        self.failure_mode = failure_mode


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

class Link(object):
    def __init__(self, src_gid, dest_gid, link_type):
        self.src_gid = src_gid
        self.dest_gid = dest_gid
        self.link_type = link_type
        self.is_failed = False
        self.bi_link_added = False # so I can keep track of parallel links, if this is true, and we try to add a link that matches this one, then a new one must be created

    def fail_link(self):
        self.is_failed = True



def main():
    global DRYRUN, SPECTRAL, COMPONENTS
    if "--dryrun" in argv:
        DRYRUN = 1
    if "--spectral" in argv:
        SPECTRAL = 1
    if "--components" in argv:
        COMPONENTS = 1

    if(len(argv) < 6):
        raise Exception("Correct usage:  python %s <router_radix> <num_conn_between_groups> <intra-file> <inter-file> <output-fail-file>" % sys.argv[0])

    router_radix = int(argv[1])
    num_conn_between_groups = int(argv[2])



    params = Params(router_radix, num_conn_between_groups, 1)
    net = Network(params)

    print(params.getSummary())


    if not DRYRUN:
        with open(argv[3],"rb") as intra:
            loadIntra(params, intra, net)
        with open(argv[4],"rb") as inter:
            loadInter(params, inter, net)

        net.fail_links(LinkType.INTRA, .5)
        net.fail_links(LinkType.INTER, .5)

        print(net)

        print("Number Intra Failed %d/%d"%(net.num_intra_failed, net.num_intra_links))
        print("Number Inter Failed %d/%d"%(net.num_inter_failed, net.num_inter_links))

        with open(argv[5],"wb") as failfd:
            writeFailed(params, failfd, net)

        is_connected = net.is_connected()

        if is_connected:
            print("Graph is Connected,",end=' ')
        else:
            print("Graph is Disconnected,",end=' ')

        if (COMPONENTS):
            n_components = csgraph.connected_components(net.get_adjacency_matrix(), return_labels=False)
            print("calculated %d components,"%n_components,end=' ')
    

        if (SPECTRAL):
            L = net.get_laplacian(include_failed_links=False)
            eigs = la.eigvals(L)
            eigs = sorted(eigs)
            connectivity = np.real(eigs[1])
            print("and %.9f 2nd lowest eigenvalue (real-part)"%connectivity)



def loadIntra(params, fd, net):
    intra_id_pairs = []

    while(True):
        buf = fd.read(12)
        if not buf:
            break
        (src_lid, dest_lid, dummy) = struct.unpack("3i",buf)
        intra_id_pairs.append((src_lid,dest_lid))

    for i in range(params.num_groups):
        for pair in intra_id_pairs:
            (src_lid,dest_lid) = pair
            src_gid = i * params.num_routers_per_group + src_lid
            dest_gid = i * params.num_routers_per_group + dest_lid
            
            net.add_link(src_gid, dest_gid, LinkType.INTRA)



def loadInter(params, fd, net):

    while(True):
        buf = fd.read(8)
        if not buf:
            break
        (src_gid,dest_gid) = struct.unpack("2i",buf)
        
        net.add_link(src_gid, dest_gid, LinkType.INTER)


def writeFailed(params, fd, net):
    failed_links = [link for link in net.link_list if link.is_failed]

    for link in failed_links:
        src_gid = link.src_gid
        dest_gid = link.dest_gid

        fd.write(struct.pack("2i",src_gid, dest_gid))    
        print("%d <-> %d  Failed"%(src_gid, dest_gid))   

    print("Written %d failed bidirectional links"%len(failed_links)) 


if __name__ == "__main__":
    main()