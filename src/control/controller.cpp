// The authoritative pod controller.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "podfabric/control/controller.hpp"

#include <algorithm>
#include <utility>

#include "podfabric/codec/codec.hpp"
#include "podfabric/core/checked.hpp"
#include "podfabric/engine/authority.hpp"
#include "podfabric/version.hpp"

namespace podfabric {
namespace {

LeaseId make_lease_id(const Uuid& id) {
  const auto parsed = LeaseId::parse("lease-" + id.to_string());
  if (parsed.ok()) {
    return parsed.value();
  }
  return LeaseId::from_canonical_literal("lease-invalid");
}

}  // namespace

std::string_view to_string(ApplyOutcome outcome) noexcept {
  switch (outcome) {
    case ApplyOutcome::Unchanged: return "UNCHANGED";
    case ApplyOutcome::Applied: return "APPLIED";
    case ApplyOutcome::EpochAdvanced: return "EPOCH_ADVANCED";
    case ApplyOutcome::Recovered: return "RECOVERED";
    case ApplyOutcome::Retired: return "RETIRED";
  }
  return "UNCHANGED";
}

std::string_view to_string(EventKind kind) noexcept {
  switch (kind) {
    case EventKind::Recovered: return "recovered";
    case EventKind::Applied: return "applied";
    case EventKind::LifecycleChanged: return "lifecycle-changed";
    case EventKind::EpochAdvanced: return "epoch-advanced";
    case EventKind::AuthorityGranted: return "authority-granted";
    case EventKind::AuthorityFenced: return "authority-fenced";
    case EventKind::AdminStateChanged: return "admin-state-changed";
    case EventKind::Retired: return "retired";
  }
  return "applied";
}

PodController::PodController(ControllerOptions options) : options_(std::move(options)) {
  if (options_.policy.require_authority_token != options_.require_authority_token) {
    options_.policy.require_authority_token = options_.require_authority_token;
  }
  clock_ = options_.clock != nullptr ? options_.clock : &default_clock_;
}

PodController::~PodController() { (void)close(); }

Status PodController::open() {
  std::unique_lock<std::mutex> write(write_mutex_);
  if (open_) {
    return Status(Code::Refused, "the pod controller is already open");
  }
  if (options_.pod.is_nil()) {
    return Status(Code::Invalid, "the pod controller has no pod identity");
  }
  PODFABRIC_TRY(validate(options_.policy));
  clock_ = options_.clock != nullptr ? options_.clock : &default_clock_;
  PODFABRIC_TRY_ASSIGN(const Incarnation fresh, Incarnation::fresh());
  incarnation_ = fresh;
  state_ = PodState{};
  state_.pod = options_.pod;
  state_.schema.assign(std::string(pod_state_schema));
  state_.incarnation = incarnation_;
  lifecycle_ = LifecycleState::Constructing;
  epoch_ = PodEpoch(0);
  state_.lifecycle = LifecycleState::Constructing;
  recovered_round_ = false;
  ambiguous_commit_ = false;
  durable_ = false;

  if (!options_.store_directory.empty()) {
    persist::StoreOptions store_options;
    store_options.directory = options_.store_directory;
    store_options.policy = options_.policy;
    store_ = std::make_unique<persist::PodStore>(store_options);
    const Status opened = store_->open();
    recovery_ = store_->recovery();
    if (!opened.ok()) {
      store_.reset();
      return opened;
    }
    durable_ = true;
    const persist::DurableState recovered = store_->state();
    if (!recovered.pod.is_nil()) {
      if (!(recovered.pod == options_.pod)) {
        (void)store_->close();
        store_.reset();
        return Status(Code::Refused, "the store directory belongs to a different pod");
      }
      epoch_ = recovered.epoch;
      lifecycle_ = recovered.lifecycle;
      expectations_ = recovered.expectations;
      tokens_ = recovered.tokens;
      revoked_ = recovered.revoked;
      fencing_ = recovered.fencing;
      admin_overrides_.clear();
      for (const auto& entry : recovered.admin) {
        admin_overrides_[entry.first] = entry.second;
      }
      recovered_round_ = true;
      ambiguous_commit_ = recovery_.ambiguous_commit;
      recovered_at_ = recovery_.recovered_at;
      restart_epoch_pending_ = !(recovered.writer == incarnation_);

      // Every token minted by a previous incarnation is dropped rather than
      // carried across the restart, and the restart itself advances the epoch.
      // Presenting one is fenced on the incarnation that minted it, which is
      // what makes a restarted process unable to resurrect its own authority.
      fenced_on_restart_ = tokens_.size();
      tokens_.clear();

      persist::DurableState next = recovered;
      next.writer = incarnation_;
      next.tokens.clear();
      next.revoked = revoked_;
      if (!(next.pod == options_.pod)) {
        return Status(Code::Refused, "the store directory belongs to a different pod");
      }
      if (lifecycle_ != LifecycleState::Retired) {
        const auto bumped = epoch_.next();
        if (!bumped.ok()) {
          return bumped.status();
        }
        epoch_ = bumped.value();
        next.epoch = epoch_;
        next.lifecycle = LifecycleState::Recovering;
        lifecycle_ = LifecycleState::Recovering;
      }
      next.last_state_digest = Digest{};
      next.admin = recovered.admin;
      const Status committed = store_->commit(next);
      if (!committed.ok()) {
        return committed;
      }
      // The recovered expectations and window are historical: they may fence
      // future evidence, but they never establish anything by themselves.
    }
  }

  state_.epoch = epoch_;
  state_.lifecycle = lifecycle_;
  open_ = true;
  publish(Event{EventKind::Recovered, epoch_, incarnation_, lifecycle_, {},
                (durable_ ? std::string(persist::to_string(recovery_.outcome))
                          : std::string("memory-only")) +
                    " fenced-leases=" + std::to_string(fenced_on_restart_)});
  return Status::success();
}

Status PodController::close() {
  std::unique_lock<std::mutex> write(write_mutex_);
  if (!open_) {
    return Status::success();
  }
  open_ = false;
  if (store_) {
    const Status closed = store_->close();
    store_.reset();
    if (!closed.ok()) {
      return closed;
    }
  }
  {
    std::unique_lock<std::shared_mutex> lock(state_mutex_);
    index_.clear();
  }
  dispatch();
  return Status::success();
}

bool PodController::is_open() const {
  std::shared_lock<std::shared_mutex> lock(state_mutex_);
  return open_;
}

PodState PodController::state() const {
  std::shared_lock<std::shared_mutex> lock(state_mutex_);
  return state_;
}

PodEpoch PodController::epoch() const {
  std::shared_lock<std::shared_mutex> lock(state_mutex_);
  return epoch_;
}

Incarnation PodController::incarnation() const {
  std::shared_lock<std::shared_mutex> lock(state_mutex_);
  return incarnation_;
}

LifecycleState PodController::lifecycle() const {
  std::shared_lock<std::shared_mutex> lock(state_mutex_);
  return lifecycle_;
}

Digest PodController::state_digest() const {
  std::shared_lock<std::shared_mutex> lock(state_mutex_);
  return state_.state_digest;
}

std::string PodController::describe() const {
  std::shared_lock<std::shared_mutex> lock(state_mutex_);
  const auto text = codec::to_text(state_);
  return text.ok() ? text.value() : std::string("state is not renderable");
}

std::vector<AuthorityToken> PodController::tokens() const {
  std::shared_lock<std::shared_mutex> lock(state_mutex_);
  return tokens_;
}

std::map<RackId, AdminState> PodController::admin_overrides() const {
  std::shared_lock<std::shared_mutex> lock(state_mutex_);
  return admin_overrides_;
}

persist::RecoveryReport PodController::recovery() const {
  std::shared_lock<std::shared_mutex> lock(state_mutex_);
  return recovery_;
}

bool PodController::ambiguous_commit() const {
  std::shared_lock<std::shared_mutex> lock(state_mutex_);
  return ambiguous_commit_;
}

RevalidationPlan PodController::plan_for(const std::vector<DepKey>& changed) const {
  std::shared_lock<std::shared_mutex> lock(state_mutex_);
  return index_.plan(changed);
}

PodController::Inputs PodController::inputs() const {
  std::shared_lock<std::shared_mutex> lock(state_mutex_);
  Inputs out;
  out.epoch = epoch_;
  out.incarnation = incarnation_;
  out.lifecycle = lifecycle_;
  out.expectations = expectations_;
  out.tokens = tokens_;
  out.revoked = revoked_;
  out.admin_overrides = admin_overrides_;
  out.recovered = recovered_round_;
  out.ambiguous = ambiguous_commit_;
  out.recovered_at = recovered_at_;
  return out;
}

std::vector<MemberExpectation> PodController::derive_expectations(
    const PodState& composed, const std::vector<MemberExpectation>& previous) {
  std::map<RackId, MemberExpectation> prior;
  for (const MemberExpectation& expectation : previous) {
    prior[expectation.rack] = expectation;
  }
  std::vector<MemberExpectation> next;
  next.reserve(composed.members.size());
  for (const MemberVerdict& verdict : composed.members) {
    if (verdict.status == MemberStatus::Retiring) {
      continue;  // a rack that left the pod leaves no expectation behind
    }
    MemberExpectation expectation;
    expectation.rack = verdict.rack;
    if (verdict.status == MemberStatus::Established) {
      expectation.generation = verdict.generation;
      expectation.digest = verdict.digest;
      expectation.membership = MembershipState::Established;
    } else {
      // Keep what the pod already believed. Dropping it would let a stale
      // generation look like a first sighting on the next round.
      const auto it = prior.find(verdict.rack);
      if (it == prior.end()) {
        continue;
      }
      expectation = it->second;
    }
    next.push_back(std::move(expectation));
  }
  std::sort(next.begin(), next.end());
  return next;
}

Status PodController::persist_state(const PodState& composed,
                                    const std::vector<MemberExpectation>& expectations,
                                    const std::vector<AuthorityToken>& tokens,
                                    const std::vector<LeaseId>& revoked) {
  if (!store_) {
    return Status::success();
  }
  persist::DurableState next;
  next.pod = options_.pod;
  next.epoch = composed.epoch;
  next.writer = incarnation_;
  next.lifecycle = composed.lifecycle;
  next.expectations = expectations;
  next.tokens = tokens;
  next.revoked = revoked;
  next.fencing = fencing_;
  next.last_state_digest = composed.state_digest;
  next.written_at = clock_->now();
  {
    std::shared_lock<std::shared_mutex> lock(state_mutex_);
    next.admin.clear();
    for (const auto& entry : admin_overrides_) {
      next.admin.emplace_back(entry.first, entry.second);
    }
  }
  std::sort(next.admin.begin(), next.admin.end());
  const Status status = store_->commit(next);
  if (!status.ok()) {
    return status;
  }
  recovered_round_ = false;
  ambiguous_commit_ = false;
  restart_epoch_pending_ = false;
  return Status::success();
}

Result<ApplyReport> PodController::apply(const PodSnapshot& evidence) {
  std::vector<Event> events;
  Result<ApplyReport> result = Status(Code::Internal, "composition did not run");
  {
    std::unique_lock<std::mutex> write(write_mutex_);
    result = apply_locked(evidence, events);
  }
  for (Event& event : events) {
    publish(std::move(event));
  }
  dispatch();
  return result;
}

Result<ApplyReport> PodController::apply_locked(const PodSnapshot& evidence,
                                                std::vector<Event>& events) {
  ApplyReport report;
  {
    if (!open_) {
      return Status(Code::Invalid, "the pod controller is not open");
    }
    const Inputs in = inputs();

    // Operator intent is layered above evidence: an operator may take a rack
    // out of service, but evidence may never take it back in.
    PodSnapshot effective = evidence;
    for (MemberRecord& member : effective.members) {
      const auto it = in.admin_overrides.find(member.rack);
      if (it != in.admin_overrides.end()) {
        member.admin = it->second;
      }
    }

    CompositionRequest request;
    request.snapshot = std::move(effective);
    request.policy = options_.policy;
    request.context.epoch = in.epoch;
    request.context.incarnation = in.incarnation;
    request.context.expectations = in.expectations;
    request.context.granted_tokens = in.tokens;
    request.context.revoked_tokens = in.revoked;
    request.context.recovered_from_store = in.recovered;
    request.context.recovered_at = in.recovered_at;
    request.context.ambiguous_commit = in.ambiguous;
    request.context.lifecycle = in.lifecycle;
    request.context.now = clock_->now();

    PODFABRIC_TRY_ASSIGN(const PodState composed, compose(request));

    const std::vector<MemberExpectation> derived =
        derive_expectations(composed, in.expectations);

    std::vector<AuthorityToken> surviving;
    for (const AuthorityToken& token : in.tokens) {
      AuthorityCheck check;
      check.pod = options_.pod;
      check.epoch = composed.epoch;
      check.incarnation = incarnation_;
      check.now = clock_->now();
      check.clock_skew = options_.policy.clock_skew;
      check.revoked = in.revoked;
      const AuthorityVerdict verdict = validate_token(token, check);
      if (verdict.valid()) {
        surviving.push_back(token);
      } else {
        events.push_back(Event{EventKind::AuthorityFenced, composed.epoch, incarnation_,
                               composed.lifecycle, token.lease.token(), verdict.detail});
      }
    }

    PODFABRIC_TRY(persist_state(composed, derived, surviving, in.revoked));

    DecisionIndex before;
    PodSnapshot previous_evidence;
    bool had_previous_evidence = false;
    PodEpoch previous_epoch;
    LifecycleState previous_lifecycle = LifecycleState::Constructing;
    LifecycleState previous_context_lifecycle = LifecycleState::Constructing;
    Digest previous_digest;
    {
      std::unique_lock<std::shared_mutex> lock(state_mutex_);
      before = index_;
      previous_evidence = last_evidence_;
      had_previous_evidence = have_last_evidence_;
      previous_epoch = epoch_;
      previous_lifecycle = lifecycle_;
      previous_context_lifecycle = context_lifecycle_;
      previous_digest = state_.state_digest;
    }

    std::vector<DepKey> changed = membership_change_keys(in.expectations, derived);
    if (had_previous_evidence) {
      const std::vector<DepKey> evidence_changes =
          snapshot_change_keys(previous_evidence, request.snapshot);
      changed.insert(changed.end(), evidence_changes.begin(), evidence_changes.end());
    }
    const std::vector<DepKey> authority_changes =
        token_change_keys(in.tokens, in.revoked, surviving, in.revoked);
    changed.insert(changed.end(), authority_changes.begin(), authority_changes.end());
    if (!(in.epoch == composed.epoch)) {
      changed.push_back(DepKey{DepKind::Epoch, std::to_string(in.epoch.value())});
    }
    if (!(in.incarnation == incarnation_)) {
      changed.push_back(DepKey{DepKind::Incarnation, in.incarnation.to_string()});
    }
    if (previous_context_lifecycle != in.lifecycle) {
      changed.push_back(
          DepKey{DepKind::Lifecycle, std::string(to_string(previous_context_lifecycle))});
    }

    {
      std::unique_lock<std::shared_mutex> lock(state_mutex_);
      state_ = composed;
      epoch_ = composed.epoch;
      lifecycle_ = composed.lifecycle;
      expectations_ = derived;
      tokens_ = surviving;
      last_evidence_ = request.snapshot;
      have_last_evidence_ = true;
      context_lifecycle_ = in.lifecycle;
      recovered_round_ = false;
      ambiguous_commit_ = false;
      index_.clear();
      for (const DecisionRecord& record : composed.decisions) {
        index_.publish(record);
      }
    }

    report.previous_epoch = previous_epoch;
    report.previous_lifecycle = previous_lifecycle;
    report.previous_digest = previous_digest;
    report.state = composed;
    report.revalidation = revalidate(before, composed, changed);

    if (in.recovered) {
      report.outcome = ApplyOutcome::Recovered;
    } else if (previous_lifecycle == LifecycleState::Retired) {
      report.outcome = ApplyOutcome::Retired;
    } else if (!(previous_epoch == composed.epoch)) {
      report.outcome = ApplyOutcome::EpochAdvanced;
    } else if (previous_digest == composed.state_digest &&
               previous_lifecycle == composed.lifecycle) {
      report.outcome = ApplyOutcome::Unchanged;
    } else {
      report.outcome = ApplyOutcome::Applied;
    }
    report.detail = report.revalidation.describe();

    if (in.recovered) {
      events.push_back(Event{EventKind::Recovered, composed.epoch, incarnation_,
                             composed.lifecycle, {},
                             std::string(persist::to_string(recovery_.outcome))});
    }
    if (!(previous_epoch == composed.epoch)) {
      events.push_back(Event{EventKind::EpochAdvanced, composed.epoch, incarnation_,
                             composed.lifecycle, {},
                             "epoch " + std::to_string(previous_epoch.value()) + " -> " +
                                 std::to_string(composed.epoch.value())});
    }
    if (previous_lifecycle != composed.lifecycle) {
      events.push_back(Event{EventKind::LifecycleChanged, composed.epoch, incarnation_,
                             composed.lifecycle, {},
                             std::string(to_string(previous_lifecycle)) + " -> " +
                                 std::string(to_string(composed.lifecycle))});
    }
    events.push_back(Event{EventKind::Applied, composed.epoch, incarnation_, composed.lifecycle,
                           {}, std::string(to_string(report.outcome))});
  }
  return report;
}

Status PodController::set_admin_state(const RackId& rack, AdminState state) {
  std::vector<Event> events;
  {
    std::unique_lock<std::mutex> write(write_mutex_);
    if (!open_) {
      return Status(Code::Invalid, "the pod controller is not open");
    }
    if (rack.is_nil()) {
      return Status(Code::Invalid, "the admin-state request names no rack");
    }
    PodSnapshot evidence;
    bool have_evidence = false;
    {
      std::unique_lock<std::shared_mutex> lock(state_mutex_);
      if (lifecycle_ == LifecycleState::Retired) {
        return Status(Code::Refused, "the pod is retired");
      }
      admin_overrides_[rack] = state;
      evidence = last_evidence_;
      have_evidence = have_last_evidence_;
    }
    if (have_evidence) {
      // Recompose straight away so the operator sees the effect without waiting
      // for the next evidence round.
      const auto recomposed = apply_locked(evidence, events);
      if (!recomposed.ok()) {
        return recomposed.status();
      }
    } else {
      PODFABRIC_TRY(persist_state(state_, expectations_, tokens_, revoked_));
    }
    {
      std::shared_lock<std::shared_mutex> lock(state_mutex_);
      events.push_back(Event{EventKind::AdminStateChanged, epoch_, incarnation_, lifecycle_,
                             rack.token(), std::string(to_string(state))});
    }
  }
  for (Event& event : events) {
    publish(std::move(event));
  }
  dispatch();
  return Status::success();
}

Status PodController::clear_admin_state(const RackId& rack) {
  std::vector<Event> events;
  {
    std::unique_lock<std::mutex> write(write_mutex_);
    if (!open_) {
      return Status(Code::Invalid, "the pod controller is not open");
    }
    PodSnapshot evidence;
    bool have_evidence = false;
    {
      std::unique_lock<std::shared_mutex> lock(state_mutex_);
      if (lifecycle_ == LifecycleState::Retired) {
        return Status(Code::Refused, "the pod is retired");
      }
      admin_overrides_.erase(rack);
      evidence = last_evidence_;
      have_evidence = have_last_evidence_;
    }
    if (have_evidence) {
      const auto recomposed = apply_locked(evidence, events);
      if (!recomposed.ok()) {
        return recomposed.status();
      }
    } else {
      PODFABRIC_TRY(persist_state(state_, expectations_, tokens_, revoked_));
    }
    {
      std::shared_lock<std::shared_mutex> lock(state_mutex_);
      events.push_back(Event{EventKind::AdminStateChanged, epoch_, incarnation_, lifecycle_,
                             rack.token(), "enabled"});
    }
  }
  for (Event& event : events) {
    publish(std::move(event));
  }
  dispatch();
  return Status::success();
}

Status PodController::retire() {
  std::vector<Event> events;
  {
    std::unique_lock<std::mutex> write(write_mutex_);
    if (!open_) {
      return Status(Code::Invalid, "the pod controller is not open");
    }
    {
      std::unique_lock<std::shared_mutex> lock(state_mutex_);
      if (lifecycle_ == LifecycleState::Retired) {
        return Status(Code::Refused, "the pod is already retired");
      }
      lifecycle_ = LifecycleState::Retired;
      state_.lifecycle = LifecycleState::Retired;
      state_.authority.status = Code::Refused;
      state_.authority.reasons.push_back(
          Reason{ReasonCode::MemberRetiring, "the pod is retired"});
      for (const AuthorityToken& token : tokens_) {
        revoked_.push_back(token.lease);
      }
      tokens_.clear();
    }
    PODFABRIC_TRY(persist_state(state_, expectations_, tokens_, revoked_));
    events.push_back(Event{EventKind::Retired, epoch_, incarnation_, LifecycleState::Retired, {},
                           "the pod is retired and will not become authoritative again"});
  }
  for (Event& event : events) {
    publish(std::move(event));
  }
  dispatch();
  return Status::success();
}

Result<AuthorityToken> PodController::mint_authority(AuthorityScope scope, const RackId& subject,
                                                     Nanos lifetime) {
  AuthorityToken granted;
  std::vector<Event> events;
  {
    std::unique_lock<std::mutex> write(write_mutex_);
    if (!open_) {
      return Status(Code::Invalid, "the pod controller is not open");
    }
    PODFABRIC_TRY_ASSIGN(const Uuid id, Uuid::random());
    AuthorityToken token;
    token.lease = make_lease_id(id);
    token.pod = options_.pod;
    token.scope = scope;
    token.subject = subject;
    token.incarnation = incarnation_;
    token.issued_at = clock_->now();
    const Nanos duration = lifetime > 0 ? lifetime : options_.default_lease_duration;
    const auto expiry = checked_add<Nanos>(token.issued_at, duration);
    if (!expiry.ok()) {
      return expiry.status();
    }
    token.expires_at = expiry.value();

    std::vector<AuthorityToken> next;
    {
      std::unique_lock<std::shared_mutex> lock(state_mutex_);
      if (lifecycle_ == LifecycleState::Retired) {
        return Status(Code::Refused, "the pod is retired and mints no authority");
      }
      const auto fencing = fencing_.next();
      if (!fencing.ok()) {
        return fencing.status();
      }
      fencing_ = fencing.value();
      token.fencing = fencing_;
      token.epoch = epoch_;
      std::string material("podfabric.authority/1|");
      material.append(options_.pod.token());
      material.append("|");
      material.append(std::to_string(epoch_.value()));
      material.append("|");
      material.append(to_string(scope));
      material.append("|");
      material.append(subject.token());
      material.append("|");
      material.append(token.lease.token());
      token.payload_digest = Digest::of(material);
      next = tokens_;
      next.push_back(token);
      granted = token;
    }
    PODFABRIC_TRY(persist_state(state_, expectations_, next, revoked_));
    {
      std::unique_lock<std::shared_mutex> lock(state_mutex_);
      tokens_ = next;
    }
    events.push_back(Event{EventKind::AuthorityGranted, epoch_, incarnation_, lifecycle_,
                           token.lease.token(), std::string(to_string(scope))});
  }
  for (Event& event : events) {
    publish(std::move(event));
  }
  dispatch();
  return granted;
}

Status PodController::revoke_authority(const LeaseId& lease) {
  std::vector<Event> events;
  {
    std::unique_lock<std::mutex> write(write_mutex_);
    if (!open_) {
      return Status(Code::Invalid, "the pod controller is not open");
    }
    if (lease.is_nil()) {
      return Status(Code::Invalid, "the revocation names no lease");
    }
    std::vector<AuthorityToken> next;
    {
      std::unique_lock<std::shared_mutex> lock(state_mutex_);
      if (std::find(revoked_.begin(), revoked_.end(), lease) != revoked_.end()) {
        return Status(Code::AlreadyExists, "the lease is already revoked");
      }
      revoked_.push_back(lease);
      for (const AuthorityToken& token : tokens_) {
        if (!(token.lease == lease)) {
          next.push_back(token);
        }
      }
    }
    PODFABRIC_TRY(persist_state(state_, expectations_, next, revoked_));
    {
      std::unique_lock<std::shared_mutex> lock(state_mutex_);
      tokens_ = next;
    }
    events.push_back(Event{EventKind::AuthorityFenced, epoch_, incarnation_, lifecycle_,
                           lease.token(), "revoked"});
  }
  for (Event& event : events) {
    publish(std::move(event));
  }
  dispatch();
  return Status::success();
}

AuthorityVerdict PodController::check_authority(const AuthorityToken& token, AuthorityScope scope,
                                                const RackId& subject) const {
  std::shared_lock<std::shared_mutex> lock(state_mutex_);
  AuthorityCheck check;
  check.pod = options_.pod;
  check.epoch = epoch_;
  check.incarnation = incarnation_;
  check.now = clock_->now();
  check.clock_skew = options_.policy.clock_skew;
  check.revoked = revoked_;
  check.required = scope;
  check.enforce_scope = true;
  const AuthorityVerdict verdict = validate_token(token, check);
  if (!verdict.valid()) {
    return verdict;
  }
  if (!subject.is_nil() && token.scope != AuthorityScope::Pod && !(token.subject == subject)) {
    AuthorityVerdict mismatch;
    mismatch.code = AuthorityVerdictCode::WrongScope;
    mismatch.status = Code::Refused;
    mismatch.detail = "token subject does not match the requested subject";
    return mismatch;
  }
  return verdict;
}

Status PodController::subscribe(Observer observer) {
  if (!observer) {
    return Status(Code::Invalid, "observer is empty");
  }
  std::lock_guard<std::mutex> guard(observer_mutex_);
  if (observers_.size() >= options_.max_observers) {
    return Status(Code::Exhausted, "the controller already has the maximum number of observers");
  }
  observers_.push_back(std::move(observer));
  return Status::success();
}

void PodController::publish(Event event) {
  std::lock_guard<std::mutex> guard(queue_mutex_);
  if (pending_.size() >= 1024) {
    return;  // bounded: a runaway observer cannot grow the queue without limit
  }
  pending_.push_back(std::move(event));
}

// Drains the pending event queue with no controller lock held. Every producer
// calls dispatch() after enqueuing, and the emptiness check happens under the
// same mutex as the enqueue, so no event can be stranded between a producer and
// a dispatcher.
void PodController::dispatch() {
  for (;;) {
    std::vector<Event> batch;
    {
      std::lock_guard<std::mutex> guard(queue_mutex_);
      if (pending_.empty()) {
        dispatching_ = false;
        return;
      }
      batch.swap(pending_);
      dispatching_ = true;
    }
    std::vector<Observer> observers;
    {
      std::lock_guard<std::mutex> guard(observer_mutex_);
      observers = observers_;
    }
    for (const Event& event : batch) {
      for (const Observer& observer : observers) {
        try {
          observer(event);
        } catch (...) {
          // An observer must never take the controller down, and it runs with
          // no lock held, so swallowing here cannot mask a lock defect.
        }
      }
    }
    std::lock_guard<std::mutex> guard(queue_mutex_);
    if (pending_.empty()) {
      dispatching_ = false;
      return;
    }
  }
}

}  // namespace podfabric
