cmake_minimum_required(VERSION 3.24)
cmake_policy(SET CMP0135 NEW)
project(MyProject)

# Disable unsupported linker options on macOS.
if (APPLE)
    set(CMAKE_C_LINK_FLAGS "")
    set(CMAKE_CXX_LINK_FLAGS "")
    # Make sure --threads is never passed to the linker.
    set(CMAKE_TRY_COMPILE_CONFIGURATION "Release")
endif ()


# Get the current working branch
execute_process(
        COMMAND git rev-parse --abbrev-ref HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_BRANCH
        OUTPUT_STRIP_TRAILING_WHITESPACE)

# Get the latest commit hash
execute_process(
        COMMAND git rev-parse HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_COMMIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE)

# Derive the rewriter version from `git describe`. On a tagged commit this
# yields the tag name (e.g. `v0.1.18`); off-tag it adds a commit count and
# short hash (e.g. `v0.1.18-5-gabc123`). Falls back to "unknown" when git
# info is unavailable (e.g. source tarball build with no .git directory).
execute_process(
        COMMAND git describe --tags --always --dirty
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE REWRITER_VERSION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
if (NOT REWRITER_VERSION)
    set(REWRITER_VERSION "unknown")
endif ()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON) # required by most contribs written in C
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS 1) # Write compile_commands.json
set(CMAKE_LINK_DEPENDS_NO_SHARED 1) # Do not relink all depended targets on .so
set(CMAKE_CONFIGURATION_TYPES "RelWithDebInfo;Debug;Release;MinSizeRel" CACHE STRING "" FORCE)
set(CMAKE_DEBUG_POSTFIX "d" CACHE STRING "Generate debug library name with a postfix.")    # To be consistent with CMakeLists from contrib libs.

# Enable the ability to organize targets into hierarchies of "folders" for capable GUI-based IDEs.
# For more info see https://cmake.org/cmake/help/latest/prop_gbl/USE_FOLDERS.html
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

if (CMAKE_SYSTEM_NAME MATCHES "Linux")
    set(OS_LINUX 1)
    add_definitions(-D OS_LINUX)
elseif (CMAKE_SYSTEM_NAME MATCHES "Darwin")
    set(OS_DARWIN 1)
    add_definitions(-D OS_DARWIN)
    # For MAP_ANON/MAP_ANONYMOUS
    add_definitions(-D _DARWIN_C_SOURCE)
else ()
    message(FATAL_ERROR "Platform ${CMAKE_SYSTEM_NAME} is not supported")
endif ()

if (OS_DARWIN)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-U,_inside_main")

    set(DEFAULT_LIBS "-nodefaultlibs")

    set(DEFAULT_LIBS "${DEFAULT_LIBS} ${COVERAGE_OPTION} -lc -lm -lpthread -ldl")

    message(STATUS "Default libraries: ${DEFAULT_LIBS}")

    set(CMAKE_CXX_STANDARD_LIBRARIES ${DEFAULT_LIBS})
    set(CMAKE_C_STANDARD_LIBRARIES ${DEFAULT_LIBS})

    # Minimal supported SDK version
    set(CMAKE_OSX_DEPLOYMENT_TARGET 10.15)

    # Unfortunately '-pthread' doesn't work with '-nodefaultlibs'.
    # Just make sure we have pthreads at all.
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)

    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -D_LIBCPP_DEBUG=0") # More checks in debug build.
