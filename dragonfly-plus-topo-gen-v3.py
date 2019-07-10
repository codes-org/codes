# yao kang (ykang17@hawk.iit.edu)
#
# Based on dragonfly-plus-topo-gen-v2.py, 
# implements relative global link arrangement
#
# Assumptions: num_router_per_group = router_radix, half spine half leaf, 
# a -- number of routers in a group; h -- number of global link per spine; 
# r -- router radix; g -- number of groups
# a == r, and (a/2)*h is divisible by (g-1)
#
# USAGE
# python <router_radix> <num_gc_per_spine> <num_groups> <intra-file> <inter-file>

import sys
from enum import Enum
import struct
import numpy as np

from collections import defaultdict

if sys.version_info[0] < 3:
    raise Exception("Python 3 or a more recent version is required.")

argv = sys.argv
if(len(argv) < 6):
        raise Exception("Correct usage:  python %s <router_radix> <num_gc_per_spine> <num_groups> <intra-file> <inter-file>" % sys.argv[0])
router_radix = int(argv[1])
num_gc_per_spine = int(argv[2])
num_groups = int(argv[3])
intraf = argv[4]
interf = argv[5]

num_spine_pg = router_radix//2
num_leaf_pg = router_radix//2
num_router_per_group = router_radix
num_hosts_per_leaf = router_radix//2
num_gc_per_group = num_gc_per_spine * num_spine_pg
max_num_group = num_gc_per_group + 1
num_gc_bt_group = num_gc_per_group // (num_groups-1)

def writeIntraconnectionFile(intra_filename):
    print("\nWriting out IntraConnection File '%s'" % intra_filename)
    with open(intra_filename, "wb") as fd:
        #Numbering: First leaf router than spine router
        for src in range(num_router_per_group):
            if src < num_leaf_pg: # I am a leaf router
                for des in range(num_spine_pg):
                    des_id = des + num_leaf_pg
                    fd.write(struct.pack("2i", src, des_id))
            else: # I am a spine router
                for des in range(num_leaf_pg):
                    fd.write(struct.pack("2i", src, des))

def rtrgid(k, g):
    id_r2_spine = k // num_gc_per_spine
    id_r2_group = id_r2_spine + num_leaf_pg
    assert(id_r2_group <= num_router_per_group)
    gid = g * num_router_per_group + id_r2_group
    return gid

def writeInterconnectionFile(inter_filename):
    print("\nWriting out InterConnection File '%s'" % inter_filename)
    with open(inter_filename, "wb") as fd:
        if num_gc_per_spine >= (num_groups-1):
            ite = num_gc_per_spine // (num_groups-1)
            for srcg in range(num_groups):
                for desg in range(srcg+1, num_groups):
                    for src_r in range(num_spine_pg):
                        src_r_gid = src_r + num_leaf_pg + srcg * num_router_per_group
                        for i in range(ite):
                            des_r = (src_r + i) % num_spine_pg
                            des_r_gid = des_r + num_leaf_pg + desg * num_router_per_group
                            # print("%d[%d] => %d[%d]" %(src_r_gid, srcg, des_r_gid, desg))
                            fd.write(struct.pack("2i",src_r_gid, des_r_gid))
                            fd.write(struct.pack("2i",des_r_gid, src_r_gid))

        else:
            for srcg in range(num_groups):
                for desg in range(num_groups):
                    if srcg != desg:
                        for i in range(num_gc_bt_group):
                            src_p = ((desg - srcg + num_groups) % num_groups) -1 + (num_groups -1) * i 
                            des_p = ((srcg - desg + num_groups) % num_groups) -1 + (num_groups -1) * i 
                            src_rtr_gid = rtrgid(src_p, srcg)
                            dest_rtr_gid = rtrgid(des_p, desg)
                            # print("%d[%d] => %d[%d]" %(src_rtr_gid, srcg, dest_rtr_gid, desg))
                            fd.write(struct.pack("2i",src_rtr_gid, dest_rtr_gid))
                      
def main():
    assert (max_num_group >= num_groups)
    assert(router_radix % 2 == 0)
    assert (router_radix//2 >= num_gc_per_spine)

    outStr = "\nDragonfly Plus Network:\n"
    outStr += "\tNumber of Groups:          %d\n" % num_groups
    outStr += "\tRouter Radix:              %d\n" % router_radix
    outStr += "\tNumber Spine Per Group:    %d\n" % num_spine_pg
    outStr += "\tNumber Leaf Per Group:     %d\n" % num_leaf_pg
    # outStr += "\tNumber Terminal Per Leaf:  %d\n" % num_hosts_per_leaf
    outStr += "\n"
    outStr += "\tNumber GC per Spine:       %d\n" % num_gc_per_spine
    outStr += "\tNumber GC per Group:       %d\n" % num_gc_per_group
    outStr += "\tNumber GC between Groups:  %d\n" % num_gc_bt_group
    outStr += "\n"
    outStr += "\tTotal Spine:               %d\n" % (num_spine_pg * num_groups)
    outStr += "\tTotal Leaf:                %d\n" % (num_leaf_pg * num_groups)
    outStr += "\tTotal Routers:             %d\n" % ((num_leaf_pg + num_spine_pg) * num_groups)
    outStr += "\tMax Number Terminals:    %d\n" % (num_leaf_pg * num_hosts_per_leaf * num_groups)
    outStr += "\t"

    print(outStr)

    writeIntraconnectionFile(intraf)
    writeInterconnectionFile(interf)

if __name__ == '__main__':
    main()