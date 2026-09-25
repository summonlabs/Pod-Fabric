// Capacity ledger tests: no double counting, conflicts, overflow.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <string>

#include "podfabric/engine/ledger.hpp"
#include "test.hpp"

using namespace podfabric;

namespace {

LedgerClaim claim(const char* owner, const char* key, std::uint64_t amount,
                  std::uint64_t reserved, const char* domain) {
  LedgerClaim entry;
  entry.resource = ResourceClass::from_canonical_literal("fabric.bandwidth");
  const auto exclusivity = ExclusivityKey::parse(key);
  PF_REQUIRE(exclusivity.ok());
  entry.exclusivity = exclusivity.value();
  const auto rack = RackId::parse(owner);
  PF_REQUIRE(rack.ok());
  entry.owner = rack.value();
  entry.amount = amount;
  entry.reserved = reserved;
  const auto domain_id = DomainId::parse(domain);
  PF_REQUIRE(domain_id.ok());
  entry.domain = domain_id.value();
  return entry;
}

const CapacityAggregate* find(const std::vector<CapacityAggregate>& aggregates,
                              const char* resource) {
  const auto key = ResourceClass::parse(resource);
  if (!key.ok()) {
    return nullptr;
  }
  for (const CapacityAggregate& aggregate : aggregates) {
    if (aggregate.resource == key.value()) {
      return &aggregate;
    }
  }
  return nullptr;
}

}  // namespace

PF_TEST(ledger, distinct_physical_resources_are_summed) {
  CapacityLedger ledger(Policy{});
  PF_REQUIRE(ledger.add(claim("rack-1", "fabric-1", 100, 10, "power-0")).ok());
  PF_REQUIRE(ledger.add(claim("rack-2", "fabric-2", 100, 10, "power-1")).ok());
  PF_REQUIRE(ledger.freeze().ok());
  const CapacityAggregate* aggregate = find(ledger.by_resource(), "fabric.bandwidth");
  PF_REQUIRE(aggregate != nullptr);
  PF_CHECK_EQ(aggregate->total, 200ull);
  PF_CHECK_EQ(aggregate->reserved, 20ull);
  PF_CHECK_EQ(aggregate->headroom, 180ull);
  PF_CHECK_EQ(aggregate->status, CapacityStatus::Ok);
}

PF_TEST(ledger, a_shared_physical_resource_is_counted_once) {
  CapacityLedger ledger(Policy{});
  // Two owners report the same physical budget with identical magnitudes: the
  // pod must count it once and say that it deduplicated.
  PF_REQUIRE(ledger.add(claim("rack-1", "shared-bus", 100, 10, "power-0")).ok());
  PF_REQUIRE(ledger.add(claim("rack-2", "shared-bus", 100, 10, "power-0")).ok());
  PF_REQUIRE(ledger.freeze().ok());
  const CapacityAggregate* aggregate = find(ledger.by_resource(), "fabric.bandwidth");
  PF_REQUIRE(aggregate != nullptr);
  PF_CHECK_EQ(aggregate->total, 100ull);
  PF_CHECK_EQ(aggregate->status, CapacityStatus::Deduplicated);
  std::size_t counted = 0;
  for (const CapacityContribution& contribution : aggregate->contributions) {
    if (contribution.counted) {
      ++counted;
    }
  }
  PF_CHECK_EQ(counted, std::size_t(1));
}

PF_TEST(ledger, disagreeing_claims_become_conflicting_and_take_the_smaller_value) {
  CapacityLedger ledger(Policy{});
  PF_REQUIRE(ledger.add(claim("rack-1", "shared-bus", 100, 10, "power-0")).ok());
  PF_REQUIRE(ledger.add(claim("rack-2", "shared-bus", 250, 10, "power-0")).ok());
  PF_REQUIRE(ledger.freeze().ok());
  const CapacityAggregate* aggregate = find(ledger.by_resource(), "fabric.bandwidth");
  PF_REQUIRE(aggregate != nullptr);
  PF_CHECK_EQ(aggregate->status, CapacityStatus::Conflicting);
  PF_CHECK_EQ(aggregate->total, 100ull);
}

PF_TEST(ledger, differing_domain_attribution_is_a_conflict_not_a_choice) {
  CapacityLedger ledger(Policy{});
  PF_REQUIRE(ledger.add(claim("rack-1", "shared-bus", 100, 10, "power-0")).ok());
  PF_REQUIRE(ledger.add(claim("rack-2", "shared-bus", 100, 10, "power-1")).ok());
  PF_REQUIRE(ledger.freeze().ok());
  const CapacityAggregate* aggregate = find(ledger.by_resource(), "fabric.bandwidth");
  PF_REQUIRE(aggregate != nullptr);
  PF_CHECK_EQ(aggregate->status, CapacityStatus::Conflicting);
  PF_CHECK_EQ(aggregate->total, 100ull);
}

