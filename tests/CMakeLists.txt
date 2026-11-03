include(CTest)

# 1. Test PBA Bitmap Allocator
add_executable(test_pba test_pba.cpp)
target_include_directories(test_pba PRIVATE
    ${PROJECT_SOURCE_DIR}/src/plugin
    ${PROJECT_SOURCE_DIR}/include
)
target_compile_options(test_pba PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>
    -UNDEBUG
)
add_test(NAME test_pba COMMAND test_pba)

# 2. Test Config Reflection
add_executable(test_config test_config.cpp)
target_include_directories(test_config PRIVATE
    ${PROJECT_SOURCE_DIR}/src/control
    ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(test_config PRIVATE
    glaze::glaze
)
add_test(NAME test_config COMMAND test_config
    ${PROJECT_SOURCE_DIR}/config/config.json
    ${PROJECT_SOURCE_DIR}/config/config.yaml
)

# 3. Test NAT Engine & Formatter
add_executable(test_nat_engine
    test_nat_engine.cpp
    ${PROJECT_SOURCE_DIR}/src/plugin/${NAT_HASH_FILE}
    ${PROJECT_SOURCE_DIR}/src/plugin/nat_engine.cpp
)
target_include_directories(test_nat_engine PRIVATE
    ${PROJECT_SOURCE_DIR}/src/plugin
    ${PROJECT_SOURCE_DIR}/include
)
target_compile_options(test_nat_engine PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>
    -UNDEBUG
)
target_link_libraries(test_nat_engine PRIVATE vppinfra)
add_test(NAME test_nat_engine COMMAND test_nat_engine)

# 3b. Test NAT bridge packet paths (ICMP, fragments, TCP state)
add_executable(test_nat_bridge
    test_nat_bridge.cpp
    ${PROJECT_SOURCE_DIR}/src/plugin/${NAT_HASH_FILE}
    ${PROJECT_SOURCE_DIR}/src/plugin/nat_bridge.cpp
    ${PROJECT_SOURCE_DIR}/src/plugin/nat_engine.cpp
)
target_include_directories(test_nat_bridge PRIVATE
    ${PROJECT_SOURCE_DIR}/src/plugin
    ${PROJECT_SOURCE_DIR}/include
)
target_compile_options(test_nat_bridge PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>
    -UNDEBUG
)
target_link_libraries(test_nat_bridge PRIVATE vppinfra)
add_test(NAME test_nat_bridge COMMAND test_nat_bridge)

# 3c. Test HA session export/import and exact port reservation
add_executable(test_ha_sync
    test_ha_sync.cpp
    ${PROJECT_SOURCE_DIR}/src/plugin/${NAT_HASH_FILE}
    ${PROJECT_SOURCE_DIR}/src/plugin/nat_engine.cpp
)
target_include_directories(test_ha_sync PRIVATE
    ${PROJECT_SOURCE_DIR}/src/plugin
    ${PROJECT_SOURCE_DIR}/include
)
target_compile_options(test_ha_sync PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>
    -UNDEBUG
)
target_link_libraries(test_ha_sync PRIVATE vppinfra)
add_test(NAME test_ha_sync COMMAND test_ha_sync)

# 3d. End-to-end TCP sync between an active and a standby process
add_executable(test_ha_transport
    test_ha_transport.cpp
    ${PROJECT_SOURCE_DIR}/src/plugin/${NAT_HASH_FILE}
    ${PROJECT_SOURCE_DIR}/src/plugin/nat_bridge.cpp
    ${PROJECT_SOURCE_DIR}/src/plugin/nat_engine.cpp
    ${PROJECT_SOURCE_DIR}/src/plugin/nat_ha_sync.cpp
)
target_include_directories(test_ha_transport PRIVATE
    ${PROJECT_SOURCE_DIR}/src/plugin
    ${PROJECT_SOURCE_DIR}/include
)
target_compile_options(test_ha_transport PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>
    -UNDEBUG
)
target_link_libraries(test_ha_transport PRIVATE vppinfra)
add_test(NAME test_ha_transport
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/run_ha_transport_test.sh
            $<TARGET_FILE:test_ha_transport>
)

# 3e. Batch lookup micro-benchmark (scalar vs prefetched batch)
add_executable(bench_batch_lookup
    bench_batch_lookup.cpp
    ${PROJECT_SOURCE_DIR}/src/plugin/${NAT_HASH_FILE}
    ${PROJECT_SOURCE_DIR}/src/plugin/nat_bridge.cpp
    ${PROJECT_SOURCE_DIR}/src/plugin/nat_engine.cpp
)
target_include_directories(bench_batch_lookup PRIVATE
    ${PROJECT_SOURCE_DIR}/src/plugin
    ${PROJECT_SOURCE_DIR}/include
)
target_compile_options(bench_batch_lookup PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>
)
target_link_libraries(bench_batch_lookup PRIVATE vppinfra)

# 3f. Research: VPP bihash vs custom flat hash under 15% churn
add_executable(bench_hash_backend
    bench_hash_backend.cpp
    ${PROJECT_SOURCE_DIR}/src/plugin/${NAT_HASH_FILE}
)
target_include_directories(bench_hash_backend PRIVATE
    ${PROJECT_SOURCE_DIR}/src/plugin
    ${PROJECT_SOURCE_DIR}/include
)
target_compile_options(bench_hash_backend PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>
    -O3
)
target_link_libraries(bench_hash_backend PRIVATE vppinfra pthread)

# 4. Strict Real Churn Benchmark (16 Workers, 15% New / 85% Existing)
add_executable(bench_real_churn_10s
    bench_real_churn_10s.cpp
    ${PROJECT_SOURCE_DIR}/src/plugin/${NAT_HASH_FILE}
    ${PROJECT_SOURCE_DIR}/src/plugin/nat_bridge.cpp
    ${PROJECT_SOURCE_DIR}/src/plugin/nat_engine.cpp
)
target_include_directories(bench_real_churn_10s PRIVATE
    ${PROJECT_SOURCE_DIR}/src/plugin
    ${PROJECT_SOURCE_DIR}/include
)
target_compile_options(bench_real_churn_10s PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>
    -g
)
if(NAT_PROFILE_NOINLINE)
    target_compile_options(bench_real_churn_10s PRIVATE
        -DNAT_PROFILE_NOINLINE=1
        -fno-lto
        -fno-omit-frame-pointer
        -g
    )
    target_link_options(bench_real_churn_10s PRIVATE -fno-lto)
endif()
target_link_libraries(bench_real_churn_10s PRIVATE vppinfra)
