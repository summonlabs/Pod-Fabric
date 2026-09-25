// The authoritative pod-level state produced by composition.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "podfabric/core/digest.hpp"
#include "podfabric/core/status.hpp"
#include "podfabric/core/time.hpp"
#include "podfabric/core/token.hpp"
#include "podfabric/model/capacity.hpp"
#include "podfabric/model/obligation.hpp"
#include "podfabric/model/primitives.hpp"
#include "podfabric/model/topology.hpp"

namespace podfabric {

enum class LifecycleState : std::uint8_t {
  Constructing = 0,
  Active = 1,
  Degraded = 2,
  Partitioned = 3,
  Recovering = 4,
  Maintenance = 5,
  Draining = 6,
  Retired = 7,
};
std::string_view to_string(LifecycleState s) noexcept;

// Lifecycle transitions are a total function with an explicit legal set.
// Retired is terminal: nothing leaves it, so authority cannot be resurrected
// from a retired pod.
bool is_legal_transition(LifecycleState from, LifecycleState to) noexcept;
LifecycleState next_lifecycle(LifecycleState current, LifecycleState desired) noexcept;

enum class MemberStatus : std::uint8_t {
  Established = 0,
  Joining = 1,
  Stale = 2,
  Fenced = 3,
  Conflicting = 4,
  Retiring = 5,
  Unsupported = 6,
  Missing = 7,
};
std::string_view to_string(MemberStatus s) noexcept;

enum class LinkStatus : std::uint8_t {
  Usable = 0,
  Degraded = 1,
  Blocked = 2,
  Conflicting = 3,
  Stale = 4,
  Indeterminate = 5,
  Unsupported = 6,
};
std::string_view to_string(LinkStatus s) noexcept;

enum class ObligationStatus : std::uint8_t {
  Satisfied = 0,
  AtRisk = 1,
  Violated = 2,
  Indeterminate = 3,
  Unsupported = 4,
};
std::string_view to_string(ObligationStatus s) noexcept;

enum class DomainStatus : std::uint8_t {
  Ok = 0,
  Degraded = 1,
  Conflicting = 2,
  Incomplete = 3,
  Indeterminate = 4,
};
std::string_view to_string(DomainStatus s) noexcept;

struct MemberVerdict {
  RackId rack{};
  MemberStatus status{MemberStatus::Missing};
  RackGeneration generation{};
  Digest digest{};
  MembershipState membership{MembershipState::Joining};
  AdminState admin{AdminState::Enabled};
  HealthState health{HealthState::Unknown};
  std::vector<DomainId> domains{};
  std::vector<Reason> reasons{};

  friend bool operator==(const MemberVerdict&, const MemberVerdict&) noexcept = default;
};

struct ConnectivityVerdict {
  RackId from{};
  RackId to{};
  LinkStatus status{LinkStatus::Indeterminate};
  ResourceClass resource{};
  std::uint64_t usable_capacity{0};
  std::vector<LinkId> links{};
  std::vector<RouteRef> routes{};
  std::vector<Reason> reasons{};

  friend bool operator==(const ConnectivityVerdict&, const ConnectivityVerdict&) noexcept = default;
};

struct DomainVerdict {
  DomainId id{};
  DomainKind kind{DomainKind::Unknown};
  DomainStatus status{DomainStatus::Ok};
  std::vector<RackId> declared_members{};
  std::vector<RackId> effective_members{};
  std::vector<Reason> reasons{};

  friend bool operator==(const DomainVerdict&, const DomainVerdict&) noexcept = default;
};

struct ObligationVerdict {
  ObligationId id{};
  ObligationKind kind{ObligationKind::CapacityFloor};
  ObligationStatus status{ObligationStatus::Indeterminate};
  std::uint32_t satisfied_domains{0};
  std::vector<Reason> reasons{};

