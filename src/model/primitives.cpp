// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "podfabric/model/primitives.hpp"

#include <algorithm>

namespace podfabric {

std::string_view to_string(EvidenceQuality q) noexcept {
  switch (q) {
    case EvidenceQuality::Real: return "REAL";
    case EvidenceQuality::Synthetic: return "SYNTHETIC";
    case EvidenceQuality::Unknown: return "UNKNOWN";
  }
  return "UNKNOWN";
}

std::string_view to_string(EvidenceClass c) noexcept {
  switch (c) {
    case EvidenceClass::Attested: return "attested";
    case EvidenceClass::Observed: return "observed";
    case EvidenceClass::Declared: return "declared";
    case EvidenceClass::Reconstructed: return "reconstructed";
  }
  return "declared";
}

std::string_view to_string(HealthState h) noexcept {
  switch (h) {
    case HealthState::Ok: return "ok";
    case HealthState::Impaired: return "impaired";
    case HealthState::Critical: return "critical";
    case HealthState::Unknown: return "unknown";
  }
  return "unknown";
}

std::string_view to_string(AdminState s) noexcept {
  switch (s) {
    case AdminState::Enabled: return "enabled";
    case AdminState::Disabled: return "disabled";
    case AdminState::Maintenance: return "maintenance";
    case AdminState::Draining: return "draining";
  }
  return "enabled";
}

std::string_view to_string(OperationalState s) noexcept {
  switch (s) {
    case OperationalState::Up: return "up";
    case OperationalState::Degraded: return "degraded";
    case OperationalState::Down: return "down";
    case OperationalState::Unknown: return "unknown";
  }
  return "unknown";
}

std::string_view to_string(MembershipState s) noexcept {
  switch (s) {
    case MembershipState::Joining: return "joining";
    case MembershipState::Established: return "established";
    case MembershipState::Leaving: return "leaving";
    case MembershipState::Fenced: return "fenced";
    case MembershipState::Retired: return "retired";
  }
  return "joining";
}

std::string_view to_string(AuthorityScope s) noexcept {
  switch (s) {
    case AuthorityScope::Pod: return "pod";
    case AuthorityScope::Member: return "member";
    case AuthorityScope::Link: return "link";
    case AuthorityScope::CapacityReservation: return "capacity-reservation";
    case AuthorityScope::MaintenanceWindow: return "maintenance-window";
  }
  return "pod";
}

std::string_view to_string(AuthorityVerdictCode c) noexcept {
  switch (c) {
    case AuthorityVerdictCode::Valid: return "VALID";
    case AuthorityVerdictCode::NoToken: return "NO_TOKEN";
    case AuthorityVerdictCode::Malformed: return "MALFORMED";
    case AuthorityVerdictCode::WrongPod: return "WRONG_POD";
    case AuthorityVerdictCode::WrongScope: return "WRONG_SCOPE";
    case AuthorityVerdictCode::StaleEpoch: return "STALE_EPOCH";
    case AuthorityVerdictCode::StaleIncarnation: return "STALE_INCARNATION";
    case AuthorityVerdictCode::Expired: return "EXPIRED";
    case AuthorityVerdictCode::NotYetValid: return "NOT_YET_VALID";
    case AuthorityVerdictCode::Revoked: return "REVOKED";
    case AuthorityVerdictCode::ClockAnomaly: return "CLOCK_ANOMALY";
  }
  return "NO_TOKEN";
}

std::string_view to_string(ReasonCode c) noexcept {
  switch (c) {
    case ReasonCode::EvidenceMissing: return "evidence-missing";
    case ReasonCode::EvidenceStale: return "evidence-stale";
    case ReasonCode::EvidenceFromFuture: return "evidence-from-future";
    case ReasonCode::GenerationMismatch: return "generation-mismatch";
    case ReasonCode::GenerationRegression: return "generation-regression";
    case ReasonCode::DigestMismatch: return "digest-mismatch";
    case ReasonCode::SchemaUnsupported: return "schema-unsupported";
    case ReasonCode::DuplicateIdentity: return "duplicate-identity";
    case ReasonCode::DomainMembershipConflict: return "domain-membership-conflict";
    case ReasonCode::DomainOverlap: return "domain-overlap";
    case ReasonCode::LinkDown: return "link-down";
    case ReasonCode::LinkAdminDown: return "link-admin-down";
    case ReasonCode::LinkUnderCapacity: return "link-under-capacity";
    case ReasonCode::LinkConflicting: return "link-conflicting";
    case ReasonCode::RouteWithdrawn: return "route-withdrawn";
    case ReasonCode::RouteStale: return "route-stale";
    case ReasonCode::RoutePathMismatch: return "route-path-mismatch";
    case ReasonCode::CapacityOvercommit: return "capacity-overcommit";
    case ReasonCode::CapacityConflict: return "capacity-conflict";
    case ReasonCode::CapacityIncomplete: return "capacity-incomplete";
    case ReasonCode::CapacityDeduplicated: return "capacity-deduplicated";
    case ReasonCode::ObligationUnmet: return "obligation-unmet";
    case ReasonCode::ObligationMargin: return "obligation-margin";
    case ReasonCode::DomainDiversityUnmet: return "domain-diversity-unmet";
    case ReasonCode::MemberFenced: return "member-fenced";
    case ReasonCode::MemberDisabled: return "member-admin-disabled";
    case ReasonCode::MemberMaintenance: return "member-maintenance";
    case ReasonCode::MemberDraining: return "member-draining";
    case ReasonCode::MemberRetiring: return "member-retiring";
    case ReasonCode::MemberUnhealthy: return "member-unhealthy";
    case ReasonCode::TokenInvalid: return "token-invalid";
    case ReasonCode::TokenExpired: return "token-expired";
    case ReasonCode::IncarnationChanged: return "incarnation-changed";
    case ReasonCode::EpochAdvanced: return "epoch-advanced";
    case ReasonCode::PolicyBoundExceeded: return "policy-bound-exceeded";
    case ReasonCode::RecoveredHistorical: return "recovered-historical";
    case ReasonCode::AmbiguousCommit: return "ambiguous-commit";
    case ReasonCode::JournalTruncated: return "journal-truncated";
    case ReasonCode::NoUsablePath: return "no-usable-path";
    case ReasonCode::ClusterRoutingUnavailable: return "cluster-routing-unavailable";
  }
  return "evidence-missing";
}

std::string render(const Reason& reason) {
  std::string out(to_string(reason.code));
  if (!reason.detail.empty()) {
    out.append(": ");
    out.append(reason.detail);
  }
  return out;
}

std::string render(const Provenance& provenance) {
  std::string out(to_string(provenance.quality));
  out.append("/");
  out.append(to_string(provenance.klass));
  if (!provenance.source.is_nil()) {
    out.append(" from ");
    out.append(provenance.source.token());
  }
  if (!provenance.detail.empty()) {
    out.append(" (");
    out.append(provenance.detail);
    out.append(")");
  }
  return out;
}

Result<Incarnation> Incarnation::fresh() {
  PODFABRIC_TRY_ASSIGN(const Uuid id, Uuid::random());
  return Incarnation(id);
}

Result<Incarnation> Incarnation::parse(std::string_view text) {
  PODFABRIC_TRY_ASSIGN(const Uuid id, Uuid::parse(text));
  return Incarnation(id);
}

}  // namespace podfabric
