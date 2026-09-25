// Seeded property tests. A failing case prints its seed.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <map>
#include <string>
#include <vector>

#include "podfabric/engine/composer.hpp"
#include "podfabric/engine/dependency.hpp"
#include "podfabric/model/synthetic.hpp"
#include "test.hpp"

using namespace podfabric;

namespace {

// An independent, deliberately naive reference model of pod-level capacity.
// It shares no code with the ledger: it groups claims by physical identity and
// sums at most one value per identity.
std::uint64_t reference_capacity(const PodSnapshot& snapshot, const char* resource_name) {
  const auto resource = ResourceClass::parse(resource_name);
  if (!resource.ok()) {
    return 0;
  }
  std::map<std::string, std::pair<std::uint64_t, std::uint64_t>> per_key;
  for (const MemberRecord& member : snapshot.members) {
    for (const CapacityClaim& claim : member.capacity) {
      if (!(claim.resource == resource.value())) {
        continue;
      }
      const std::string key = claim.exclusivity.token();
      const auto it = per_key.find(key);
      if (it == per_key.end()) {
        per_key[key] = {claim.amount, claim.reserved};
        continue;
      }
      const std::uint64_t amount = std::min(it->second.first, claim.amount);
      const std::uint64_t reserved =
          std::max(it->second.second, std::min(claim.reserved, amount));
      per_key[key] = {amount, reserved};
    }
  }
  std::uint64_t total = 0;
  for (const auto& entry : per_key) {
    total += entry.second.first;
  }
  return total;
}

CompositionRequest request_for(const PodSnapshot& snapshot) {
  CompositionRequest request;
  request.snapshot = snapshot;
  request.context.epoch = PodEpoch(1);
  request.context.expectations = expectations_for(snapshot);
  return request;
}

SyntheticOptions seeded(std::uint64_t seed, std::uint32_t racks, std::uint32_t domains) {
  SyntheticOptions options;
  options.pod = PodId::parse("pod-property").value();
  options.racks = racks;
  options.power_domains = domains;
  options.seed = seed;
  options.capacity_per_rack = 1 + (seed % 97);
  options.reserved_per_rack = seed % 11;
  options.full_mesh = (seed % 3) != 0;
  options.observed_at = 1700000000LL * nanos_per_second;
  return options;
}

}  // namespace

PF_TEST(property, composition_is_deterministic_for_every_seed) {
  for (std::uint64_t seed = 1; seed <= 40; ++seed) {
    SyntheticOptions options = seeded(seed, 2 + static_cast<std::uint32_t>(seed % 5),
                                      1 + static_cast<std::uint32_t>(seed % 3));
    PodSnapshot snapshot = synthesize(options);
    const auto first = compose(request_for(snapshot));
    PF_REQUIRE(first.ok());
    std::reverse(snapshot.members.begin(), snapshot.members.end());
    std::reverse(snapshot.links.begin(), snapshot.links.end());
    std::reverse(snapshot.domains.begin(), snapshot.domains.end());
    std::reverse(snapshot.obligations.begin(), snapshot.obligations.end());
    const auto second = compose(request_for(snapshot));
    PF_REQUIRE(second.ok());
    PF_CHECK_MSG(second.value().state_digest == first.value().state_digest,
                 "seed=" + std::to_string(seed));
    PF_CHECK_MSG(second.value() == first.value(), "seed=" + std::to_string(seed));
  }
}

PF_TEST(property, capacity_aggregate_matches_the_reference_model) {
  for (std::uint64_t seed = 1; seed <= 40; ++seed) {
    const PodSnapshot snapshot = synthesize(seeded(seed, 1 + static_cast<std::uint32_t>(seed % 6),
                                                   1 + static_cast<std::uint32_t>(seed % 4)));
    const auto state = compose(request_for(snapshot));
    PF_REQUIRE(state.ok());
    const CapacityAggregate* aggregate = state.value().find_capacity(
        ResourceClass::from_canonical_literal("fabric.bandwidth"));
    PF_REQUIRE(aggregate != nullptr);
    const std::uint64_t expected = reference_capacity(snapshot, "fabric.bandwidth");
    PF_CHECK_MSG(aggregate->total == expected,
                 "seed=" + std::to_string(seed) + " ledger=" + std::to_string(aggregate->total) +
                     " reference=" + std::to_string(expected));
  }
}

