cmake_minimum_required(VERSION 3.25)

project(PodFabric
  VERSION 1.0.0
  DESCRIPTION "Generation-bound pod-level runtime for coordinated rack groups"
  HOMEPAGE_URL "https://github.com/summonlabs/Pod-Fabric"
  LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(PODFABRIC_BUILD_TOOLS "Build the podfabd daemon and podfabricctl" ON)
option(PODFABRIC_BUILD_EXAMPLES "Build the examples" ON)
option(PODFABRIC_BUILD_TESTS "Build the test suite" ON)
option(PODFABRIC_BUILD_BENCHMARKS "Build the benchmarks" ON)
option(PODFABRIC_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(PODFABRIC_ENABLE_ASAN "Build with AddressSanitizer" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# ---------------------------------------------------------------------------
# Warnings and sanitizer flags. Applied per target rather than through an
# exported INTERFACE target so that the installed package carries only genuine
# usage requirements.
# ---------------------------------------------------------------------------
function(podfabric_configure_target target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc)
    if(PODFABRIC_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
    if(PODFABRIC_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
  if(PODFABRIC_ENABLE_ASAN)
    if(MSVC)
      target_compile_options(${target} PRIVATE /fsanitize=address /Zi)
      target_link_options(${target} PRIVATE /INCREMENTAL:NO)
    else()
      target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer -g)
      target_link_options(${target} PRIVATE -fsanitize=address)
    endif()
  endif()
endfunction()

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------
set(PODFABRIC_SOURCES
  src/core/bytes.cpp
  src/core/digest.cpp
  src/core/status.cpp
  src/core/time.cpp
  src/core/token.cpp
  src/model/capacity.cpp
  src/model/obligation.cpp
  src/model/pod_state.cpp
  src/model/policy.cpp
  src/model/primitives.cpp
  src/model/topology.cpp
  src/codec/codec.cpp
  src/codec/text.cpp
  src/control/client.cpp
  src/control/controller.cpp
  src/control/protocol.cpp
  src/control/server.cpp
  src/engine/authority.cpp
  src/engine/composer.cpp
  src/engine/dependency.cpp
  src/engine/ledger.cpp
  src/model/snapshot.cpp
  src/model/synthetic.cpp
  src/persist/durable_state.cpp
  src/persist/journal.cpp
  src/persist/store.cpp
  src/transport/frame.cpp
  src/transport/socket.cpp
  src/platform/file.cpp
)

add_library(podfabric STATIC ${PODFABRIC_SOURCES})
add_library(PodFabric::podfabric ALIAS podfabric)
target_include_directories(podfabric
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
podfabric_configure_target(podfabric)
if(WIN32)
  # bcrypt provides the OS entropy source; ws2_32 the loopback transport. Both
  # are part of the public link interface of a static library.
  target_link_libraries(podfabric PUBLIC ws2_32 PRIVATE bcrypt)
endif()
set_target_properties(podfabric PROPERTIES
  OUTPUT_NAME podfabric
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR})

# ---------------------------------------------------------------------------
# Install and export
# ---------------------------------------------------------------------------
install(TARGETS podfabric
  EXPORT PodFabricTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT PodFabricTargets
  FILE PodFabricTargets.cmake
  NAMESPACE PodFabric::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/PodFabric)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/PodFabricConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/PodFabricConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/PodFabric)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/PodFabricConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/PodFabricConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/PodFabricConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/PodFabric)

install(FILES LICENSE NOTICE
  DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/doc/PodFabric)

# ---------------------------------------------------------------------------
# Subdirectories
# ---------------------------------------------------------------------------
if(PODFABRIC_BUILD_TOOLS)
  add_subdirectory(apps)
endif()
if(PODFABRIC_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
if(PODFABRIC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
if(PODFABRIC_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()
