#!/bin/bash

(
    sudo apt-get update
    sudo apt install -y pkg-config libssl-dev
    sudo apt-get install -y git cmake ccache python3 ninja-build nasm yasm gawk lsb-release wget software-properties-common gnupg
    sudo bash -c "$(wget -O - https://apt.llvm.org/llvm.sh)"
) || true

(
    export CC=clang-19
    export CXX=clang++-19
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

    ninja -C build -j$(nproc)
) || true
git submodule foreach --recursive 'git reset --hard && git clean -fdx' > /dev/null
