cmake_minimum_required(VERSION 3.28)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "Minimum policy version" FORCE)
project(nat LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wcast-align
    -Wunused
    -Wno-address-of-packed-member
    -O3
    -march=native
    -flto=auto
    -fomit-frame-pointer
)

add_link_options(
    -flto=auto
    -O3
    -march=native
)

# High Availability (HA) toggle: default OFF for maximum standalone performance
option(NAT_ENABLE_HA "Enable High Availability synchronization and failover daemon" OFF)
option(NAT_PROFILE_NOINLINE "Disable forced inlining for perf attribution builds" OFF)
option(NAT_EXPERIMENTAL_FLAT_HASH "Use the experimental flat hash backend" OFF)
if(NAT_EXPERIMENTAL_FLAT_HASH)
    set(NAT_HASH_FILE flat_bihash_wrapper.c)
else()
    set(NAT_HASH_FILE bihash_wrapper.c)
endif()
if(NAT_ENABLE_HA)
    add_compile_definitions(NAT_ENABLE_HA=1)
else()
    add_compile_definitions(NAT_ENABLE_HA=0)
endif()

include(FetchContent)

# 1. Locate or fetch Glaze C++23 JSON/YAML/Reflection library
find_package(glaze QUIET)
if(NOT TARGET glaze::glaze)
    message(STATUS "Fetching Glaze v8.0.0 via FetchContent...")
    FetchContent_Declare(
        glaze
        GIT_REPOSITORY https://github.com/stephenberry/glaze.git
        GIT_TAG v8.0.0
    )
    FetchContent_MakeAvailable(glaze)
endif()

# 2. Locate VPP headers, libraries, and plugin framework
find_package(VPP REQUIRED)

# Include paths for VPP and project headers
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${VPP_INCLUDE_DIR}
    ${VPP_INCLUDE_DIR}/vpp_plugins
)

link_directories(
    /usr/lib/x86_64-linux-gnu
    /usr/lib/x86_64-linux-gnu/vpp
    ${VPP_LIBRARY_DIR}
)

# 4. Build components
enable_testing()
add_subdirectory(src/control)
add_subdirectory(src/plugin)
add_subdirectory(tests)
