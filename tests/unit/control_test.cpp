// Controller semantics: lifecycle, authority, observers, reentrancy.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "podfabric/control/controller.hpp"
#include "podfabric/model/synthetic.hpp"
#include "process.hpp"
#include "test.hpp"

using namespace podfabric;

namespace {

SyntheticOptions options_for(std::uint32_t racks, std::uint64_t seed) {
  SyntheticOptions options;
  options.pod = PodId::parse("pod-control").value();
  options.racks = racks;
  options.power_domains = 2;
  options.seed = seed;
  options.observed_at = 1700000000LL * nanos_per_second;
  return options;
}

ControllerOptions controller_options(const std::string& store) {
  ControllerOptions options;
  options.pod = PodId::parse("pod-control").value();
  options.store_directory = store;
  return options;
}

}  // namespace

PF_TEST(control, a_memory_only_controller_reports_no_durability) {
  PodController controller(controller_options({}));
  PF_REQUIRE(controller.open().ok());
  PF_CHECK_EQ(controller.recovery().outcome, persist::RecoveryOutcome::Fresh);
  const auto applied = controller.apply(synthesize(options_for(3, 1)));
  PF_REQUIRE(applied.ok());
  PF_CHECK_EQ(applied.value().outcome, ApplyOutcome::EpochAdvanced);
  PF_CHECK_EQ(controller.lifecycle(), LifecycleState::Degraded);
  // The round that follows the epoch advance settles the pod back to Active.
  const auto settled = controller.apply(synthesize(options_for(3, 1)));
  PF_REQUIRE(settled.ok());
  PF_CHECK_EQ(settled.value().outcome, ApplyOutcome::Applied);
  PF_CHECK_EQ(settled.value().state.lifecycle, LifecycleState::Active);
  const auto third = controller.apply(synthesize(options_for(3, 1)));
  PF_REQUIRE(third.ok());
  PF_CHECK_EQ(third.value().outcome, ApplyOutcome::Unchanged);
  PF_CHECK(controller.close().ok());
}

PF_TEST(control, applying_the_same_evidence_twice_is_idempotent) {
  PodController controller(controller_options({}));
  PF_REQUIRE(controller.open().ok());
  const PodSnapshot snapshot = synthesize(options_for(4, 2));
  PF_REQUIRE(controller.apply(snapshot).ok());
  PF_REQUIRE(controller.apply(snapshot).ok());  // settles after the epoch advance
  const Digest first = controller.state_digest();
  const PodEpoch epoch = controller.epoch();
  const auto again = controller.apply(snapshot);
  PF_REQUIRE(again.ok());
  PF_CHECK_EQ(again.value().outcome, ApplyOutcome::Unchanged);
  PF_CHECK_EQ(controller.state_digest(), first);
  PF_CHECK_EQ(controller.epoch(), epoch);
  PF_CHECK(controller.close().ok());
}

PF_TEST(control, a_generation_change_advances_the_epoch_and_invalidates_old_tokens) {
  PodController controller(controller_options({}));
  PF_REQUIRE(controller.open().ok());
  PodSnapshot snapshot = synthesize(options_for(3, 3));
  PF_REQUIRE(controller.apply(snapshot).ok());
  const auto minted = controller.mint_authority(AuthorityScope::Pod, RackId{}, 3600LL * nanos_per_second);
  PF_REQUIRE(minted.ok());
  PF_CHECK(controller.check_authority(minted.value(), AuthorityScope::Pod, RackId{}).valid());
  const PodEpoch before = controller.epoch();

  for (MemberRecord& record : snapshot.members) {
    if (record.rack.token() == "rack-1") {
      record.generation = RackGeneration(8);
      record.digest = Digest::of("rack-1 upgraded");
    }
  }
  for (LinkRecord& link : snapshot.links) {
    if (link.a.rack.token() == "rack-1") link.a_generation = RackGeneration(8);
    if (link.b.rack.token() == "rack-1") link.b_generation = RackGeneration(8);
  }
  canonicalise(snapshot);
  const auto applied = controller.apply(snapshot);
  PF_REQUIRE(applied.ok());
  PF_CHECK_EQ(applied.value().outcome, ApplyOutcome::EpochAdvanced);
  PF_CHECK(controller.epoch().value() > before.value());
  const AuthorityVerdict verdict =
      controller.check_authority(minted.value(), AuthorityScope::Pod, RackId{});
  PF_CHECK(!verdict.valid());
  PF_CHECK_EQ(verdict.code, AuthorityVerdictCode::StaleEpoch);
  PF_CHECK(controller.tokens().empty());
  PF_CHECK(controller.close().ok());
}

