// Generation, incarnation, provenance and authority primitives.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "podfabric/core/checked.hpp"
#include "podfabric/core/digest.hpp"
#include "podfabric/core/status.hpp"
#include "podfabric/core/time.hpp"
#include "podfabric/core/token.hpp"

namespace podfabric {

// A strictly monotonic counter. Generations, epochs and sequence numbers are
// distinct types so that an epoch can never be compared with a generation by
// accident.
template <class Tag>
class Sequence {
 public:
  constexpr Sequence() noexcept = default;
  explicit constexpr Sequence(std::uint64_t value) noexcept : value_(value) {}

  constexpr std::uint64_t value() const noexcept { return value_; }
  constexpr bool is_zero() const noexcept { return value_ == 0; }

  // Overflow of a generation counter is an error, never a wrap.
  Result<Sequence> next() const {
    PODFABRIC_TRY_ASSIGN(const std::uint64_t bumped, checked_add<std::uint64_t>(value_, 1));
    return Sequence(bumped);
  }

  friend constexpr bool operator==(Sequence, Sequence) noexcept = default;
  friend constexpr auto operator<=>(Sequence, Sequence) noexcept = default;

 private:
  std::uint64_t value_{0};
};

struct RackGenerationTag;
struct PodEpochTag;
struct RouteGenerationTag;
struct LogSequenceTag;
struct FencingSequenceTag;

using RackGeneration = Sequence<RackGenerationTag>;
using PodEpoch = Sequence<PodEpochTag>;
using RouteGeneration = Sequence<RouteGenerationTag>;
using LogSequence = Sequence<LogSequenceTag>;
using FencingSequence = Sequence<FencingSequenceTag>;

// Identity of one controller process lifetime. Changes on every start, so any
// artefact stamped with a previous incarnation is fenced even if its lease has
// not expired.
class Incarnation {
 public:
  constexpr Incarnation() noexcept = default;
  explicit Incarnation(Uuid id) noexcept : id_(id) {}

  static Result<Incarnation> fresh();
  static Result<Incarnation> parse(std::string_view text);
  static Incarnation from_uuid(Uuid id) noexcept { return Incarnation(id); }

  const Uuid& id() const noexcept { return id_; }
  bool is_nil() const noexcept { return id_.is_nil(); }
  std::string to_string() const { return id_.to_string(); }

  friend bool operator==(const Incarnation&, const Incarnation&) noexcept = default;
  friend auto operator<=>(const Incarnation&, const Incarnation&) noexcept = default;

 private:
  Uuid id_{};
};

// Where a piece of evidence came from and whether it describes real hardware.
// SYNTHETIC evidence is legitimate (recorded traces, generated topologies) but
// is never presented as REAL.
enum class EvidenceQuality : std::uint8_t { Real = 0, Synthetic = 1, Unknown = 2 };

// How the evidence was obtained.
enum class EvidenceClass : std::uint8_t {
  Attested = 0,   // signed/verified report from the owning component
  Observed = 1,   // direct observation of live state
  Declared = 2,   // configuration or intent
  Reconstructed = 3,  // recovered from durable state after a restart
};

std::string_view to_string(EvidenceQuality q) noexcept;
std::string_view to_string(EvidenceClass c) noexcept;

struct Provenance {
  EvidenceQuality quality{EvidenceQuality::Unknown};
  EvidenceClass klass{EvidenceClass::Declared};
  NodeId source{};
  std::uint64_t source_sequence{0};
  std::string detail{};

