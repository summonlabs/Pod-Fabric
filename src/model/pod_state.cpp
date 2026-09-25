// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "podfabric/model/pod_state.hpp"

#include <algorithm>

#include "podfabric/version.hpp"

namespace podfabric {
namespace {

// Legal lifecycle transitions.
//
// Constructing may only become Active (a pod that never reached quorum is still
// constructing). Active and Degraded interchange freely. Partitioned is entered
// from any live state and must pass through Recovering. Maintenance and Draining
// are administrative and return to the live states. Retired is terminal.
bool legal(LifecycleState from, LifecycleState to) noexcept {
  if (from == to) {
    return true;
  }
  switch (from) {
    case LifecycleState::Constructing:
      return to == LifecycleState::Active || to == LifecycleState::Degraded ||
             to == LifecycleState::Partitioned || to == LifecycleState::Recovering ||
             to == LifecycleState::Maintenance || to == LifecycleState::Draining ||
             to == LifecycleState::Retired;
    case LifecycleState::Active:
      return to == LifecycleState::Degraded || to == LifecycleState::Partitioned ||
             to == LifecycleState::Recovering || to == LifecycleState::Maintenance ||
             to == LifecycleState::Draining || to == LifecycleState::Retired;
    case LifecycleState::Degraded:
      return to == LifecycleState::Active || to == LifecycleState::Partitioned ||
             to == LifecycleState::Recovering || to == LifecycleState::Maintenance ||
             to == LifecycleState::Draining || to == LifecycleState::Retired;
    case LifecycleState::Partitioned:
      return to == LifecycleState::Recovering || to == LifecycleState::Retired;
    case LifecycleState::Recovering:
      return to == LifecycleState::Active || to == LifecycleState::Degraded ||
             to == LifecycleState::Partitioned || to == LifecycleState::Retired;
    case LifecycleState::Maintenance:
      return to == LifecycleState::Active || to == LifecycleState::Degraded ||
             to == LifecycleState::Partitioned || to == LifecycleState::Recovering ||
             to == LifecycleState::Retired;
    case LifecycleState::Draining:
      return to == LifecycleState::Active || to == LifecycleState::Degraded ||
             to == LifecycleState::Partitioned || to == LifecycleState::Recovering ||
             to == LifecycleState::Maintenance || to == LifecycleState::Retired;
    case LifecycleState::Retired:
      return false;
  }
  return false;
}

}  // namespace

std::string_view to_string(LifecycleState s) noexcept {
  switch (s) {
    case LifecycleState::Constructing: return "constructing";
    case LifecycleState::Active: return "active";
    case LifecycleState::Degraded: return "degraded";
    case LifecycleState::Partitioned: return "partitioned";
    case LifecycleState::Recovering: return "recovering";
    case LifecycleState::Maintenance: return "maintenance";
    case LifecycleState::Draining: return "draining";
    case LifecycleState::Retired: return "retired";
  }
  return "constructing";
}

bool is_legal_transition(LifecycleState from, LifecycleState to) noexcept { return legal(from, to); }

LifecycleState next_lifecycle(LifecycleState current, LifecycleState desired) noexcept {
  if (current == LifecycleState::Retired) {
    return LifecycleState::Retired;  // terminal: nothing leaves and nothing re-enters
  }
  if (legal(current, desired)) {
    return desired;
  }
  if (current == LifecycleState::Partitioned &&
      (desired == LifecycleState::Active || desired == LifecycleState::Degraded)) {
    // A healed partition reports Recovering first; the next round may report
    // the live state.
    return LifecycleState::Recovering;
  }
  // An illegal request never silently applies. A partition heals through
  // Recovering before it may report Active again, so a pod cannot pretend to
  // be healthy in the same round in which it regained connectivity.
  if (desired == LifecycleState::Active || desired == LifecycleState::Degraded) {
    if (current == LifecycleState::Recovering) {
      return desired;
    }
    return LifecycleState::Partitioned;
  }
  if (desired == LifecycleState::Recovering) {
    return LifecycleState::Partitioned;
  }
  return current;
}

std::string_view to_string(MemberStatus s) noexcept {
  switch (s) {
    case MemberStatus::Established: return "established";
    case MemberStatus::Joining: return "joining";
    case MemberStatus::Stale: return "stale";
    case MemberStatus::Fenced: return "fenced";
    case MemberStatus::Conflicting: return "conflicting";
    case MemberStatus::Retiring: return "retiring";
    case MemberStatus::Unsupported: return "unsupported";
    case MemberStatus::Missing: return "missing";
  }
  return "missing";
}

std::string_view to_string(LinkStatus s) noexcept {
  switch (s) {
    case LinkStatus::Usable: return "usable";
    case LinkStatus::Degraded: return "degraded";
    case LinkStatus::Blocked: return "blocked";
    case LinkStatus::Conflicting: return "conflicting";
    case LinkStatus::Stale: return "stale";
    case LinkStatus::Indeterminate: return "indeterminate";
    case LinkStatus::Unsupported: return "unsupported";
  }
  return "indeterminate";
}

std::string_view to_string(ObligationStatus s) noexcept {
  switch (s) {
    case ObligationStatus::Satisfied: return "satisfied";
    case ObligationStatus::AtRisk: return "at-risk";
    case ObligationStatus::Violated: return "violated";
    case ObligationStatus::Indeterminate: return "indeterminate";
    case ObligationStatus::Unsupported: return "unsupported";
  }
  return "indeterminate";
}

std::string_view to_string(DomainStatus s) noexcept {
  switch (s) {
    case DomainStatus::Ok: return "ok";
    case DomainStatus::Degraded: return "degraded";
    case DomainStatus::Conflicting: return "conflicting";
    case DomainStatus::Incomplete: return "incomplete";
    case DomainStatus::Indeterminate: return "indeterminate";
  }
  return "indeterminate";
}

std::string_view to_string(DecisionKind k) noexcept {
  switch (k) {
    case DecisionKind::PodLifecycle: return "lifecycle";
    case DecisionKind::MemberState: return "member";
    case DecisionKind::Connectivity: return "connectivity";
    case DecisionKind::DomainState: return "domain";
    case DecisionKind::CapacityAggregate: return "capacity";
    case DecisionKind::DomainCapacity: return "domain-capacity";
    case DecisionKind::ObligationState: return "obligation";
    case DecisionKind::PodAuthority: return "authority";
  }
  return "lifecycle";
}

std::string_view to_string(DepKind k) noexcept {
  switch (k) {
    case DepKind::Member: return "member";
    case DepKind::Link: return "link";
    case DepKind::Route: return "route";
    case DepKind::Domain: return "domain";
    case DepKind::Obligation: return "obligation";
    case DepKind::Token: return "token";
    case DepKind::Epoch: return "epoch";
    case DepKind::Incarnation: return "incarnation";
    case DepKind::Decision: return "decision";
    case DepKind::Lifecycle: return "lifecycle";
  }
  return "member";
}

std::string_view to_string(FenceKind k) noexcept {
  switch (k) {
    case FenceKind::Member: return "member";
    case FenceKind::Link: return "link";
    case FenceKind::Route: return "route";
    case FenceKind::Token: return "token";
    case FenceKind::Capacity: return "capacity";
    case FenceKind::Pod: return "pod";
  }
  return "member";
}

DecisionId make_decision_id(DecisionKind kind, std::string_view subject) {
  std::string token(to_string(kind));
  token.push_back('.');
  token.append(subject);
  // Decision identities are derived, so a non-canonical subject (for example an
  // opaque token rendered with characters outside the canonical set) is folded
  // into a canonical form rather than rejected: the identity only has to be
  // stable and unique, not pretty.
  for (char& c : token) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-' ||
                    c == '_' || c == ':';
    if (!ok) {
      c = '_';
    }
  }
  const auto parsed = DecisionId::parse(token);
  if (parsed.ok()) {
    return parsed.value();
  }
  return DecisionId::from_canonical_literal("decision.invalid");
}

