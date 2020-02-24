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
import math
DRYRUN = 0
SPECTRAL = 0
COMPONENTS = 0


def get_assigned_router_id_from_terminal(params, term_gid, rail_id):
    num_planes = params.num_planes
    num_rails = params.num_injection_rails
    num_cn_per_group = params.num_leaf_per_group * params.num_hosts_per_leaf

    group_id = int(term_gid / num_cn_per_group)
    planar_group_id = int(group_id % params.num_groups)
    local_router_id = int(term_gid / params.num_hosts_per_leaf) % params.num_leaf_per_group
    router_id_on_plane_0 = int(planar_group_id * params.num_routers_per_group) + local_router_id

    if num_planes == 1:
        if num_rails == 1:
            return int(router_id_on_plane_0)

        else: #now just all rails go to same router - possibly change this later
            return int(router_id_on_plane_0)
    else:
        rpp = params.total_routers / params.num_planes
        if num_planes == num_rails:
            return int(router_id_on_plane_0 + (rail_id * rpp))
        else:
            print("different rails per plane not allowed - only rails == planes")
            exit(0)



class LinkType(Enum):
    INTRA = 1
    INTER = 2
    TERMINAL = 3
    WILDROUTER = 4



class Network(object):
    def __init__(self, params):
        self.link_list = []
        self.link_map = {}
        self.adj_map = {}

        self.term_link_list = []
        self.term_link_map = {}

        self.num_intra_links = 0
        self.num_inter_links = 0
        self.num_terminal_links = 0

        self.num_intra_failed = 0
        self.num_inter_failed = 0
        self.num_terminal_failed = 0

        self.params = params

    def add_link(self, src_gid, dest_gid, plane_id, link_type):
        id_set = frozenset([src_gid, dest_gid, link_type])
        
        if id_set not in self.link_map:
            # print("New Link %d -> %d"%(src_gid,dest_gid))
            new_link = Link(src_gid, dest_gid, plane_id, link_type)
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
                new_link = Link(src_gid, dest_gid, plane_id, link_type)
                self.link_map[id_set].append(new_link)
                self.link_list.append(new_link)

        self.num_intra_links = len([link for link in self.link_list if link.link_type == LinkType.INTRA])
        self.num_inter_links = len([link for link in self.link_list if link.link_type == LinkType.INTER])

    def add_term_link(self, router_gid, terminal_gid, rail_id):
        id_set = frozenset([router_gid, terminal_gid, LinkType.TERMINAL])
        # num_existing_links = 0
        # if id_set in self.term_link_map:
        #     num_existing_links = len(self.term_link_map[id_set])

        new_link = Link(router_gid, terminal_gid, rail_id, LinkType.TERMINAL)

        if id_set not in self.term_link_map:
            self.term_link_map[id_set] = [new_link]
        else:
            self.term_link_map[id_set].append(new_link)

        # print("New Term Link r%d -> t%d"%(router_gid,terminal_gid), end=' ')
        # print(id_set)

        self.term_link_list.append(new_link)
        self.num_terminal_links += 1

    def fail_links(self, link_type, percent_to_fail, plane_id = -1):
        if link_type == LinkType.WILDROUTER:
            if plane_id is -1:
                links_to_consider = [link for link in self.link_list if link.link_type != LinkType.TERMINAL]
            else:
                links_to_consider = [link for link in self.link_list if link.rail_id == plane_id]
        else:
            if plane_id is -1: #then we don't care which plane it comes from
                links_to_consider = [link for link in self.link_list if link.link_type == link_type] 
            else: #only fail from specific plane
                links_to_consider = [link for link in self.link_list if link.link_type == link_type if link.rail_id == plane_id] 

        num_to_fail = int(len(links_to_consider)*percent_to_fail)

        links_to_fail = random.sample(links_to_consider, k=num_to_fail)

        for link in links_to_fail:
            link.is_failed = True
            if link.link_type is LinkType.INTRA:
                self.num_intra_failed += 1
            elif link.link_type is LinkType.INTER:
                self.num_inter_failed += 1
            else:
                self.num_terminal_failed += 1

    def fail_term_links_safe(self, percent_to_fail):
        if self.params.num_planes == 1: #then all rails on a router go to same terminal
            for router_id in range(self.params.total_routers):
                for tlid in range(self.params.num_hosts_per_leaf):
                    for i in range(self.params.num_injection_rails-1): #will always leave one rail untouched
                        fail_roll = random.random()
                        if fail_roll > percent_to_fail:
                            term_gid = self.params.num_hosts_per_leaf * router_id + tlid

                            id_set = frozenset([router_id, term_gid, LinkType.TERMINAL])
                            for link in self.term_link_map[id_set]:
                                if link.is_failed == 0:
                                    link.is_failed = 1
                                    self.num_terminal_failed += 1
                                    break

    def get_adjacency_matrix(self, include_failed_links=False, plane_id=0):
        A = np.zeros((self.params.routers_per_plane, self.params.routers_per_plane))
        rpp = self.params.routers_per_plane


        for link in self.link_list:
            if link.rail_id == plane_id:
                if not include_failed_links:
                    if not link.is_failed:
                        A[link.src_gid%rpp,link.dest_gid%rpp] += 1
                        A[link.dest_gid%rpp,link.src_gid%rpp] += 1
                else:
                    A[link.src_gid%rpp,link.dest_gid%rpp] += 1
                    A[link.dest_gid%rpp,link.src_gid%rpp] += 1

        return A
    
    def calc_adjacency_map(self, include_failed_links=False):
        for link in self.link_list:
            plane_id = link.rail_id
            if link.is_failed:
                continue

            if plane_id not in self.adj_map:
                self.adj_map[plane_id] = {}

            if link.src_gid in self.adj_map[plane_id]:
                self.adj_map[plane_id][link.src_gid].append(link.dest_gid)
            else:
                self.adj_map[plane_id][link.src_gid] = [link.dest_gid]
            if link.dest_gid in self.adj_map[plane_id]:
                self.adj_map[plane_id][link.dest_gid].append(link.src_gid)
            else:
                self.adj_map[plane_id][link.dest_gid] = [link.src_gid]
        

    def get_laplacian(self, include_failed_links=False, plane_id=0):
        A = self.get_adjacency_matrix(include_failed_links, plane_id)        
        L = csgraph.laplacian(A)
        return L

    def dfs_recursive(self, gid, visited, plane_id):
        visited[gid%self.params.routers_per_plane] = True

        for neighbor in self.adj_map[plane_id][gid]:
            neighbor_pgid = neighbor % self.params.routers_per_plane
            if visited[neighbor_pgid] == False:
                self.dfs_recursive(neighbor, visited, plane_id)
    
    def is_connected(self, plane_id):
        visited = [False] * (self.params.routers_per_plane)
        try:
            self.dfs_recursive(0+(plane_id*self.params.routers_per_plane), visited, plane_id)
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
    def __init__(self, radix, num_conn_between_groups, total_terminals, num_injection_rails, num_planes, failure_mode):
        self.router_radix = radix
        self.num_conn_between_groups = num_conn_between_groups
        self.total_terminals = total_terminals
        self.num_injection_rails = num_injection_rails
        self.num_planes = num_planes

        self.num_routers_per_group = int((radix)) #a = (radix + 1)/2
        self.num_spine_per_group = self.num_routers_per_group/2
        self.num_leaf_per_group = self.num_spine_per_group
        self.num_gc_per_spine = int(self.num_routers_per_group // 2)
        self.num_gc_per_group = self.num_gc_per_spine * self.num_spine_per_group

        largest_num_groups = (radix//2)**2 + 1
        num_groups = (largest_num_groups - 1) // num_conn_between_groups + 1

        self.num_groups = num_groups
        self.routers_per_plane = self.num_routers_per_group * self.num_groups
        self.total_routers = self.routers_per_plane * self.num_planes
        self.num_hosts_per_leaf = int(total_terminals / (self.total_routers/2))
        self.cn_radix = num_injection_rails * self.num_hosts_per_leaf
        
        self.failure_mode = failure_mode


    def getSummary(self):
        outStr = "\nDragonfly (Dally) Network:\n"
        outStr += "\tNumber of Groups (per plane): %d\n" % self.num_groups
        outStr += "\tRouter Radix:                 %d\n" % self.router_radix
        outStr += "\tNumber Spine Per Group:       %d\n" % self.num_spine_per_group
        outStr += "\tNumber Leaf Per Group:        %d\n" % self.num_spine_per_group
        outStr += "\tNumber Terminal Per Leaf:     %d\n" % self.num_hosts_per_leaf
        outStr += "\n"
        outStr += "\tNumber Term Links per Router: %d\n" % self.cn_radix
        outStr += "\n"
        outStr += "\tNumber GC per Router:         %d\n" % self.num_gc_per_spine
        outStr += "\tNumber GC per Group:          %d\n" % self.num_gc_per_group
        outStr += "\tNumber GC between Groups:     %d\n" % self.num_conn_between_groups
        outStr += "\n"
        outStr += "\tNum Routers per plane:        %d\n" % self.routers_per_plane
        outStr += "\tTotal Routers:                %d\n" % self.total_routers
        outStr += "\tTotal Number Terminals:       %d\n" % (self.total_terminals)
        outStr += "\t"
        return outStr

class Link(object):
    def __init__(self, src_gid, dest_gid, rail_id, link_type):
        self.src_gid = src_gid
        self.dest_gid = dest_gid
        self.link_type = link_type
        self.rail_id = rail_id
        self.is_failed = False
        self.bi_link_added = False # so I can keep track of parallel links, if this is true, and we try to add a link that matches this one, then a new one must be created

    def fail_link(self):
        self.is_failed = True

    def __lt__(self, other):
        return (self.src_gid < other.src_gid)

    def __str__(self):
        return "srcgid: %d  destgid:  %d  type:  %s   is_failed:  %d"%(self.src_gid,self.dest_gid,self.link_type,self.is_failed)


def main():
    global DRYRUN, SPECTRAL, COMPONENTS
    if "--dryrun" in argv:
        DRYRUN = 1
    if "--spectral" in argv:
        SPECTRAL = 1
    if "--components" in argv:
        COMPONENTS = 1

    if(len(argv) < 9):
        raise Exception("Correct usage:  python %s <router_radix> <num_conn_between_groups> <num_terminals> <num_injection_rails> <num_planes> <intra-file> <inter-file> <output-fail-file>" % sys.argv[0])

    router_radix = int(argv[1])
    num_conn_between_groups = int(argv[2])
    num_terminals = int(argv[3])
    num_injection_rails = int(argv[4])
    num_planes = int(argv[5])
    percent_to_fail = float(argv[6])
    # percent_intra_fail = float(argv[6])
    # percent_inter_fail = float(argv[7])

    params = Params(router_radix, num_conn_between_groups, num_terminals, num_injection_rails, num_planes, 1)
    net = Network(params)

    print(params.getSummary())


    if not DRYRUN:
        with open(argv[7],"rb") as intra:
            loadIntra(params, intra, net)
        with open(argv[8],"rb") as inter:
            loadInter(params, inter, net)

        add_terminal_links(params, net)

        net.fail_links(LinkType.WILDROUTER, percent_to_fail, -1) #fail any link, with a given percentage total, from any plane
        # net.fail_links(LinkType.INTRA, percent_intra_fail, 0)
        # net.fail_links(LinkType.INTER, percent_inter_fail, 0)
        # net.fail_term_links_safe(.5)

        # print(net)

        meta_content = ""
        meta_content += params.getSummary() + '\n'
        meta_content += "Number Intra Failed %d/%d\n"%(net.num_intra_failed, net.num_intra_links)
        meta_content += "Number Inter Failed %d/%d\n"%(net.num_inter_failed, net.num_inter_links)
        meta_content += "Number Term Failed  %d/%d\n"%(net.num_terminal_failed, net.num_terminal_links)

        with open(argv[9],"wb") as failfd:
            meta_content += writeFailed(params, failfd, net) + "\n"

        net.calc_adjacency_map()
        for p in range(params.num_planes):
            
            is_connected = net.is_connected(p)

            if is_connected:
                outstr = "Plane %d is Connected "%p
                meta_content += outstr
            else:
                outstr = "Plane %d is Disconnected "%p
                meta_content += outstr

            if (COMPONENTS):
                n_components = csgraph.connected_components(net.get_adjacency_matrix(plane_id=p), return_labels=False)
                outstr = "calculated %d components, "%(n_components)
                meta_content += outstr
    

            if (SPECTRAL):
                L = net.get_laplacian(include_failed_links=False,plane_id=p)
                eigs = la.eigvals(L)
                eigs = sorted(eigs)
                connectivity = np.real(eigs[1])
                outstr = "calculated %.9f 2nd lowest eigenvalue (real-part) "%connectivity
                meta_content += outstr
            
            meta_content += "\n"

        print(meta_content)

        metafilename = argv[9] + "-meta"
        with open(metafilename,"w") as metafd:
            metafd.write(meta_content)

def loadIntra(params, fd, net):
    intra_id_pairs = []

    while(True):
        buf = fd.read(8)
        if not buf:
            break
        (src_lid, dest_lid) = struct.unpack("2i",buf)
        intra_id_pairs.append((src_lid,dest_lid))

    # print("loaded %d intra links"%len(intra_id_pairs))
    # print(intra_id_pairs)

    for p in range(params.num_planes):
        for i in range(params.num_groups):
            for pair in intra_id_pairs:
                (src_lid,dest_lid) = pair
                src_gid = (i * params.num_routers_per_group + src_lid) + (p*params.routers_per_plane)
                dest_gid = (i * params.num_routers_per_group + dest_lid) + (p*params.routers_per_plane)
                
                net.add_link(src_gid, dest_gid, p, LinkType.INTRA)



def loadInter(params, fd, net):
    inter_id_pairs = []

    while(True):
        buf = fd.read(8)
        if not buf:
            break
        (src_gid,dest_gid) = struct.unpack("2i",buf)
        inter_id_pairs.append((src_gid,dest_gid))

    for p in range(params.num_planes):
        for pair in inter_id_pairs:
            (src_pgid,dest_pgid) = pair
            src_gid = src_pgid + (p*params.routers_per_plane)
            dest_gid = dest_pgid + (p*params.routers_per_plane)

            net.add_link(src_gid, dest_gid, p, LinkType.INTER)

def add_terminal_links(params, net):
    total_terminals = params.total_terminals
    num_rails = params.num_injection_rails
    
    for tgid in range(total_terminals):
        for rail_id in range(num_rails):
            assigned_router_id = get_assigned_router_id_from_terminal(params, tgid, rail_id)
            # print("Gen r%d -> t%d rail %d"%(assigned_router_id, tgid,rail_id))
            net.add_term_link(assigned_router_id, tgid, rail_id)

                



def writeFailed(params, fd, net):
    failed_links = [link for link in net.link_list if link.is_failed]
    failed_terms = [link for link in net.term_link_list if link.is_failed]
    failed_links.extend(failed_terms)

    # for link in failed_interlinks:
    #     print(str(link))

    for link in sorted(failed_links):
        src_gid = link.src_gid
        dest_gid = link.dest_gid
        rail_id = link.rail_id
        link_type = link.link_type

        outstr = ''
        fd.write(struct.pack("4i",src_gid, dest_gid, rail_id, link_type.value))
        if link_type is not LinkType.TERMINAL: #terminal link bidirectional is handled by the model
            fd.write(struct.pack("4i",dest_gid, src_gid, rail_id, link_type.value))
        outstr+= "%d <-> %d  rail=%d  type=%s   Failed\n"%(src_gid, dest_gid, rail_id, link_type)
        print(outstr,end='')

    outstr = "Written %d failed bidirectional links (%d total written links)"%(len(failed_links),len(failed_links)*2)
    print(outstr)
    return outstr 

if __name__ == "__main__":
    main()