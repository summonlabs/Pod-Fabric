add_library(podfabric_cli STATIC cli.cpp)
target_include_directories(podfabric_cli PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(podfabric_cli PUBLIC PodFabric::podfabric)
podfabric_configure_target(podfabric_cli)

add_executable(podfabd podfabd/main.cpp)
target_link_libraries(podfabd PRIVATE podfabric_cli)
podfabric_configure_target(podfabd)

add_executable(podfabricctl podfabricctl/main.cpp)
target_link_libraries(podfabricctl PRIVATE podfabric_cli)
podfabric_configure_target(podfabricctl)

install(TARGETS podfabd podfabricctl
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
