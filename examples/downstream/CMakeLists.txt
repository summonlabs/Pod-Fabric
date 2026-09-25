cmake_minimum_required(VERSION 3.25)

# A stand-alone consumer of the installed PodFabric package. It is deliberately
# a separate CMake project: it must configure, build and run against an
# installed prefix only, with no access to the Pod Fabric source tree.
project(PodFabricDownstream LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PodFabric 1.0 REQUIRED)

add_executable(podfabric_consumer main.cpp)
target_link_libraries(podfabric_consumer PRIVATE PodFabric::podfabric)