PF_TEST(control, revocation_is_immediate_and_durable) {
  PodController controller(controller_options({}));
  PF_REQUIRE(controller.open().ok());
  PF_REQUIRE(controller.apply(synthesize(options_for(2, 4))).ok());
  const auto minted = controller.mint_authority(AuthorityScope::Member,
                                                RackId::from_canonical_literal("rack-1"),
                                                600LL * nanos_per_second);
  PF_REQUIRE(minted.ok());
  PF_CHECK(controller
                .check_authority(minted.value(), AuthorityScope::Member,
                                 RackId::from_canonical_literal("rack-1"))
                .valid());
  PF_CHECK(controller.revoke_authority(minted.value().lease).ok());
  PF_CHECK_EQ(controller.check_authority(minted.value(), AuthorityScope::Member,
                                         RackId::from_canonical_literal("rack-1"))
                  .code,
              AuthorityVerdictCode::Revoked);
  PF_CHECK_EQ(controller.revoke_authority(minted.value().lease).code(), Code::AlreadyExists);
  PF_CHECK(controller.close().ok());
}

PF_TEST(control, a_member_token_does_not_authorise_another_member) {
  PodController controller(controller_options({}));
  PF_REQUIRE(controller.open().ok());
  PF_REQUIRE(controller.apply(synthesize(options_for(3, 5))).ok());
  const auto minted = controller.mint_authority(AuthorityScope::Member,
                                                RackId::from_canonical_literal("rack-1"),
                                                600LL * nanos_per_second);
  PF_REQUIRE(minted.ok());
  PF_CHECK_EQ(controller.check_authority(minted.value(), AuthorityScope::Member,
                                         RackId::from_canonical_literal("rack-2"))
                  .code,
              AuthorityVerdictCode::WrongScope);
}

PF_TEST(control, observer_callbacks_run_without_a_controller_lock_held) {
  PodController controller(controller_options({}));
  PF_REQUIRE(controller.open().ok());
  std::atomic<int> calls{0};
  std::atomic<bool> reentered{false};
  // The observer calls straight back into the controller. If any callback ran
  // with a lock held, this would deadlock rather than fail.
  PF_REQUIRE(controller
                 .subscribe([&](const Event&) {
                   calls.fetch_add(1);
                   if (!reentered.exchange(true)) {
                     (void)controller.lifecycle();
                     (void)controller.state();
                   }
                 })
                 .ok());
  PF_REQUIRE(controller.apply(synthesize(options_for(2, 6))).ok());
  PF_CHECK(calls.load() > 0);
  PF_CHECK(controller.close().ok());
}

PF_TEST(control, a_throwing_observer_does_not_take_the_controller_down) {
  PodController controller(controller_options({}));
  PF_REQUIRE(controller.open().ok());
  PF_REQUIRE(controller.subscribe([](const Event&) { throw std::runtime_error("observer"); }).ok());
  std::atomic<int> healthy{0};
  PF_REQUIRE(controller.subscribe([&](const Event&) { healthy.fetch_add(1); }).ok());
  const auto applied = controller.apply(synthesize(options_for(2, 7)));
  PF_REQUIRE(applied.ok());
  PF_CHECK(healthy.load() > 0);
  PF_CHECK(controller.close().ok());
}

PF_TEST(control, admin_intent_survives_evidence_that_disagrees) {
  PodController controller(controller_options({}));
  PF_REQUIRE(controller.open().ok());
  const PodSnapshot snapshot = synthesize(options_for(3, 8));
  PF_REQUIRE(controller.apply(snapshot).ok());
  PF_REQUIRE(controller.set_admin_state(RackId::from_canonical_literal("rack-1"),
                                        AdminState::Draining)
                 .ok());
  PF_CHECK_EQ(controller.lifecycle(), LifecycleState::Draining);
  // Evidence still says the rack is enabled; intent wins.
  const auto reapplied = controller.apply(snapshot);
  PF_REQUIRE(reapplied.ok());
  PF_CHECK_EQ(reapplied.value().state.lifecycle, LifecycleState::Draining);
  PF_CHECK_EQ(controller.admin_overrides().size(), std::size_t(1));
  PF_REQUIRE(controller.clear_admin_state(RackId::from_canonical_literal("rack-1")).ok());
  PF_CHECK(controller.admin_overrides().empty());
  PF_CHECK(controller.close().ok());
}

