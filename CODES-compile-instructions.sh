#!/usr/bin/env bash
set -euo pipefail
set -x

# Switches
swm_enable=0
union_enable=0
torch_enable=1

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

if [ ! -d codes/.git ]; then
    git clone https://github.com/codes-org/codes --depth=100 --branch=v1.5.0
else
    echo "Using existing codes checkout: $(realpath codes)"
fi

if [ ! -d ross/.git ]; then
    git clone https://github.com/ross-org/ross --depth=100 --branch=v8.1.0
else
    echo "Using existing ross checkout: $(realpath ross)"
fi

if [ $swm_enable = 1 ]; then
    git clone https://github.com/pmodels/argobots --depth=1
    git clone https://github.com/codes-org/swm-workloads --branch=v1.2
fi

if [ $union_enable = 1 ]; then
    # Downloading conceptual
    curl -L https://sourceforge.net/projects/conceptual/files/conceptual/1.5.1b/conceptual-1.5.1b.tar.gz -o conceptual-1.5.1b.tar.gz
    tar xvf conceptual-1.5.1b.tar.gz
    # Downloading union
    git clone https://github.com/SPEAR-UIC/Union
    pushd Union && git checkout 99b3df3 && popd
fi

##### COMPILING #####

mkdir -p ross/build
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
    mkdir -p build
    pushd build
    ../configure --disable-shared --prefix="$(realpath ./bin)" CC=mpicc CXX=mpicxx CFLAGS=-g CXXFLAGS=-g
    #make V=1 && make install
    make -j4 && make install
    err=$?
    [[ $err -ne 0 ]] && exit $err
    popd && popd

    pushd argobots
    ./autogen.sh
    mkdir -p build
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
    # Python 2 override. Union expects Python 2 ONLY
    mkdir -p python-override
    ln -s /usr/bin/python2 python-override/python
    # compiling
    ./prepare.sh
    PYTHON=python2 ./configure --disable-shared --with-conceptual="$(realpath ../conceptual-1.5.1b/install)" --with-conceptual-src="$(realpath ../conceptual-1.5.1b)" --prefix="$(realpath ./install)" CC=mpicc CXX=mpicxx
    PATH="$PWD/python-override:$PATH" make -j4 && make install
    err=$?
    [[ $err -ne 0 ]] && exit $err
    popd
fi


# Build local ZMQML requester library required by director-client.C
pushd codes/src/surrogate/zmqml
make clean
make
test -f libzmqmlrequester.so
test -f zmqmlrequester.h
popd

# Make imported zmqmlrequester target visible to doc/example and tests.
python3 - <<'INNERPY'
from pathlib import Path
cm = Path("codes/src/CMakeLists.txt")
text = cm.read_text()
old = "add_library(zmqmlrequester SHARED IMPORTED )"
new = "add_library(zmqmlrequester SHARED IMPORTED GLOBAL)"
if old in text:
    cm.write_text(text.replace(old, new))
elif new in text:
    pass
else:
    raise SystemExit("Could not find zmqmlrequester imported target line in codes/src/CMakeLists.txt")
INNERPY

mkdir -p codes/build
pushd codes/build

torch_cmake_prefix=""
torch_dir=""

if [ "$torch_enable" = 1 ]; then
    torch_cmake_prefix="$(python3 - <<'INNERPY'
import torch
print(torch.utils.cmake_prefix_path)
INNERPY
)"
    torch_dir="${torch_cmake_prefix}/Torch"

    if [ ! -f "${torch_dir}/TorchConfig.cmake" ]; then
        echo "ERROR: TorchConfig.cmake not found at: ${torch_dir}/TorchConfig.cmake" >&2
        echo "       torch.utils.cmake_prefix_path returned: ${torch_cmake_prefix}" >&2
        exit 1
    fi

    echo "Using Torch CMake prefix: ${torch_cmake_prefix}"
    echo "Using Torch_DIR: ${torch_dir}"

    # CUDA is intentionally opt-in.
    # Default to CPU-only Torch-JIT compilation unless CUDA_HOME is explicitly set.
    #
    # To enable CUDA, run for example:
    #   export CUDA_HOME=/usr/local/cuda-12.4
    #   ./CODES-compile-instructions.sh
    torch_cuda_version="$(python3 - <<'INNERPY'
