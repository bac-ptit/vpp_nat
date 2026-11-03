add_executable(nat-ctl
    main.cpp
    nat_configurator.cpp
    ha_manager.cpp
)

target_include_directories(nat-ctl PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/include
    ${VPP_INCLUDE_DIR}
    /usr/include
)

target_link_libraries(nat-ctl PRIVATE
    glaze::glaze
    vapiclient
    vppapiclient
    vlibmemoryclient
    vppinfra
    pthread
)
