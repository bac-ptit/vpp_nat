add_vpp_plugin(nat_pba
  SOURCES
    nat_pba_node.c
    ${NAT_HASH_FILE}
    nat_bridge.cpp
    nat_engine.cpp
    nat_ha_sync.cpp
)

target_link_libraries(nat_pba_plugin
    vppinfra
)

target_include_directories(nat_pba_plugin PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/include
)

set_target_properties(nat_pba_plugin PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
)

target_compile_options(nat_pba_plugin PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
    -O3
    -march=native
    -flto=auto
    -fomit-frame-pointer
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wconversion
    -Wsign-conversion
    -Wcast-align
    -Wunused
    -Wno-address-of-packed-member
)

target_link_options(nat_pba_plugin PRIVATE
    -flto=auto
    -O3
    -march=native
)
