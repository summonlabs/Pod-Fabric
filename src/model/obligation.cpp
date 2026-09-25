// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "podfabric/model/obligation.hpp"

namespace podfabric {

std::string_view to_string(ObligationKind k) noexcept {
  switch (k) {
    case ObligationKind::ProtectedPath: return "protected-path";
    case ObligationKind::CapacityFloor: return "capacity-floor";
    case ObligationKind::DomainDiversity: return "domain-diversity";
    case ObligationKind::MaintenanceWindow: return "maintenance-window";
  }
  return "capacity-floor";
}

}  // namespace podfabric
