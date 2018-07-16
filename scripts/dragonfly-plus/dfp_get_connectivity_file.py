# Neil McGlohon - Rensselaer Polytechnic Institute (c) 2018

import sys
from enum import Enum
import re
import struct
import csv
argv = sys.argv

# ----------------------- README -------------------------
# Usage: python3 dfp_get_connectivity_file.py <config_filename> <intra_filename> <inter_filename> <output_filename>

# Note: This assumes that the modelnet_order is that of (terminal, router). This will break otherwise.


# Example usage:
#   Have configuration file:      "example/dfp-test.conf"
#   Have intra-connection file:   "example/dfp-test-intra"
#   Have inter-connection file:   "example/dfp-test-inter"
#   Want output to file:          "example/output"
#
#   python3 dfp_get_connectivity_file.py example/dfp-test.conf example/dfp-test-intra example/dfp-test-inter example/output


# Options:
#   --quiet     Suppress print output
#   --debug     Show all print output

# --------------------------------------------------------


def auto_str(cls):
    def __str__(self):
        return '%s(%s)' % (type(self).__name__,', '.join('%s=%s' % item for item in sorted(vars(self).items())))
    cls.__str__ = __str__
    return cls



# ENUMS --------------------------------------------------

class Loudness(Enum):
    DEBUG = 0 #prints all output
    LOUD = 2 #prints a lot of output
    STANDARD = 3 #prints what step its working on
    QUIET = 4 #print no output

class LP_Type(Enum):
    ROUTER = 0
    TERMINAL = 1
    MODELNET = 3 #LPs that aren't relevant to the model topology

class ConnectionType(Enum):
    INTRA = 0
    INTER = 1
    TERM = 3

# CLASSES -------------------------------------------------

@auto_str
class Params():
    def __init__(self):
        self.loaded = False
        #Modelnet Group Parameters
        self.mn_num_rep = 0
        self.mn_nw_lp = 0
        self.mn_num_term_rep = 0
        self.mn_num_router_rep = 0

        #derived modelnet params
        self.mn_num_lp_per_rep = 0
        self.mn_total_lps = 0 #total LPs including those from nw-lp

        #Dragonfly Custom Parameters
        self.num_groups = 0
        self.num_router_spine = 0
        self.num_router_leaf = 0
        self.num_term_per_router = 0
        self.intra_filename = 0
        self.inter_filename = 0

        #derived dragonfly params
        self.num_routers_per_group = 0
        self.num_terminals_per_group = 0
        self.total_leaf = 0
        self.total_spine = 0
        self.total_routers = 0
        self.total_terminals = 0
        self.total_df_lps = 0 #Doesn't care about nw-lp stuff


    def load_config(self, filename, intra_filename, inter_filename):
        log("Loading Configuraiton...")

        with open(filename) as f:
            for line in f:
                line = line.strip()
                line = line.split("=") #there was weird behavior, modelnet_dragonfly_custom_router wasn't being found
                val = re.findall('"([^"]*)"', line[-1]) #regex for the value in quotes
                if len(val) > 0:
                    val = val[0] #OOR if the regex finds no parameter - this conditional avoids that error

                if "repetitions" in line:
                    self.mn_num_rep = int(val)
                elif "nw-lp" in line:
                    self.mn_nw_lp = int(val)
                elif "modelnet_dragonfly_plus" in line:
                    self.mn_num_term_rep = int(val)
                elif "modelnet_dragonfly_plus_router" in line:
                    self.mn_num_router_rep = int(val)
                elif "num_groups" in line:
                    self.num_groups = int(val)
                elif "num_router_spine" in line:
                    self.num_router_spine = int(val)
                elif "num_router_leaf" in line:
                    self.num_router_leaf = int(val)
                elif "num_cns_per_router" in line:
                    self.num_term_per_router = int(val)

        self.intra_filename = intra_filename
        self.inter_filename = inter_filename

        self.mn_num_lp_per_rep = self.mn_nw_lp + self.mn_num_term_rep + self.mn_num_router_rep
        self.mn_total_lps = self.mn_num_rep * self.mn_num_lp_per_rep

        self.num_routers_per_group = self.num_router_spine + self.num_router_leaf
        self.num_terminals_per_group = self.num_term_per_router * self.num_router_leaf
        self.total_leaf = self.num_router_leaf * self.num_groups
        self.total_spine = self.num_router_spine * self.num_groups
        self.total_routers = self.num_groups * self.num_routers_per_group

        self.total_terminals = self.total_leaf * self.num_term_per_router
        self.total_df_lps = self.total_routers + self.total_terminals

        self.loaded = True


    def toStringPretty(self):
        outstr = ""
        outstr += "ModelNet Parameters:\n"
        outstr += "\tRepetitions:           %d\n" % self.mn_num_rep
        outstr += "\tnw-lp:                 %d\n" % self.mn_nw_lp
        outstr += "\tTerminals per Rep:     %d\n" % self.mn_num_term_rep
        outstr += "\tRouters per Rep:       %d\n" % self.mn_num_router_rep
        outstr += "\tNum LP Per Rep:        %d\n" % self.mn_num_lp_per_rep
        outstr += "\tTotal LPs (w/ mn):     %d\n" % self.mn_total_lps
        outstr += "\n"
        outstr += "Dragonfly Plus Parameters:\n"
        outstr += "\tNum Groups:            %d\n" % self.num_groups
        outstr += "\tNum Spine per Group:   %d\n" % self.num_router_spine
        outstr += "\tNum Leaf per Group:    %d\n" % self.num_router_leaf
        outstr += "\tNum Term Per Router:   %d\n" % self.num_term_per_router
        outstr += "\tNum Routers per Group: %d\n" % self.num_routers_per_group
        outstr += "\tNum Term per Group:    %d\n" % self.num_terminals_per_group
        outstr += "\tTotal Spine:           %d\n" % self.total_spine
        outstr += "\tTotal Leaf:            %d\n" % self.total_leaf
        outstr += "\tTotal Routers:         %d\n" % self.total_routers
        outstr += "\tTotal Terminals:       %d\n" % self.total_terminals
        outstr += "\tTotal LPs (DFP only):   %d\n" % self.total_df_lps

        return outstr

