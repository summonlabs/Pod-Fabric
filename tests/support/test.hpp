// A small self-contained test framework.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// The project deliberately takes no external test dependency: the harness is
// part of the repository, is built with the same warning flags as the library,
// and adds no network fetch to the build.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace podfabric::test {

struct TestFailure {
  std::string message;
};

struct Case {
  const char* suite;
  const char* name;
  void (*body)();
};

class Registry {
 public:
  static Registry& instance();
  void add(const char* suite, const char* name, void (*body)());
  const std::vector<Case>& cases() const noexcept { return cases_; }

 private:
  std::vector<Case> cases_{};
};

struct Registrar {
  Registrar(const char* suite, const char* name, void (*body)()) {
    Registry::instance().add(suite, name, body);
  }
};

// Records a failure and continues the current case.
void record_failure(const char* file, int line, std::string message);
// Records a failure and aborts the current case.
[[noreturn]] void fail_now(const char* file, int line, std::string message);

// Runs every case whose suite matches the filter (empty means all) and returns
// the number of failed cases. Failing seeds are printed as part of the message.
int run_all(const std::string& suite_filter, bool list_only);

std::string describe_bytes(std::size_t count);

}  // namespace podfabric::test

#define PF_TEST(suite_name, case_name)                                            \
  static void pf_case_##suite_name##_##case_name();                               \
  static const ::podfabric::test::Registrar pf_reg_##suite_name##_##case_name(    \
      #suite_name, #case_name, &pf_case_##suite_name##_##case_name);              \
  static void pf_case_##suite_name##_##case_name()

#define PF_CHECK(expression)                                                      \
  do {                                                                            \
    if (!(expression)) {                                                          \
      ::podfabric::test::record_failure(__FILE__, __LINE__,                       \
                                        "check failed: " #expression);            \
    }                                                                             \
  } while (false)

#define PF_CHECK_MSG(expression, detail)                                          \
  do {                                                                            \
    if (!(expression)) {                                                          \
      ::podfabric::test::record_failure(__FILE__, __LINE__,                       \
                                        std::string("check failed: " #expression) \
                                            + " | " + (detail));                  \
    }                                                                             \
  } while (false)

#define PF_REQUIRE(expression)                                                    \
  do {                                                                            \
    if (!(expression)) {                                                          \
      ::podfabric::test::fail_now(__FILE__, __LINE__,                             \
                                  "requirement failed: " #expression);            \
    }                                                                             \
  } while (false)

#define PF_REQUIRE_MSG(expression, detail)                                          do {                                                                                if (!(expression)) {                                                                ::podfabric::test::fail_now(__FILE__, __LINE__,                                                               std::string("requirement failed: " #expression)                                       + " | " + (detail));                            }                                                                               } while (false)

#define PF_CHECK_EQ(left, right)                                                  \
  do {                                                                            \
    const auto& pf_left = (left);                                                 \
    const auto& pf_right = (right);                                               \
    if (!(pf_left == pf_right)) {                                                 \
      ::podfabric::test::record_failure(__FILE__, __LINE__,                       \
                                        "check failed: " #left " == " #right);    \
    }                                                                             \
  } while (false)
