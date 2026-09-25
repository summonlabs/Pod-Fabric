// The authoritative pod controller.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

#include "podfabric/core/status.hpp"
#include "podfabric/core/time.hpp"
#include "podfabric/engine/composer.hpp"
#include "podfabric/engine/dependency.hpp"
#include "podfabric/model/pod_state.hpp"
#include "podfabric/model/snapshot.hpp"
#include "podfabric/persist/store.hpp"

namespace podfabric {

struct ControllerOptions {
  PodId pod{};
  Policy policy{};
  // Empty means the controller is memory-only: nothing survives a restart and
  // the pod reports its own durability as absent.
  std::filesystem::path store_directory{};
  bool require_authority_token{false};
  Nanos default_lease_duration{120 * nanos_per_second};
  std::size_t max_observers{32};
  // Supplied by the caller and not owned. Defaults to the system clock.
  Clock* clock{nullptr};
};

enum class ApplyOutcome : std::uint8_t {
  Unchanged = 0,   // the composed state digest did not move
  Applied,         // the state moved within the same epoch
  EpochAdvanced,   // a member generation change forced a new pod epoch
  Recovered,       // the first round after a restart
  Retired,         // the pod is retired and refuses further change
};
std::string_view to_string(ApplyOutcome outcome) noexcept;

struct ApplyReport {
  ApplyOutcome outcome{ApplyOutcome::Unchanged};
  PodState state{};
  RevalidationReport revalidation{};
  LifecycleState previous_lifecycle{LifecycleState::Constructing};
  PodEpoch previous_epoch{};
  Digest previous_digest{};
  std::string detail{};

  friend bool operator==(const ApplyReport&, const ApplyReport&) noexcept = default;
};

enum class EventKind : std::uint8_t {
  Recovered = 0,
  Applied,
  LifecycleChanged,
  EpochAdvanced,
  AuthorityGranted,
  AuthorityFenced,
  AdminStateChanged,
  Retired,
};
std::string_view to_string(EventKind kind) noexcept;

struct Event {
  EventKind kind{EventKind::Applied};
  PodEpoch epoch{};
  Incarnation incarnation{};
  LifecycleState lifecycle{LifecycleState::Constructing};
  std::string subject{};
  std::string detail{};
};

// Observer callbacks are invoked with no controller lock held, which is a
// documented invariant rather than an accident: an observer may call back into
// the controller, including from the same thread, without deadlocking.
using Observer = std::function<void(const Event&)>;

class PodController {
 public:
  explicit PodController(ControllerOptions options);
  PodController(const PodController&) = delete;
  PodController& operator=(const PodController&) = delete;
  ~PodController();

  // Opens the store (if any), recovers durable state, and mints a fresh
  // incarnation. The store directory is locked for the lifetime of the object.
  Status open();
  Status close();
  bool is_open() const;

  // ---- reads -------------------------------------------------------------
  PodState state() const;
  PodEpoch epoch() const;
  Incarnation incarnation() const;
  LifecycleState lifecycle() const;
  Digest state_digest() const;
  std::string describe() const;
  persist::RecoveryReport recovery() const;
  std::vector<AuthorityToken> tokens() const;
  std::map<RackId, AdminState> admin_overrides() const;
  RevalidationPlan plan_for(const std::vector<DepKey>& changed) const;
  bool ambiguous_commit() const;

  // ---- writes ------------------------------------------------------------
  // Applies one round of evidence. The controller owns the epoch, the
  // incarnation, the recorded expectations and the authority tokens; the caller
  // supplies only evidence and operator intent.
  Result<ApplyReport> apply(const PodSnapshot& evidence);
  Status set_admin_state(const RackId& rack, AdminState state);
  Status clear_admin_state(const RackId& rack);
  // Terminal: a retired pod never becomes authoritative again.
  Status retire();

  Result<AuthorityToken> mint_authority(AuthorityScope scope, const RackId& subject,
                                        Nanos lifetime);
  Status revoke_authority(const LeaseId& lease);
  AuthorityVerdict check_authority(const AuthorityToken& token, AuthorityScope scope,
                                   const RackId& subject) const;

  Status subscribe(Observer observer);

 private:
  // Everything the compose step needs, copied out under a shared lock so that
  // composition never runs with a lock held.
  struct Inputs {
    PodEpoch epoch{};
    Incarnation incarnation{};
    LifecycleState lifecycle{LifecycleState::Constructing};
    std::vector<MemberExpectation> expectations{};
    std::vector<AuthorityToken> tokens{};
    std::vector<LeaseId> revoked{};
    std::map<RackId, AdminState> admin_overrides{};
    bool recovered{false};
    bool ambiguous{false};
    Nanos recovered_at{0};
  };

  Inputs inputs() const;
  // The composition path, callable only with write_mutex_ held.
  Result<ApplyReport> apply_locked(const PodSnapshot& evidence, std::vector<Event>& events);
  Status persist_state(const PodState& composed,
                       const std::vector<MemberExpectation>& expectations,
                       const std::vector<AuthorityToken>& tokens,
                       const std::vector<LeaseId>& revoked);
  void dispatch();
  void publish(Event event);
  static std::vector<MemberExpectation> derive_expectations(
      const PodState& composed, const std::vector<MemberExpectation>& previous);

  ControllerOptions options_;
  SystemClock default_clock_{};
  Clock* clock_{nullptr};

  mutable std::shared_mutex state_mutex_{};
  std::mutex write_mutex_{};

  std::unique_ptr<persist::PodStore> store_{};
  bool open_{false};
  Incarnation incarnation_{};
  PodEpoch epoch_{};
  LifecycleState lifecycle_{LifecycleState::Constructing};
  // The lifecycle that was supplied as context to the last composition, which
  // is the value the previous lifecycle decision depends on.
  LifecycleState context_lifecycle_{LifecycleState::Constructing};
  PodState state_{};
  std::vector<MemberExpectation> expectations_{};
  std::vector<AuthorityToken> tokens_{};
  std::vector<LeaseId> revoked_{};
  FencingSequence fencing_{};
  std::map<RackId, AdminState> admin_overrides_{};
  PodSnapshot last_evidence_{};
  bool have_last_evidence_{false};
  DecisionIndex index_{};
  bool recovered_round_{false};
  bool restart_epoch_pending_{false};
  bool ambiguous_commit_{false};
  bool durable_{false};
  std::size_t fenced_on_restart_{0};
  Nanos recovered_at_{0};
  persist::RecoveryReport recovery_{};

  std::mutex observer_mutex_{};
  std::mutex queue_mutex_{};
  std::vector<Observer> observers_{};
  std::vector<Event> pending_{};
  bool dispatching_{false};
};

}  // namespace podfabric
