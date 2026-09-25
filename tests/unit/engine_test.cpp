// Composition engine tests: the required pod-level properties.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <string>

#include "podfabric/engine/composer.hpp"
#include "podfabric/engine/dependency.hpp"
#include "podfabric/model/synthetic.hpp"
#include "test.hpp"

using namespace podfabric;

namespace {

SyntheticOptions base_options() {
  SyntheticOptions options;
  const auto pod = PodId::parse("pod-engine");
  options.pod = pod.ok() ? pod.value() : PodId{};
  options.racks = 4;
  options.power_domains = 2;
  options.seed = 3;
  options.observed_at = 1700000000LL * nanos_per_second;
  return options;
}

// Obligations are removed so that an authority status reflects the member
// verdict under test rather than a downstream obligation violation.
SyntheticOptions bare_options() {
  SyntheticOptions options = base_options();
  options.with_obligations = false;
  return options;
}

CompositionRequest request_for(const PodSnapshot& snapshot, PodEpoch epoch = PodEpoch(1)) {
  CompositionRequest request;
  request.snapshot = snapshot;
  request.context.epoch = epoch;
  request.context.expectations = expectations_for(snapshot);
  return request;
}

const MemberVerdict* member(const PodState& state, const char* rack) {
  const auto id = RackId::parse(rack);
  return id.ok() ? state.find_member(id.value()) : nullptr;
}

}  // namespace

PF_TEST(engine, a_healthy_pod_is_active_and_authoritative) {
  const PodSnapshot snapshot = synthesize(base_options());
  const auto state = compose(request_for(snapshot));
  PF_REQUIRE(state.ok());
  PF_CHECK_EQ(state.value().lifecycle, LifecycleState::Active);
  PF_CHECK(state.value().authority.authoritative());
  PF_CHECK_EQ(state.value().authority.established_members, 4u);
  PF_CHECK(state.value().fences.empty());
  PF_CHECK(state.value().degradations.empty());
  PF_CHECK(!state.value().state_digest.is_zero());
}

PF_TEST(engine, composition_is_deterministic_under_input_reordering) {
  PodSnapshot snapshot = synthesize(base_options());
  const auto first = compose(request_for(snapshot));
  PF_REQUIRE(first.ok());
  std::reverse(snapshot.members.begin(), snapshot.members.end());
  std::reverse(snapshot.links.begin(), snapshot.links.end());
  std::reverse(snapshot.domains.begin(), snapshot.domains.end());
  std::reverse(snapshot.obligations.begin(), snapshot.obligations.end());
  const auto second = compose(request_for(snapshot));
  PF_REQUIRE(second.ok());
  PF_CHECK_EQ(second.value().state_digest, first.value().state_digest);
  PF_CHECK(second.value() == first.value());
}

PF_TEST(engine, a_stale_rack_generation_cannot_sustain_pod_authority) {
  const PodSnapshot live = synthesize(base_options());
  CompositionRequest request = request_for(live);
  request.context.expectations = expectations_for(live);
  for (MemberExpectation& expectation : request.context.expectations) {
    if (expectation.rack.token() == "rack-2") {
      expectation.generation = RackGeneration(9);  // the pod has moved on
    }
  }
  const auto state = compose(request);
  PF_REQUIRE(state.ok());
  const MemberVerdict* verdict = member(state.value(), "rack-2");
  PF_REQUIRE(verdict != nullptr);
  PF_CHECK_EQ(verdict->status, MemberStatus::Stale);
  PF_CHECK_EQ(verdict->generation, RackGeneration(7));
  PF_CHECK(!state.value().authority.authoritative());
  PF_CHECK_EQ(state.value().authority.status, Code::Stale);
  PF_CHECK_EQ(state.value().authority.stale_members, 1u);
  PF_CHECK_EQ(state.value().lifecycle, LifecycleState::Degraded);
  bool fenced = false;
  for (const FenceAction& fence : state.value().fences) {
    if (fence.kind == FenceKind::Member && fence.subject == "rack-2") {
      fenced = true;
    }
  }
  PF_CHECK(fenced);
  // The stale rack contributes no capacity and no connectivity.
  const CapacityAggregate* aggregate = state.value().find_capacity(
      ResourceClass::from_canonical_literal("fabric.bandwidth"));
  PF_REQUIRE(aggregate != nullptr);
  PF_CHECK_EQ(aggregate->total, 300ull);
  PF_CHECK_EQ(aggregate->status, CapacityStatus::Incomplete);
}

