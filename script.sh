#!/usr/bin/env bash
set -euo pipefail

# 函数定义
usage() {
    cat <<EOF
Usage: $0 <command> [args]

Commands:
  prepare                   安装环境依赖（Linux/macOS 自动识别）
  build                     完整重新构建（删除 build 目录）
  rebuild                   增量编译（快速，推荐用于日常开发）
  test                      构建并运行单元测试
  clean                     清理 build 目录
  docker list               列出本地所有 Docker 镜像
  docker build <version>    根据版本号构建 Docker 镜像
  docker push  <version>    将指定版本的镜像推送到远端仓库

优化建议:
  - 首次构建使用: ./script.sh build
  - 后续修改使用: ./script.sh rebuild (快 10 倍以上)
  - 测试优化:     ./script.sh test
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

# 编译项目（完整重新构建）
build() {
    echo "=========================================="
    echo "完整重新构建（删除 build 目录）"
    echo "注意：首次构建约需 15-20 分钟"
    echo "=========================================="
    START_TIME=$(date +%s)
    
    prepare
    echo ">>> [1/5] 清理旧的构建目录"
    rm -rf build/
    
    echo ">>> [2/5] 修补子模块兼容性问题"
    sed -i 's/const size_type count_/size_type count_/' \
        clickHouse/contrib/flatbuffers/include/flatbuffers/stl_emulation.h
    
    # 修复 ClickHouse 最新版 numactl-cmake 漏掉的 C 源码导致 link error (undefined symbol: resolve_affinity 等)
    sed -i 's|"${LIBRARY_DIR}/syscall.c"|"${LIBRARY_DIR}/syscall.c"\n    "${LIBRARY_DIR}/distance.c"\n    "${LIBRARY_DIR}/affinity.c"\n    "${LIBRARY_DIR}/sysfs.c"\n    "${LIBRARY_DIR}/rtnetlink.c"|g' \
        clickHouse/contrib/numactl-cmake/CMakeLists.txt

    pushd clickHouse/src > /dev/null
    rm -rf clickHouse
    ln -s . clickHouse
    popd > /dev/null

    rm -rf contrib
    ln -s clickHouse/contrib contrib

    echo ">>> [3/5] 配置 CMake（优化：禁用 ThinLTO + 使用 mold 链接器）"
    cmake -B build -S . -GNinja \
      -DENABLE_RUST=OFF \
      -DENABLE_TESTS=OFF \
      -DBUILD_TESTING=ON \
      -DENABLE_THINLTO=OFF \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=mold

    echo ">>> [4/5] 构建 OpenSSL 库"
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
    echo ">>> [5/5] 编译主程序（使用 $JOBS 个并行任务）"
    ninja -C build -j"$JOBS" clickhouse_sentio_rewriter
    
    # 如果启用了测试，也编译测试
    if grep -q "BUILD_TESTING:BOOL=ON" build/CMakeCache.txt 2>/dev/null; then
        echo ">>> [5.1/5] 编译测试程序"
        ninja -C build -j"$JOBS" rewriter_tests || echo "Warning: rewriter_tests build skipped"
    fi

    # 清理子模块改动
    git submodule foreach --recursive 'git reset --hard && git clean -fdx' >/dev/null
    
    END_TIME=$(date +%s)
    DURATION=$((END_TIME - START_TIME))
    echo "=========================================="
    echo "✅ 构建完成！总耗时: ${DURATION} 秒 ($((DURATION / 60)) 分 $((DURATION % 60)) 秒)"
    echo "可执行文件: build/clickhouse_sentio_rewriter"
    echo "=========================================="
}

# 增量编译（快速，适合日常开发）
rebuild() {
    echo "=========================================="
    echo "增量编译模式（只重新编译修改的文件）"
    echo "预计耗时: 30秒 - 3分钟"
    echo "=========================================="
    START_TIME=$(date +%s)
    
    if [ ! -d "build" ]; then
        echo "❌ build 目录不存在，请先运行: ./script.sh build"
        exit 1
    fi
    
    JOBS=$(
        command -v nproc   >/dev/null && nproc ||
        sysctl -n hw.ncpu  2>/dev/null ||
        getconf _NPROCESSORS_ONLN 2>/dev/null ||
        echo 4
    )
    
    echo ">>> [1/2] 重新编译修改的源文件"
    cd build
    ninja -j"$JOBS" clickhouse_sentio_rewriter
    cd ..
    
    # 如果启用了测试，也编译测试
    if grep -q "BUILD_TESTING:BOOL=ON" build/CMakeCache.txt 2>/dev/null; then
        echo ">>> [2/2] 重新编译测试程序"
        cd build
        ninja -j"$JOBS" rewriter_tests || echo "Warning: rewriter_tests build skipped"
        cd ..
    fi
    
    END_TIME=$(date +%s)
    DURATION=$((END_TIME - START_TIME))
    echo "=========================================="
    echo "✅ 增量编译完成！耗时: ${DURATION} 秒"
    echo "可执行文件: build/clickhouse_sentio_rewriter"
    echo "=========================================="
}

# 清理构建目录
clean() {
    echo ">>> 清理 build 目录..."
    rm -rf build/
    echo ">>> 清理完成"
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
    echo "构建 Docker 镜像: $ver"
    echo "=========================================="
    
    # 检查本地二进制文件是否存在
    if [ ! -f "build/clickhouse_sentio_rewriter" ]; then
        echo "❌ 错误: build/clickhouse_sentio_rewriter 不存在"
        echo "请先运行: ./script.sh build 或 ./script.sh rebuild"
        exit 1
    fi
    
    echo ">>> [1/3] 更新 Docker 中的二进制文件"
    cp -v build/clickhouse_sentio_rewriter docker/bare/root/clickhouse_sentio_rewriter
    
    local old_size=$(du -h docker/bare/root/clickhouse_sentio_rewriter | awk '{print $1}')
    echo "二进制文件大小: $old_size"
    
    echo ">>> [2/3] 构建 Docker 镜像"
    (
      cd docker/bare
      docker buildx build --platform linux/amd64 . \
          -t us-west1-docker.pkg.dev/sentio-352722/sentio/sql-rewriter:"${ver}"
    )
    
    echo ">>> [3/3] Docker 镜像构建完成"
    echo "=========================================="
    echo "✅ 镜像标签: sql-rewriter:$ver"
    echo "=========================================="
    echo "下一步: ./script.sh docker push $ver"
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
