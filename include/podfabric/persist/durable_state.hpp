// The durable subset of pod state.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "podfabric/core/digest.hpp"
#include "podfabric/core/status.hpp"
#include "podfabric/core/time.hpp"
#include "podfabric/core/token.hpp"
#include "podfabric/model/member.hpp"
#include "podfabric/model/pod_state.hpp"
#include "podfabric/model/primitives.hpp"

namespace podfabric::persist {

inline constexpr std::uint16_t durable_state_version = 1;
inline constexpr std::string_view durable_state_schema = "podfabric.durable/1";

// Exactly what survives a restart. Derived verdicts do not: they are recomputed
// from fresh evidence, because a recovered verdict would be historical.
struct DurableState {
  std::uint16_t version{durable_state_version};
  PodId pod{};
  PodEpoch epoch{};
  // The controller incarnation that produced this record. Recovery compares it
  // with the live incarnation to fence every artefact of the previous run.
  Incarnation writer{};
  LifecycleState lifecycle{LifecycleState::Constructing};
  std::vector<MemberExpectation> expectations{};
  std::vector<AuthorityToken> tokens{};
  std::vector<LeaseId> revoked{};
  // Operator intent is durable: a maintenance window survives a restart.
  std::vector<std::pair<RackId, AdminState>> admin{};
  FencingSequence fencing{};
  Digest last_state_digest{};
  // Journal sequence of the record that produced this state.
  std::uint64_t sequence{0};
  Nanos written_at{0};

  friend bool operator==(const DurableState&, const DurableState&) noexcept = default;
};

// Binary encoding with an explicit revision so an older or newer record is
// UNSUPPORTED rather than misread.
Status encode(const DurableState& state, std::vector<std::byte>& out);
Result<DurableState> decode(std::span<const std::byte> bytes);
Digest digest_of(const DurableState& state);

}  // namespace podfabric::persist