PF_TEST(engine, the_same_generation_with_a_different_digest_is_a_conflict) {
  PodSnapshot snapshot = synthesize(base_options());
  CompositionRequest request = request_for(snapshot);
  for (MemberExpectation& expectation : request.context.expectations) {
    if (expectation.rack.token() == "rack-3") {
      expectation.digest = Digest::of("a different descriptor at the same generation");
    }
  }
  const auto state = compose(request);
  PF_REQUIRE(state.ok());
  const MemberVerdict* verdict = member(state.value(), "rack-3");
  PF_REQUIRE(verdict != nullptr);
  PF_CHECK_EQ(verdict->status, MemberStatus::Conflicting);
  PF_CHECK_EQ(state.value().authority.status, Code::Conflicting);
  PF_CHECK(!state.value().authority.authoritative());
}

PF_TEST(engine, duplicate_records_for_one_rack_are_conflicting) {
  PodSnapshot snapshot = synthesize(bare_options());
  CompositionRequest request = request_for(snapshot);
  MemberRecord duplicate = snapshot.members.front();
  duplicate.digest = Digest::of("another descriptor");
  duplicate.generation = RackGeneration(8);
  snapshot.members.push_back(duplicate);
  PF_REQUIRE(validate(snapshot, Policy{}).ok());
  request.snapshot = snapshot;
  const auto state = compose(request);
  PF_REQUIRE(state.ok());
  const MemberVerdict* verdict = member(state.value(), snapshot.members.front().rack.token().c_str());
  PF_REQUIRE(verdict != nullptr);
  PF_CHECK_EQ(verdict->status, MemberStatus::Conflicting);
  PF_CHECK_EQ(state.value().authority.status, Code::Conflicting);
}

PF_TEST(engine, an_unsupported_rack_schema_is_reported_not_guessed) {
  PodSnapshot snapshot = synthesize(bare_options());
  snapshot.members.front().rnf_schema = "rnf.descriptor/99";
  const auto state = compose(request_for(snapshot));
  PF_REQUIRE(state.ok());
  const MemberVerdict* verdict = member(state.value(), snapshot.members.front().rack.token().c_str());
  PF_REQUIRE(verdict != nullptr);
  PF_CHECK_EQ(verdict->status, MemberStatus::Unsupported);
  PF_CHECK_EQ(state.value().authority.status, Code::Unsupported);
}

PF_TEST(engine, an_unattested_join_is_not_adopted) {
  SyntheticOptions options = bare_options();
  options.klass = EvidenceClass::Declared;
  const PodSnapshot snapshot = synthesize(options);
  CompositionRequest request;
  request.snapshot = snapshot;
  request.context.epoch = PodEpoch(1);
  const auto state = compose(request);
  PF_REQUIRE(state.ok());
  for (const MemberVerdict& verdict : state.value().members) {
    PF_CHECK_EQ(verdict.status, MemberStatus::Joining);
  }
  PF_CHECK_EQ(state.value().lifecycle, LifecycleState::Constructing);
  PF_CHECK_EQ(state.value().authority.status, Code::Incomplete);
}

PF_TEST(engine, capacity_is_never_double_counted_across_members) {
  const PodSnapshot snapshot = synthesize(base_options());
  const auto state = compose(request_for(snapshot));
  PF_REQUIRE(state.ok());
  const CapacityAggregate* aggregate = state.value().find_capacity(
      ResourceClass::from_canonical_literal("fabric.bandwidth"));
  PF_REQUIRE(aggregate != nullptr);
  PF_CHECK_EQ(aggregate->total, 400ull);
  std::uint64_t counted = 0;
  for (const CapacityContribution& contribution : aggregate->contributions) {
    if (contribution.counted) {
      counted += contribution.amount;
    }
  }
  PF_CHECK_EQ(counted, aggregate->total);
  std::uint64_t per_domain = 0;
  for (const CapacityAggregate& scoped : state.value().domain_capacity) {
    per_domain += scoped.total;
  }
  PF_CHECK_EQ(per_domain, aggregate->total);
}