  friend bool operator==(const Provenance&, const Provenance&) noexcept = default;
  friend auto operator<=>(const Provenance&, const Provenance&) noexcept = default;
};

// Coarse health as reported by the owning subsystem. Pod Fabric never
// re-derives rack-internal health; it only consumes the verdict.
enum class HealthState : std::uint8_t { Ok = 0, Impaired = 1, Critical = 2, Unknown = 3 };
std::string_view to_string(HealthState h) noexcept;

enum class AdminState : std::uint8_t { Enabled = 0, Disabled = 1, Maintenance = 2, Draining = 3 };
std::string_view to_string(AdminState s) noexcept;

enum class OperationalState : std::uint8_t { Up = 0, Degraded = 1, Down = 2, Unknown = 3 };
std::string_view to_string(OperationalState s) noexcept;

// Membership of a rack in the pod.
enum class MembershipState : std::uint8_t {
  Joining = 0,
  Established = 1,
  Leaving = 2,
  Fenced = 3,
  Retired = 4,
};
std::string_view to_string(MembershipState s) noexcept;

// Scope of an authority grant.
enum class AuthorityScope : std::uint8_t {
  Pod = 0,
  Member = 1,
  Link = 2,
  CapacityReservation = 3,
  MaintenanceWindow = 4,
};
std::string_view to_string(AuthorityScope s) noexcept;

// A fencing token. Validity is a conjunction: right pod, right epoch, right
// controller incarnation, unexpired, unrevoked. Failing any one of those makes
// the token STALE or REFUSED, never "probably fine".
struct AuthorityToken {
  LeaseId lease{};
  PodId pod{};
  PodEpoch epoch{};
  Incarnation incarnation{};
  AuthorityScope scope{AuthorityScope::Pod};
  RackId subject{};
  Nanos issued_at{0};
  Nanos expires_at{0};
  FencingSequence fencing{};
  Digest payload_digest{};

  friend bool operator==(const AuthorityToken&, const AuthorityToken&) noexcept = default;
};

enum class AuthorityVerdictCode : std::uint8_t {
  Valid = 0,
  NoToken,
  Malformed,
  WrongPod,
  WrongScope,
  StaleEpoch,
  StaleIncarnation,
  Expired,
  NotYetValid,
  Revoked,
  ClockAnomaly,
};
std::string_view to_string(AuthorityVerdictCode c) noexcept;

struct AuthorityVerdict {
  AuthorityVerdictCode code{AuthorityVerdictCode::NoToken};
  Code status{Code::Unknown};
  std::string detail{};

  bool valid() const noexcept { return code == AuthorityVerdictCode::Valid; }
};

// Reason codes attached to decisions, verdicts and fence actions. The pod state
// is only useful if every non-OK value explains itself.
enum class ReasonCode : std::uint8_t {
  EvidenceMissing = 0,
  EvidenceStale,
  EvidenceFromFuture,
  GenerationMismatch,
  GenerationRegression,
  DigestMismatch,
  SchemaUnsupported,
  DuplicateIdentity,
  DomainMembershipConflict,
  DomainOverlap,
  LinkDown,
  LinkAdminDown,
  LinkUnderCapacity,
  LinkConflicting,
  RouteWithdrawn,
  RouteStale,
  RoutePathMismatch,
  CapacityOvercommit,
  CapacityConflict,
  CapacityIncomplete,
  CapacityDeduplicated,
  ObligationUnmet,
  ObligationMargin,
  DomainDiversityUnmet,
  MemberFenced,
  MemberDisabled,
  MemberMaintenance,
  MemberDraining,
  MemberRetiring,
  MemberUnhealthy,
  TokenInvalid,
  TokenExpired,
  IncarnationChanged,
  EpochAdvanced,
  PolicyBoundExceeded,
  RecoveredHistorical,
  AmbiguousCommit,
  JournalTruncated,
  NoUsablePath,
  ClusterRoutingUnavailable,
};
std::string_view to_string(ReasonCode c) noexcept;

struct Reason {
  ReasonCode code{ReasonCode::EvidenceMissing};
  std::string detail{};

  friend bool operator==(const Reason&, const Reason&) noexcept = default;
  friend auto operator<=>(const Reason&, const Reason&) noexcept = default;
};

// Canonical rendering used in diagnostics, the CLI and the wire protocol.
std::string render(const Reason& reason);
std::string render(const Provenance& provenance);

// Sorts and de-duplicates a list so that two compositions of the same inputs
// are byte-identical.
template <class T>
void canonicalise(std::vector<T>& items) {
  std::sort(items.begin(), items.end());
  items.erase(std::unique(items.begin(), items.end()), items.end());
}

}  // namespace podfabric