endif ()
if (OS_LINUX)
    set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} -fno-pie")
    set(CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_C_FLAGS_RELWITHDEBINFO} -fno-pie")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -no-pie -Wl,-no-pie")
    set(DEFAULT_LIBS "-nodefaultlibs")

    # We need builtins from Clang's RT even without libcxx - for ubsan+int128.
    # See https://bugs.llvm.org/show_bug.cgi?id=16404
    execute_process(COMMAND ${CMAKE_CXX_COMPILER} --target=${CMAKE_CXX_COMPILER_TARGET} --print-libgcc-file-name --rtlib=compiler-rt OUTPUT_VARIABLE BUILTINS_LIBRARY OUTPUT_STRIP_TRAILING_WHITESPACE)

    # Apparently, in clang-19, the UBSan support library for C++ was moved out into ubsan_standalone_cxx.a, so we have to include both.
    if (SANITIZE STREQUAL undefined)
        string(REPLACE "builtins.a" "ubsan_standalone_cxx.a" EXTRA_BUILTINS_LIBRARY "${BUILTINS_LIBRARY}")
    endif ()

    if (NOT EXISTS "${BUILTINS_LIBRARY}")
        set(BUILTINS_LIBRARY "-lgcc")
    endif ()

    if (OS_ANDROID)
        # pthread and rt are included in libc
        set(DEFAULT_LIBS "${DEFAULT_LIBS} ${BUILTINS_LIBRARY} ${EXTRA_BUILTINS_LIBRARY} ${COVERAGE_OPTION} -lc -lm -ldl")
    elseif (USE_MUSL)
        set(DEFAULT_LIBS "${DEFAULT_LIBS} ${BUILTINS_LIBRARY} ${EXTRA_BUILTINS_LIBRARY} ${COVERAGE_OPTION} -static -lc")
    else ()
        set(DEFAULT_LIBS "${DEFAULT_LIBS} ${BUILTINS_LIBRARY} ${EXTRA_BUILTINS_LIBRARY} ${COVERAGE_OPTION} -lc -lm -lrt -lpthread -ldl")
    endif ()

    message(STATUS "Default libraries: ${DEFAULT_LIBS}")

    set(CMAKE_CXX_STANDARD_LIBRARIES ${DEFAULT_LIBS})
    set(CMAKE_C_STANDARD_LIBRARIES ${DEFAULT_LIBS})
endif ()

if (OS_LINUX)
    set(_LLD_PARALLEL "-Wl,--threads=16")
else ()
    set(_LLD_PARALLEL "")
endif ()

set(CMAKE_EXE_LINKER_FLAGS
        "${CMAKE_EXE_LINKER_FLAGS} ${_LLD_PARALLEL}"
        CACHE STRING "Global exe linker flags" FORCE)
set(CMAKE_SHARED_LINKER_FLAGS
        "${CMAKE_SHARED_LINKER_FLAGS} ${_LLD_PARALLEL}"
        CACHE STRING "Global shared linker flags" FORCE)
set(CMAKE_MODULE_LINKER_FLAGS
        "${CMAKE_MODULE_LINKER_FLAGS} ${_LLD_PARALLEL}"
        CACHE STRING "Global module linker flags" FORCE)

# Add ClickHouse submodule
add_subdirectory(clickHouse)
no_warning(enum-constexpr-conversion)

# Protobuf's generated map accessors and its runtime both instantiate Abseil
# hashing code.  Those translation units must therefore use the same x86 ISA
# policy: mixing ClickHouse's x86-64-v2/PCLMUL runtime with generic generated
# or consumer code changes the inline hash selected for the same map.
#
# Mirror ClickHouse's supported AMD64 policy from cpu_features.cmake on every
# supported OS, including Intel macOS.  The cache variable is the source of
# truth so -DX86_ARCH_LEVEL=1 remains a valid compatibility build, while
# levels 2-4 carry ClickHouse's extra PCLMUL baseline.  The target is
# intentionally empty on non-x86 architectures, where these flags are invalid.
add_library(rewriter_clickhouse_x86_policy INTERFACE)
if (CMAKE_SYSTEM_PROCESSOR MATCHES "^(amd64|AMD64|x86_64|X86_64)$")
    if (NOT DEFINED X86_ARCH_LEVEL OR NOT X86_ARCH_LEVEL MATCHES "^[1-4]$")
        message(FATAL_ERROR
            "ClickHouse X86_ARCH_LEVEL must be one of 1, 2, 3, or 4; got "
            "'${X86_ARCH_LEVEL}'")
    endif ()

    if (X86_ARCH_LEVEL VERSION_GREATER_EQUAL 2)
        target_compile_options(rewriter_clickhouse_x86_policy INTERFACE
            "-march=x86-64-v${X86_ARCH_LEVEL}"
            -mpclmul)
    endif ()
    message(STATUS
        "Rewriter protobuf consumers use ClickHouse x86 ISA level ${X86_ARCH_LEVEL}")
else ()
    message(STATUS
        "Rewriter protobuf x86 ISA policy is not applicable to "
        "${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR}")
endif ()

