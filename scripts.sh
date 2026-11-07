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
  - First-time build:           ./scripts.sh build
  - For later iterations:       ./scripts.sh rebuild  (10x faster)
  - Run tests:                  ./scripts.sh test
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
    # ClickHouse 26.8 rounds the CPU-dependent alternative signal-stack size up
    # to a page itself (src/Common/ThreadStatus.cpp), so the former
    # thread-stack-alignment patch is no longer applied.
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
            # ClickHouse 26.8 requires Clang >= 21 (cmake/tools.cmake). Install and
            # use Clang 22, matching the reference sql-rewriter's 26.3 build.
            wget -qO- https://apt.llvm.org/llvm.sh | sudo bash -s -- 22
            export CC=clang-22
            export CXX=clang++-22
            ;;
        Darwin*)
            echo ">>> Detected macOS, using Homebrew"
            if ! command -v brew >/dev/null; then
                echo "Homebrew not found! Install it first: https://brew.sh"
                exit 1
            fi
            brew update
            brew install ccache cmake ninja libtool gettext binutils grep findutils nasm bash llvm mold
            local llvm_prefix
            llvm_prefix="$(brew --prefix llvm)"
            export CC="${llvm_prefix}/bin/clang"
            export CXX="${llvm_prefix}/bin/clang++"
            export CPPFLAGS="-I${llvm_prefix}/include"
            export PATH="${llvm_prefix}/bin:$PATH"
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
    local compiler_target_args=()
    if [[ "$(uname -s)" == Linux* && "$(uname -m)" == "x86_64" ]]; then
        compiler_target_args=(
          -DCMAKE_C_COMPILER_TARGET=x86_64-linux-gnu
          -DCMAKE_CXX_COMPILER_TARGET=x86_64-linux-gnu
          -DCMAKE_ASM_COMPILER_TARGET=x86_64-linux-gnu
        )
    fi
    cmake -B build -S . -GNinja \
      -DENABLE_RUST=OFF \
      -DENABLE_TESTS=OFF \
      -DBUILD_TESTING=ON \
      -DENABLE_THINLTO=OFF \
      -DENABLE_LLVM_LIBC_MATH=OFF \
      -DENABLE_XRAY=OFF \
      -DENABLE_WASMEDGE=OFF \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      ${compiler_target_args[@]+"${compiler_target_args[@]}"} \
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
    ninja -C build -j"$JOBS" clickhousegate_rewriter clickhousegate_rewriter_cli

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
    echo "Binary: build/clickhousegate_rewriter"
    echo "=========================================="
}

