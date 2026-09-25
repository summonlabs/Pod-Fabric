// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "test.hpp"

#include <cstdio>
#include <exception>

namespace podfabric::test {
namespace {

Case* g_current = nullptr;
int g_case_failures = 0;

}  // namespace

Registry& Registry::instance() {
  static Registry registry;
  return registry;
}

void Registry::add(const char* suite, const char* name, void (*body)()) {
  cases_.push_back(Case{suite, name, body});
}

void record_failure(const char* file, int line, std::string message) {
  ++g_case_failures;
  std::printf("    FAIL %s:%d %s\n", file, line, message.c_str());
  std::fflush(stdout);
}

void fail_now(const char* file, int line, std::string message) {
  ++g_case_failures;
  std::printf("    ABORT %s:%d %s\n", file, line, message.c_str());
  std::fflush(stdout);
  throw TestFailure{std::move(message)};
}

std::string describe_bytes(std::size_t count) {
  return std::to_string(count) + " bytes";
}

int run_all(const std::string& suite_filter, bool list_only) {
  auto& registry = Registry::instance();
  if (list_only) {
    for (const Case& entry : registry.cases()) {
      std::printf("%s.%s\n", entry.suite, entry.name);
    }
    return 0;
  }
  std::size_t executed = 0;
  int failed_cases = 0;
  for (const Case& entry : registry.cases()) {
    if (!suite_filter.empty() && suite_filter != entry.suite) {
      continue;
    }
    g_current = const_cast<Case*>(&entry);
    g_case_failures = 0;
    std::printf("[ RUN  ] %s.%s\n", entry.suite, entry.name);
    std::fflush(stdout);
    try {
      entry.body();
    } catch (const TestFailure&) {
      // Already reported.
    } catch (const std::exception& error) {
      record_failure(__FILE__, __LINE__,
                     std::string("unhandled exception: ") + error.what());
    } catch (...) {
      record_failure(__FILE__, __LINE__, "unhandled non-standard exception");
    }
    ++executed;
    if (g_case_failures == 0) {
      std::printf("[  OK  ] %s.%s\n", entry.suite, entry.name);
    } else {
      ++failed_cases;
      std::printf("[ FAIL ] %s.%s (%d failures)\n", entry.suite, entry.name, g_case_failures);
    }
    std::fflush(stdout);
  }
  (void)g_current;
  std::printf("\n%s: %zu case(s) executed, %d failed\n",
              suite_filter.empty() ? "all suites" : suite_filter.c_str(), executed,
              failed_cases);
  if (executed == 0) {
    std::printf("no cases matched the filter\n");
    return 1;
  }
  return failed_cases;
}

}  // namespace podfabric::test