set(REWRITER_PROTO_PATH "third_party/rewriter-proto/proto/rewriter.proto")
if (NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${REWRITER_PROTO_PATH}")
    message(FATAL_ERROR
        "Missing ${REWRITER_PROTO_PATH}; run "
        "`git submodule update --init third_party/rewriter-proto`")
endif ()

PROTOBUF_GENERATE_GRPC_CPP(
    rewriter_grpc_proto_sources
    rewriter_grpc_proto_headers
    "${REWRITER_PROTO_PATH}")
set(REWRITER_PROTO_GENERATED_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/third_party/rewriter-proto/proto")
# Ignore warnings while compiling protobuf-generated *.pb.h and *.pb.cpp files.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -w")
# Disable clang-tidy for protobuf-generated *.pb.h and *.pb.cpp files.
set(CMAKE_CXX_CLANG_TIDY "")

add_library(rewriter_grpc_protos ${rewriter_grpc_proto_headers} ${rewriter_grpc_proto_sources})
target_include_directories(rewriter_grpc_protos SYSTEM PUBLIC
    ${CMAKE_CURRENT_BINARY_DIR}
    ${REWRITER_PROTO_GENERATED_DIR})
# Publish the ISA policy with the generated-message target so every protobuf
# consumer inherits it through the normal dependency graph.
target_link_libraries(rewriter_grpc_protos PUBLIC
    ch_contrib::grpc
    rewriter_clickhouse_x86_policy)

message(STATUS "Using headers ${rewriter_grpc_proto_headers}")

# Shared object library — everything except entry points.
add_library(rewriter_core OBJECT
    src/rewriter-server.cc
    src/query_preprocessor.cc
    src/snapshot_query_profiles.cc
    src/snapshot_executable_identity.cc
    src/ASTTransformers.cc
    src/rewriter-http-server.cc
    src/handlers/use.cc
    src/handlers/show_tables.cc
    src/handlers/show_databases.cc
    src/handlers/writes.cc
    src/handlers/select.cc
    src/handlers/name_rewrite.cc
    src/handlers/storage_integrity.cc
    src/handlers/table_reference.cc
    src/handlers/system_tables.cc
    src/handlers/describe.cc
    src/handlers/exists.cc
    src/handlers/show_create.cc
    src/handlers/grant.cc
    src/handlers/snapshot_query.cc
    src/handlers/materialize.cc
    src/handlers/join_swap.cc)
target_include_directories(rewriter_core SYSTEM PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${REWRITER_PROTO_GENERATED_DIR})
target_link_libraries(rewriter_core PUBLIC
    clickhouse_parsers clickhouse_common_io dbms clickhouse_functions
    rewriter_grpc_protos absl::debugging_internal OpenSSL::Crypto)
# Stamp the version / commit hash into rewriter-server.cc ONLY (that's the
# sole consumer). Previously this was a rewriter_core PUBLIC define, which
# meant any change to REWRITER_VERSION — including `git describe --dirty`
# flipping on/off during dev — invalidated every TU in rewriter_core (~12
# files) plus both executables. Narrowing to the one source file keeps
# incremental rebuilds at ~1 TU + 1 link when only the version string
# changed, which is the common case on each release cut.
set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rewriter-server.cc
    PROPERTIES COMPILE_DEFINITIONS
      "GIT_COMMIT_HASH=\"${GIT_COMMIT_HASH}\";REWRITER_VERSION=\"${REWRITER_VERSION}\"")

# Server executable.
clickhouse_add_executable(clickhousegate_rewriter
    src/rewriter-server-main.cc
    $<TARGET_OBJECTS:rewriter_core>)
target_link_libraries(clickhousegate_rewriter PRIVATE rewriter_core)

# CLI executable — one-shot rewriter for scripting.
clickhouse_add_executable(clickhousegate_rewriter_cli
    src/rewriter-cli.cc
    $<TARGET_OBJECTS:rewriter_core>)
target_link_libraries(clickhousegate_rewriter_cli PRIVATE rewriter_core)

option(BUILD_TESTING "Build unit tests" OFF)
if (BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif ()

if (OS_DARWIN)
    # Strip any --threads occurrences out of the linker flags on macOS.
    string(REPLACE "-Wl,--threads=16" "" CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")
    string(REPLACE "-Wl,--threads=16" "" CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS}")
    string(REPLACE "-Wl,--threads=16" "" CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS}")

    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}" CACHE STRING "Global exe linker flags" FORCE)
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS}" CACHE STRING "Global shared linker flags" FORCE)
    set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS}" CACHE STRING "Global module linker flags" FORCE)
endif ()