PF_TEST(engine, two_members_claiming_one_budget_is_deduplicated_not_summed) {
  PodSnapshot snapshot = synthesize(base_options());
  // Point rack-2 at rack-1's physical budget, attributed to the shared
  // switching plane that both racks declare.
  const DomainId shared = DomainId::from_canonical_literal("plane-shared-0");
  snapshot.members[0].capacity.front().domain = shared;
  snapshot.members[1].capacity.front().exclusivity =
      snapshot.members[0].capacity.front().exclusivity;
  snapshot.members[1].capacity.front().domain = shared;
  canonicalise(snapshot);
  const auto state = compose(request_for(snapshot));
  PF_REQUIRE(state.ok());
  const CapacityAggregate* aggregate = state.value().find_capacity(
      ResourceClass::from_canonical_literal("fabric.bandwidth"));
  PF_REQUIRE(aggregate != nullptr);
  PF_CHECK_EQ(aggregate->total, 300ull);
  PF_CHECK_MSG(aggregate->status == CapacityStatus::Deduplicated,
               std::string(to_string(aggregate->status)) + " contributions=" +
                   std::to_string(aggregate->contributions.size()));
}

PF_TEST(engine, a_link_observed_against_an_old_generation_is_stale) {
  PodSnapshot snapshot = synthesize(base_options());
  for (LinkRecord& link : snapshot.links) {
    if (link.a.rack.token() == "rack-1" || link.b.rack.token() == "rack-1") {
      link.a_generation = RackGeneration(6);
    }
  }
  const auto state = compose(request_for(snapshot));
  PF_REQUIRE(state.ok());
  for (const ConnectivityVerdict& pair : state.value().connectivity) {
    if (pair.from.token() == "rack-1" || pair.to.token() == "rack-1") {
      PF_CHECK_MSG(pair.status == LinkStatus::Stale,
                   pair.from.token() + "->" + pair.to.token() + " is " +
                       std::string(to_string(pair.status)) + " links=" +
                       std::to_string(pair.links.size()));
    }
  }
  PF_CHECK_EQ(state.value().lifecycle, LifecycleState::Degraded);
}

PF_TEST(engine, a_route_installed_at_an_old_generation_is_stale) {
  PodSnapshot snapshot = synthesize(base_options());
  // Replace the direct links with a single transit route through rack-2.
  snapshot.links.clear();
  RouteEvidence route;
  route.ref = RouteRef::from_canonical_literal("route-alpha");
  route.generation = RouteGeneration(4);
  route.authority_digest = Digest::of("routing decision");
  route.resource = ResourceClass::from_canonical_literal("fabric.bandwidth");
  route.committed_capacity = 250;
  route.state = RouteState::Installed;
  route.path = {snapshot.members[0].rack, snapshot.members[1].rack, snapshot.members[2].rack};
  route.path_generations = {RackGeneration(6), RackGeneration(6), RackGeneration(6)};
  snapshot.routes.push_back(route);
  canonicalise(snapshot);
  const auto state = compose(request_for(snapshot));
  PF_REQUIRE(state.ok());
  PF_CHECK(!state.value().connectivity.empty());
  for (const ConnectivityVerdict& pair : state.value().connectivity) {
    PF_CHECK_MSG(pair.status == LinkStatus::Stale,
                 pair.from.token() + "->" + pair.to.token() + " is " +
                     std::string(to_string(pair.status)));
  }

  PodSnapshot fresh = snapshot;
  for (RouteEvidence& entry : fresh.routes) {
    entry.path_generations = {RackGeneration(7), RackGeneration(7), RackGeneration(7)};
  }
  const auto healthy = compose(request_for(fresh));
  PF_REQUIRE(healthy.ok());
  const ConnectivityVerdict* pair = nullptr;
  for (const ConnectivityVerdict& candidate : healthy.value().connectivity) {
    if (candidate.from.token() == "rack-1" && candidate.to.token() == "rack-3") {
      pair = &candidate;
    }
  }
  PF_REQUIRE(pair != nullptr);
  PF_CHECK_EQ(pair->status, LinkStatus::Usable);
  PF_CHECK_EQ(pair->usable_capacity, 250ull);
}

