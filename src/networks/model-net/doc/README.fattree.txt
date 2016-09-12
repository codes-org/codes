*** README file for fattree network model ***

Each repetition represents a leaf level switch, nodes connected to it, and
higher level switches that may be needed to construct the fat-tree.

modelnet_fattree = radix of switch/2
fattree_switch = number of levels in the fattree (2 or 3)

Supported PARAMS:

packet_size, chunk_size (ideally kept same)
modelnet_scheduler - NIC message scheduler
modelnet_order=( "fattree" ); 
router_delay : delay caused by switched in ns
num_levels : number of levels in the fattree (same as fattree_switch)
switch_count : number of leaf level switches (same as repetitions)
switch_radix : radix of the switches
vc_size : size of switch VCs in bytes
cn_vc_size : size of VC between NIC and switch in bytes
link_bandwidth, cn_bandwidth : in GB/s
routing : {adaptive, static}

If static routing is chosen, two more PARAMS must be provided:
routing_folder :  folder that contain lft files generated using method described below.
dot_file : name used for dotfile generation in the method described below.
(dump_topo should be set to 0 or not set when during simulations)

To generate static routing tables, first do an "empty" run to dump the topology
of the fat-tree by setting the following PARAMS:
routing : static
routing_folder : folder to which topology files should be written
dot_file : prefix used for creating topology files inside the folder
dump_topo :  1

When dump_topo is set, the simulator dumps the topology inside the folder
specified by routing_folder and exits. Next, follow these steps created by Jens 
to generate the routing tables stored as LFT files:

(you should replace $P_PATH with your path)
1. Install fall-in-place toolchain: (patch files can be found in src/util/patches folder of CODES):
wget http://htor.inf.ethz.ch/sec/fts.tgz
tar xzf fts.tgz
cd fault_tolerance_simulation/
rm 0001-*.patch 0002-*.patch 0003-*.patch 0004-*.patch 0005-*.patch
tar xzf $P_PATH/sar.patches.tgz
 
wget http://downloads.openfabrics.org/management/opensm-3.3.20.tar.gz
mv opensm-3.3.20.tar.gz opensm.tar.gz
wget http://downloads.openfabrics.org/ibutils/ibutils-1.5.7-0.2.gbd7e502.tar.gz
mv ibutils-1.5.7-0.2.gbd7e502.tar.gz ibutils.tar.gz
wget http://downloads.openfabrics.org/management/infiniband-diags-1.6.7.tar.gz
mv infiniband-diags-1.6.7.tar.gz infiniband-diags.tar.gz
wget https://www.openfabrics.org/downloads/management/libibmad-1.3.12.tar.gz
mv libibmad-1.3.12.tar.gz libibmad.tar.gz
wget https://www.openfabrics.org/downloads/management/libibumad-1.3.10.2.tar.gz
mv libibumad-1.3.10.2.tar.gz libibumad.tar.gz
 
patch -p1 < $P_PATH/fts.patch
 
./simuate.py -s

2. Add LFT creating scripts to the fall-in-place toolchain.
cd $HOME/simulation/scripts
patch -p1 < $P_PATH/lft.patch
chmod +x post_process_*
chmod +x create_static_lft.sh 

3. Choose a routing algorithm which should be used by OpenSM
 (possible options: updn, dnup, ftree, lash, dor, torus-2QoS, dfsssp, sssp)
export OSM_ROUTING="ftree" 
~/simulation/scripts/create_static_lft.sh routing_folder dot_file

(here routing_folder and dot_file should be same as the one used during the run used to dump the topology)

Now, the routing table stored as LFT files should be in the routing_folder.
