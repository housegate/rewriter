#!/usr/bin/env bash
set -euo pipefail

# Function definitions
usage() {
    cat <<EOF
Usage: $0 <command> [args]

Commands:
  prepare                   Install build dependencies (auto-detect Linux/macOS).
  build                     Full rebuild (wipes the build/ directory).
  rebuild                   Incremental build (fast, recommended for day-to-day work).
  test                      Build and run unit tests.
  clean                     Remove the build/ directory.
  docker list               List local Docker images.
  docker build <version>    Build a Docker image tagged <version>.
  docker push  <version>    Push the image for <version> to the remote registry.

Tips:
  - First-time build:           ./script.sh build
  - For later iterations:       ./script.sh rebuild  (10x faster)
  - Run tests:                  ./script.sh test
EOF
}

# Ensure the ClickHouse source + contrib symlinks exist. cmake re-reads them on each configure.
ensure_symlinks() {
    # clickHouse/src/clickHouse -> .  so `#include "clickHouse/..."` resolves.
    if [ ! -e clickHouse/src/clickHouse ]; then
        (cd clickHouse/src && ln -s . clickHouse)
    fi
    # ./contrib -> clickHouse/contrib  so the top-level CMakeLists.txt finds it.
    if [ ! -e contrib ]; then
        ln -s clickHouse/contrib contrib
    fi
}

# Re-apply compatibility patches to submodule sources, idempotently. Called on every
# build/rebuild so that when `git submodule foreach git clean` or a manual reset
# wipes them, the next build re-adds them — otherwise ninja hits missing symbols
# like `undefined symbol: resolve_affinity` when relinking numactl.
ensure_patches() {
    local flatbuf=clickHouse/contrib/flatbuffers/include/flatbuffers/stl_emulation.h
    if [ -f "$flatbuf" ] && grep -q 'const size_type count_' "$flatbuf"; then
        sed -i 's/const size_type count_/size_type count_/' "$flatbuf"
    fi

    local numactl=clickHouse/contrib/numactl-cmake/CMakeLists.txt
    # Only patch if distance.c is not already listed, to avoid duplicate appends.
    if [ -f "$numactl" ] && ! grep -q '"${LIBRARY_DIR}/distance.c"' "$numactl"; then
        sed -i 's|"${LIBRARY_DIR}/syscall.c"|"${LIBRARY_DIR}/syscall.c"\n    "${LIBRARY_DIR}/distance.c"\n    "${LIBRARY_DIR}/affinity.c"\n    "${LIBRARY_DIR}/sysfs.c"\n    "${LIBRARY_DIR}/rtnetlink.c"|g' \
            "$numactl"
    fi
}

# Install build dependencies.
prepare() {
    echo ">>> Preparing environment"
    case "$(uname -s)" in
        Linux*)
            echo ">>> Detected Linux, using APT"
            sudo apt-get update
            sudo apt-get install -y \
                pkg-config libssl-dev \
                git cmake ccache python3 ninja-build mold \
                nasm yasm gawk lsb-release wget software-properties-common gnupg
            echo ">>> Installing LLVM via apt.llvm.org"
            sudo bash -c "$(wget -qO- https://apt.llvm.org/llvm.sh)"
            export CC=clang-20
            export CXX=clang++-20
            ;;
        Darwin*)
            echo ">>> Detected macOS, using Homebrew"
            if ! command -v brew >/dev/null; then
                echo "Homebrew not found! Install it first: https://brew.sh"
                exit 1
            fi
            brew update
            brew install ccache cmake ninja libtool gettext binutils grep findutils nasm bash llvm@19 mold
            export CC="/opt/homebrew/opt/llvm@19/bin/clang"
            export CXX="/opt/homebrew/opt/llvm@19/bin/clang++"
            export CPPFLAGS="-I/opt/homebrew/opt/llvm@19/include"
            export PATH="/opt/homebrew/opt/llvm@19/bin:$PATH"
            ;;
        *)
            echo "Unsupported OS: $(uname -s)"
            exit 1
            ;;
    esac
    echo ">>> prepare done"
}

# Full rebuild (wipes build/).
build() {
    echo "=========================================="
    echo "Full rebuild (wipes build/)"
    echo "Note: first build takes ~15-20 minutes."
    echo "=========================================="
    START_TIME=$(date +%s)

    prepare
    echo ">>> [1/5] Clean previous build directory"
    rm -rf build/

    echo ">>> [2/5] Apply submodule compatibility patches"
    ensure_patches

    pushd clickHouse/src > /dev/null
    rm -rf clickHouse
    popd > /dev/null
    rm -rf contrib
    ensure_symlinks

    echo ">>> [3/5] Configure CMake (ThinLTO off, mold linker)"
    cmake -B build -S . -GNinja \
      -DENABLE_RUST=OFF \
      -DENABLE_TESTS=OFF \
      -DBUILD_TESTING=ON \
      -DENABLE_THINLTO=OFF \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=mold

    echo ">>> [4/5] Build OpenSSL libraries"
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

    JOBS=$(
        command -v nproc   >/dev/null && nproc ||
        sysctl -n hw.ncpu  2>/dev/null ||
        getconf _NPROCESSORS_ONLN 2>/dev/null ||
        echo 4
    )
    echo ">>> [5/5] Build the main target (-j$JOBS)"
    ninja -C build -j"$JOBS" clickhouse_sentio_rewriter clickhouse_sentio_rewriter_cli

    # If testing is enabled, build tests too.
    if grep -q "BUILD_TESTING:BOOL=ON" build/CMakeCache.txt 2>/dev/null; then
        echo ">>> [5.1/5] Build test binary"
        ninja -C build -j"$JOBS" rewriter_tests || echo "Warning: rewriter_tests build skipped"
    fi

    # Undo any submodule changes the build made (reset patches).
    # ensure_patches on the next build re-applies them idempotently.
    git submodule foreach --recursive 'git reset --hard && git clean -fdx' >/dev/null
    # The submodule cleanup above also wipes the clickHouse/src/clickHouse
    # self-symlink. Without it, cmake reconfigures fail to resolve
    # Dictionaries/DictionaryFactory.h. Restore it here.
    ensure_symlinks

    END_TIME=$(date +%s)
    DURATION=$((END_TIME - START_TIME))
    echo "=========================================="
    echo "Build done in ${DURATION}s ($((DURATION / 60))m $((DURATION % 60))s)"
    echo "Binary: build/clickhouse_sentio_rewriter"
    echo "=========================================="
}