PF_TEST(engine, a_withdrawn_route_blocks_connectivity) {
  PodSnapshot snapshot = synthesize(base_options());
  snapshot.links.clear();
  RouteEvidence route;
  route.ref = RouteRef::from_canonical_literal("route-beta");
  route.generation = RouteGeneration(1);
  route.authority_digest = Digest::of("routing decision");
  route.resource = ResourceClass::from_canonical_literal("fabric.bandwidth");
  route.committed_capacity = 100;
  route.state = RouteState::Withdrawn;
  route.path = {snapshot.members[0].rack, snapshot.members[1].rack};
  route.path_generations = {RackGeneration(7), RackGeneration(7)};
  snapshot.routes.push_back(route);
  canonicalise(snapshot);
  const auto state = compose(request_for(snapshot));
  PF_REQUIRE(state.ok());
  bool any_blocked = false;
  for (const ConnectivityVerdict& pair : state.value().connectivity) {
    if (pair.status == LinkStatus::Blocked) {
      any_blocked = true;
    }
  }
  PF_CHECK(any_blocked);
  PF_CHECK(!state.value().authority.authoritative());
}

PF_TEST(engine, domain_diversity_never_counts_overlapping_domains_twice) {
  SyntheticOptions options = base_options();
  options.racks = 4;
  options.power_domains = 2;
  PodSnapshot snapshot = synthesize(options);
  // Both power domains claim every rack: they overlap completely, so at most
  // one of them can count towards a diversity requirement.
  for (FailureDomainRecord& domain : snapshot.domains) {
    if (domain.kind == DomainKind::Power) {
      domain.members = {snapshot.members[0].rack, snapshot.members[1].rack,
                        snapshot.members[2].rack, snapshot.members[3].rack};
    }
  }
  for (MemberRecord& record : snapshot.members) {
    record.failure_domains.clear();
    for (const FailureDomainRecord& domain : snapshot.domains) {
      record.failure_domains.push_back(domain.id);
    }
    canonicalise(record.failure_domains);
  }
  canonicalise(snapshot);
  const auto state = compose(request_for(snapshot));
  PF_REQUIRE(state.ok());
  for (const ObligationVerdict& verdict : state.value().obligations) {
    if (verdict.kind == ObligationKind::DomainDiversity) {
      PF_CHECK(verdict.status == ObligationStatus::Violated ||
               verdict.status == ObligationStatus::Indeterminate);
      PF_CHECK(verdict.satisfied_domains <= 1u);
    }
  }
}

PF_TEST(engine, a_violated_mandatory_obligation_refuses_authority) {
  PodSnapshot snapshot = synthesize(base_options());
  Obligation heavy;
  heavy.id = ObligationId::from_canonical_literal("obligation.impossible");
  heavy.kind = ObligationKind::CapacityFloor;
  heavy.mandatory = true;
  CapacityRequirement requirement;
  requirement.resource = ResourceClass::from_canonical_literal("fabric.bandwidth");
  requirement.minimum = 100000;
  heavy.requirements.push_back(requirement);
  snapshot.obligations.push_back(heavy);
  canonicalise(snapshot);
  const auto state = compose(request_for(snapshot));
  PF_REQUIRE(state.ok());
  const ObligationVerdict* verdict = nullptr;
  for (const ObligationVerdict& candidate : state.value().obligations) {
    if (candidate.id.token() == "obligation.impossible") {
      verdict = &candidate;
    }
  }
  PF_REQUIRE(verdict != nullptr);
  PF_CHECK_EQ(verdict->status, ObligationStatus::Violated);
  PF_CHECK_EQ(state.value().authority.status, Code::Refused);
  PF_CHECK_EQ(state.value().lifecycle, LifecycleState::Degraded);
}

PF_TEST(engine, an_obligation_on_a_conflicted_aggregate_is_indeterminate) {
  PodSnapshot snapshot = synthesize(base_options());
  snapshot.members[1].capacity.front().exclusivity =
      snapshot.members[0].capacity.front().exclusivity;
  snapshot.members[1].capacity.front().domain = snapshot.members[0].capacity.front().domain;
  snapshot.members[1].capacity.front().amount = 999;
  snapshot.members[1].capacity.front().reserved = 0;
  canonicalise(snapshot);
  const auto state = compose(request_for(snapshot));
  PF_REQUIRE(state.ok());
  for (const ObligationVerdict& verdict : state.value().obligations) {
    if (verdict.kind == ObligationKind::CapacityFloor) {
      PF_CHECK_EQ(verdict.status, ObligationStatus::Indeterminate);
    }
  }
  PF_CHECK(!state.value().authority.authoritative());
}

