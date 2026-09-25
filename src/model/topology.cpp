// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "podfabric/model/topology.hpp"

namespace podfabric {

std::string_view to_string(RouteState s) noexcept {
  switch (s) {
    case RouteState::Installed: return "installed";
    case RouteState::Pending: return "pending";
    case RouteState::Withdrawn: return "withdrawn";
    case RouteState::Failed: return "failed";
    case RouteState::Unknown: return "unknown";
  }
  return "unknown";
}

std::string_view to_string(DomainKind k) noexcept {
  switch (k) {
    case DomainKind::Rack: return "rack";
    case DomainKind::Power: return "power";
    case DomainKind::SwitchPlane: return "switch-plane";
    case DomainKind::Row: return "row";
    case DomainKind::Site: return "site";
    case DomainKind::Unknown: return "unknown";
  }
  return "unknown";
}

}  // namespace podfabric
