// Dependency indexing and selective revalidation.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <string>

#include "podfabric/engine/composer.hpp"
#include "podfabric/engine/dependency.hpp"
#include "podfabric/model/synthetic.hpp"
#include "test.hpp"

using namespace podfabric;

namespace {

SyntheticOptions options_for(std::uint32_t racks, std::uint64_t seed) {
  SyntheticOptions options;
  const auto pod = PodId::parse("pod-deps");
  options.pod = pod.ok() ? pod.value() : PodId{};
  options.racks = racks;
  options.power_domains = 2;
  options.seed = seed;
  options.observed_at = 1700000000LL * nanos_per_second;
  return options;
}

DecisionIndex publish(const PodState& state) {
  DecisionIndex index;
  for (const DecisionRecord& record : state.decisions) {
    index.publish(record);
  }
  return index;
}

}  // namespace

PF_TEST(dependency, the_plan_covers_the_dependents_of_a_changed_input) {
  const PodSnapshot snapshot = synthesize(options_for(4, 1));
  CompositionRequest request;
  request.snapshot = snapshot;
  request.context.epoch = PodEpoch(1);
  request.context.expectations = expectations_for(snapshot);
  const auto state = compose(request);
  PF_REQUIRE(state.ok());
  const DecisionIndex index = publish(state.value());
  PF_CHECK_EQ(index.size(), state.value().decisions.size());

  const RevalidationPlan plan =
      index.plan({DepKey{DepKind::Member, "rack-2"}});
  PF_CHECK(plan.contains(make_decision_id(DecisionKind::MemberState, "rack-2")));
  PF_CHECK(plan.contains(make_decision_id(DecisionKind::PodLifecycle, "pod")));
  PF_CHECK(plan.contains(make_decision_id(DecisionKind::PodAuthority, "pod")));
  PF_CHECK(plan.contains(make_decision_id(DecisionKind::Connectivity, "rack-1.rack-2")));
  // A pair that does not mention rack-2 is reachable only through the
  // lifecycle and authority fan-in, never directly.
  const DecisionId untouched = make_decision_id(DecisionKind::Connectivity, "rack-3.rack-4");
  const DecisionRecord* record = index.find(untouched);
  PF_REQUIRE(record != nullptr);
  bool directly_dependent = false;
  for (const DepKey& dep : record->deps) {
    if (dep.kind == DepKind::Member && dep.subject == "rack-2") {
      directly_dependent = true;
    }
  }
  PF_CHECK(!directly_dependent);

  const RevalidationPlan unknown = index.plan({DepKey{DepKind::Member, "rack-does-not-exist"}});
  PF_CHECK_EQ(unknown.size(), std::size_t(0));
}

PF_TEST(dependency, one_rack_reincarnation_only_invalidates_its_dependents) {
  PodSnapshot before = synthesize(options_for(4, 7));
  CompositionRequest first;
  first.snapshot = before;
  first.context.epoch = PodEpoch(1);
  first.context.expectations = expectations_for(before);
  const auto first_state = compose(first);
  PF_REQUIRE(first_state.ok());
  const DecisionIndex index = publish(first_state.value());

  // Rack 2 is reincarnated: a new generation with a new descriptor digest.
  PodSnapshot after = before;
  for (MemberRecord& record : after.members) {
    if (record.rack.token() == "rack-2") {
      record.generation = RackGeneration(8);
      record.digest = Digest::of("rack-2 reincarnated");
    }
  }
  for (LinkRecord& link : after.links) {
    if (link.a.rack.token() == "rack-2") link.a_generation = RackGeneration(8);
    if (link.b.rack.token() == "rack-2") link.b_generation = RackGeneration(8);
  }
  canonicalise(after);

  CompositionRequest second;
  second.snapshot = after;
  second.context.epoch = PodEpoch(1);
  second.context.expectations = expectations_for(before);
  const auto second_state = compose(second);
  PF_REQUIRE(second_state.ok());
  PF_CHECK_EQ(second_state.value().epoch, PodEpoch(2));

  std::vector<DepKey> changed = membership_change_keys(first.context.expectations,
                                                       expectations_for(after));
  const std::vector<DepKey> evidence_changes = snapshot_change_keys(before, after);
  changed.insert(changed.end(), evidence_changes.begin(), evidence_changes.end());
  const RevalidationReport report = revalidate(index, second_state.value(), changed);
  PF_CHECK_MSG(report.sound(), report.describe());
  PF_CHECK(report.recomputed_changed.size() > 0);
  PF_CHECK(report.recomputed_unchanged.size() > 0);
  // Every decision that changed mentions rack-2 or is the fan-in pair.
  for (const DecisionId& id : report.recomputed_changed) {
    const bool fan_in = id.token() == "lifecycle.pod" || id.token() == "authority.pod";
    PF_CHECK_MSG(fan_in || id.token().find("rack-2") != std::string::npos, id.token());
  }
  // A rack that nobody touched keeps every one of its decisions.
  const DecisionId untouched_member = make_decision_id(DecisionKind::MemberState, "rack-4");
  PF_CHECK(!report.plan.contains(untouched_member));
}