PF_TEST(engine, a_restart_never_reports_authority_it_did_not_re_establish) {
  const PodSnapshot snapshot = synthesize(base_options());
  CompositionRequest request = request_for(snapshot);
  request.context.recovered_from_store = true;
  request.context.lifecycle = LifecycleState::Active;
  request.context.recovered_at = snapshot.observed_at;
  const auto state = compose(request);
  PF_REQUIRE(state.ok());
  PF_CHECK_EQ(state.value().lifecycle, LifecycleState::Recovering);
  PF_CHECK_EQ(state.value().authority.status, Code::Indeterminate);
  PF_CHECK(!state.value().authority.authoritative());
  PF_CHECK(state.value().authority.recovered);
  bool explained = false;
  for (const Reason& reason : state.value().authority.reasons) {
    if (reason.code == ReasonCode::RecoveredHistorical) {
      explained = true;
    }
  }
  PF_CHECK(explained);

  CompositionRequest follow_up = request_for(snapshot);
  follow_up.context.lifecycle = LifecycleState::Recovering;
  const auto healed = compose(follow_up);
  PF_REQUIRE(healed.ok());
  PF_CHECK_EQ(healed.value().lifecycle, LifecycleState::Active);
  PF_CHECK(healed.value().authority.authoritative());
}

PF_TEST(engine, an_ambiguous_durable_commit_is_indeterminate) {
  const PodSnapshot snapshot = synthesize(base_options());
  CompositionRequest request = request_for(snapshot);
  request.context.ambiguous_commit = true;
  const auto state = compose(request);
  PF_REQUIRE(state.ok());
  PF_CHECK_EQ(state.value().lifecycle, LifecycleState::Partitioned);
  PF_CHECK_EQ(state.value().authority.status, Code::Indeterminate);
}

PF_TEST(engine, a_generation_advance_raises_the_epoch_and_only_once) {
  PodSnapshot snapshot = synthesize(base_options());
  CompositionRequest request = request_for(snapshot, PodEpoch(4));
  request.context.expectations = expectations_for(snapshot);
  request.context.expectations[0].generation = RackGeneration(6);
  const auto state = compose(request);
  PF_REQUIRE(state.ok());
  PF_CHECK_EQ(state.value().epoch, PodEpoch(5));
  PF_CHECK_EQ(member(state.value(), "rack-1")->status, MemberStatus::Established);
  PF_CHECK_EQ(member(state.value(), "rack-2")->status, MemberStatus::Established);
}

PF_TEST(engine, maintenance_and_drain_are_operator_intent_not_evidence) {
  PodSnapshot snapshot = synthesize(base_options());
  snapshot.members[0].admin = AdminState::Maintenance;
  const auto state = compose(request_for(snapshot));
  PF_REQUIRE(state.ok());
  PF_CHECK_EQ(state.value().lifecycle, LifecycleState::Draining);
  PF_CHECK(member(state.value(), "rack-1")->status == MemberStatus::Established);
  snapshot.members[0].admin = AdminState::Disabled;
  const auto disabled = compose(request_for(snapshot));
  PF_REQUIRE(disabled.ok());
  const CapacityAggregate* aggregate = disabled.value().find_capacity(
      ResourceClass::from_canonical_literal("fabric.bandwidth"));
  PF_REQUIRE(aggregate != nullptr);
  PF_CHECK_EQ(aggregate->total, 300ull);
}

PF_TEST(engine, a_disabled_rack_fences_its_links) {
  PodSnapshot snapshot = synthesize(base_options());
  snapshot.members[0].health = HealthState::Critical;
  const auto state = compose(request_for(snapshot));
  PF_REQUIRE(state.ok());
  for (const ConnectivityVerdict& pair : state.value().connectivity) {
    if (pair.from.token() == "rack-1" || pair.to.token() == "rack-1") {
      PF_CHECK_EQ(pair.status, LinkStatus::Blocked);
    }
  }
  PF_CHECK(!state.value().authority.authoritative());
}

