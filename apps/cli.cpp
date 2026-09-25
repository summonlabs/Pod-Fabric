// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "cli.hpp"

#include <cstdio>
#include <cstdlib>

namespace podfabric::cli {
namespace {

Result<std::uint64_t> to_unsigned(const std::string& text, const std::string& what) {
  if (text.empty() || text.size() > 20) {
    return Status(Code::Invalid, what + " is not an unsigned integer");
  }
  std::uint64_t value = 0;
  for (char c : text) {
    if (c < '0' || c > '9') {
      return Status(Code::Invalid, what + " is not an unsigned integer");
    }
    value = value * 10ull + static_cast<std::uint64_t>(c - '0');
  }
  return value;
}

}  // namespace

Result<Options> parse(int argc, char** argv, const std::set<std::string>& value_flags,
                      const std::set<std::string>& bool_flags, std::size_t max_positional) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string token = argv[i] == nullptr ? std::string() : std::string(argv[i]);
    if (token.empty()) {
      return Status(Code::Invalid, "an empty argument was supplied");
    }
    if (token.rfind("--", 0) != 0) {
      if (options.positional.size() >= max_positional) {
        return Status(Code::Invalid, "too many positional arguments");
      }
      options.positional.push_back(token);
      continue;
    }
    const std::size_t equals = token.find('=');
    const std::string name = equals == std::string::npos ? token : token.substr(0, equals);
    const std::string inline_value =
        equals == std::string::npos ? std::string() : token.substr(equals + 1);
    if (bool_flags.find(name) != bool_flags.end()) {
      if (equals != std::string::npos) {
        return Status(Code::Invalid, name + " does not take a value");
      }
      options.flags.insert(name);
      continue;
    }
    if (value_flags.find(name) == value_flags.end()) {
      return Status(Code::Invalid, "unrecognised option " + name);
    }
    if (equals != std::string::npos) {
      options.values[name].push_back(inline_value);
      continue;
    }
    if (i + 1 >= argc || argv[i + 1] == nullptr) {
      return Status(Code::Invalid, name + " requires a value");
    }
    options.values[name].push_back(std::string(argv[++i]));
  }
  return options;
}

bool has_flag(const Options& options, const std::string& name) {
  return options.flags.find(name) != options.flags.end();
}

std::string value_or(const Options& options, const std::string& name,
                     const std::string& fallback) {
  const auto it = options.values.find(name);
  if (it == options.values.end() || it->second.empty()) {
    return fallback;
  }
  return it->second.back();
}

Result<std::uint64_t> unsigned_or(const Options& options, const std::string& name,
                                  std::uint64_t fallback) {
  const auto it = options.values.find(name);
  if (it == options.values.end() || it->second.empty()) {
    return fallback;
  }
  return to_unsigned(it->second.back(), name);
}

Result<std::int64_t> signed_or(const Options& options, const std::string& name,
                               std::int64_t fallback) {
  const auto it = options.values.find(name);
  if (it == options.values.end() || it->second.empty()) {
    return fallback;
  }
  std::string text = it->second.back();
  bool negative = false;
  if (!text.empty() && (text.front() == '-' || text.front() == '+')) {
    negative = text.front() == '-';
    text.erase(text.begin());
  }
  PODFABRIC_TRY_ASSIGN(const std::uint64_t magnitude, to_unsigned(text, name));
  if (magnitude > 0x7FFFFFFFFFFFFFFFull) {
    return Status(Code::Invalid, name + " is out of range");
  }
  const auto value = static_cast<std::int64_t>(magnitude);
  return negative ? -value : value;
}

Result<std::string> required(const Options& options, const std::string& name) {
  const std::string value = value_or(options, name, {});
  if (value.empty()) {
    return Status(Code::Invalid, name + " is required");
  }
  return value;
}

void print_usage(const char* program, const char* summary, const std::vector<std::string>& lines) {
  std::printf("%s - %s\n\nUsage:\n", program, summary);
  for (const std::string& line : lines) {
    std::printf("  %s\n", line.c_str());
  }
  std::printf("\n");
}

}  // namespace podfabric::cli