# Incremental build (fast; for day-to-day work).
# If build/ doesn't exist yet, fall through to a full build — saves the caller
# one step and keeps `./scripts.sh rebuild` as the "just make it work" verb.
rebuild() {
    if [ ! -d "build" ]; then
        echo "build/ does not exist; falling through to full build."
        build
        return $?
    fi

    echo "=========================================="
    echo "Incremental build (only recompiles changed files)"
    echo "Expected time: 30s-3min."
    echo "=========================================="
    START_TIME=$(date +%s)

    # Restore symlinks and patches before configure/ninja so incremental
    # builds don't trip on missing files or missing symbols.
    ensure_symlinks
    ensure_patches

    # Re-run cmake configure so `git describe`-derived REWRITER_VERSION /
    # GIT_COMMIT_HASH are refreshed and any CMakeLists.txt edits get picked
    # up. cmake is a fast no-op (~1-3s) when nothing actually changed.
    # Without this, `rebuild` on a freshly-checked-out tag would ship the
    # previous tag's version string (CMakeCache is sticky across ninja-only
    # runs) — that's how v0.0.7 used to get stamped as v0.0.6.
    echo ">>> [1/3] Refresh cmake configure"
    local compiler_target_args=()
    if [[ "$(uname -s)" == Linux* && "$(uname -m)" == "x86_64" ]]; then
        compiler_target_args=(
          -DCMAKE_C_COMPILER_TARGET=x86_64-linux-gnu
          -DCMAKE_CXX_COMPILER_TARGET=x86_64-linux-gnu
          -DCMAKE_ASM_COMPILER_TARGET=x86_64-linux-gnu
        )
    fi
    cmake -B build -S . -GNinja \
      -DENABLE_RUST=OFF \
      -DENABLE_TESTS=OFF \
      -DBUILD_TESTING=ON \
      -DENABLE_THINLTO=OFF \
      -DENABLE_LLVM_LIBC_MATH=OFF \
      -DENABLE_XRAY=OFF \
      -DENABLE_WASMEDGE=OFF \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      ${compiler_target_args[@]+"${compiler_target_args[@]}"} \
      -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=mold

    JOBS=$(
        command -v nproc   >/dev/null && nproc ||
        sysctl -n hw.ncpu  2>/dev/null ||
        getconf _NPROCESSORS_ONLN 2>/dev/null ||
        echo 4
    )

    echo ">>> [2/3] Recompile changed sources"
    cd build
    ninja -j"$JOBS" clickhousegate_rewriter clickhousegate_rewriter_cli
    cd ..

    # If testing is enabled, build tests too.
    if grep -q "BUILD_TESTING:BOOL=ON" build/CMakeCache.txt 2>/dev/null; then
        echo ">>> [3/3] Recompile tests"
        cd build
        ninja -j"$JOBS" rewriter_tests || echo "Warning: rewriter_tests build skipped"
        cd ..
    fi

    END_TIME=$(date +%s)
    DURATION=$((END_TIME - START_TIME))
    echo "=========================================="
    echo "Incremental build done in ${DURATION}s."
    echo "Binary: build/clickhousegate_rewriter"
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

# Make sure `docker-credential-gcloud` is on PATH so that the `gcloud` cred
# helper configured in ~/.docker/config.json can mint access tokens for
# *.pkg.dev / gcr.io pushes. Interactive SSH sessions usually have gcloud SDK
# sourced via ~/.bash_profile, but non-interactive shells (e.g. CI over SSH)
# do not — that's why `docker push` fails with `Unauthenticated request` in
# the release workflow while it works when you SSH in by hand.
ensure_gcloud_on_path() {
    # Already reachable? Nothing to do.
    if command -v docker-credential-gcloud >/dev/null 2>&1; then
        return 0
    fi
    # Prefer the official path.bash.inc shipped with the SDK (also sets up
    # completion etc. as a side-effect). Fall back to a few common install
    # locations if it's missing.
    local inc
    for inc in \
        "$HOME/google-cloud-sdk/path.bash.inc" \
        /usr/lib/google-cloud-sdk/path.bash.inc \
        /snap/google-cloud-sdk/current/path.bash.inc; do
        if [ -f "$inc" ]; then
            # shellcheck disable=SC1090
            source "$inc"
            break
        fi
    done
    if ! command -v docker-credential-gcloud >/dev/null 2>&1; then
        echo "WARN: docker-credential-gcloud still not on PATH;" \
             "pushes to *.pkg.dev / gcr.io will likely fail auth." >&2
    fi
}

# Image repository for docker build/push, e.g.
# <region>-docker.pkg.dev/<project>/<repository>/housegate-rewriter.
image_repo() {
    if [ -z "${REWRITER_IMAGE_REPO:-}" ]; then
        echo "Error: set REWRITER_IMAGE_REPO to the image repository" >&2
        exit 1
    fi
    printf '%s' "$REWRITER_IMAGE_REPO"
}

docker_build_image() {
    local ver=$1
    local repo
    repo=$(image_repo)
    echo "=========================================="
    echo "Build Docker image: $ver"
    echo "=========================================="

    if [ ! -f "docker/bare/root" ]; then
        ./docker/bare/prepare
    fi

    # Check local binary exists.
    if [ ! -f "build/clickhousegate_rewriter" ]; then
        echo "build/clickhousegate_rewriter is missing."
        echo "Run ./scripts.sh build or ./scripts.sh rebuild first."
        exit 1
    fi

    echo ">>> [1/3] Copy binary into the Docker context"
    cp -v build/clickhousegate_rewriter docker/bare/root/clickhousegate_rewriter

    local old_size=$(du -h docker/bare/root/clickhousegate_rewriter | awk '{print $1}')
    echo "Binary size: $old_size"

    echo ">>> [2/3] Build Docker image"
    (
      cd docker/bare
      docker buildx build --platform linux/amd64 . \
          -t "${repo}:${ver}"
    )

    echo ">>> [3/3] Docker image built"
    echo "=========================================="
    echo "Image tag: housegate-rewriter:$ver"
    echo "=========================================="
    echo "Next: ./scripts.sh docker push $ver"
    echo "=========================================="
}

docker_push_image() {
    local ver=$1
    local repo
    repo=$(image_repo)
    ensure_gcloud_on_path
    echo ">>> Pushing Docker image with tag: $ver"
    (
      cd docker/bare
      docker push "${repo}:${ver}"
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