PF_TEST(engine, an_unknown_health_rack_withholds_its_capacity) {
  PodSnapshot snapshot = synthesize(base_options());
  snapshot.members[0].health = HealthState::Unknown;
  const auto state = compose(request_for(snapshot));
  PF_REQUIRE(state.ok());
  const CapacityAggregate* aggregate = state.value().find_capacity(
      ResourceClass::from_canonical_literal("fabric.bandwidth"));
  PF_REQUIRE(aggregate != nullptr);
  PF_CHECK_EQ(aggregate->total, 300ull);
  PF_CHECK_EQ(aggregate->status, CapacityStatus::Incomplete);
}

PF_TEST(engine, aged_evidence_is_stale) {
  PodSnapshot snapshot = synthesize(base_options());
  for (MemberRecord& record : snapshot.members) {
    record.observed_at = snapshot.observed_at - 120 * nanos_per_second;
  }
  const auto state = compose(request_for(snapshot));
  PF_REQUIRE(state.ok());
  for (const MemberVerdict& verdict : state.value().members) {
    PF_CHECK_EQ(verdict.status, MemberStatus::Stale);
  }
}

PF_TEST(engine, a_snapshot_without_an_observation_time_is_refused) {
  PodSnapshot snapshot = synthesize(base_options());
  snapshot.observed_at = 0;
  const auto state = compose(request_for(snapshot));
  PF_CHECK(state.code() == Code::Invalid);
}

PF_TEST(engine, domain_membership_disagreement_is_conflicting) {
  PodSnapshot snapshot = synthesize(base_options());
  snapshot.domains.front().members.clear();
  canonicalise(snapshot);
  const auto state = compose(request_for(snapshot));
  PF_REQUIRE(state.ok());
  bool conflicting = false;
  for (const DomainVerdict& verdict : state.value().domains) {
    if (verdict.status == DomainStatus::Conflicting) {
      conflicting = true;
    }
  }
  PF_CHECK(conflicting);
  PF_CHECK(!state.value().authority.authoritative());
}

PF_TEST(engine, a_rack_in_two_domains_of_one_kind_is_an_overlap) {
  PodSnapshot snapshot = synthesize(base_options());
  for (MemberRecord& record : snapshot.members) {
    record.failure_domains.push_back(DomainId::from_canonical_literal("power-0"));
    record.failure_domains.push_back(DomainId::from_canonical_literal("power-1"));
    canonicalise(record.failure_domains);
  }
  for (FailureDomainRecord& domain : snapshot.domains) {
    if (domain.kind == DomainKind::Power) {
      domain.members.clear();
      for (const MemberRecord& record : snapshot.members) {
        domain.members.push_back(record.rack);
      }
    }
  }
  canonicalise(snapshot);
  const auto state = compose(request_for(snapshot));
  PF_REQUIRE(state.ok());
  for (const DomainVerdict& verdict : state.value().domains) {
    if (verdict.kind == DomainKind::Power) {
      PF_CHECK_EQ(verdict.status, DomainStatus::Conflicting);
    }
  }
}

PF_TEST(engine, a_sparse_pod_reports_missing_members_as_stale) {
  SyntheticOptions options = base_options();
  options.omit_every = 2;
  const PodSnapshot snapshot = synthesize(options);
  PF_CHECK_EQ(snapshot.members.size(), std::size_t(2));
  CompositionRequest request = request_for(snapshot);
  request.context.expectations = expectations_for(synthesize(base_options()));
  const auto state = compose(request);
  PF_REQUIRE(state.ok());
  std::size_t missing = 0;
  for (const MemberVerdict& verdict : state.value().members) {
    if (verdict.status == MemberStatus::Missing) {
      ++missing;
    }
  }
  PF_CHECK_EQ(missing, std::size_t(2));
  PF_CHECK_EQ(state.value().authority.stale_members, 2u);
}

PF_TEST(engine, every_decision_explains_itself) {
  const PodSnapshot snapshot = synthesize(base_options());
  const auto state = compose(request_for(snapshot));
  PF_REQUIRE(state.ok());
  for (const DecisionRecord& record : state.value().decisions) {
    PF_CHECK(!record.id.is_nil());
    PF_CHECK(!record.fingerprint.is_zero());
  }
  const DecisionRecord* lifecycle = state.value().find_decision(
      make_decision_id(DecisionKind::PodLifecycle, "pod"));
  PF_REQUIRE(lifecycle != nullptr);
  PF_CHECK(lifecycle->deps.size() > 4);
}
