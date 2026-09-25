// Pod membership records projected from Rack Network Fabric evidence.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "podfabric/core/digest.hpp"
#include "podfabric/core/time.hpp"
#include "podfabric/core/token.hpp"
#include "podfabric/model/capacity.hpp"
#include "podfabric/model/primitives.hpp"

namespace podfabric {

// What Pod Fabric records about one member rack.
//
// Pod Fabric does not own rack-internal truth. This record is a projection of
// the Rack Network Fabric descriptor: the exact generation and content digest
// are carried so that every pod-level decision can be bound to the precise rack
// state it consumed.
struct MemberRecord {
  RackId rack{};
  RackGeneration generation{};
  Digest digest{};
  // Schema revision of the Rack Network Fabric descriptor that produced the
  // digest. An unknown revision is UNSUPPORTED, never assumed compatible.
  std::string rnf_schema{};
  // Self-declared membership of this rack in the pod. Joining, Leaving,
  // Fenced and Retired are honoured as exclusions; Established still has to
  // pass generation and provenance checks before the pod adopts it.
  MembershipState membership{MembershipState::Established};
  AdminState admin{AdminState::Enabled};
  HealthState health{HealthState::Unknown};
  std::vector<DomainId> failure_domains{};
  std::vector<CapacityClaim> capacity{};
  std::vector<PortRef> ports{};
  Provenance provenance{};
  Nanos observed_at{0};

  friend bool operator==(const MemberRecord&, const MemberRecord&) noexcept = default;
};

// The generation the pod last accepted for a rack. This is the durable
// expectation that makes staleness detectable: evidence older than the
// expectation is STALE, evidence that reuses the expectation's generation with
// different content is CONFLICTING.
struct MemberExpectation {
  RackId rack{};
  RackGeneration generation{};
  Digest digest{};
  MembershipState membership{MembershipState::Joining};

  friend bool operator==(const MemberExpectation&, const MemberExpectation&) noexcept = default;
  friend auto operator<=>(const MemberExpectation&, const MemberExpectation&) noexcept = default;
};

}  // namespace podfabric
