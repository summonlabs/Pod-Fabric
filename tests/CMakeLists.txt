add_library(podfabric_test_support STATIC
  support/test.cpp
  support/test_main.cpp
  support/process.cpp)
target_include_directories(podfabric_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/support)
target_link_libraries(podfabric_test_support PUBLIC PodFabric::podfabric)
podfabric_configure_target(podfabric_test_support)

add_executable(podfabric_tests
  unit/core_test.cpp
  unit/model_test.cpp
  unit/codec_test.cpp
  unit/ledger_test.cpp
  unit/engine_test.cpp
  unit/dependency_test.cpp
  unit/authority_test.cpp
  unit/persist_test.cpp
  unit/transport_test.cpp
  unit/control_test.cpp
  property/property_test.cpp
  adversarial/adversarial_test.cpp
  concurrency/concurrency_test.cpp
  multiprocess/multiprocess_test.cpp)
target_link_libraries(podfabric_tests PRIVATE podfabric_test_support)
podfabric_configure_target(podfabric_tests)

if(TARGET podfabd)
  target_compile_definitions(podfabric_tests PRIVATE
    PODFABRIC_DAEMON_PATH="$<TARGET_FILE:podfabd>"
    PODFABRIC_CTL_PATH="$<TARGET_FILE:podfabricctl>")
endif()

# No test declares a timeout: a hang is a defect to diagnose, not a result to
# classify. Every case must terminate on its own.
set(PODFABRIC_TEST_SUITES
  core model codec ledger engine dependency authority persist transport control
  property adversarial concurrency multiprocess)
foreach(suite IN LISTS PODFABRIC_TEST_SUITES)
  add_test(NAME podfabric.${suite} COMMAND podfabric_tests --suite=${suite})
endforeach()
