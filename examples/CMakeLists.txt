add_executable(example_compose_pod compose_pod.cpp)
target_link_libraries(example_compose_pod PRIVATE PodFabric::podfabric)
podfabric_configure_target(example_compose_pod)

add_executable(example_selective_revalidation selective_revalidation.cpp)
target_link_libraries(example_selective_revalidation PRIVATE PodFabric::podfabric)
podfabric_configure_target(example_selective_revalidation)

# examples/downstream is deliberately not added here: it is a separate CMake
# project that consumes an installed PodFabric package through find_package.
