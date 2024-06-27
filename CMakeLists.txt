cmake_minimum_required(VERSION 3.20)
project(MyProject)

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

set (OS_DARWIN 1)
    add_definitions(-D OS_DARWIN)
    # For MAP_ANON/MAP_ANONYMOUS
    add_definitions(-D _DARWIN_C_SOURCE)

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

# Add ClickHouse submodule
add_subdirectory(ClickHouse)
no_warning(enum-constexpr-conversion)
# Add your executable or library
clickhouse_add_executable(MyExecutable src/main.cc)

# Link ClickHouse libraries
target_link_libraries(MyExecutable PRIVATE clickhouse_parsers clickhouse_common_io dbms)