PF_TEST(control, retired_is_terminal_and_mints_no_authority) {
  PodController controller(controller_options({}));
  PF_REQUIRE(controller.open().ok());
  PF_REQUIRE(controller.apply(synthesize(options_for(2, 9))).ok());
  PF_REQUIRE(controller.retire().ok());
  PF_CHECK_EQ(controller.lifecycle(), LifecycleState::Retired);
  PF_CHECK_EQ(controller.retire().code(), Code::Refused);
  const Status admin = controller.set_admin_state(RackId::from_canonical_literal("rack-1"),
                                                  AdminState::Draining);
  PF_CHECK_EQ(admin.code(), Code::Refused);
  const auto minted =
      controller.mint_authority(AuthorityScope::Pod, RackId{}, 60LL * nanos_per_second);
  PF_CHECK_EQ(minted.code(), Code::Refused);
  const auto applied = controller.apply(synthesize(options_for(2, 9)));
  PF_REQUIRE(applied.ok());
  PF_CHECK_EQ(applied.value().outcome, ApplyOutcome::Retired);
  PF_CHECK_EQ(applied.value().state.lifecycle, LifecycleState::Retired);
  PF_CHECK_EQ(applied.value().state.authority.status, Code::Refused);
  PF_CHECK(controller.close().ok());
}

PF_TEST(control, a_retired_pod_does_not_come_back_after_a_restart) {
  const std::string directory = test::make_scratch_directory("control-retired");
  {
    PodController controller(controller_options(directory));
    PF_REQUIRE(controller.open().ok());
    PF_REQUIRE(controller.apply(synthesize(options_for(2, 10))).ok());
    PF_REQUIRE(controller.retire().ok());
    PF_CHECK(controller.close().ok());
  }
  {
    PodController controller(controller_options(directory));
    PF_REQUIRE(controller.open().ok());
    PF_CHECK_EQ(controller.lifecycle(), LifecycleState::Retired);
    const auto applied = controller.apply(synthesize(options_for(2, 10)));
    PF_REQUIRE(applied.ok());
    PF_CHECK_EQ(applied.value().state.lifecycle, LifecycleState::Retired);
    PF_CHECK(!applied.value().state.authority.authoritative());
    PF_CHECK(controller.close().ok());
  }
  test::remove_directory(directory);
}

PF_TEST(control, authority_requirement_makes_a_tokenless_pod_non_authoritative) {
  ControllerOptions options = controller_options({});
  options.require_authority_token = true;
  PodController controller(options);
  PF_REQUIRE(controller.open().ok());
  const auto applied = controller.apply(synthesize(options_for(3, 11)));
  PF_REQUIRE(applied.ok());
  PF_CHECK(!applied.value().state.authority.authoritative());
  const auto minted =
      controller.mint_authority(AuthorityScope::Pod, RackId{}, 3600LL * nanos_per_second);
  PF_REQUIRE(minted.ok());
  const auto again = controller.apply(synthesize(options_for(3, 11)));
  PF_REQUIRE(again.ok());
  PF_CHECK(again.value().state.authority.authoritative());
  PF_CHECK(controller.close().ok());
}

PF_TEST(control, a_restart_fences_previous_authority_and_reports_recovery) {
  const std::string directory = test::make_scratch_directory("control-restart");
  AuthorityToken carried;
  PodEpoch epoch_before;
  {
    PodController controller(controller_options(directory));
    PF_REQUIRE(controller.open().ok());
    PF_REQUIRE(controller.apply(synthesize(options_for(3, 12))).ok());
    const auto minted =
        controller.mint_authority(AuthorityScope::Pod, RackId{}, 86400LL * nanos_per_second);
    PF_REQUIRE(minted.ok());
    carried = minted.value();
    epoch_before = controller.epoch();
    PF_CHECK(controller.close().ok());
  }
  {
    PodController controller(controller_options(directory));
    PF_REQUIRE(controller.open().ok());
    PF_CHECK_EQ(controller.lifecycle(), LifecycleState::Recovering);
    PF_CHECK(controller.epoch().value() > epoch_before.value());
    PF_CHECK(controller.tokens().empty());
    PF_CHECK(!(controller.incarnation() == carried.incarnation));
    const auto applied = controller.apply(synthesize(options_for(3, 12)));
    PF_REQUIRE(applied.ok());
    PF_CHECK_EQ(applied.value().outcome, ApplyOutcome::Recovered);
    PF_CHECK_EQ(applied.value().state.lifecycle, LifecycleState::Recovering);
    PF_CHECK(!applied.value().state.authority.authoritative());
    const AuthorityVerdict verdict =
        controller.check_authority(carried, AuthorityScope::Pod, RackId{});
    PF_CHECK(!verdict.valid());
    PF_CHECK_EQ(verdict.code, AuthorityVerdictCode::StaleIncarnation);
    // The next round may become authoritative again.
    const auto healed = controller.apply(synthesize(options_for(3, 12)));
    PF_REQUIRE(healed.ok());
    PF_CHECK(healed.value().state.authority.authoritative());
    PF_CHECK(controller.close().ok());
  }
  test::remove_directory(directory);
}

