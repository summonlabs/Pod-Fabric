// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdio>
#include <cstring>
#include <string>

#include "test.hpp"

int main(int argc, char** argv) {
  std::string suite;
  bool list_only = false;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i] == nullptr ? std::string() : std::string(argv[i]);
    if (argument == "--list") {
      list_only = true;
      continue;
    }
    if (argument.rfind("--suite=", 0) == 0) {
      suite = argument.substr(8);
      continue;
    }
    if (argument == "--suite" && i + 1 < argc && argv[i + 1] != nullptr) {
      suite = argv[++i];
      continue;
    }
    std::fprintf(stderr, "podfabric_tests: unrecognised argument '%s'\n", argument.c_str());
    return 2;
  }
  return podfabric::test::run_all(suite, list_only);
}