void canonicalise(PodState& state) {
  std::sort(state.members.begin(), state.members.end(),
            [](const MemberVerdict& a, const MemberVerdict& b) { return a.rack < b.rack; });
  std::sort(state.connectivity.begin(), state.connectivity.end(),
            [](const ConnectivityVerdict& a, const ConnectivityVerdict& b) {
              if (a.from != b.from) return a.from < b.from;
              return a.to < b.to;
            });
  std::sort(state.capacity.begin(), state.capacity.end(),
            [](const CapacityAggregate& a, const CapacityAggregate& b) {
              return a.resource < b.resource;
            });
  std::sort(state.domain_capacity.begin(), state.domain_capacity.end(),
            [](const CapacityAggregate& a, const CapacityAggregate& b) {
              if (a.resource != b.resource) return a.resource < b.resource;
              return a.domain < b.domain;
            });
  std::sort(state.domains.begin(), state.domains.end(),
            [](const DomainVerdict& a, const DomainVerdict& b) { return a.id < b.id; });
  std::sort(state.obligations.begin(), state.obligations.end(),
            [](const ObligationVerdict& a, const ObligationVerdict& b) { return a.id < b.id; });
  std::sort(state.decisions.begin(), state.decisions.end(),
            [](const DecisionRecord& a, const DecisionRecord& b) { return a.id < b.id; });
  std::sort(state.fences.begin(), state.fences.end());

  for (auto& m : state.members) {
    canonicalise(m.domains);
    canonicalise(m.reasons);
  }
  for (auto& c : state.connectivity) {
    std::sort(c.links.begin(), c.links.end());
    std::sort(c.routes.begin(), c.routes.end());
    canonicalise(c.reasons);
  }
  for (auto& c : state.capacity) {
    std::sort(c.contributions.begin(), c.contributions.end());
    canonicalise(c.reasons);
  }
  for (auto& c : state.domain_capacity) {
    std::sort(c.contributions.begin(), c.contributions.end());
    canonicalise(c.reasons);
  }
  for (auto& d : state.domains) {
    std::sort(d.declared_members.begin(), d.declared_members.end());
    std::sort(d.effective_members.begin(), d.effective_members.end());
    canonicalise(d.reasons);
  }
  for (auto& o : state.obligations) {
    canonicalise(o.reasons);
  }
  for (auto& d : state.decisions) {
    std::sort(d.deps.begin(), d.deps.end());
    d.deps.erase(std::unique(d.deps.begin(), d.deps.end()), d.deps.end());
    canonicalise(d.reasons);
  }
  canonicalise(state.degradations);
}

