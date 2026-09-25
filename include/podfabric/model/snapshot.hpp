// The composition input document.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "podfabric/core/digest.hpp"
#include "podfabric/core/status.hpp"
#include "podfabric/core/time.hpp"
#include "podfabric/core/token.hpp"
#include "podfabric/model/member.hpp"
#include "podfabric/model/obligation.hpp"
#include "podfabric/model/pod_state.hpp"
#include "podfabric/model/policy.hpp"
#include "podfabric/model/primitives.hpp"
#include "podfabric/model/topology.hpp"

namespace podfabric {

// Evidence supplied by the surrounding system for one composition round.
//
// This is raw material, not authority. Nothing in here is trusted because it
// arrived; it is trusted because it matches the generation the pod recorded.
struct PodSnapshot {
  PodId pod{};
  std::string schema{};
  std::vector<MemberRecord> members{};
  std::vector<LinkRecord> links{};
  std::vector<RouteEvidence> routes{};
  std::vector<FailureDomainRecord> domains{};
  std::vector<Obligation> obligations{};
  Provenance provenance{};
  Nanos observed_at{0};

  friend bool operator==(const PodSnapshot&, const PodSnapshot&) noexcept = default;
};

// What the pod already believes. Supplied by the controller from durable state.
struct CompositionContext {
  PodEpoch epoch{};
  Incarnation incarnation{};
  std::vector<MemberExpectation> expectations{};
  // Tokens the pod has granted and not revoked. Presented tokens are validated
  // against this set, the current epoch and the current incarnation.
  std::vector<AuthorityToken> granted_tokens{};
  std::vector<LeaseId> revoked_tokens{};
  // The coordinator's own clock reading. Evidence freshness is judged against
  // the snapshot's observation time, but authority leases are judged against
  // this value, because a lease was minted by the coordinator's clock and not
  // by the clock of any rack. Zero falls back to the snapshot time.
  Nanos now{0};
  // True when the context was rebuilt from durable state after a restart. All
  // recovered evidence is historical: it can fence, it cannot establish.
  bool recovered_from_store{false};
  Nanos recovered_at{0};
  // Set when recovery could not determine whether the last durable commit
  // landed. Session decisions that depend on it are INDETERMINATE.
  bool ambiguous_commit{false};
  // Lifecycle the controller currently holds. Composition proposes the next
  // lifecycle from this value so that a partition heals through Recovering
  // instead of jumping straight back to Active.
  LifecycleState lifecycle{LifecycleState::Constructing};

  friend bool operator==(const CompositionContext&, const CompositionContext&) noexcept = default;
};

struct CompositionRequest {
  PodSnapshot snapshot{};
  CompositionContext context{};
  Policy policy{};
};

// Validation of a snapshot's internal well-formedness, independent of any
// recorded expectation. Bounds are enforced here so that later stages can
// assume a bounded, identity-unique document.
Status validate(const PodSnapshot& snapshot, const Policy& policy);

// Canonical form: members/links/routes/domains/obligations sorted by identity.
void canonicalise(PodSnapshot& snapshot);

}  // namespace podfabric
