// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "podfabric/model/capacity.hpp"

namespace podfabric {

std::string_view to_string(CapacityStatus s) noexcept {
  switch (s) {
    case CapacityStatus::Ok: return "OK";
    case CapacityStatus::Deduplicated: return "DEDUPLICATED";
    case CapacityStatus::Conflicting: return "CONFLICTING";
    case CapacityStatus::Incomplete: return "INCOMPLETE";
    case CapacityStatus::Indeterminate: return "INDETERMINATE";
    case CapacityStatus::Exhausted: return "EXHAUSTED";
    case CapacityStatus::Unsupported: return "UNSUPPORTED";
  }
  return "INDETERMINATE";
}

}  // namespace podfabric
