#!/bin/bash
set -euo pipefail
linux_setup() {
    echo ">>> Detected Linux, using APT package manager"
    sudo apt-get update
    sudo apt-get install -y \
        pkg-config libssl-dev \
        git cmake ccache python3 ninja-build \
        nasm yasm gawk lsb-release wget software-properties-common gnupg

    echo ">>> Installing LLVM via official apt.llvm.org script"
    sudo bash -c "$(wget -qO- https://apt.llvm.org/llvm.sh)"
    export CC=clang-19
    export CXX=clang++-19
}

mac_setup() {
    echo ">>> Detected macOS, using Homebrew"
    if ! command -v brew >/dev/null; then
        echo "Homebrew not found!  Please install Homebrew first: https://brew.sh"
        exit 1
    fi

    brew update
    brew install ccache cmake ninja libtool gettext binutils grep findutils nasm bash
    brew install llvm@19
    export CC="/opt/homebrew/opt/llvm@19/bin/clang"
    export CXX="/opt/homebrew/opt/llvm@19/bin/clang++"
    export CPPFLAGS="-I/opt/homebrew/opt/llvm@19/include"
    export PATH="/opt/homebrew/opt/llvm@19/bin:$PATH"
}

case "$(uname -s)" in
    Linux*)   linux_setup ;;
    Darwin*)  mac_setup   ;;
    *)        echo "Unsupported operating system: $(uname -s)"; exit 1 ;;
esac

(
    rm -rf build/
    sed -i 's/const size_type count_/size_type count_/' \
        clickHouse/contrib/flatbuffers/include/flatbuffers/stl_emulation.h
    pushd clickHouse/src > /dev/null
    ln -sf . clickHouse
    popd > /dev/null

    cmake -B build -S . -GNinja

    ARCH=$(uname -m)
    INC=clickHouse/contrib/openssl-cmake/linux_${ARCH}/include
    LIB=build/clickHouse/contrib/openssl-cmake
    export OPENSSL_DIR=$PWD/$LIB
    export OPENSSL_LIB_DIR=$PWD/$LIB
    export OPENSSL_INCLUDE_DIR=$PWD/$INC
    export OPENSSL_STATIC=1
    export OPENSSL_NO_VENDOR=1

    ninja -C build libcrypto.a libssl.a
    mkdir -p build/contrib/openssl-cmake
    ln -sf $OPENSSL_LIB_DIR/libssl.a    build/contrib/openssl-cmake/libssl.a
    ln -sf $OPENSSL_LIB_DIR/libcrypto.a build/contrib/openssl-cmake/libcrypto.a

    JOBS=$( \
        command -v nproc   >/dev/null && nproc ||
        sysctl -n hw.ncpu  2>/dev/null ||
        getconf _NPROCESSORS_ONLN 2>/dev/null ||
        echo 4 )

    echo ">>> Building with -j$JOBS"
    ninja -C build -j"$JOBS"
) || true

git submodule foreach --recursive 'git reset --hard && git clean -fdx' > /dev/null
echo ">>> All done!"
