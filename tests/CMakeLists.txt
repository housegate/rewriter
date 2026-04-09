cmake_minimum_required(VERSION 3.24)

# ---------- 引入 GoogleTest ----------
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

if (OS_DARWIN)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++")
elseif(OS_LINUX)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++ -nostdinc++ -isystem ${PROJECT_SOURCE_DIR}/clickHouse/contrib/llvm-project/libcxx/include -isystem ${PROJECT_SOURCE_DIR}/clickHouse/contrib/llvm-project/libcxxabi/include")
endif()

FetchContent_MakeAvailable(googletest)

find_package(Threads REQUIRED)

clickhouse_add_executable(rewriter_tests
    rewriter_test.cc
    ${PROJECT_SOURCE_DIR}/src/ASTTransformers.cc
    ${PROJECT_SOURCE_DIR}/src/rewriter-server.cc
)

target_include_directories(rewriter_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/protos
    ${PROJECT_BINARY_DIR}/protos
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_BINARY_DIR}/src
)

target_compile_definitions(rewriter_tests PRIVATE REWRITER_RELEASE_MAIN=0)

if (OS_DARWIN)
    target_compile_options(rewriter_tests PRIVATE -stdlib=libc++)
    target_link_options(  rewriter_tests PRIVATE -stdlib=libc++)
    target_link_libraries(rewriter_tests PRIVATE c++ c++abi)
elseif(OS_LINUX)
    target_compile_options(rewriter_tests PRIVATE -stdlib=libc++)
    target_link_options(  rewriter_tests PRIVATE -stdlib=libc++)
    # clickhouse_add_executable handles libc++ linked automatically, no need to link c++ directly.
endif()

target_link_libraries(rewriter_tests PRIVATE
    gtest_main gtest
    dl m Threads::Threads

    clickhouse_parsers clickhouse_common_io dbms
    clickhouse_functions rewriter_grpc_protos absl::debugging_internal
)

add_test(NAME rewriter_tests COMMAND rewriter_tests)
