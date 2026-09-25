// Double-count-free capacity accounting.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "podfabric/core/status.hpp"
#include "podfabric/model/capacity.hpp"
#include "podfabric/model/policy.hpp"

namespace podfabric {

// One unit of capacity offered to the pod.
//
// The exclusivity key is the physical identity of the resource. The ledger
// guarantees that a physical resource is summed at most once no matter how many
// owners report it, which is what makes pod-level aggregation double-count-free
// by construction rather than by convention.
struct LedgerClaim {
  ResourceClass resource{};
  ExclusivityKey exclusivity{};
  RackId owner{};
  LinkId via_link{};
  std::uint64_t amount{0};
  std::uint64_t reserved{0};
  DomainId domain{};
  Provenance provenance{};

  friend bool operator==(const LedgerClaim&, const LedgerClaim&) noexcept = default;
};

// Severity order for aggregate statuses. Satisfied obligations require Ok or
// Deduplicated; anything at or above Conflicting is a conflict, and Exhausted
// means the resource is over-committed.
int capacity_severity(CapacityStatus status) noexcept;
CapacityStatus worst_capacity_status(CapacityStatus a, CapacityStatus b) noexcept;

class CapacityLedger {
 public:
  explicit CapacityLedger(const Policy& policy) : policy_(policy) {}

  // Records a claim the caller has already decided is usable.
  Status add(const LedgerClaim& claim);

  // Records a claim that must not be counted, with the reason why. The claim
  // stays visible in the aggregate as an explicitly uncounted contribution, so
  // information is never silently dropped.
  Status add_suppressed(const LedgerClaim& claim, Reason reason);

  // Computes the aggregates. Order independent and idempotent.
  Status freeze();

  const std::vector<CapacityAggregate>& by_resource() const noexcept { return by_resource_; }
  const std::vector<CapacityAggregate>& by_domain() const noexcept { return by_domain_; }
  const std::vector<Reason>& ledger_reasons() const noexcept { return reasons_; }

 private:
  struct Slot {
    ResourceClass resource{};
    ExclusivityKey exclusivity{};
    std::vector<LedgerClaim> usable{};
    std::vector<CapacityContribution> uncounted{};
    std::vector<Reason> notes{};
  };

  Status record(const LedgerClaim& claim, const Reason& reason, bool counted);
  Status validate_claim(const LedgerClaim& claim) const;

  Policy policy_;
  std::map<std::pair<ResourceClass, ExclusivityKey>, Slot> slots_{};
  std::vector<Reason> reasons_{};
  std::vector<CapacityAggregate> by_resource_{};
  std::vector<CapacityAggregate> by_domain_{};
};

}  // namespace podfabric
