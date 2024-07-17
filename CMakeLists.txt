cmake_minimum_required(VERSION 3.20)
project(MyProject)

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

set (CMAKE_CXX_STANDARD 23)
set (CMAKE_CXX_EXTENSIONS OFF)
set (CMAKE_CXX_STANDARD_REQUIRED ON)

set (CMAKE_C_STANDARD 11)
set (CMAKE_C_EXTENSIONS ON) # required by most contribs written in C
set (CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS 1) # Write compile_commands.json
set(CMAKE_LINK_DEPENDS_NO_SHARED 1) # Do not relink all depended targets on .so
set(CMAKE_CONFIGURATION_TYPES "RelWithDebInfo;Debug;Release;MinSizeRel" CACHE STRING "" FORCE)
set(CMAKE_DEBUG_POSTFIX "d" CACHE STRING "Generate debug library name with a postfix.")    # To be consistent with CMakeLists from contrib libs.

# Enable the ability to organize targets into hierarchies of "folders" for capable GUI-based IDEs.
# For more info see https://cmake.org/cmake/help/latest/prop_gbl/USE_FOLDERS.html
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

if (CMAKE_SYSTEM_NAME MATCHES "Linux")
    set (OS_LINUX 1)
    add_definitions(-D OS_LINUX)
elseif (CMAKE_SYSTEM_NAME MATCHES "Darwin")
    set (OS_DARWIN 1)
    add_definitions(-D OS_DARWIN)
    # For MAP_ANON/MAP_ANONYMOUS
    add_definitions(-D _DARWIN_C_SOURCE)
else ()
    message (FATAL_ERROR "Platform ${CMAKE_SYSTEM_NAME} is not supported")
endif ()

if (OS_DARWIN)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-U,_inside_main")
    
    set (DEFAULT_LIBS "-nodefaultlibs")

    set (DEFAULT_LIBS "${DEFAULT_LIBS} ${COVERAGE_OPTION} -lc -lm -lpthread -ldl")

    message(STATUS "Default libraries: ${DEFAULT_LIBS}")

    set(CMAKE_CXX_STANDARD_LIBRARIES ${DEFAULT_LIBS})
    set(CMAKE_C_STANDARD_LIBRARIES ${DEFAULT_LIBS})

    # Minimal supported SDK version
    set(CMAKE_OSX_DEPLOYMENT_TARGET 10.15)

    # Unfortunately '-pthread' doesn't work with '-nodefaultlibs'.
    # Just make sure we have pthreads at all.
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)

    set (CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -D_LIBCPP_DEBUG=0") # More checks in debug build.
endif()
if (OS_LINUX)
    set (CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} -fno-pie")
    set (CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_C_FLAGS_RELWITHDEBINFO} -fno-pie")
    set (CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -no-pie -Wl,-no-pie")
    set (DEFAULT_LIBS "-nodefaultlibs")

    # We need builtins from Clang's RT even without libcxx - for ubsan+int128.
    # See https://bugs.llvm.org/show_bug.cgi?id=16404
    execute_process (COMMAND ${CMAKE_CXX_COMPILER} --target=${CMAKE_CXX_COMPILER_TARGET} --print-libgcc-file-name --rtlib=compiler-rt OUTPUT_VARIABLE BUILTINS_LIBRARY OUTPUT_STRIP_TRAILING_WHITESPACE)

    # Apparently, in clang-19, the UBSan support library for C++ was moved out into ubsan_standalone_cxx.a, so we have to include both.
    if (SANITIZE STREQUAL undefined)
        string(REPLACE "builtins.a" "ubsan_standalone_cxx.a" EXTRA_BUILTINS_LIBRARY "${BUILTINS_LIBRARY}")
    endif ()

    if (NOT EXISTS "${BUILTINS_LIBRARY}")
        set (BUILTINS_LIBRARY "-lgcc")
    endif ()

    if (OS_ANDROID)
        # pthread and rt are included in libc
        set (DEFAULT_LIBS "${DEFAULT_LIBS} ${BUILTINS_LIBRARY} ${EXTRA_BUILTINS_LIBRARY} ${COVERAGE_OPTION} -lc -lm -ldl")
    elseif (USE_MUSL)
        set (DEFAULT_LIBS "${DEFAULT_LIBS} ${BUILTINS_LIBRARY} ${EXTRA_BUILTINS_LIBRARY} ${COVERAGE_OPTION} -static -lc")
    else ()
        set (DEFAULT_LIBS "${DEFAULT_LIBS} ${BUILTINS_LIBRARY} ${EXTRA_BUILTINS_LIBRARY} ${COVERAGE_OPTION} -lc -lm -lrt -lpthread -ldl")
    endif ()

    message(STATUS "Default libraries: ${DEFAULT_LIBS}")

    set(CMAKE_CXX_STANDARD_LIBRARIES ${DEFAULT_LIBS})
    set(CMAKE_C_STANDARD_LIBRARIES ${DEFAULT_LIBS})
endif()

# Add ClickHouse submodule
add_subdirectory(clickHouse)
no_warning(enum-constexpr-conversion)

PROTOBUF_GENERATE_GRPC_CPP(rewriter_grpc_proto_sources rewriter_grpc_proto_headers protos/rewriter.proto)
# Ignore warnings while compiling protobuf-generated *.pb.h and *.pb.cpp files.
set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -w")
# Disable clang-tidy for protobuf-generated *.pb.h and *.pb.cpp files.
set (CMAKE_CXX_CLANG_TIDY "")




add_library(rewriter_grpc_protos ${rewriter_grpc_proto_headers} ${rewriter_grpc_proto_sources})
target_include_directories(rewriter_grpc_protos SYSTEM PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries (rewriter_grpc_protos PUBLIC ch_contrib::grpc)

message(STATUS "Using headers ${rewriter_grpc_proto_headers}")
# include(${rewriter_grpc_proto_headers})
# set(SOURCES src/rewriter-server.cc ${rewriter_grpc_proto_headers})
set(SOURCES src/rewriter-server.cc src/ASTTransformers.cc)

# Add your executable or library
# clickhouse_add_executable(MyExecutable src/rewriter-server.cc)
clickhouse_add_executable(MyExecutable ${SOURCES})
target_include_directories(MyExecutable SYSTEM PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/protos)
target_include_directories(MyExecutable SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(MyExecutable PRIVATE
    "-DGIT_COMMIT_HASH=\"${GIT_COMMIT_HASH}\"")
# Link ClickHouse libraries
target_link_libraries(MyExecutable PRIVATE clickhouse_parsers clickhouse_common_io dbms rewriter_grpc_protos)