# Incremental build (fast; for day-to-day work).
rebuild() {
    echo "=========================================="
    echo "Incremental build (only recompiles changed files)"
    echo "Expected time: 30s-3min."
    echo "=========================================="
    START_TIME=$(date +%s)

    if [ ! -d "build" ]; then
        echo "build/ does not exist; run ./script.sh build first."
        exit 1
    fi

    # Restore symlinks and patches before ninja so incremental configure/build
    # doesn't trip on missing files or missing symbols.
    ensure_symlinks
    ensure_patches

    JOBS=$(
        command -v nproc   >/dev/null && nproc ||
        sysctl -n hw.ncpu  2>/dev/null ||
        getconf _NPROCESSORS_ONLN 2>/dev/null ||
        echo 4
    )

    echo ">>> [1/2] Recompile changed sources"
    cd build
    ninja -j"$JOBS" clickhouse_sentio_rewriter clickhouse_sentio_rewriter_cli
    cd ..

    # If testing is enabled, build tests too.
    if grep -q "BUILD_TESTING:BOOL=ON" build/CMakeCache.txt 2>/dev/null; then
        echo ">>> [2/2] Recompile tests"
        cd build
        ninja -j"$JOBS" rewriter_tests || echo "Warning: rewriter_tests build skipped"
        cd ..
    fi

    END_TIME=$(date +%s)
    DURATION=$((END_TIME - START_TIME))
    echo "=========================================="
    echo "Incremental build done in ${DURATION}s."
    echo "Binary: build/clickhouse_sentio_rewriter"
    echo "=========================================="
}

# Clean the build directory.
clean() {
    echo ">>> Cleaning build/"
    rm -rf build/
    echo ">>> Done."
}

test() {
    build
    ctest --test-dir build --output-on-failure
}

docker_list() {
    echo ">>> Docker images list:"
    docker images
}

docker_build_image() {
    local ver=$1
    echo "=========================================="
    echo "Build Docker image: $ver"
    echo "=========================================="

    # Check local binary exists.
    if [ ! -f "build/clickhouse_sentio_rewriter" ]; then
        echo "build/clickhouse_sentio_rewriter is missing."
        echo "Run ./script.sh build or ./script.sh rebuild first."
        exit 1
    fi

    echo ">>> [1/3] Copy binary into the Docker context"
    cp -v build/clickhouse_sentio_rewriter docker/bare/root/clickhouse_sentio_rewriter

    local old_size=$(du -h docker/bare/root/clickhouse_sentio_rewriter | awk '{print $1}')
    echo "Binary size: $old_size"

    echo ">>> [2/3] Build Docker image"
    (
      cd docker/bare
      docker buildx build --platform linux/amd64 . \
          -t us-west1-docker.pkg.dev/sentio-352722/sentio/sql-rewriter:"${ver}"
    )

    echo ">>> [3/3] Docker image built"
    echo "=========================================="
    echo "Image tag: sql-rewriter:$ver"
    echo "=========================================="
    echo "Next: ./script.sh docker push $ver"
    echo "=========================================="
}

docker_push_image() {
    local ver=$1
    echo ">>> Pushing Docker image with tag: $ver"
    (
      cd docker/bare
      docker push us-west1-docker.pkg.dev/sentio-352722/sentio/sql-rewriter:"${ver}"
    )
    echo ">>> Back to project root ($(pwd))"
}

# Command dispatch.
if [[ $# -lt 1 ]]; then
    usage
    exit 1
fi

cmd=$1; shift
case "$cmd" in
    prepare)
        prepare
        ;;
    build)
        build
        ;;
    rebuild)
        rebuild
        ;;
    clean)
        clean
        ;;
    test)
        test
        ;;
    docker)
        if [[ $# -lt 1 ]]; then
            usage
            exit 1
        fi
        subcmd=$1; shift
        case "$subcmd" in
            list)
                docker_list
                ;;
            build)
                if [[ $# -lt 1 ]]; then
                    echo "Error: missing version for 'docker build'"
                    usage
                    exit 1
                fi
                docker_build_image "$1"
                ;;
            push)
                if [[ $# -lt 1 ]]; then
                    echo "Error: missing version for 'docker push'"
                    usage
                    exit 1
                fi
                docker_push_image "$1"
                ;;
            *)
                usage
                exit 1
                ;;
        esac
        ;;
    *)
        usage
        exit 1
        ;;
esac