PF_TEST(property, shared_budgets_never_double_count) {
  for (std::uint64_t seed = 1; seed <= 25; ++seed) {
    PodSnapshot snapshot = synthesize(seeded(seed, 3 + static_cast<std::uint32_t>(seed % 4), 2));
    // Force every member to report the same physical budget.
    for (MemberRecord& record : snapshot.members) {
      record.capacity.front().exclusivity =
          snapshot.members.front().capacity.front().exclusivity;
      record.capacity.front().domain = snapshot.members.front().capacity.front().domain;
      record.capacity.front().amount = 10 + (seed % 7);
      record.capacity.front().reserved = seed % 5;
    }
    canonicalise(snapshot);
    const auto state = compose(request_for(snapshot));
    PF_REQUIRE(state.ok());
    const CapacityAggregate* aggregate = state.value().find_capacity(
        ResourceClass::from_canonical_literal("fabric.bandwidth"));
    PF_REQUIRE(aggregate != nullptr);
    PF_CHECK_MSG(aggregate->total == 10 + (seed % 7),
                 "seed=" + std::to_string(seed) + " total=" + std::to_string(aggregate->total));
    std::size_t counted = 0;
    for (const CapacityContribution& contribution : aggregate->contributions) {
      if (contribution.counted) {
        ++counted;
      }
    }
    PF_CHECK_MSG(counted == 1, "seed=" + std::to_string(seed));
  }
}

PF_TEST(property, per_domain_breakdown_always_sums_to_the_pod_total) {
  for (std::uint64_t seed = 1; seed <= 25; ++seed) {
    const PodSnapshot snapshot = synthesize(
        seeded(seed, 2 + static_cast<std::uint32_t>(seed % 5), 1 + static_cast<std::uint32_t>(seed % 3)));
    const auto state = compose(request_for(snapshot));
    PF_REQUIRE(state.ok());
    std::map<std::string, std::uint64_t> per_resource;
    for (const CapacityAggregate& scoped : state.value().domain_capacity) {
      per_resource[scoped.resource.token()] += scoped.total;
    }
    for (const CapacityAggregate& whole : state.value().capacity) {
      PF_CHECK_MSG(per_resource[whole.resource.token()] == whole.total,
                   "seed=" + std::to_string(seed) + " resource=" + whole.resource.token());
    }
  }
}

PF_TEST(property, selective_revalidation_is_sound_for_random_mutations) {
  for (std::uint64_t seed = 1; seed <= 25; ++seed) {
    SyntheticOptions options = seeded(seed, 4, 2);
    PodSnapshot before = synthesize(options);
    if (before.links.empty()) {
      continue;
    }
    const auto first_state = compose(request_for(before));
    PF_REQUIRE(first_state.ok());
    DecisionIndex index;
    for (const DecisionRecord& record : first_state.value().decisions) {
      index.publish(record);
    }

    PodSnapshot after = before;
    const std::uint32_t choice = static_cast<std::uint32_t>(seed % 4);
    if (choice == 0) {
      after.members[seed % after.members.size()].generation = RackGeneration(11);
      after.members[seed % after.members.size()].digest = Digest::of("mutated");
    } else if (choice == 1) {
      after.links[seed % after.links.size()].oper = OperationalState::Down;
    } else if (choice == 2) {
      after.members[seed % after.members.size()].health = HealthState::Impaired;
    } else {
      after.members[seed % after.members.size()].capacity.front().amount += 1;
    }
    canonicalise(after);

    const auto second_state = compose(request_for(after));
    PF_REQUIRE(second_state.ok());
    std::vector<DepKey> changed = snapshot_change_keys(before, after);
    const RevalidationReport report = revalidate(index, second_state.value(), changed);
    PF_CHECK_MSG(report.sound(), "seed=" + std::to_string(seed) + " " + report.describe());
  }
}