import torch
print(torch.version.cuda or "")
INNERPY
)"

    cuda_arch=""
    if [ -z "${CUDA_HOME:-}" ] && [ -n "${torch_cuda_version}" ]; then
        echo "ERROR: CUDA_HOME is not set, so this script is defaulting to CPU-only Torch-JIT compilation." >&2
        echo "       However, the active Python environment has a CUDA-enabled PyTorch build:" >&2
        echo "       torch.version.cuda=${torch_cuda_version}" >&2
        echo "" >&2
        echo "       CMake cannot use a CUDA-enabled PyTorch package as a CPU-only LibTorch package." >&2
        echo "       Choose one of the following:" >&2
        echo "         1. For CPU-only compilation, install a CPU-only PyTorch build in this environment." >&2
        echo "         2. For CUDA compilation, export CUDA_HOME to your CUDA toolkit root." >&2
        echo "" >&2
        echo "       Example CUDA build:" >&2
        echo "         export CUDA_HOME=/usr/local/cuda-12.4" >&2
        echo "         bash CODES-compile-instructions.sh" >&2
        exit 1
    fi

    if [ -n "${CUDA_HOME:-}" ]; then
        if [ ! -f "${CUDA_HOME}/include/cuda_runtime_api.h" ]; then
            echo "ERROR: CUDA_HOME is set, but missing CUDA header: ${CUDA_HOME}/include/cuda_runtime_api.h" >&2
            exit 1
        fi

        if [ ! -f "${CUDA_HOME}/lib64/libcudart.so" ] && [ ! -f "${CUDA_HOME}/lib/libcudart.so" ]; then
            echo "ERROR: CUDA_HOME is set, but missing CUDA runtime library under ${CUDA_HOME}/lib64 or ${CUDA_HOME}/lib" >&2
            exit 1
        fi

        if [ ! -x "${CUDA_HOME}/bin/nvcc" ]; then
            echo "ERROR: CUDA_HOME is set, but missing CUDA compiler: ${CUDA_HOME}/bin/nvcc" >&2
            exit 1
        fi

        if [ ! -d "${CUDA_HOME}/nvvm/libdevice" ]; then
            echo "ERROR: CUDA_HOME is set, but missing CUDA libdevice directory: ${CUDA_HOME}/nvvm/libdevice" >&2
            exit 1
        fi

        if command -v nvidia-smi >/dev/null 2>&1; then
            cuda_arch="$(nvidia-smi --query-gpu=compute_cap --format=csv,noheader 2>/dev/null | head -n1 | tr -d '.[:space:]' || true)"
        fi

        if [ -z "${cuda_arch}" ]; then
            echo "WARNING: Could not auto-detect GPU compute capability with nvidia-smi." >&2
            echo "         Falling back to CMAKE_CUDA_ARCHITECTURES=80." >&2
            cuda_arch="80"
        fi

        export CUDA_HOME
        export CUDA_PATH="${CUDA_HOME}"
        export CUDA_ROOT="${CUDA_HOME}"
        export CUDA_TOOLKIT_ROOT_DIR="${CUDA_HOME}"
        export CUDAToolkit_ROOT="${CUDA_HOME}"
        export CUDACXX="${CUDA_HOME}/bin/nvcc"
        export PATH="${CUDA_HOME}/bin:${PATH}"
        export LD_LIBRARY_PATH="${CUDA_HOME}/lib64:${CUDA_HOME}/lib:${LD_LIBRARY_PATH:-}"

        echo "CUDA_HOME is set; enabling CUDA Torch-JIT compilation."
        echo "Using CUDA_HOME: ${CUDA_HOME}"
        echo "Using CUDACXX: ${CUDACXX}"
        echo "Using CMAKE_CUDA_ARCHITECTURES=${cuda_arch}"
    else
        echo "CUDA_HOME is not set; forcing CPU-only Torch-JIT compilation."

        # Prevent accidental CUDA discovery from /usr/local/cuda, nvcc on PATH,
        # inherited CMake cache variables, or CUDA-enabled PyTorch metadata.
        unset CUDA_HOME
        unset CUDA_PATH
        unset CUDA_ROOT
        unset CUDA_TOOLKIT_ROOT_DIR
        unset CUDAToolkit_ROOT
        unset CUDACXX
        unset CMAKE_CUDA_COMPILER
    fi
fi

cmake_prefix_path="$(realpath "$CUR_DIR/ross/build/bin")"
if [ "$torch_enable" = 1 ]; then
    cmake_prefix_path="${cmake_prefix_path};${torch_cmake_prefix}"
fi

make_args_codes=(
    -DCMAKE_PREFIX_PATH="${cmake_prefix_path}"
    -DCMAKE_CXX_COMPILER=mpicxx -DCMAKE_C_COMPILER=mpicc
    -DCMAKE_C_FLAGS="-g -Wall"
    -DCMAKE_CXX_FLAGS="-g -Wall"
    -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
    -DCMAKE_INSTALL_PREFIX="$(realpath bin)"
    -DZMQML_BUILD_PATH="$(realpath "$CUR_DIR/codes/src/surrogate/zmqml")"
    -DZeroMQ_INCLUDE_DIR=/usr/include
    -DZeroMQ_LIBRARY=/usr/lib/x86_64-linux-gnu/libzmq.so
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
if [ "$torch_enable" = 1 ]; then
    make_args_codes=(
        "${make_args_codes[@]}"
        -DUSE_TORCH=true
        -DTorch_DIR="${torch_dir}"
    )

    if [ -n "${CUDA_HOME:-}" ]; then
        make_args_codes=(
            "${make_args_codes[@]}"
            -DCUDA_TOOLKIT_ROOT_DIR="${CUDA_HOME}"
            -DCUDAToolkit_ROOT="${CUDA_HOME}"
            -DCUDA_PATH="${CUDA_HOME}"
            -DCUDA_ROOT="${CUDA_HOME}"
            -DCMAKE_CUDA_COMPILER="${CUDA_HOME}/bin/nvcc"
            -DCMAKE_CUDA_ARCHITECTURES="${cuda_arch}"
            -DCUDA_INCLUDE_DIRS="${CUDA_HOME}/include"
            -DCUDA_CUDART_LIBRARY="${CUDA_HOME}/lib64/libcudart.so"
        )
    else
        make_args_codes=(
            "${make_args_codes[@]}"
            -DCMAKE_DISABLE_FIND_PACKAGE_CUDA=ON
            -DCMAKE_DISABLE_FIND_PACKAGE_CUDAToolkit=ON
        )
    fi
else
    make_args_codes=("${make_args_codes[@]}" -DUSE_TORCH=false)
fi

cmake .. "${make_args_codes[@]}"
#make VERBOSE=1
make -j4
err=$?
[[ $err -ne 0 ]] && exit $err

popd
