// Protected obligations that pod authority must not silently break.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "podfabric/core/token.hpp"
#include "podfabric/model/capacity.hpp"
#include "podfabric/model/primitives.hpp"
#include "podfabric/model/topology.hpp"

namespace podfabric {

enum class ObligationKind : std::uint8_t {
  ProtectedPath = 0,     // named racks must stay mutually reachable
  CapacityFloor = 1,     // a resource floor must remain available
  DomainDiversity = 2,   // N rack-disjoint failure domains must remain serviceable
  MaintenanceWindow = 3, // a maintenance window must remain admissible
};
std::string_view to_string(ObligationKind k) noexcept;

struct Obligation {
  ObligationId id{};
  ObligationKind kind{ObligationKind::CapacityFloor};
  TenantId tenant{};
  std::string description{};
  // Racks the obligation speaks about. Empty means "the whole pod".
  std::vector<RackId> protected_racks{};
  // For DomainDiversity: how many mutually rack-disjoint domains must be
  // serviceable. Overlapping domains count once, so diversity can never be
  // inflated by overlapping domain definitions.
  std::uint32_t required_domains{0};
  DomainKind diversity_kind{DomainKind::Power};
  std::vector<CapacityRequirement> requirements{};
  // A mandatory obligation that is violated forces the pod out of Active.
  bool mandatory{true};
  Provenance provenance{};

  friend bool operator==(const Obligation&, const Obligation&) noexcept = default;
};

}  // namespace podfabric