class Network(object):
    def __init__(self):
        self.all_lps = {} # maps tw_lpid_df to the lp object. Note, the key is not the lp_gid but the lpid for the Dragonfly LP (Ignores nw-lp)
        self.all_routers = {}
        self.all_terminals = {}


        self.lps_created = 0 #running total of dragonfly lps created
        self.routers_created = 0 #running total of dragonfly routers created
        self.terminals_created = 0 #running total of dragonfly terminals created

        log("Creating LPs from Parameters...")

        assert(params.loaded)
        for repi in range(params.mn_num_rep):
            for lpi in range(params.mn_num_lp_per_rep):
                lp_gid = repi * params.mn_num_lp_per_rep + lpi
                if lpi < params.mn_nw_lp:
                    continue #we don't care about nw-lp
                elif ( (lpi >= params.mn_nw_lp) and (lpi < (params.mn_nw_lp + params.mn_num_term_rep)) ): #terminal
                    group_id = self.terminals_created // params.num_terminals_per_group
                    newTerm = Terminal(lp_gid, self.lps_created, group_id, self.terminals_created)
                    self.all_lps[self.lps_created] = newTerm
                    self.all_terminals[self.terminals_created] = newTerm
                    self.terminals_created += 1
                    self.lps_created += 1
                else: #router
                    group_id = self.routers_created // params.num_routers_per_group
                    newRouter = Router(lp_gid, self.lps_created, group_id, self.routers_created)
                    self.all_routers[self.routers_created] = newRouter
                    self.all_lps[self.lps_created] = newRouter
                    self.routers_created += 1
                    self.lps_created += 1

        self.parseConnFiles()
        self.spawnTerminalConnections()

    def getGIDfromDFID(self, theID):
        lp = self.all_lps[theID]
        return lp.gid

    def getGIDfromRelativeID(self, theID, lptype):
        # lp = DragonflyLP(0,0,0,0)
        if lptype is LP_Type.ROUTER:
            lp = self.all_routers[theID]
        elif lptype is LP_Type.TERMINAL:
            lp = self.all_terminals[theID]

        return lp.gid

    def getAssignedRouterIDfromTermID(self, term_id):
        group_id = term_id // (params.num_router_leaf * params.num_term_per_router)
        local_router_id = term_id % params.num_term_per_router
        return (group_id * params.num_routers_per_group) + local_router_id

    def parseInterLine(self, line_struct):
        src_router = self.all_routers[line_struct[0]]
        dest_router = self.all_routers[line_struct[1]]

        src_router.connectToRouter(dest_router, ConnectionType.INTER)
        log("INTER %d (%dr) -> %d (%dr)" % (src_router.lpid_df, src_router.router_id, dest_router.lpid_df, dest_router.router_id), Loudness.DEBUG)

    #make connections between each local router in each group
    def parseIntraLine(self, line_struct):
        for gi in range(params.num_groups):
            src_router_id_df = gi * params.num_routers_per_group + line_struct[0] #the DF router ID of the source
            dest_router_id_df = gi * params.num_routers_per_group + line_struct[1] #the DF router ID of the dest

            src_router = self.all_routers[src_router_id_df]
            dest_router = self.all_routers[dest_router_id_df]

            src_router.connectToRouter(dest_router, ConnectionType.INTRA)

            log("INTRA G%d %di (%dr) -> %di (%dr)" % (src_router.group_id, line_struct[0], src_router.router_id, line_struct[1], dest_router.router_id), Loudness.DEBUG)

    def parseConnFile(self, filename, struct_format, isIntra=False):
        struct_len = struct.calcsize(struct_format)
        struct_unpack = struct.Struct(struct_format).unpack_from

        with open(filename, 'rb') as f:
            while True:
                data = f.read(struct_len)
                if not data:
                    break
                line_struct = struct_unpack(data)

                if isIntra:
                    self.parseIntraLine(line_struct)
                else:
                    self.parseInterLine(line_struct)


    def parseConnFiles(self):
        log("Parsing Interconnection File...")
        self.parseConnFile(params.inter_filename, "=2i")
        log("Parsing Intraconnection File...")
        self.parseConnFile(params.intra_filename, "=2i", isIntra=True) #DFP uses 2 ints, DFC uses 3

    # for each terminal, add connections to and from the router that terminal is assigned to
    def spawnTerminalConnections(self):
        log("Spawning Terminal Connections...")
        for t in self.all_terminals.values():
            term_id = t.terminal_id
            assigned_router_id = self.getAssignedRouterIDfromTermID(term_id)
            dest_router = self.all_routers[assigned_router_id]

            t.connectToRouter(dest_router)
            log("TERM G%d %d (%dt) -> %d (%dr)" % (t.group_id, t.gid, t.terminal_id, dest_router.gid, dest_router.router_id), Loudness.DEBUG)
            log("TERM G%d %d (%dr) -> %d (%dt)" % (t.group_id, dest_router.gid, dest_router.router_id, t.gid, t.terminal_id), Loudness.DEBUG)


    def printRouterConnSizes(self):
        for r in self.all_routers.values():
            num_inter = 0
            num_intra = 0
            num_term = 0
            for conn in r.connections:
                if conn.conn_type is ConnectionType.INTER:
                    num_inter += 1
                if conn.conn_type is ConnectionType.INTRA:
                    num_intra += 1
                if conn.conn_type is ConnectionType.TERM:
                    num_term += 1

            print("Router %d: Intra: %d     Inter: %d     Term: %d" % (r.router_id, num_intra, num_inter, num_term))

    def writeOutputFile(self, filename):
        log("Writing Output File: %s..." %filename)
        #writes the output file in CSV format with pairs of tw_lpid_gids
        with open(filename, "w") as csvf:
            cw = csv.writer(csvf, delimiter=",")
            for lp in self.all_lps.values():
                for conn in lp.connections:
                    cw.writerow([str(conn.source_obj.gid), str(conn.dest_obj.gid), str(conn.source_obj.type.value), str(conn.dest_obj.type.value)])
                    # log("%d, %d,  SRCTYPE:%d DESTTYPE:%d" % (conn.source_obj.gid, conn.dest_obj.gid, conn.source_obj.type.value, conn.dest_obj.type.value), Loudness.DEBUG)