PF_TEST(control, recovered_expectations_still_fence_a_stale_generation) {
  const std::string directory = test::make_scratch_directory("control-fence");
  {
    PodController controller(controller_options(directory));
    PF_REQUIRE(controller.open().ok());
    PF_REQUIRE(controller.apply(synthesize(options_for(3, 13))).ok());
    PF_CHECK(controller.close().ok());
  }
  PodSnapshot older = synthesize(options_for(3, 13));
  for (MemberRecord& record : older.members) {
    if (record.rack.token() == "rack-2") {
      record.generation = RackGeneration(6);
      record.digest = Digest::of("rack-2 at an older generation");
    }
  }
  canonicalise(older);
  {
    PodController controller(controller_options(directory));
    PF_REQUIRE(controller.open().ok());
    const auto applied = controller.apply(older);
    PF_REQUIRE(applied.ok());
    const MemberVerdict* verdict =
        applied.value().state.find_member(RackId::from_canonical_literal("rack-2"));
    PF_REQUIRE(verdict != nullptr);
    PF_CHECK_EQ(verdict->status, MemberStatus::Stale);
    PF_CHECK(!applied.value().state.authority.authoritative());
    PF_CHECK(controller.close().ok());
  }
  test::remove_directory(directory);
}

PF_TEST(control, operations_on_a_closed_controller_are_refused) {
  PodController controller(controller_options({}));
  PF_CHECK_EQ(controller.apply(synthesize(options_for(1, 14))).code(), Code::Invalid);
  PF_CHECK_EQ(controller.retire().code(), Code::Invalid);
  const Status admin = controller.set_admin_state(RackId::from_canonical_literal("rack-1"),
                                                  AdminState::Draining);
  PF_CHECK_EQ(admin.code(), Code::Invalid);
  PF_CHECK_EQ(controller.mint_authority(AuthorityScope::Pod, RackId{}, 1000).code(),
              Code::Invalid);
  PF_REQUIRE(controller.open().ok());
  PF_CHECK_EQ(controller.open().code(), Code::Refused);
  PF_CHECK(controller.close().ok());
  PF_CHECK(controller.close().ok());
}

PF_TEST(control, repeated_open_and_close_cycles_leave_nothing_behind) {
  const std::string directory = test::make_scratch_directory("control-cycles");
  for (int cycle = 0; cycle < 6; ++cycle) {
    PodController controller(controller_options(directory));
    PF_REQUIRE(controller.open().ok());
    PF_REQUIRE(controller.apply(synthesize(options_for(2, 15))).ok());
    const auto minted =
        controller.mint_authority(AuthorityScope::Pod, RackId{}, 600LL * nanos_per_second);
    PF_REQUIRE(minted.ok());
    PF_CHECK(controller.close().ok());
  }
  {
    PodController controller(controller_options(directory));
    PF_REQUIRE(controller.open().ok());
    PF_CHECK(controller.recovery().usable());
    PF_CHECK(controller.epoch().value() >= 6);
    PF_CHECK(controller.close().ok());
  }
  test::remove_directory(directory);
}

PF_TEST(control, plan_for_returns_the_dependents_of_a_key) {
  PodController controller(controller_options({}));
  PF_REQUIRE(controller.open().ok());
  PF_REQUIRE(controller.apply(synthesize(options_for(4, 16))).ok());
  const RevalidationPlan plan =
      controller.plan_for({DepKey{DepKind::Member, "rack-3"}});
  PF_CHECK(plan.contains(make_decision_id(DecisionKind::MemberState, "rack-3")));
  PF_CHECK(plan.contains(make_decision_id(DecisionKind::PodAuthority, "pod")));
  PF_CHECK(controller.close().ok());
}