  friend bool operator==(const ObligationVerdict&, const ObligationVerdict&) noexcept = default;
};

enum class DecisionKind : std::uint8_t {
  PodLifecycle = 0,
  MemberState = 1,
  Connectivity = 2,
  DomainState = 3,
  CapacityAggregate = 4,
  DomainCapacity = 5,
  ObligationState = 6,
  PodAuthority = 7,
};
std::string_view to_string(DecisionKind k) noexcept;

enum class DepKind : std::uint8_t {
  Member = 0,
  Link = 1,
  Route = 2,
  Domain = 3,
  Obligation = 4,
  Token = 5,
  Epoch = 6,
  Incarnation = 7,
  Decision = 8,
  // The lifecycle the controller held when the round started, which is an
  // input to the transition rather than an observed fact.
  Lifecycle = 9,
};
std::string_view to_string(DepKind k) noexcept;

// A dependency edge. The subject is the canonical identity of the depended-on
// object; for Epoch and Incarnation it is a rendered counter value.
struct DepKey {
  DepKind kind{DepKind::Member};
  std::string subject{};

  friend bool operator==(const DepKey&, const DepKey&) noexcept = default;
  friend auto operator<=>(const DepKey&, const DepKey&) noexcept = default;
};

// Every derived fact is recorded as a decision with the exact inputs it used.
// Revalidation after a membership change consults this graph; nothing else.
struct DecisionRecord {
  DecisionId id{};
  DecisionKind kind{DecisionKind::PodLifecycle};
  Code status{Code::Ok};
  std::vector<DepKey> deps{};
  // Digest of the canonical encoding of this record. Equal fingerprints mean
  // the decisions are identical byte for byte, which is what makes selective
  // revalidation checkable rather than asserted.
  Digest fingerprint{};
  std::vector<Reason> reasons{};

  friend bool operator==(const DecisionRecord&, const DecisionRecord&) noexcept = default;
};

enum class FenceKind : std::uint8_t { Member = 0, Link = 1, Route = 2, Token = 3, Capacity = 4, Pod = 5 };
std::string_view to_string(FenceKind k) noexcept;

// An instruction to stop using something. Fences are emitted for every object
// whose evidence is stale, conflicting or revoked.
struct FenceAction {
  FenceKind kind{FenceKind::Member};
  std::string subject{};
  Reason reason{};

  friend bool operator==(const FenceAction&, const FenceAction&) noexcept = default;
  friend auto operator<=>(const FenceAction&, const FenceAction&) noexcept = default;
};

// Aggregate authority of the pod itself.
struct PodAuthority {
  // Ok only when the pod can act without qualification.
  Code status{Code::Unknown};
  PodEpoch epoch{};
  Incarnation incarnation{};
  std::uint32_t established_members{0};
  std::uint32_t fenced_members{0};
  std::uint32_t conflicting_members{0};
  std::uint32_t stale_members{0};
  bool recovered{false};
  std::vector<Reason> reasons{};

  bool authoritative() const noexcept { return status == Code::Ok; }

  friend bool operator==(const PodAuthority&, const PodAuthority&) noexcept = default;
};

struct PodState {
  PodId pod{};
  std::string schema{};
  PodEpoch epoch{};
  Incarnation incarnation{};
  LifecycleState lifecycle{LifecycleState::Constructing};
  std::vector<MemberVerdict> members{};
  std::vector<ConnectivityVerdict> connectivity{};
  std::vector<CapacityAggregate> capacity{};
  std::vector<CapacityAggregate> domain_capacity{};
  std::vector<DomainVerdict> domains{};
  std::vector<ObligationVerdict> obligations{};
  std::vector<DecisionRecord> decisions{};
  PodAuthority authority{};
  std::vector<FenceAction> fences{};
  std::vector<Reason> degradations{};
  // Digest of the verdicts alone. It moves when, and only when, the pod's
  // authoritative state moves, which is what "unchanged" means to a caller.
  Digest state_digest{};
  // Digest of the whole document including the decision dependency records,
  // which also record which inputs the round consumed.
  Digest decision_digest{};
  Nanos composed_at{0};
  Provenance provenance{};

  friend bool operator==(const PodState&, const PodState&) noexcept = default;

  const MemberVerdict* find_member(const RackId& rack) const;
  const DomainVerdict* find_domain(const DomainId& id) const;
  const CapacityAggregate* find_capacity(const ResourceClass& resource) const;
  const DecisionRecord* find_decision(const DecisionId& id) const;
};

// Canonicalises every derived collection so that composing the same inputs
// twice produces byte-identical documents.
void canonicalise(PodState& state);

// Deterministic identity of a decision: "<kind>.<subject>".
DecisionId make_decision_id(DecisionKind kind, std::string_view subject);

}  // namespace podfabric
