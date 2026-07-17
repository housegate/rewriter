cmake_minimum_required(VERSION 3.24)

# ---------- GoogleTest via FetchContent ----------
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
    $<TARGET_OBJECTS:rewriter_core>
)

target_include_directories(rewriter_tests PRIVATE
    ${REWRITER_PROTO_GENERATED_DIR}
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_BINARY_DIR}/src
)


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
