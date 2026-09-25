// Deterministic synthetic evidence for examples, tests and benchmarks.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "podfabric/model/snapshot.hpp"

namespace podfabric {

// Generates a pod snapshot from a seed. Everything it produces is marked
// SYNTHETIC: it describes no real hardware and must never be presented as if
// it did.
struct SyntheticOptions {
  PodId pod{};
  std::string id_prefix{"rack"};
  std::uint32_t racks{4};
  std::uint32_t power_domains{2};
  std::uint64_t seed{1};
  RackGeneration generation{};
  std::uint64_t capacity_per_rack{100};
  std::uint64_t reserved_per_rack{10};
  ResourceClass resource{};
  // When false only a ring of inter-rack links is produced.
  bool full_mesh{true};
  Nanos observed_at{0};
  HealthState health{HealthState::Ok};
  MembershipState membership{MembershipState::Established};
  EvidenceClass klass{EvidenceClass::Observed};
  // Every Nth rack is left out of the snapshot, which exercises the STALE and
  // MISSING member paths.
  std::uint32_t omit_every{0};
  bool with_obligations{true};
};

PodSnapshot synthesize(const SyntheticOptions& options);

// The expectations a pod would record after accepting the whole snapshot.
std::vector<MemberExpectation> expectations_for(const PodSnapshot& snapshot);

}  // namespace podfabric
