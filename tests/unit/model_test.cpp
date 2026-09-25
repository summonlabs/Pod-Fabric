// Model tests: lifecycle, policy, snapshot validation, canonical ordering.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <string>

#include "podfabric/model/pod_state.hpp"
#include "podfabric/model/policy.hpp"
#include "podfabric/model/snapshot.hpp"
#include "podfabric/model/synthetic.hpp"
#include "test.hpp"

using namespace podfabric;

namespace {

PodSnapshot sample() {
  SyntheticOptions options;
  const auto pod = PodId::parse("pod-model");
  options.pod = pod.ok() ? pod.value() : PodId{};
  options.racks = 3;
  options.power_domains = 2;
  return synthesize(options);
}

}  // namespace

PF_TEST(model, lifecycle_transitions_are_explicit) {
  PF_CHECK(is_legal_transition(LifecycleState::Constructing, LifecycleState::Active));
  PF_CHECK(is_legal_transition(LifecycleState::Active, LifecycleState::Degraded));
  PF_CHECK(is_legal_transition(LifecycleState::Degraded, LifecycleState::Active));
  PF_CHECK(is_legal_transition(LifecycleState::Active, LifecycleState::Partitioned));
  PF_CHECK(is_legal_transition(LifecycleState::Partitioned, LifecycleState::Recovering));
  PF_CHECK(is_legal_transition(LifecycleState::Recovering, LifecycleState::Active));
  PF_CHECK(is_legal_transition(LifecycleState::Active, LifecycleState::Retired));
  // Retired is terminal in every direction.
  for (auto target : {LifecycleState::Constructing, LifecycleState::Active,
                      LifecycleState::Degraded, LifecycleState::Partitioned,
                      LifecycleState::Recovering, LifecycleState::Maintenance,
                      LifecycleState::Draining}) {
    PF_CHECK(!is_legal_transition(LifecycleState::Retired, target));
  }
  for (auto source : {LifecycleState::Constructing, LifecycleState::Active,
                      LifecycleState::Degraded, LifecycleState::Partitioned,
                      LifecycleState::Recovering, LifecycleState::Maintenance,
                      LifecycleState::Draining, LifecycleState::Retired}) {
    PF_CHECK(is_legal_transition(source, source));
  }
  // A partition never heals directly back to Active.
  PF_CHECK_EQ(next_lifecycle(LifecycleState::Partitioned, LifecycleState::Active),
              LifecycleState::Recovering);
  PF_CHECK_EQ(next_lifecycle(LifecycleState::Recovering, LifecycleState::Active),
              LifecycleState::Active);
  PF_CHECK_EQ(next_lifecycle(LifecycleState::Retired, LifecycleState::Active),
              LifecycleState::Retired);
}

PF_TEST(model, policy_validation_rejects_incoherent_bounds) {
  Policy policy;
  PF_CHECK(validate(policy).ok());
  Policy zero_amount = policy;
  zero_amount.max_capacity_amount = 0;
  PF_CHECK(validate(zero_amount).code() == Code::Invalid);
  Policy inverted = policy;
  inverted.max_capacity_total = inverted.max_capacity_amount - 1;
  PF_CHECK(validate(inverted).code() == Code::Invalid);
  Policy negative_ttl = policy;
  negative_ttl.evidence_ttl = -1;
  PF_CHECK(validate(negative_ttl).code() == Code::Invalid);
  Policy no_schemas = policy;
  no_schemas.supported_rnf_schemas.clear();
  PF_CHECK(validate(no_schemas).code() == Code::Invalid);
  Policy tiny = policy;
  tiny.max_journal_record_bytes = 8;
  PF_CHECK(validate(tiny).code() == Code::Invalid);
}

PF_TEST(model, snapshot_validation_accepts_a_synthetic_pod) {
  const PodSnapshot snapshot = sample();
  PF_CHECK(validate(snapshot, Policy{}).ok());
  PF_CHECK_EQ(snapshot.members.size(), std::size_t(3));
}

PF_TEST(model, snapshot_validation_rejects_structural_defects) {
  const Policy policy;
  {
    PodSnapshot bad = sample();
    bad.schema = "podfabric.snapshot/999";
    PF_CHECK(validate(bad, policy).code() == Code::Unsupported);
  }
  {
    PodSnapshot bad = sample();
    bad.pod = PodId{};
    PF_CHECK(validate(bad, policy).code() == Code::Invalid);
  }
  {
    PodSnapshot bad = sample();
    bad.observed_at = 0;
    PF_CHECK(validate(bad, policy).code() == Code::Invalid);
  }
  {
    PodSnapshot bad = sample();
    bad.members.front().generation = RackGeneration(0);
    PF_CHECK(validate(bad, policy).code() == Code::Invalid);
  }
  {
    PodSnapshot bad = sample();
    bad.members.front().digest = Digest{};
    PF_CHECK(validate(bad, policy).code() == Code::Invalid);
  }
  {
    PodSnapshot bad = sample();
    bad.links.front().b.rack = bad.links.front().a.rack;
    PF_CHECK(validate(bad, policy).code() == Code::Invalid);
  }
  {
    PodSnapshot bad = sample();
    bad.links.front().id = bad.links.back().id;
    PF_CHECK(validate(bad, policy).code() == Code::Invalid);
  }
  {
    PodSnapshot bad = sample();
    bad.routes.push_back(RouteEvidence{});
    PF_CHECK(validate(bad, policy).code() == Code::Invalid);
  }
  {
    PodSnapshot bad = sample();
    bad.domains.push_back(bad.domains.front());
    PF_CHECK(validate(bad, policy).code() == Code::Invalid);
  }
  {
    PodSnapshot bad = sample();
    bad.members.front().capacity.front().reserved =
        bad.members.front().capacity.front().amount + 1;
    PF_CHECK(validate(bad, policy).code() == Code::Invalid);
  }
  {
    PodSnapshot bad = sample();
    Policy small;
    small.max_members = 1;
    PF_CHECK(validate(bad, small).code() == Code::Exhausted);
  }
  {
    PodSnapshot bad = sample();
    bad.domains.front().members.push_back(bad.domains.front().members.front());
    PF_CHECK(validate(bad, policy).code() == Code::Invalid);
  }
}

PF_TEST(model, canonicalisation_sorts_every_collection) {
  PodSnapshot snapshot = sample();
  std::reverse(snapshot.members.begin(), snapshot.members.end());
  std::reverse(snapshot.links.begin(), snapshot.links.end());
  std::reverse(snapshot.domains.begin(), snapshot.domains.end());
  canonicalise(snapshot);
  for (std::size_t i = 1; i < snapshot.members.size(); ++i) {
    PF_CHECK(snapshot.members[i - 1].rack < snapshot.members[i].rack);
  }
  for (std::size_t i = 1; i < snapshot.links.size(); ++i) {
    PF_CHECK(snapshot.links[i - 1].id < snapshot.links[i].id);
  }
  for (std::size_t i = 1; i < snapshot.domains.size(); ++i) {
    PF_CHECK(snapshot.domains[i - 1].id < snapshot.domains[i].id);
  }
  PF_CHECK(validate(snapshot, Policy{}).ok());
}