class DragonflyLP(object):
    def __init__(self, tw_lpid_gid, tw_lpid_df, group_id, lptype):
        self.gid = tw_lpid_gid # The actual GID of the LP in the simulation - used for the output to CSV
        self.lpid_df = tw_lpid_df # The lpid of the LP relative to the Dragonfly Model. Used as key in network LP dictionary
        self.group_id = group_id
        self.type = lptype # Is this a terminal or Router?
        self.connections = [] # The connections to other LPs

    def addConnection(self, conn):
        self.connections.append(conn)


class Router(DragonflyLP):
    def __init__(self, tw_lpid_gid, tw_lpid_df, group_id, router_id):
        self.router_id = router_id
        super().__init__(tw_lpid_gid, tw_lpid_df, group_id, LP_Type.ROUTER)
        log("New Router:   %d      --  Router ID: %d  -- Group: %d" % (self.gid, self.router_id, self.group_id) ,Loudness.DEBUG)

    def connectToRouter(self, other_rtr, conn_type):
        #connect one way since each connection is explicitly laid out in the file
        conn = Connection(self, other_rtr, conn_type)
        self.addConnection(conn)


class Terminal(DragonflyLP):
    def __init__(self, tw_lpid_gid, tw_lpid_df, group_id, terminal_id):
        self.terminal_id = terminal_id
        super().__init__(tw_lpid_gid, tw_lpid_df, group_id, LP_Type.TERMINAL)
        log("New Terminal: %d      --  Term ID:   %d -- Group: %d" % (self.gid, self.terminal_id, self.group_id) ,Loudness.DEBUG)

    def connectToRouter(self, rtr):
        #makes two connections since each connection is not explicitly laid out in a file
        conn = Connection(self, rtr, ConnectionType.TERM)
        oconn = Connection(rtr, self, ConnectionType.TERM)

        self.addConnection(conn) #connect to
        rtr.addConnection(oconn) #connect from


