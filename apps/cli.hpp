// Shared argument parsing for the command-line tools.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "podfabric/core/status.hpp"

namespace podfabric::cli {

struct Options {
  std::vector<std::string> positional{};
  std::map<std::string, std::vector<std::string>> values{};
  std::set<std::string> flags{};
};

// Parses "--name value", "--name=value" and "--flag". Unknown options are
// REFUSED rather than ignored so a typo cannot silently change behaviour.
Result<Options> parse(int argc, char** argv, const std::set<std::string>& value_flags,
                      const std::set<std::string>& bool_flags, std::size_t max_positional);

bool has_flag(const Options& options, const std::string& name);
std::string value_or(const Options& options, const std::string& name, const std::string& fallback);
Result<std::uint64_t> unsigned_or(const Options& options, const std::string& name,
                                  std::uint64_t fallback);
Result<std::int64_t> signed_or(const Options& options, const std::string& name,
                               std::int64_t fallback);
Result<std::string> required(const Options& options, const std::string& name);

void print_usage(const char* program, const char* summary, const std::vector<std::string>& lines);

}  // namespace podfabric::cli
