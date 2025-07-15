#!/usr/bin bash -x

# Switches
swm_enable=1
union_enable=1
torch_enable=0

# Uncomment below for MPICH
#export PATH=/usr/local/mpich-4.1.2/bin/:"$PATH"
# Note: remember to compile MPICH with nemesis not with UCX support

################## Actual scripts starts from here ##################

# SWM has to be enabled for UNION to work
if [ $union_enable = 1 ]; then
    swm_enable=1
fi

# What to compile
CUR_DIR="$PWD"

##### Downloading everything #####

git clone https://github.com/codes-org/codes --branch=develop
git clone https://github.com/ross-org/ross --depth=100 --branch=develop

if [ $swm_enable = 1 ]; then
    git clone https://github.com/pmodels/argobots --depth=1
    # This version is one commit ahead
    git clone https://github.com/helq/swm-workloads --branch=total-iterations-communication
fi

if [ $union_enable = 1 ]; then
    # Downloading conceptual
    curl -L https://sourceforge.net/projects/conceptual/files/conceptual/1.5.1b/conceptual-1.5.1b.tar.gz -o conceptual-1.5.1b.tar.gz
    tar xvf conceptual-1.5.1b.tar.gz
    # Downloading union
    git clone https://github.com/helq/Union --branch=master
fi

##### COMPILING #####

mkdir ross/build
pushd ross/build
cmake .. -DROSS_BUILD_MODELS=ON -DCMAKE_INSTALL_PREFIX="$(realpath ./bin)" \
  -DCMAKE_C_COMPILER=mpicc -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS="-g -Wall"
#make VERBOSE=1
make install -j4
err=$?
[[ $err -ne 0 ]] && exit $err
popd

if [ $swm_enable = 1 ]; then
    pushd swm-workloads/swm
    ./prepare.sh
    mkdir build
    pushd build
    ../configure --disable-shared --prefix="$(realpath ./bin)" CC=mpicc CXX=mpicxx CFLAGS=-g CXXFLAGS=-g
    #make V=1 && make install
    make -j4 && make install
    err=$?
    [[ $err -ne 0 ]] && exit $err
    popd && popd

    pushd argobots
    ./autogen.sh
    mkdir build
    pushd build
    #../configure --enable-debug=all --disable-fast --disable-shared --prefix="$(realpath ./bin)" CC=mpicc CXX=mpicxx CFLAGS=-g CXXFLAGS=-g
    ../configure --disable-shared --prefix="$(realpath ./bin)" CC=mpicc CXX=mpicxx CFLAGS=-g CXXFLAGS=-g
    #make V=1 && make install
    make -j4 && make install
    err=$?
    [[ $err -ne 0 ]] && exit $err
    popd && popd
fi

if [ $union_enable = 1 ]; then
    pushd conceptual-1.5.1b
    PYTHON=python2 ./configure --prefix="$(realpath ./install)" LIBS=-lm
    make -j4 && make install
    err=$?
    [[ $err -ne 0 ]] && exit $err
    popd

    pushd Union
    ./prepare.sh
    PYTHON=python2 ./configure --disable-shared --with-conceptual="$(realpath ../conceptual-1.5.1b/install)" --with-conceptual-src="$(realpath ../conceptual-1.5.1b)" --prefix="$(realpath ./install)" CC=mpicc CXX=mpicxx
    make -j4 && make install
    err=$?
    [[ $err -ne 0 ]] && exit $err
    popd
fi


mkdir codes/build
pushd codes/build

make_args_codes=(
    -DCMAKE_PREFIX_PATH="$(realpath "$CUR_DIR/ross/build/bin")"
    -DCMAKE_CXX_COMPILER=mpicxx -DCMAKE_C_COMPILER=mpicc
    -DCMAKE_C_FLAGS="-g -Wall"
    -DCMAKE_CXX_FLAGS="-g -Wall"
    -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
    -DCMAKE_INSTALL_PREFIX="$(realpath bin)"
)
if [ $swm_enable = 1 ]; then
    make_args_codes=(
        "${make_args_codes[@]}"
        -DSWM_PKG_CONFIG_PATH="$(realpath "$CUR_DIR/swm-workloads/swm/build/maint")"
        -DARGOBOTS_PKG_CONFIG_PATH="$(realpath "$CUR_DIR/argobots/build/maint")"
    )
fi
if [ $union_enable = 1 ]; then
    make_args_codes=(
        "${make_args_codes[@]}"
        -DUNION_PKG_CONFIG_PATH="$(realpath "$CUR_DIR/Union/install/lib/pkgconfig")"
    )
fi
if [ $torch_enable = 1 ]; then
    make_args_codes=("${make_args_codes[@]}" -DUSE_TORCH=true)
else
    make_args_codes=("${make_args_codes[@]}" -DUSE_TORCH=false)
fi

cmake .. "${make_args_codes[@]}"
#make VERBOSE=1
make -j4
err=$?
[[ $err -ne 0 ]] && exit $err

popd
