// Capacity claims and aggregate accounts.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "podfabric/core/status.hpp"
#include "podfabric/core/token.hpp"
#include "podfabric/model/primitives.hpp"

namespace podfabric {

// A capacity claim as reported by an owning subsystem.
//
// The exclusivity key names the physical resource that backs the number. Two
// claims that name the same physical resource may never both be summed: that is
// the mechanism by which pod-level aggregation cannot double-count. A claim
// that is genuinely partitionable must use a distinct key per partition.
struct CapacityClaim {
  ResourceClass resource{};
  ExclusivityKey exclusivity{};
  // Absolute unit count, in units implied by the resource class.
  std::uint64_t amount{0};
  // Portion of the amount already committed to protected obligations.
  std::uint64_t reserved{0};
  // Failure domain the capacity is attributed to. Must be a domain declared by
  // the claiming object; a mismatch is CONFLICTING.
  DomainId domain{};
  Provenance provenance{};

  friend bool operator==(const CapacityClaim&, const CapacityClaim&) noexcept = default;
  friend auto operator<=>(const CapacityClaim&, const CapacityClaim&) noexcept = default;
};

enum class CapacityStatus : std::uint8_t {
  Ok = 0,
  Deduplicated,
  Conflicting,
  Incomplete,
  Indeterminate,
  Exhausted,
  Unsupported,
};
std::string_view to_string(CapacityStatus s) noexcept;

// One counted unit in the aggregate, with the owner that contributed it. Kept
// in the output so that "no double counting" is inspectable rather than
// asserted.
struct CapacityContribution {
  RackId owner{};
  LinkId via_link{};
  ExclusivityKey exclusivity{};
  std::uint64_t amount{0};
  std::uint64_t reserved{0};
  DomainId domain{};
  Provenance provenance{};
  bool counted{true};
  Reason not_counted_reason{};

  friend bool operator==(const CapacityContribution&, const CapacityContribution&) noexcept = default;
  friend auto operator<=>(const CapacityContribution&, const CapacityContribution&) noexcept = default;
};

struct CapacityAggregate {
  ResourceClass resource{};
  DomainId domain{};  // nil means "whole pod"
  std::uint64_t total{0};
  std::uint64_t reserved{0};
  std::uint64_t headroom{0};
  CapacityStatus status{CapacityStatus::Ok};
  std::vector<CapacityContribution> contributions{};
  std::vector<Reason> reasons{};

  friend bool operator==(const CapacityAggregate&, const CapacityAggregate&) noexcept = default;
};

// A requirement expressed by an obligation.
struct CapacityRequirement {
  ResourceClass resource{};
  std::uint64_t minimum{0};
  // Additional margin, as a percentage of the minimum, that must remain free
  // for the obligation to be considered comfortably satisfied.
  std::uint32_t margin_percent{0};

  friend bool operator==(const CapacityRequirement&, const CapacityRequirement&) noexcept = default;
};

}  // namespace podfabric