const MemberVerdict* PodState::find_member(const RackId& rack) const {
  const auto it = std::lower_bound(
      members.begin(), members.end(), rack,
      [](const MemberVerdict& v, const RackId& id) { return v.rack < id; });
  if (it == members.end() || !(it->rack == rack)) {
    return nullptr;
  }
  return &*it;
}

const DomainVerdict* PodState::find_domain(const DomainId& id) const {
  const auto it = std::lower_bound(
      domains.begin(), domains.end(), id,
      [](const DomainVerdict& v, const DomainId& key) { return v.id < key; });
  if (it == domains.end() || !(it->id == id)) {
    return nullptr;
  }
  return &*it;
}

const CapacityAggregate* PodState::find_capacity(const ResourceClass& resource) const {
  const auto it = std::lower_bound(
      capacity.begin(), capacity.end(), resource,
      [](const CapacityAggregate& v, const ResourceClass& key) { return v.resource < key; });
  if (it == capacity.end() || !(it->resource == resource)) {
    return nullptr;
  }
  return &*it;
}

const DecisionRecord* PodState::find_decision(const DecisionId& id) const {
  const auto it = std::lower_bound(
      decisions.begin(), decisions.end(), id,
      [](const DecisionRecord& v, const DecisionId& key) { return v.id < key; });
  if (it == decisions.end() || !(it->id == id)) {
    return nullptr;
  }
  return &*it;
}

}  // namespace podfabric