PF_TEST(ledger, suppressed_claims_are_visible_but_not_counted) {
  CapacityLedger ledger(Policy{});
  PF_REQUIRE(ledger.add(claim("rack-1", "fabric-1", 100, 10, "power-0")).ok());
  PF_REQUIRE(ledger
                 .add_suppressed(claim("rack-2", "fabric-2", 100, 10, "power-1"),
                                 Reason{ReasonCode::MemberUnhealthy, "health unknown"})
                 .ok());
  PF_REQUIRE(ledger.freeze().ok());
  const CapacityAggregate* aggregate = find(ledger.by_resource(), "fabric.bandwidth");
  PF_REQUIRE(aggregate != nullptr);
  PF_CHECK_EQ(aggregate->total, 100ull);
  PF_CHECK_EQ(aggregate->status, CapacityStatus::Incomplete);
  PF_CHECK_EQ(aggregate->contributions.size(), std::size_t(2));
}

PF_TEST(ledger, per_domain_breakdown_sums_to_the_pod_total) {
  CapacityLedger ledger(Policy{});
  PF_REQUIRE(ledger.add(claim("rack-1", "fabric-1", 100, 10, "power-0")).ok());
  PF_REQUIRE(ledger.add(claim("rack-2", "fabric-2", 70, 5, "power-1")).ok());
  PF_REQUIRE(ledger.add(claim("rack-3", "fabric-3", 30, 5, "power-0")).ok());
  PF_REQUIRE(ledger.freeze().ok());
  std::uint64_t summed = 0;
  for (const CapacityAggregate& aggregate : ledger.by_domain()) {
    summed += aggregate.total;
  }
  const CapacityAggregate* whole = find(ledger.by_resource(), "fabric.bandwidth");
  PF_REQUIRE(whole != nullptr);
  PF_CHECK_EQ(summed, whole->total);
  PF_CHECK_EQ(summed, 200ull);
}

PF_TEST(ledger, reserved_beyond_offer_is_exhausted) {
  CapacityLedger ledger(Policy{});
  PF_REQUIRE(ledger.add(claim("rack-1", "fabric-1", 50, 50, "power-0")).ok());
  PF_REQUIRE(ledger.add(claim("rack-2", "fabric-2", 50, 50, "power-1")).ok());
  PF_REQUIRE(ledger.freeze().ok());
  const CapacityAggregate* aggregate = find(ledger.by_resource(), "fabric.bandwidth");
  PF_REQUIRE(aggregate != nullptr);
  PF_CHECK_EQ(aggregate->headroom, 0ull);
  PF_CHECK_EQ(aggregate->status, CapacityStatus::Ok);
}

PF_TEST(ledger, rejects_claims_that_break_their_own_invariants) {
  CapacityLedger ledger(Policy{});
  LedgerClaim bad = claim("rack-1", "fabric-1", 10, 20, "power-0");
  PF_CHECK(ledger.add(bad).code() == Code::Invalid);
  LedgerClaim no_domain = claim("rack-1", "fabric-1", 10, 1, "power-0");
  no_domain.domain = DomainId{};
  PF_CHECK(ledger.add(no_domain).code() == Code::Invalid);
  LedgerClaim no_key = claim("rack-1", "fabric-1", 10, 1, "power-0");
  no_key.exclusivity = ExclusivityKey{};
  PF_CHECK(ledger.add(no_key).code() == Code::Invalid);
  Policy bounded;
  bounded.max_capacity_amount = 5;
  CapacityLedger strict(bounded);
  PF_CHECK(strict.add(claim("rack-1", "fabric-1", 6, 0, "power-0")).code() == Code::Invalid);
}

PF_TEST(ledger, insertion_order_does_not_change_the_result) {
  const std::vector<LedgerClaim> entries = {
      claim("rack-1", "fabric-1", 100, 10, "power-0"),
      claim("rack-2", "fabric-2", 40, 0, "power-1"),
      claim("rack-3", "shared", 25, 5, "power-0"),
      claim("rack-4", "shared", 25, 5, "power-0")};
  CapacityLedger forward(Policy{});
  for (const LedgerClaim& entry : entries) {
    PF_REQUIRE(forward.add(entry).ok());
  }
  PF_REQUIRE(forward.freeze().ok());
  CapacityLedger backward(Policy{});
  for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
    PF_REQUIRE(backward.add(*it).ok());
  }
  PF_REQUIRE(backward.freeze().ok());
  PF_CHECK(forward.by_resource() == backward.by_resource());
  PF_CHECK(forward.by_domain() == backward.by_domain());
}