PF_TEST(dependency, an_unpredicted_change_is_detected) {
  const PodSnapshot snapshot = synthesize(options_for(3, 2));
  CompositionRequest request;
  request.snapshot = snapshot;
  request.context.epoch = PodEpoch(1);
  request.context.expectations = expectations_for(snapshot);
  const auto state = compose(request);
  PF_REQUIRE(state.ok());
  const DecisionIndex index = publish(state.value());
  // Claiming that nothing changed must be reported as unsound, because the
  // evidence actually moved.
  PodSnapshot mutated = snapshot;
  mutated.members[0].health = HealthState::Critical;
  CompositionRequest second;
  second.snapshot = mutated;
  second.context.epoch = PodEpoch(1);
  second.context.expectations = expectations_for(snapshot);
  const auto second_state = compose(second);
  PF_REQUIRE(second_state.ok());
  const RevalidationReport report = revalidate(index, second_state.value(), {});
  PF_CHECK(!report.sound());
  PF_CHECK(!report.changed_outside_plan.empty());
}

PF_TEST(dependency, the_plan_is_a_bounded_transitive_closure) {
  const PodSnapshot snapshot = synthesize(options_for(6, 4));
  CompositionRequest request;
  request.snapshot = snapshot;
  request.context.epoch = PodEpoch(1);
  request.context.expectations = expectations_for(snapshot);
  const auto state = compose(request);
  PF_REQUIRE(state.ok());
  const DecisionIndex index = publish(state.value());
  const RevalidationPlan plan = index.plan({DepKey{DepKind::Epoch, "1"}});
  // Lifecycle and authority depend on the epoch; everything else is reached
  // through the lifecycle fan-in.
  PF_CHECK(plan.contains(make_decision_id(DecisionKind::PodLifecycle, "pod")));
  PF_CHECK(plan.contains(make_decision_id(DecisionKind::PodAuthority, "pod")));
  PF_CHECK(plan.closure_steps <= state.value().decisions.size() * 2);
}

PF_TEST(dependency, token_change_keys_handles_one_sided_leases) {
  // Regression: a lease present on only one side must be reported as a change
  // and must never be dereferenced through an end iterator.
  const auto incarnation = Incarnation::fresh();
  PF_REQUIRE(incarnation.ok());
  AuthorityToken present;
  present.lease = LeaseId::from_canonical_literal("lease-a");
  present.pod = PodId::from_canonical_literal("pod-deps");
  present.epoch = PodEpoch(1);
  present.incarnation = incarnation.value();
  present.issued_at = 1000;
  present.expires_at = 2000;
  const std::vector<AuthorityToken> before{present};
  const std::vector<AuthorityToken> after{};
  const std::vector<LeaseId> none{};
  const std::vector<LeaseId> revoked{present.lease};

  const std::vector<DepKey> removed = token_change_keys(before, none, after, none);
  PF_CHECK_EQ(removed.size(), std::size_t(1));
  PF_CHECK_EQ(removed.front().subject, std::string("lease-a"));

  const std::vector<DepKey> added = token_change_keys(after, none, before, none);
  PF_CHECK_EQ(added.size(), std::size_t(1));

  const std::vector<DepKey> revoked_only = token_change_keys(after, none, after, revoked);
  PF_CHECK_EQ(revoked_only.size(), std::size_t(1));

  PF_CHECK(token_change_keys(before, none, before, none).empty());
  PF_CHECK(token_change_keys(after, none, after, none).empty());

  AuthorityToken modified = present;
  modified.fencing = FencingSequence(9);
  PF_CHECK_EQ(token_change_keys(before, none, std::vector<AuthorityToken>{modified}, none).size(),
              std::size_t(1));
}

PF_TEST(dependency, snapshot_change_keys_cover_every_collection) {
  const PodSnapshot snapshot = synthesize(options_for(3, 5));
  PodSnapshot mutated = snapshot;
  mutated.members[0].health = HealthState::Impaired;
  mutated.links[0].oper = OperationalState::Down;
  RouteEvidence route;
  route.ref = RouteRef::from_canonical_literal("route-added");
  route.generation = RouteGeneration(1);
  route.authority_digest = Digest::of("x");
  route.resource = ResourceClass::from_canonical_literal("fabric.bandwidth");
  route.committed_capacity = 10;
  route.state = RouteState::Installed;
  route.path = {snapshot.members[0].rack, snapshot.members[1].rack};
  route.path_generations = {RackGeneration(7), RackGeneration(7)};
  mutated.routes.push_back(route);
  mutated.domains[0].description = "changed";
  mutated.obligations[0].mandatory = !mutated.obligations[0].mandatory;
  canonicalise(mutated);

  const std::vector<DepKey> changed = snapshot_change_keys(snapshot, mutated);
  bool saw_member = false;
  bool saw_link = false;
  bool saw_route = false;
  bool saw_domain = false;
  bool saw_obligation = false;
  for (const DepKey& key : changed) {
    saw_member |= key.kind == DepKind::Member;
    saw_link |= key.kind == DepKind::Link;
    saw_route |= key.kind == DepKind::Route;
    saw_domain |= key.kind == DepKind::Domain;
    saw_obligation |= key.kind == DepKind::Obligation;
  }
  PF_CHECK(saw_member);
  PF_CHECK(saw_link);
  PF_CHECK(saw_route);
  PF_CHECK(saw_domain);
  PF_CHECK(saw_obligation);
  PF_CHECK(snapshot_change_keys(snapshot, snapshot).empty());
}