class Connection(object):
    def __init__(self,src_obj, dest_obj, conn_type):
        self.source_obj = src_obj
        self.dest_obj = dest_obj
        self.conn_type = conn_type


# GLOBALS ----------------------------------------------------
params = Params()


# UTILITY ----------------------------------------------------
def log(mystr, loudness=Loudness.STANDARD):
    global LOUDNESS
    if loudness.value >= LOUDNESS.value:
        if loudness.value < Loudness.QUIET.value:
            print(mystr)


#just prints out each integer in the file
def decodeBinaryFile(filename):
    print("File: %s" % filename)
    struct_format = "=1i"
    struct_len = struct.calcsize(struct_format)
    struct_unpack = struct.Struct(struct_format).unpack_from

    with open(filename, 'rb') as f:
        while True:
            data = f.read(struct_len)
            if not data:
                break
            line_struct = struct_unpack(data)
            print("%d " % line_struct[0], end="")


def parseOptionArguments():
    global LOUDNESS

    if "--debug" in argv:
        LOUDNESS = Loudness.DEBUG

    if "--quiet" in argv:
        LOUDNESS = Loudness.QUIET


def main():
    global LOUDNESS
    LOUDNESS = Loudness.STANDARD

    if len(argv) < 5:
        print("ERROR: Usage: python3 dfp_get_connectivity_file.py <config_filename> <intra_filename> <inter_filename> <output_filename> [--<option>]")
        exit(1)

    parseOptionArguments()

    param_filename = argv[1]
    intra_filename = argv[2]
    inter_filename = argv[3]
    output_filename = argv[4]
    params.load_config(param_filename, intra_filename, inter_filename)

    log(params.toStringPretty())

    theNet = Network()
    theNet.writeOutputFile(output_filename)

    # theNet.printRouterConnSizes() #you can check how many connections each router has with this uncommented



if __name__ == '__main__':
    main()
