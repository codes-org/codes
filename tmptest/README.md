:# Union
Workload Manager for Integration of Conceptual as an Online Workload for CODES


# Installation

### Installing Conceptual (mandatory)

Download Conceptual at https://ccsweb.lanl.gov/~pakin/software/conceptual/download.html (version 1.5.1 or greater)

```bash
tar xvf conceptual-1.5.1.tar.gz
cd conceptual-1.5.1
./configure --prefix=/path/to/conceptual/install
make
make install
```

### Installing Boost-Python (currently mandatory, we may remove this soon)

Download boost at http://www.boost.org/users/download/ (version 1.68 or greater)

```bash
tar xvf boost_1_68_0.tar.gz
cd boost_1_68_0 
./bootstrap.sh --prefix=/path/to/boost/install  --with-libraries=python
./b2 install
```

### Installing Union    
```bash
cd union
./prepare.sh
./configure --with-boost=/path/to/boost/install --with-conceptual=/path/to/conceptual/install --prefix=/path/to/union/install CC=mpicc CXX=mpicxx
make
make install
```

# Workload Simulation with CODES

### Installing ROSS

```bash
git clone https://github.com/carothersc/ROSS.git 
mkdir build-ross
cd build-ross
cmake -DCMAKE_INSTALL_PREFIX:path=path/to/ross/install -DCMAKE_C_COMPILER=$(which mpicc) -DCMAKE_CXX_COMPILER=$(which mpicxx) ../ROSS
make install
```

### Installing Argobots

```bash
git clone https://github.com/pmodels/argobots.git
./autogen.sh
./configure --prefix=/path/to/argobots/install
make
make install
```

### Installing SWM workloads

```bash
git clone https://github.com/codes-org/SWM-workloads.git
cd swm
./prepare.sh
./configure --with-boost=/path/to/boost/install --prefix=/path/to/swm/install CC=mpicc CXX=mpicxx
make
make install
```

### Installing CODES (kronos-union branch)

```bash
git clone https://github.com/codes-org/codes.git
cd codes
./prepare.sh
mkdir build
cd build
../configure --with-online=true --with-boost=/path/to/boost/install PKG_CONFIG_PATH=/home/path/to/argobots/install/lib/pkgconfig:/path/to/ross/install/lib/pkgconfig:/path/to/union/install/lib/pkgconfig:/path/to/swm/install/lib/pkgconfig --with-union=true --prefix=/path/to/codes/install CC=mpicc CXX=mpicxx 
make
make install
```

### Run Test Simulations
The tmptest directory includes all necessary configuration files to run the test simulation.

- Copy milc_skeleton.json to /path/to/swm/install/share/
- Copy conceptual.json to /path/to/union/install/share/
- Change the path for "intra-group-connections" and "intra-group-connections" in dfdally-72-par.conf
- Run the following command:

```bash
/path/to/codes/install/bin/model-net-mpi-replay --sync=1 --workload_type=conc-online --lp-io-use-suffix=1 --workload_conf_file=/path/to/codes/tmptest/conf/jacobi_MILC.conf --alloc_file=/path/to/codes/tmptest/conf/rand_node0-1d-72-jacobi_MILC.conf --lp-io-dir=tmptest-jacobiS_MILC -- /path/to/codes/tmptest/conf/dfdally-72-par.conf > tmptest-jacobiS_MILC.output 
```



