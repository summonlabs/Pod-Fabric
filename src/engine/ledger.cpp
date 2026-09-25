// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "podfabric/engine/ledger.hpp"

#include <algorithm>

#include "podfabric/core/checked.hpp"

namespace podfabric {

int capacity_severity(CapacityStatus status) noexcept {
  switch (status) {
    case CapacityStatus::Ok: return 0;
    case CapacityStatus::Deduplicated: return 1;
    case CapacityStatus::Incomplete: return 2;
    case CapacityStatus::Indeterminate: return 3;
    case CapacityStatus::Conflicting: return 4;
    case CapacityStatus::Exhausted: return 5;
    case CapacityStatus::Unsupported: return 6;
  }
  return 3;
}

CapacityStatus worst_capacity_status(CapacityStatus a, CapacityStatus b) noexcept {
  return capacity_severity(a) >= capacity_severity(b) ? a : b;
}

Status CapacityLedger::validate_claim(const LedgerClaim& claim) const {
  if (claim.resource.is_nil()) {
    return Status(Code::Invalid, "capacity claim has no resource class");
  }
  if (claim.exclusivity.is_nil()) {
    return Status(Code::Invalid, "capacity claim has no exclusivity key");
  }
  if (claim.domain.is_nil()) {
    return Status(Code::Invalid, "capacity claim has no failure-domain attribution");
  }
  if (claim.amount > policy_.max_capacity_amount) {
    return Status(Code::Invalid, "capacity claim amount exceeds the policy bound");
  }
  if (claim.reserved > claim.amount) {
    return Status(Code::Invalid, "capacity claim reserves more than it offers");
  }
  return Status::success();
}

Status CapacityLedger::add(const LedgerClaim& claim) {
  return record(claim, Reason{}, true);
}

Status CapacityLedger::add_suppressed(const LedgerClaim& claim, Reason reason) {
  return record(claim, reason, false);
}

Status CapacityLedger::record(const LedgerClaim& claim, const Reason& reason, bool counted) {
  PODFABRIC_TRY(validate_claim(claim));
  auto& slot = slots_[std::make_pair(claim.resource, claim.exclusivity)];
  slot.resource = claim.resource;
  slot.exclusivity = claim.exclusivity;
  if (counted) {
    slot.usable.push_back(claim);
  } else {
    CapacityContribution contribution;
    contribution.owner = claim.owner;
    contribution.via_link = claim.via_link;
    contribution.exclusivity = claim.exclusivity;
    contribution.amount = claim.amount;
    contribution.reserved = claim.reserved;
    contribution.domain = claim.domain;
    contribution.provenance = claim.provenance;
    contribution.counted = false;
    contribution.not_counted_reason = reason;
    slot.uncounted.push_back(contribution);
    slot.notes.push_back(reason);
  }
  return Status::success();
}

Status CapacityLedger::freeze() {
  by_resource_.clear();
  by_domain_.clear();
  reasons_.clear();

  std::map<ResourceClass, CapacityAggregate> resource_aggregates;
  std::map<std::pair<ResourceClass, DomainId>, CapacityAggregate> domain_aggregates;
  std::map<ResourceClass, std::uint64_t> running_total;
  std::map<ResourceClass, std::uint64_t> running_reserved;

  for (auto& entry : slots_) {
    Slot& slot = entry.second;
    CapacityAggregate& pod_aggregate = resource_aggregates[slot.resource];
    pod_aggregate.resource = slot.resource;
    for (const Reason& note : slot.notes) {
      pod_aggregate.reasons.push_back(note);
      reasons_.push_back(note);
    }
    for (const auto& contribution : slot.uncounted) {
      pod_aggregate.contributions.push_back(contribution);
      pod_aggregate.status = worst_capacity_status(pod_aggregate.status, CapacityStatus::Incomplete);
      if (contribution.not_counted_reason.code == ReasonCode::CapacityConflict) {
        pod_aggregate.status = worst_capacity_status(pod_aggregate.status, CapacityStatus::Conflicting);
      }
      if (contribution.not_counted_reason.code == ReasonCode::CapacityDeduplicated) {
        pod_aggregate.status = worst_capacity_status(pod_aggregate.status, CapacityStatus::Deduplicated);
      }
    }

    if (slot.usable.empty()) {
      pod_aggregate.status = worst_capacity_status(pod_aggregate.status, CapacityStatus::Incomplete);
      continue;
    }

    // Canonical order so the "winner" of a shared resource is deterministic.
    std::sort(slot.usable.begin(), slot.usable.end(), [](const LedgerClaim& a, const LedgerClaim& b) {
      if (a.amount != b.amount) return a.amount < b.amount;
      if (a.reserved != b.reserved) return a.reserved < b.reserved;
      if (!(a.owner == b.owner)) return a.owner < b.owner;
      if (!(a.domain == b.domain)) return a.domain < b.domain;
      return false;
    });

    const LedgerClaim& primary = slot.usable.front();
    std::uint64_t amount = primary.amount;
    std::uint64_t reserved = std::min(primary.reserved, amount);
    bool conflict = false;
    for (std::size_t i = 1; i < slot.usable.size(); ++i) {
      const LedgerClaim& other = slot.usable[i];
      if (other.amount != primary.amount || other.reserved != primary.reserved ||
          !(other.domain == primary.domain)) {
        conflict = true;
      }
      amount = std::min(amount, other.amount);
      reserved = std::max(reserved, std::min(other.reserved, amount));
    }
    if (slot.usable.size() > 1 && !conflict) {
      pod_aggregate.status = worst_capacity_status(pod_aggregate.status, CapacityStatus::Deduplicated);
    }

    CapacityContribution winner;
    winner.owner = primary.owner;
    winner.via_link = primary.via_link;
    winner.exclusivity = primary.exclusivity;
    winner.amount = amount;
    winner.reserved = reserved;
    winner.domain = primary.domain;
    winner.provenance = primary.provenance;
    winner.counted = true;
    pod_aggregate.contributions.push_back(winner);

    for (std::size_t i = 1; i < slot.usable.size(); ++i) {
      CapacityContribution dropped;
      dropped.owner = slot.usable[i].owner;
      dropped.via_link = slot.usable[i].via_link;
      dropped.exclusivity = slot.usable[i].exclusivity;
      dropped.amount = slot.usable[i].amount;
      dropped.reserved = slot.usable[i].reserved;
      dropped.domain = slot.usable[i].domain;
      dropped.provenance = slot.usable[i].provenance;
      dropped.counted = false;
      dropped.not_counted_reason =
          conflict ? Reason{ReasonCode::CapacityConflict,
                            "the same physical resource is reported with different magnitudes"}
                   : Reason{ReasonCode::CapacityDeduplicated,
                            "the same physical resource is reported by more than one owner"};
      pod_aggregate.contributions.push_back(dropped);
      pod_aggregate.reasons.push_back(dropped.not_counted_reason);
      reasons_.push_back(dropped.not_counted_reason);
    }
    if (conflict) {
      pod_aggregate.status = worst_capacity_status(pod_aggregate.status, CapacityStatus::Conflicting);
    }

    auto& total = running_total[slot.resource];
    auto& committed = running_reserved[slot.resource];
    const auto added = checked_add<std::uint64_t>(total, amount);
    if (!added.ok()) {
      pod_aggregate.status = worst_capacity_status(pod_aggregate.status, CapacityStatus::Indeterminate);
      pod_aggregate.reasons.push_back(
          Reason{ReasonCode::PolicyBoundExceeded, "aggregate capacity overflowed the counter"});
      reasons_.push_back(pod_aggregate.reasons.back());
    } else {
      total = added.value();
    }
    const auto reserved_sum = checked_add<std::uint64_t>(committed, reserved);
    if (!reserved_sum.ok()) {
      pod_aggregate.status = worst_capacity_status(pod_aggregate.status, CapacityStatus::Indeterminate);
    } else {
      committed = reserved_sum.value();
    }
    if (total > policy_.max_capacity_total) {
      pod_aggregate.status = worst_capacity_status(pod_aggregate.status, CapacityStatus::Indeterminate);
    }

    // Per-domain aggregates are breakdowns of exactly the counted units
    // attributed to that domain, so the sum over domains equals the pod total.
    auto& domain_aggregate = domain_aggregates[std::make_pair(slot.resource, primary.domain)];
    domain_aggregate.resource = slot.resource;
    domain_aggregate.domain = primary.domain;
    domain_aggregate.contributions.push_back(winner);
    for (std::size_t i = 1; i < slot.usable.size(); ++i) {
      CapacityContribution dropped;
      dropped.owner = slot.usable[i].owner;
      dropped.via_link = slot.usable[i].via_link;
      dropped.exclusivity = slot.usable[i].exclusivity;
      dropped.amount = slot.usable[i].amount;
      dropped.reserved = slot.usable[i].reserved;
      dropped.domain = slot.usable[i].domain;
      dropped.provenance = slot.usable[i].provenance;
      dropped.counted = false;
      dropped.not_counted_reason =
          conflict ? Reason{ReasonCode::CapacityConflict,
                            "the same physical resource is reported with different magnitudes"}
                   : Reason{ReasonCode::CapacityDeduplicated,
                            "the same physical resource is reported by more than one owner"};
      domain_aggregate.contributions.push_back(dropped);
    }
    domain_aggregate.status = pod_aggregate.status;
    domain_aggregate.reasons = pod_aggregate.reasons;
  }

  for (auto& entry : resource_aggregates) {
    CapacityAggregate& aggregate = entry.second;
    const ResourceClass& resource = entry.first;
    aggregate.total = running_total[resource];
    aggregate.reserved = running_reserved[resource];
    if (aggregate.reserved > aggregate.total) {
      aggregate.headroom = 0;
      aggregate.status = worst_capacity_status(aggregate.status, CapacityStatus::Exhausted);
      aggregate.reasons.push_back(
          Reason{ReasonCode::CapacityOvercommit, "reserved capacity exceeds the offered total"});
      reasons_.push_back(aggregate.reasons.back());
    } else {
      aggregate.headroom = aggregate.total - aggregate.reserved;
    }
    canonicalise(aggregate.reasons);
    std::sort(aggregate.contributions.begin(), aggregate.contributions.end());
    by_resource_.push_back(std::move(aggregate));
  }

  for (auto& entry : domain_aggregates) {
    CapacityAggregate& aggregate = entry.second;
    std::uint64_t total = 0;
    std::uint64_t reserved = 0;
    for (const auto& contribution : aggregate.contributions) {
      if (!contribution.counted) continue;
      const auto added = checked_add<std::uint64_t>(total, contribution.amount);
      const auto committed = checked_add<std::uint64_t>(reserved, contribution.reserved);
      if (!added.ok() || !committed.ok()) {
        aggregate.status = worst_capacity_status(aggregate.status, CapacityStatus::Indeterminate);
        break;
      }
      total = added.value();
      reserved = committed.value();
    }
    aggregate.total = total;
    aggregate.reserved = reserved;
    aggregate.headroom = reserved > total ? 0 : total - reserved;
    if (reserved > total) {
      aggregate.status = worst_capacity_status(aggregate.status, CapacityStatus::Exhausted);
    }
    canonicalise(aggregate.reasons);
    std::sort(aggregate.contributions.begin(), aggregate.contributions.end());
    by_domain_.push_back(std::move(aggregate));
  }

  canonicalise(reasons_);
  std::sort(by_resource_.begin(), by_resource_.end(),
            [](const CapacityAggregate& a, const CapacityAggregate& b) {
              return a.resource < b.resource;
            });
  std::sort(by_domain_.begin(), by_domain_.end(),
            [](const CapacityAggregate& a, const CapacityAggregate& b) {
              if (!(a.resource == b.resource)) return a.resource < b.resource;
              return a.domain < b.domain;
            });
  return Status::success();
}

}  // namespace podfabric