PF_TEST(property, a_rack_generation_advance_never_invalidates_an_unrelated_rack) {
  for (std::uint64_t seed = 1; seed <= 20; ++seed) {
    SyntheticOptions options = seeded(seed, 5, 2);
    const PodSnapshot before = synthesize(options);
    const auto first_state = compose(request_for(before));
    PF_REQUIRE(first_state.ok());
    DecisionIndex index;
    for (const DecisionRecord& record : first_state.value().decisions) {
      index.publish(record);
    }
    PodSnapshot after = before;
    const RackId target = before.members[seed % before.members.size()].rack;
    const RackId untouched = before.members[(seed + 3) % before.members.size()].rack;
    if (target == untouched) {
      continue;
    }
    for (MemberRecord& record : after.members) {
      if (record.rack == target) {
        record.generation = RackGeneration(12);
        record.digest = Digest::of("reincarnated");
      }
    }
    for (LinkRecord& link : after.links) {
      if (link.a.rack == target) link.a_generation = RackGeneration(12);
      if (link.b.rack == target) link.b_generation = RackGeneration(12);
    }
    canonicalise(after);
    const auto second_state = compose(request_for(after));
    PF_REQUIRE(second_state.ok());
    const std::vector<DepKey> changed = snapshot_change_keys(before, after);
    const RevalidationReport report = revalidate(index, second_state.value(), changed);
    PF_CHECK_MSG(report.sound(), "seed=" + std::to_string(seed) + " " + report.describe());
    const DecisionId untouched_decision = make_decision_id(DecisionKind::MemberState,
                                                           untouched.token());
    PF_CHECK_MSG(!report.plan.contains(untouched_decision), "seed=" + std::to_string(seed));
    const DecisionRecord* previous = index.find(untouched_decision);
    const DecisionRecord* current = second_state.value().find_decision(untouched_decision);
    PF_REQUIRE(previous != nullptr && current != nullptr);
    PF_CHECK_MSG(previous->fingerprint == current->fingerprint, "seed=" + std::to_string(seed));
  }
}

PF_TEST(property, a_raised_generation_never_resurrects_stale_authority) {
  for (std::uint64_t seed = 1; seed <= 20; ++seed) {
    SyntheticOptions options = seeded(seed, 3, 2);
    PodSnapshot snapshot = synthesize(options);
    CompositionRequest request = request_for(snapshot);
    for (MemberExpectation& expectation : request.context.expectations) {
      expectation.generation = RackGeneration(50);
    }
    const auto state = compose(request);
    PF_REQUIRE(state.ok());
    for (const MemberVerdict& verdict : state.value().members) {
      PF_CHECK_MSG(verdict.status == MemberStatus::Stale, "seed=" + std::to_string(seed));
    }
    PF_CHECK_MSG(!state.value().authority.authoritative(), "seed=" + std::to_string(seed));
    // The pod must report at least STALE authority: a fenced member can never
    // leave the pod claiming to be fully authoritative.
    PF_CHECK_MSG(severity(state.value().authority.status) >= severity(Code::Stale),
                 "seed=" + std::to_string(seed) + " status=" +
                     std::string(to_string(state.value().authority.status)));
  }
}

PF_TEST(property, lifecycle_never_takes_an_illegal_step_under_random_events) {
  const auto pod = PodId::parse("pod-property");
  PF_REQUIRE(pod.ok());
  LifecycleState current = LifecycleState::Constructing;
  for (std::uint64_t seed = 1; seed <= 200; ++seed) {
    const auto desired = static_cast<LifecycleState>(seed % 8);
    const LifecycleState next = next_lifecycle(current, desired);
    PF_CHECK_MSG(is_legal_transition(current, next),
                 "seed=" + std::to_string(seed) + " from=" + std::string(to_string(current)) +
                     " to=" + std::string(to_string(next)));
    if (current == LifecycleState::Retired) {
      PF_CHECK_EQ(next, LifecycleState::Retired);
    }
    current = next;
  }
}

PF_TEST(property, evidence_bounds_are_never_exceeded) {
  for (std::uint64_t seed = 1; seed <= 15; ++seed) {
    const std::uint32_t racks = 1 + static_cast<std::uint32_t>(seed % 8);
    SyntheticOptions options = seeded(seed, racks, 2);
    const PodSnapshot snapshot = synthesize(options);
    Policy policy;
    policy.max_members = racks;
    CompositionRequest request = request_for(snapshot);
    request.policy = policy;
    PF_CHECK(compose(request).ok());
    policy.max_members = racks - 1;
    request.policy = policy;
    PF_CHECK_MSG(compose(request).code() == Code::Exhausted, "seed=" + std::to_string(seed));
  }
}
