// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "podfabric/core/status.hpp"

namespace podfabric {

std::string_view to_string(Code code) noexcept {
  switch (code) {
    case Code::Ok: return "OK";
    case Code::Unknown: return "UNKNOWN";
    case Code::Unsupported: return "UNSUPPORTED";
    case Code::Stale: return "STALE";
    case Code::Conflicting: return "CONFLICTING";
    case Code::Incomplete: return "INCOMPLETE";
    case Code::Indeterminate: return "INDETERMINATE";
    case Code::Refused: return "REFUSED";
    case Code::Cancelled: return "CANCELLED";
    case Code::Invalid: return "INVALID";
    case Code::NotFound: return "NOT_FOUND";
    case Code::AlreadyExists: return "ALREADY_EXISTS";
    case Code::Exhausted: return "EXHAUSTED";
    case Code::Corrupt: return "CORRUPT";
    case Code::Unavailable: return "UNAVAILABLE";
    case Code::Internal: return "INTERNAL";
  }
  return "UNKNOWN";
}

bool is_ok(Code code) noexcept { return code == Code::Ok; }

int severity(Code code) noexcept {
  switch (code) {
    case Code::Ok: return 0;
    case Code::Unknown: return 1;
    case Code::NotFound: return 2;
    case Code::AlreadyExists: return 3;
    case Code::Cancelled: return 4;
    case Code::Incomplete: return 5;
    case Code::Stale: return 6;
    case Code::Unsupported: return 7;
    case Code::Indeterminate: return 8;
    case Code::Conflicting: return 9;
    case Code::Unavailable: return 10;
    case Code::Exhausted: return 11;
    case Code::Refused: return 12;
    case Code::Corrupt: return 13;
    case Code::Invalid: return 14;
    case Code::Internal: return 15;
  }
  return 15;
}

Code worst(Code a, Code b) noexcept { return severity(a) >= severity(b) ? a : b; }

std::string Status::to_string() const {
  std::string out(podfabric::to_string(code_));
  if (!message_.empty()) {
    out.append(": ");
    out.append(message_);
  }
  return out;
}

}  // namespace podfabric
