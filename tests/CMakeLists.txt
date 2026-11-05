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
    snapshot_query_contract_test.cc
    snapshot_query_test.cc
    snapshot_query_profiles_test.cc
    dos_hardening_test.cc
    error_redaction_test.cc
    $<TARGET_OBJECTS:rewriter_core>
)

target_include_directories(rewriter_tests PRIVATE
    ${REWRITER_PROTO_GENERATED_DIR}
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_BINARY_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(rewriter_tests PRIVATE
    REWRITER_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/testdata")


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
    clickhouse_functions rewriter_grpc_protos absl::debugging_internal OpenSSL::Crypto
)

add_test(NAME rewriter_tests COMMAND rewriter_tests)

# Exercise the real process entry point and Poco/gRPC startup lifecycle. The
# handler tests above link rewriter_core directly and cannot catch failures in
# ClickHouse worker-thread runtime initialization.
add_test(
    NAME rewriter_startup_smoke
    COMMAND bash
            ${CMAKE_CURRENT_SOURCE_DIR}/rewriter_startup_smoke.sh
            $<TARGET_FILE:clickhousegate_rewriter>)
set_tests_properties(rewriter_startup_smoke PROPERTIES TIMEOUT 20)

# Two-stage measured probes are opt-in to an externally generated profile;
# The measured verification lane supplies both service and test profiles;
# an ordinary build without those external artifacts reports an explicit skip.
add_test(NAME snapshot_profile_startup
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/snapshot_profile_startup.py
            $<TARGET_FILE:clickhousegate_rewriter>)
set_tests_properties(snapshot_profile_startup PROPERTIES TIMEOUT 240 SKIP_RETURN_CODE 77)
