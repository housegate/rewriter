#!/usr/bin/env bash
set -euo pipefail

# 函数定义
usage() {
    cat <<EOF
Usage: $0 <command> [args]

Commands:
  prepare                   安装环境依赖（Linux/macOS 自动识别）
  build                     配置并编译整个项目
  docker list               列出本地所有 Docker 镜像
  docker build <version>    根据版本号构建 Docker 镜像
  docker push  <version>    将指定版本的镜像推送到远端仓库
EOF
}

# 安装依赖
prepare() {
    echo ">>> Preparing environment"
    case "$(uname -s)" in
        Linux*)
            echo ">>> Detected Linux, using APT"
            sudo apt-get update
            sudo apt-get install -y \
                pkg-config libssl-dev \
                git cmake ccache python3 ninja-build \
                nasm yasm gawk lsb-release wget software-properties-common gnupg
            echo ">>> Installing LLVM via apt.llvm.org"
            sudo bash -c "$(wget -qO- https://apt.llvm.org/llvm.sh)"
            export CC=clang-19
            export CXX=clang++-19
            ;;
        Darwin*)
            echo ">>> Detected macOS, using Homebrew"
            if ! command -v brew >/dev/null; then
                echo "Homebrew not found! Install it first: https://brew.sh"
                exit 1
            fi
            brew update
            brew install ccache cmake ninja libtool gettext binutils grep findutils nasm bash llvm@19
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

# 编译项目
build() {
    prepare
    echo ">>> Starting build"
    rm -rf build/
    sed -i 's/const size_type count_/size_type count_/' \
        clickHouse/contrib/flatbuffers/include/flatbuffers/stl_emulation.h

    pushd clickHouse/src > /dev/null
    rm -rf clickHouse
    ln -s . clickHouse
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

    JOBS=$(
        command -v nproc   >/dev/null && nproc ||
        sysctl -n hw.ncpu  2>/dev/null ||
        getconf _NPROCESSORS_ONLN 2>/dev/null ||
        echo 4
    )
    echo ">>> Building with -j$JOBS"
    ninja -C build -j"$JOBS"

    # 清理子模块改动
    git submodule foreach --recursive 'git reset --hard && git clean -fdx' >/dev/null
    echo ">>> build done"
}

docker_list() {
    echo ">>> Docker images list:"
    docker images
}

docker_build_image() {
    local ver=$1
    echo ">>> Building Docker image with tag: $ver"
    (
      cd docker/bare
      docker buildx build --platform linux/amd64 . \
          -t us-west1-docker.pkg.dev/sentio-352722/sentio/sql-rewriter:"${ver}"
    )
    echo ">>> Back to project root ($(pwd))"
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

# 主逻辑分发
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
