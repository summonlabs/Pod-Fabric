// Deterministic pod-level composition.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "podfabric/engine/composer.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

#include "podfabric/codec/codec.hpp"
#include "podfabric/core/checked.hpp"
#include "podfabric/engine/authority.hpp"
#include "podfabric/engine/ledger.hpp"
#include "podfabric/version.hpp"

namespace podfabric {
namespace {

int link_severity(LinkStatus status) noexcept {
  switch (status) {
    case LinkStatus::Usable: return 0;
    case LinkStatus::Degraded: return 1;
    case LinkStatus::Stale: return 2;
    case LinkStatus::Indeterminate: return 3;
    case LinkStatus::Blocked: return 4;
    case LinkStatus::Conflicting: return 5;
    case LinkStatus::Unsupported: return 6;
  }
  return 3;
}

LinkStatus worst_link(LinkStatus a, LinkStatus b) noexcept {
  return link_severity(a) >= link_severity(b) ? a : b;
}

int obligation_severity(ObligationStatus status) noexcept {
  switch (status) {
    case ObligationStatus::Satisfied: return 0;
    case ObligationStatus::AtRisk: return 1;
    case ObligationStatus::Indeterminate: return 2;
    case ObligationStatus::Unsupported: return 3;
    case ObligationStatus::Violated: return 4;
  }
  return 2;
}

ObligationStatus worst_obligation(ObligationStatus a, ObligationStatus b) noexcept {
  return obligation_severity(a) >= obligation_severity(b) ? a : b;
}

// Maps a verdict to the outcome code it contributes to pod authority.
Code member_code(MemberStatus status) noexcept {
  switch (status) {
    case MemberStatus::Established: return Code::Ok;
    case MemberStatus::Joining: return Code::Incomplete;
    case MemberStatus::Stale: return Code::Stale;
    case MemberStatus::Fenced: return Code::Stale;
    case MemberStatus::Retiring: return Code::Stale;
    case MemberStatus::Conflicting: return Code::Conflicting;
    case MemberStatus::Unsupported: return Code::Unsupported;
    case MemberStatus::Missing: return Code::Stale;
  }
  return Code::Indeterminate;
}

Code domain_code(DomainStatus status) noexcept {
  switch (status) {
    case DomainStatus::Ok: return Code::Ok;
    case DomainStatus::Degraded: return Code::Incomplete;
    case DomainStatus::Conflicting: return Code::Conflicting;
    case DomainStatus::Incomplete: return Code::Incomplete;
    case DomainStatus::Indeterminate: return Code::Indeterminate;
  }
  return Code::Indeterminate;
}

Code capacity_code(CapacityStatus status) noexcept {
  switch (status) {
    case CapacityStatus::Ok: return Code::Ok;
    case CapacityStatus::Deduplicated: return Code::Ok;
    case CapacityStatus::Incomplete: return Code::Incomplete;
    case CapacityStatus::Indeterminate: return Code::Indeterminate;
    case CapacityStatus::Conflicting: return Code::Conflicting;
    case CapacityStatus::Exhausted: return Code::Exhausted;
    case CapacityStatus::Unsupported: return Code::Unsupported;
  }
  return Code::Indeterminate;
}

bool capacity_is_usable(CapacityStatus status) noexcept {
  return status == CapacityStatus::Ok || status == CapacityStatus::Deduplicated;
}

struct MemberWork {
  RackId rack{};
  const MemberRecord* record{nullptr};
  std::size_t record_count{0};
  bool has_expectation{false};
  MemberExpectation expectation{};
  MemberVerdict verdict{};
  bool established{false};
  bool capacity_usable{false};
  bool connectivity_usable{false};
  bool adopted{false};
};

struct DomainWork {
  DomainId id{};
  DomainKind kind{DomainKind::Unknown};
  const FailureDomainRecord* record{nullptr};
  bool implicit{false};
  std::vector<RackId> declared{};
  DomainVerdict verdict{};
};

struct PairWork {
  std::pair<RackId, RackId> key{};
  std::vector<const LinkRecord*> links{};
  std::vector<const RouteEvidence*> routes{};
  ConnectivityVerdict verdict{};
};

struct LinkOutcome {
  LinkStatus status{LinkStatus::Indeterminate};
  std::uint64_t capacity{0};
  ResourceClass resource{};
  std::vector<Reason> reasons{};
};

class Composer {
 public:
  explicit Composer(const CompositionRequest& request)
      : request_(request),
        policy_(request.policy),
        evaluated_at_(request.snapshot.observed_at),
        ledger_(request.policy) {}

  Result<PodState> run() {
    PODFABRIC_TRY(validate(policy_));
    PODFABRIC_TRY(validate(request_.snapshot, policy_));
    PODFABRIC_TRY(validate_context());

    state_.pod = request_.snapshot.pod;
    state_.schema.assign(std::string(pod_state_schema));
    state_.epoch = request_.context.epoch;
    state_.incarnation = request_.context.incarnation;
    state_.lifecycle = request_.context.lifecycle;
    state_.composed_at = evaluated_at_;
    state_.provenance = request_.snapshot.provenance;

    PODFABRIC_TRY(evaluate_members());
    PODFABRIC_TRY(adopt_generation_advances());
    PODFABRIC_TRY(evaluate_domains());
    PODFABRIC_TRY(evaluate_capacity());
    PODFABRIC_TRY(evaluate_connectivity());
    PODFABRIC_TRY(evaluate_obligations());
    PODFABRIC_TRY(evaluate_lifecycle());
    PODFABRIC_TRY(evaluate_authority());
    emit_decisions();
    finalise();
    return state_;
  }

 private:
  const PodSnapshot& snapshot() const noexcept { return request_.snapshot; }

  Status validate_context() const {
    if (request_.context.expectations.size() > policy_.max_members) {
      return Status(Code::Exhausted, "composition context declares too many expectations");
    }
    if (request_.context.granted_tokens.size() > policy_.max_members * 4u) {
      return Status(Code::Exhausted, "composition context declares too many authority tokens");
    }
    std::set<RackId> racks;
    for (const MemberExpectation& expectation : request_.context.expectations) {
      if (expectation.rack.is_nil()) {
        return Status(Code::Invalid, "member expectation has no rack identity");
      }
      if (!racks.insert(expectation.rack).second) {
        return Status(Code::Invalid, "member expectation is duplicated");
      }
    }
    return Status::success();
  }

  const MemberWork* find_member(const RackId& rack) const {
    const auto it = member_index_.find(rack);
    return it == member_index_.end() ? nullptr : &members_[it->second];
  }

  const DomainWork* find_domain(const DomainId& id) const {
    const auto it = domain_index_.find(id);
    return it == domain_index_.end() ? nullptr : &domains_[it->second];
  }

  // Freshness of an individual record. A zero timestamp means the record
  // inherits the snapshot's observation time, which is the common case for
  // coherent single-source snapshots.
  bool fresh(Nanos observed, Reason& reason) const {
    if (observed == 0 || policy_.evidence_ttl <= 0) {
      return true;
    }
    const Nanos age = evaluated_at_ - observed;
    if (age > policy_.evidence_ttl) {
      reason = Reason{ReasonCode::EvidenceStale, "evidence is older than the configured lifetime"};
      return false;
    }
    if (age < -policy_.clock_skew) {
      reason = Reason{ReasonCode::EvidenceFromFuture,
                      "evidence is stamped further ahead than the clock-skew tolerance"};
      return false;
    }
    return true;
  }

  bool schema_supported(const std::string& schema) const {
    return std::find(policy_.supported_rnf_schemas.begin(), policy_.supported_rnf_schemas.end(),
                     schema) != policy_.supported_rnf_schemas.end();
  }

  bool join_admissible(const MemberRecord& record) const {
    if (record.provenance.quality == EvidenceQuality::Unknown) {
      return false;
    }
    if (!policy_.require_attested_join) {
      return true;
    }
    return record.provenance.klass == EvidenceClass::Attested ||
           record.provenance.klass == EvidenceClass::Observed;
  }

  void fence(FenceKind kind, std::string subject, Reason reason) {
    FenceAction action;
    action.kind = kind;
    action.subject = std::move(subject);
    action.reason = std::move(reason);
    fences_.push_back(std::move(action));
  }

  // ---- members -----------------------------------------------------------

  Status evaluate_members() {
    std::map<RackId, std::vector<const MemberRecord*>> grouped;
    for (const MemberRecord& member : snapshot().members) {
      grouped[member.rack].push_back(&member);
    }
    std::map<RackId, MemberExpectation> expectations;
    for (const MemberExpectation& expectation : request_.context.expectations) {
      expectations[expectation.rack] = expectation;
    }

    std::set<RackId> racks;
    for (const auto& entry : grouped) racks.insert(entry.first);
    for (const auto& entry : expectations) racks.insert(entry.first);
    if (racks.size() > policy_.max_members) {
      return Status(Code::Exhausted, "composition covers more racks than the policy allows");
    }

    members_.reserve(racks.size());
    for (const RackId& rack : racks) {
      MemberWork work;
      work.rack = rack;
      MemberVerdict& verdict = work.verdict;
      verdict.rack = rack;

      const auto records = grouped.find(rack);
      if (records != grouped.end()) {
        work.record_count = records->second.size();
        work.record = records->second.front();
      }
      const auto expectation = expectations.find(rack);
      if (expectation != expectations.end()) {
        work.has_expectation = true;
        work.expectation = expectation->second;
      }

      if (work.record_count > 1) {
        verdict.status = MemberStatus::Conflicting;
        work.verdict.reasons.push_back(
            Reason{ReasonCode::DuplicateIdentity,
                   "more than one record claims this rack in the same observation"});
        members_.push_back(std::move(work));
        continue;
      }
      if (work.record == nullptr) {
        verdict.status = MemberStatus::Missing;
        verdict.membership = MembershipState::Established;
        work.verdict.reasons.push_back(
            Reason{ReasonCode::EvidenceMissing, "the pod expects this rack but no evidence arrived"});
        members_.push_back(std::move(work));
        continue;
      }

      const MemberRecord& record = *work.record;
      verdict.generation = record.generation;
      verdict.digest = record.digest;
      verdict.membership = record.membership;
      verdict.admin = record.admin;
      verdict.health = record.health;
      verdict.domains = record.failure_domains;
      canonicalise(verdict.domains);

      Reason freshness;
      if (!fresh(record.observed_at, freshness)) {
        verdict.status = MemberStatus::Stale;
        verdict.reasons.push_back(freshness);
        members_.push_back(std::move(work));
        continue;
      }
      if (!schema_supported(record.rnf_schema)) {
        verdict.status = MemberStatus::Unsupported;
        verdict.reasons.push_back(
            Reason{ReasonCode::SchemaUnsupported,
                   "Rack Network Fabric descriptor revision '" + record.rnf_schema +
                       "' is not accepted by this pod"});
        members_.push_back(std::move(work));
        continue;
      }
      if (record.membership == MembershipState::Retired ||
          record.membership == MembershipState::Leaving) {
        verdict.status = MemberStatus::Retiring;
        verdict.reasons.push_back(Reason{ReasonCode::MemberRetiring,
                                         "the rack has declared that it is leaving the pod"});
        members_.push_back(std::move(work));
        continue;
      }
      if (record.membership == MembershipState::Fenced) {
        verdict.status = MemberStatus::Fenced;
        verdict.reasons.push_back(Reason{ReasonCode::MemberFenced,
                                         "the rack has declared itself fenced"});
        members_.push_back(std::move(work));
        continue;
      }

      bool establish = false;
      if (work.has_expectation) {
        const MemberExpectation& expected = work.expectation;
        if (record.generation < expected.generation) {
          verdict.status = MemberStatus::Stale;
          verdict.reasons.push_back(
              Reason{ReasonCode::GenerationRegression,
                     "evidence generation " + std::to_string(record.generation.value()) +
                         " is behind the recorded generation " +
                         std::to_string(expected.generation.value())});
        } else if (record.generation == expected.generation) {
          if (!(record.digest == expected.digest)) {
            verdict.status = MemberStatus::Conflicting;
            verdict.reasons.push_back(
                Reason{ReasonCode::DigestMismatch,
                       "the recorded generation is reported with a different descriptor digest"});
          } else {
            establish = true;
          }
        } else {
          establish = true;
          work.adopted = true;
        }
      } else {
        establish = true;
        work.adopted = true;
      }

      if (establish) {
        if (record.membership == MembershipState::Joining) {
          verdict.status = MemberStatus::Joining;
          verdict.reasons.push_back(
              Reason{ReasonCode::EvidenceMissing, "the rack has not finished joining"});
          establish = false;
        } else if (work.adopted && !join_admissible(record)) {
          verdict.status = MemberStatus::Joining;
          verdict.reasons.push_back(
              Reason{ReasonCode::EvidenceMissing,
                     "the rack is only declared, not attested or observed, so it is not adopted"});
          establish = false;
        }
      }

      if (establish) {
        verdict.status = MemberStatus::Established;
        work.established = true;
        switch (record.health) {
          case HealthState::Ok:
            work.capacity_usable = true;
            work.connectivity_usable = true;
            break;
          case HealthState::Impaired:
            work.capacity_usable = true;
            work.connectivity_usable = true;
            verdict.reasons.push_back(
                Reason{ReasonCode::MemberUnhealthy, "the rack reports impaired health"});
            break;
          case HealthState::Critical:
            verdict.reasons.push_back(Reason{ReasonCode::MemberUnhealthy,
                                             "the rack reports critical health; its capacity and "
                                             "links are withheld"});
            break;
          case HealthState::Unknown:
            verdict.reasons.push_back(
                Reason{ReasonCode::EvidenceMissing,
                       "the rack health is unknown; its capacity and links are withheld"});
            break;
        }
        if (record.admin == AdminState::Maintenance) {
          verdict.reasons.push_back(
              Reason{ReasonCode::MemberMaintenance, "the rack is in an administrative window"});
        } else if (record.admin == AdminState::Draining) {
          verdict.reasons.push_back(
              Reason{ReasonCode::MemberDraining, "the rack is draining"});
        } else if (record.admin == AdminState::Disabled) {
          work.capacity_usable = false;
          work.connectivity_usable = false;
          verdict.reasons.push_back(
              Reason{ReasonCode::MemberDisabled, "the rack is administratively disabled"});
        }
      }

      members_.push_back(std::move(work));
    }

    std::sort(members_.begin(), members_.end(),
              [](const MemberWork& a, const MemberWork& b) { return a.rack < b.rack; });
    for (std::size_t i = 0; i < members_.size(); ++i) {
      member_index_[members_[i].rack] = i;
    }
    for (const MemberWork& work : members_) {
      if (work.verdict.status == MemberStatus::Established) {
        continue;
      }
      fence(FenceKind::Member, work.rack.token(),
            work.verdict.reasons.empty()
                ? Reason{ReasonCode::MemberFenced, "the rack is not established"}
                : work.verdict.reasons.front());
    }
    return Status::success();
  }

  // A generation advance is adopted in the same round it is observed, which
  // advances the pod epoch exactly once for any number of joins. A stale or
  // conflicting generation is never adopted: it is fenced above.
  Status adopt_generation_advances() {
    bool any = false;
    for (const MemberWork& work : members_) {
      if (work.established && work.adopted) {
        any = true;
        break;
      }
    }
    if (!any) {
      return Status::success();
    }
    std::vector<MemberExpectation> next;
    next.reserve(members_.size());
    for (const MemberWork& work : members_) {
      MemberExpectation expectation;
      expectation.rack = work.rack;
      if (work.established) {
        expectation.generation = work.verdict.generation;
        expectation.digest = work.verdict.digest;
        expectation.membership = MembershipState::Established;
      } else if (work.has_expectation) {
        expectation = work.expectation;
      } else {
        continue;
      }
      next.push_back(std::move(expectation));
    }
    (void)next;

    if (request_.context.epoch.is_zero()) {
      state_.epoch = PodEpoch(1);
    } else {
      PODFABRIC_TRY_ASSIGN(const PodEpoch bumped, request_.context.epoch.next());
      state_.epoch = bumped;
    }
    return Status::success();
  }

  // ---- failure domains ---------------------------------------------------

  Status evaluate_domains() {
    std::map<DomainId, const FailureDomainRecord*> declared;
    for (const FailureDomainRecord& domain : snapshot().domains) {
      declared[domain.id] = &domain;
    }
    // Domains referenced by members but never declared are materialised as
    // implicit domains so that the reference is visible instead of dangling.
    std::set<DomainId> referenced;
    for (const MemberWork& work : members_) {
      for (const DomainId& id : work.verdict.domains) {
        referenced.insert(id);
      }
    }

    std::set<DomainId> all;
    for (const auto& entry : declared) all.insert(entry.first);
    for (const DomainId& id : referenced) all.insert(id);
    if (all.size() > policy_.max_domains) {
      return Status(Code::Exhausted, "composition covers more failure domains than allowed");
    }

    domains_.reserve(all.size());
    for (const DomainId& id : all) {
      DomainWork work;
      work.id = id;
      const auto it = declared.find(id);
      if (it != declared.end()) {
        work.record = it->second;
        work.kind = it->second->kind;
        work.declared = it->second->members;
      } else {
        work.implicit = true;
        work.kind = DomainKind::Unknown;
      }
      canonicalise(work.declared);
      work.verdict.id = id;
      work.verdict.kind = work.kind;
      work.verdict.declared_members = work.declared;
      domains_.push_back(std::move(work));
    }
    std::sort(domains_.begin(), domains_.end(),
              [](const DomainWork& a, const DomainWork& b) { return a.id < b.id; });
    for (std::size_t i = 0; i < domains_.size(); ++i) {
      domain_index_[domains_[i].id] = i;
    }

    // Membership consistency in both directions.
    for (DomainWork& work : domains_) {
      if (work.implicit) {
        work.verdict.status = DomainStatus::Incomplete;
        work.verdict.reasons.push_back(
            Reason{ReasonCode::EvidenceMissing, "the failure domain is referenced but never declared"});
      }
      for (const RackId& rack : work.declared) {
        const MemberWork* member = find_member(rack);
        if (member == nullptr || member->record == nullptr) {
          work.verdict.reasons.push_back(
              Reason{ReasonCode::EvidenceMissing,
                     "the failure domain lists a rack that the snapshot does not describe"});
          continue;
        }
        const auto& domains = member->verdict.domains;
        if (std::find(domains.begin(), domains.end(), work.id) == domains.end()) {
          work.verdict.status = DomainStatus::Conflicting;
          work.verdict.reasons.push_back(
              Reason{ReasonCode::DomainMembershipConflict,
                     "the rack does not declare this failure domain"});
        }
      }
      for (const MemberWork& member : members_) {
        if (std::find(member.verdict.domains.begin(), member.verdict.domains.end(), work.id) ==
            member.verdict.domains.end()) {
          continue;
        }
        if (std::find(work.declared.begin(), work.declared.end(), member.rack) ==
            work.declared.end()) {
          work.verdict.status = DomainStatus::Conflicting;
          work.verdict.reasons.push_back(
              Reason{ReasonCode::DomainMembershipConflict,
                     "the rack declares this failure domain but the domain does not list it"});
        }
      }
      for (const RackId& rack : work.declared) {
        const MemberWork* member = find_member(rack);
        if (member != nullptr && member->established && member->connectivity_usable) {
          work.verdict.effective_members.push_back(rack);
        }
      }
      canonicalise(work.verdict.effective_members);

      if (work.verdict.status != DomainStatus::Conflicting) {
        if (work.declared.empty()) {
          work.verdict.status = DomainStatus::Incomplete;
          work.verdict.reasons.push_back(
              Reason{ReasonCode::EvidenceMissing, "the failure domain has no members"});
        } else if (work.verdict.effective_members.empty()) {
          work.verdict.status = DomainStatus::Degraded;
          work.verdict.reasons.push_back(
              Reason{ReasonCode::MemberFenced, "no member of the failure domain is established"});
        } else if (work.implicit) {
          work.verdict.status = DomainStatus::Incomplete;
        } else {
          work.verdict.status = DomainStatus::Ok;
        }
      }
    }

    // A rack in two domains of the same kind must never be counted twice for
    // diversity, and the ambiguity is reported rather than resolved by order.
    std::map<RackId, std::map<DomainKind, std::vector<DomainId>>> by_kind;
    for (const MemberWork& member : members_) {
      for (const DomainId& id : member.verdict.domains) {
        const DomainWork* domain = find_domain(id);
        const DomainKind kind = domain == nullptr ? DomainKind::Unknown : domain->kind;
        by_kind[member.rack][kind].push_back(id);
      }
    }
    for (auto& entry : by_kind) {
      for (auto& kind_entry : entry.second) {
        if (kind_entry.second.size() < 2) {
          continue;
        }
        std::sort(kind_entry.second.begin(), kind_entry.second.end());
        for (const DomainId& id : kind_entry.second) {
          DomainWork* domain = const_cast<DomainWork*>(find_domain(id));
          if (domain != nullptr) {
            domain->verdict.status = DomainStatus::Conflicting;
            domain->verdict.reasons.push_back(
                Reason{ReasonCode::DomainOverlap,
                       "the failure domain overlaps another domain of the same kind"});
          }
        }
        const auto index = member_index_.find(entry.first);
        if (index != member_index_.end()) {
          members_[index->second].verdict.reasons.push_back(
              Reason{ReasonCode::DomainOverlap,
                     "the rack belongs to more than one failure domain of the same kind"});
        }
      }
    }

    for (const DomainWork& work : domains_) {
      if (work.verdict.status == DomainStatus::Conflicting) {
        fence(FenceKind::Member, work.id.token(),
              Reason{ReasonCode::DomainOverlap, "failure-domain membership is inconsistent"});
      }
    }
    return Status::success();
  }

  // ---- capacity ----------------------------------------------------------

  Status evaluate_capacity() {
    for (const MemberWork& work : members_) {
      if (work.record == nullptr) {
        continue;
      }
      for (const CapacityClaim& claim : work.record->capacity) {
        LedgerClaim entry;
        entry.resource = claim.resource;
        entry.exclusivity = claim.exclusivity;
        entry.owner = work.rack;
        entry.amount = claim.amount;
        entry.reserved = claim.reserved;
        entry.domain = claim.domain;
        entry.provenance = claim.provenance;

        if (!work.established) {
          PODFABRIC_TRY(ledger_.add_suppressed(
              entry, Reason{ReasonCode::MemberFenced,
                            "capacity from a rack that is not established is not counted"}));
          continue;
        }
        if (!work.capacity_usable) {
          PODFABRIC_TRY(ledger_.add_suppressed(
              entry, Reason{ReasonCode::MemberUnhealthy,
                            "capacity from a rack without usable health evidence is not counted"}));
          continue;
        }
        const auto& domains = work.verdict.domains;
        if (std::find(domains.begin(), domains.end(), claim.domain) == domains.end()) {
          PODFABRIC_TRY(ledger_.add_suppressed(
              entry, Reason{ReasonCode::DomainMembershipConflict,
                            "capacity is attributed to a failure domain the rack does not declare"}));
          continue;
        }
        PODFABRIC_TRY(ledger_.add(entry));
      }
    }
    PODFABRIC_TRY(ledger_.freeze());
    state_.capacity = ledger_.by_resource();
    state_.domain_capacity = ledger_.by_domain();
    return Status::success();
  }

  // ---- connectivity ------------------------------------------------------

  LinkOutcome classify_link(const LinkRecord& link) const {
    LinkOutcome outcome;
    outcome.resource = link.resource;
    const MemberWork* a = find_member(link.a.rack);
    const MemberWork* b = find_member(link.b.rack);
    if (a == nullptr || b == nullptr) {
      outcome.status = LinkStatus::Indeterminate;
      outcome.reasons.push_back(
          Reason{ReasonCode::EvidenceMissing, "the link names a rack that is not a pod member"});
      return outcome;
    }
    if (!a->established || !b->established) {
      outcome.status = LinkStatus::Blocked;
      outcome.reasons.push_back(
          Reason{ReasonCode::MemberFenced, "a link endpoint is not an established member"});
      return outcome;
    }
    if (!a->connectivity_usable || !b->connectivity_usable) {
      outcome.status = LinkStatus::Blocked;
      outcome.reasons.push_back(Reason{ReasonCode::MemberUnhealthy,
                                       "a link endpoint has no usable health evidence"});
      return outcome;
    }
    if (link.admin != AdminState::Enabled) {
      outcome.status = LinkStatus::Blocked;
      outcome.reasons.push_back(
          Reason{ReasonCode::LinkAdminDown, "the link is not administratively enabled"});
      return outcome;
    }
    if (link.oper == OperationalState::Down) {
      outcome.status = LinkStatus::Blocked;
      outcome.reasons.push_back(Reason{ReasonCode::LinkDown, "the link reports itself down"});
      return outcome;
    }
    if (link.oper == OperationalState::Unknown) {
      outcome.status = LinkStatus::Indeterminate;
      outcome.reasons.push_back(
          Reason{ReasonCode::EvidenceMissing, "the link operational state is unknown"});
      return outcome;
    }
    if (!(link.a_generation == a->verdict.generation) ||
        !(link.b_generation == b->verdict.generation)) {
      outcome.status = LinkStatus::Stale;
      outcome.reasons.push_back(
          Reason{ReasonCode::GenerationMismatch,
                 "the link was observed against a superseded rack generation"});
      return outcome;
    }
    Reason freshness;
    if (!fresh(link.observed_at, freshness)) {
      outcome.status = LinkStatus::Stale;
      outcome.reasons.push_back(freshness);
      return outcome;
    }
    if (!link.route.is_nil()) {
      const RouteEvidence* referenced = nullptr;
      for (const RouteEvidence& route : snapshot().routes) {
        if (route.ref == link.route) {
          referenced = &route;
          break;
        }
      }
      if (referenced == nullptr) {
        outcome.status = LinkStatus::Indeterminate;
        outcome.reasons.push_back(
            Reason{ReasonCode::ClusterRoutingUnavailable,
                   "the link cites a routing decision that is not present in the evidence"});
        return outcome;
      }
      if (referenced->state == RouteState::Withdrawn || referenced->state == RouteState::Failed) {
        outcome.status = LinkStatus::Blocked;
        outcome.reasons.push_back(
            Reason{ReasonCode::RouteWithdrawn, "the routing decision cited by the link is gone"});
        return outcome;
      }
      if (referenced->state != RouteState::Installed) {
        outcome.status = LinkStatus::Indeterminate;
        outcome.reasons.push_back(
            Reason{ReasonCode::ClusterRoutingUnavailable, "the cited routing decision is not settled"});
        return outcome;
      }
    }

    // Failure-domain awareness: a link that shares fate with an unhealthy
    // domain cannot be reported better than that domain.
    const DomainWork* domain = find_domain(link.domain);
    if (domain != nullptr && domain->record != nullptr) {
      if (domain->verdict.status == DomainStatus::Conflicting) {
        outcome.status = LinkStatus::Conflicting;
        outcome.reasons.push_back(
            Reason{ReasonCode::LinkConflicting, "the link failure domain is in conflict"});
        return outcome;
      }
      if (domain->verdict.status == DomainStatus::Degraded ||
          domain->verdict.status == DomainStatus::Incomplete) {
        outcome.status = LinkStatus::Degraded;
        outcome.capacity = link.capacity;
        outcome.reasons.push_back(Reason{ReasonCode::LinkUnderCapacity,
                                         "the link shares fate with an unhealthy failure domain"});
        return outcome;
      }
    }

    if (link.oper == OperationalState::Degraded || a->verdict.health == HealthState::Impaired ||
        b->verdict.health == HealthState::Impaired) {
      outcome.status = LinkStatus::Degraded;
      outcome.capacity = link.capacity;
      outcome.reasons.push_back(
          Reason{ReasonCode::LinkUnderCapacity, "the link or one of its endpoints is degraded"});
      return outcome;
    }
    outcome.status = LinkStatus::Usable;
    outcome.capacity = link.capacity;
    return outcome;
  }

  LinkOutcome classify_route(const RouteEvidence& route, const std::pair<RackId, RackId>& key) const {
    LinkOutcome outcome;
    outcome.resource = route.resource;
    if (route.state == RouteState::Withdrawn || route.state == RouteState::Failed) {
      outcome.status = LinkStatus::Blocked;
      outcome.reasons.push_back(
          Reason{ReasonCode::RouteWithdrawn, "the routing decision is no longer installed"});
      return outcome;
    }
    if (route.state != RouteState::Installed) {
      outcome.status = LinkStatus::Indeterminate;
      outcome.reasons.push_back(
          Reason{ReasonCode::ClusterRoutingUnavailable, "the routing decision is not settled"});
      return outcome;
    }
    Reason freshness;
    if (!fresh(route.observed_at, freshness)) {
      outcome.status = LinkStatus::Stale;
      outcome.reasons.push_back(freshness);
      return outcome;
    }
    for (std::size_t i = 0; i < route.path.size(); ++i) {
      const MemberWork* hop = find_member(route.path[i]);
      if (hop == nullptr || !hop->established) {
        outcome.status = LinkStatus::Blocked;
        outcome.reasons.push_back(
            Reason{ReasonCode::MemberFenced, "the route traverses a rack that is not established"});
        return outcome;
      }
      if (!(route.path_generations[i] == hop->verdict.generation)) {
        outcome.status = LinkStatus::Stale;
        outcome.reasons.push_back(
            Reason{ReasonCode::GenerationMismatch,
                   "the route was installed against a superseded rack generation"});
        return outcome;
      }
      if (!hop->connectivity_usable) {
        outcome.status = LinkStatus::Blocked;
        outcome.reasons.push_back(
            Reason{ReasonCode::MemberUnhealthy, "the route traverses a rack without usable health"});
        return outcome;
      }
    }
    outcome.status = LinkStatus::Usable;
    outcome.capacity = route.committed_capacity;
    (void)key;
    return outcome;
  }

  Status evaluate_connectivity() {
    std::map<std::pair<RackId, RackId>, PairWork> collected;
    auto add_pair = [&collected](const RackId& left, const RackId& right) {
      if (left == right || left.is_nil() || right.is_nil()) {
        return;
      }
      const RackId& lo = left < right ? left : right;
      const RackId& hi = left < right ? right : left;
      collected[std::make_pair(lo, hi)];
    };

    for (const LinkRecord& link : snapshot().links) {
      add_pair(link.a.rack, link.b.rack);
    }
    for (const RouteEvidence& route : snapshot().routes) {
      for (std::size_t i = 0; i + 1 < route.path.size(); ++i) {
        add_pair(route.path[i], route.path[i + 1]);
      }
      add_pair(route.path.front(), route.path.back());
    }
    if (policy_.require_full_coverage) {
      for (const MemberWork& a : members_) {
        if (!a.established) continue;
        for (const MemberWork& b : members_) {
          if (!b.established) continue;
          add_pair(a.rack, b.rack);
        }
      }
    }

    for (const LinkRecord& link : snapshot().links) {
      auto key = std::make_pair(link.a.rack, link.b.rack);
      if (key.second < key.first) {
        std::swap(key.first, key.second);
      }
      const auto it = collected.find(key);
      if (it != collected.end()) {
        it->second.links.push_back(&link);
      }
    }
    for (const RouteEvidence& route : snapshot().routes) {
      for (std::size_t i = 0; i + 1 < route.path.size(); ++i) {
        auto key = std::make_pair(route.path[i], route.path[i + 1]);
        if (key.second < key.first) {
          std::swap(key.first, key.second);
        }
        const auto it = collected.find(key);
        if (it != collected.end() && std::find(it->second.routes.begin(), it->second.routes.end(),
                                               &route) == it->second.routes.end()) {
          it->second.routes.push_back(&route);
        }
      }
      auto key = std::make_pair(route.path.front(), route.path.back());
      if (key.second < key.first) {
        std::swap(key.first, key.second);
      }
      const auto it = collected.find(key);
      if (it != collected.end() && std::find(it->second.routes.begin(), it->second.routes.end(),
                                             &route) == it->second.routes.end()) {
        it->second.routes.push_back(&route);
      }
    }

    pairs_.reserve(collected.size());
    for (auto& entry : collected) {
      PairWork work;
      work.key = entry.first;
      work.links = entry.second.links;
      work.routes = entry.second.routes;
      pairs_.push_back(std::move(work));
    }
    std::sort(pairs_.begin(), pairs_.end(),
              [](const PairWork& a, const PairWork& b) { return a.key < b.key; });

    for (PairWork& work : pairs_) {
      ConnectivityVerdict& verdict = work.verdict;
      verdict.from = work.key.first;
      verdict.to = work.key.second;

      bool any_usable = false;
      bool any_degraded = false;
      bool any_bad = false;
      bool have_bad = false;
      LinkStatus worst_bad = LinkStatus::Indeterminate;
      std::uint64_t direct = 0;
      std::uint64_t routed = 0;
      std::set<ResourceClass> direct_resources;
      std::set<ResourceClass> routed_resources;

      for (const LinkRecord* link : work.links) {
        const LinkOutcome outcome = classify_link(*link);
        verdict.links.push_back(link->id);
        for (const Reason& reason : outcome.reasons) {
          verdict.reasons.push_back(reason);
        }
        if (outcome.status == LinkStatus::Usable) {
          any_usable = true;
        } else if (outcome.status == LinkStatus::Degraded) {
          any_degraded = true;
        } else {
          any_bad = true;
          worst_bad = have_bad ? worst_link(worst_bad, outcome.status) : outcome.status;
          have_bad = true;
        }
        if (outcome.capacity > 0) {
          const auto added = checked_add<std::uint64_t>(direct, outcome.capacity);
          if (!added.ok()) {
            verdict.reasons.push_back(
                Reason{ReasonCode::PolicyBoundExceeded, "link capacity sum overflowed"});
          } else {
            direct = added.value();
          }
          direct_resources.insert(outcome.resource);
        }
      }
      for (const RouteEvidence* route : work.routes) {
        const LinkOutcome outcome = classify_route(*route, work.key);
        verdict.routes.push_back(route->ref);
        for (const Reason& reason : outcome.reasons) {
          verdict.reasons.push_back(reason);
        }
        if (outcome.status == LinkStatus::Usable) {
          any_usable = true;
        } else if (outcome.status == LinkStatus::Degraded) {
          any_degraded = true;
        } else {
          any_bad = true;
          worst_bad = have_bad ? worst_link(worst_bad, outcome.status) : outcome.status;
          have_bad = true;
        }
        if (outcome.capacity > routed) {
          routed = outcome.capacity;
          routed_resources.clear();
          routed_resources.insert(outcome.resource);
        }
      }

      if (direct_resources.size() > 1 || routed_resources.size() > 1 ||
          (!direct_resources.empty() && !routed_resources.empty() &&
           !(*direct_resources.begin() == *routed_resources.begin()))) {
        verdict.status = LinkStatus::Conflicting;
        verdict.reasons.push_back(
            Reason{ReasonCode::LinkConflicting,
                   "the rack pair carries more than one resource class, so no single capacity "
                   "figure is authoritative"});
        verdict.usable_capacity = 0;
      } else if (any_usable || any_degraded) {
        verdict.status = (any_bad || any_degraded) ? LinkStatus::Degraded : LinkStatus::Usable;
        // Direct links and routed transit share the same physical fabric, so
        // the pair figure never adds them together.
        verdict.usable_capacity = std::max(direct, routed);
        verdict.resource = direct_resources.empty() ? *routed_resources.begin()
                                                    : *direct_resources.begin();
      } else if (any_bad) {
        verdict.status = worst_bad;
      } else {
        verdict.status = LinkStatus::Indeterminate;
        verdict.reasons.push_back(
            Reason{ReasonCode::NoUsablePath, "no evidence describes this rack pair"});
      }

      if (verdict.status == LinkStatus::Stale || verdict.status == LinkStatus::Blocked ||
          verdict.status == LinkStatus::Conflicting) {
        fence(FenceKind::Link, verdict.from.token() + "." + verdict.to.token(),
              verdict.reasons.empty()
                  ? Reason{ReasonCode::NoUsablePath, "the rack pair is not usable"}
                  : verdict.reasons.front());
      }
      for (const RouteEvidence* route : work.routes) {
        if (route->state == RouteState::Withdrawn || route->state == RouteState::Failed) {
          fence(FenceKind::Route, route->ref.token(),
                Reason{ReasonCode::RouteWithdrawn, "the routing decision is no longer installed"});
        }
      }
    }
    return Status::success();
  }

  // ---- obligations -------------------------------------------------------

  struct DiversityResult {
    std::uint32_t count{0};
    bool conflicted{false};
    bool have_kind{false};
  };

  // Deterministic greedy packing of pairwise rack-disjoint serviceable domains.
  // It never over-reports diversity: if it returns N, N disjoint domains really
  // exist. Being conservative in the other direction is deliberate.
  DiversityResult diversity(DomainKind kind, bool exclude_drained,
                            const std::set<RackId>* restrict) const {
    DiversityResult result;
    std::vector<const DomainWork*> candidates;
    for (const DomainWork& domain : domains_) {
      if (domain.kind != kind) {
        continue;
      }
      result.have_kind = true;
      if (domain.verdict.status == DomainStatus::Conflicting) {
        result.conflicted = true;
        continue;
      }
      if (domain.implicit) {
        continue;
      }
      bool serviceable = false;
      for (const RackId& rack : domain.verdict.declared_members) {
        const MemberWork* member = find_member(rack);
        if (member == nullptr || !member->established || !member->connectivity_usable) {
          continue;
        }
        if (exclude_drained && member->verdict.admin != AdminState::Enabled) {
          continue;
        }
        if (restrict != nullptr && restrict->find(rack) == restrict->end()) {
          continue;
        }
        serviceable = true;
        break;
      }
      if (serviceable) {
        candidates.push_back(&domain);
      }
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const DomainWork* a, const DomainWork* b) {
                if (a->verdict.declared_members.size() != b->verdict.declared_members.size()) {
                  return a->verdict.declared_members.size() < b->verdict.declared_members.size();
                }
                return a->id < b->id;
              });
    std::set<RackId> used;
    for (const DomainWork* domain : candidates) {
      bool overlaps = false;
      for (const RackId& rack : domain->verdict.declared_members) {
        if (used.find(rack) != used.end()) {
          overlaps = true;
          break;
        }
      }
      if (overlaps) {
        continue;
      }
      for (const RackId& rack : domain->verdict.declared_members) {
        used.insert(rack);
      }
      ++result.count;
    }
    return result;
  }

  const ConnectivityVerdict* find_pair(const RackId& a, const RackId& b) const {
    const RackId& lo = a < b ? a : b;
    const RackId& hi = a < b ? b : a;
    const auto key = std::make_pair(lo, hi);
    const auto it = std::lower_bound(
        pairs_.begin(), pairs_.end(), key,
        [](const PairWork& work, const std::pair<RackId, RackId>& probe) {
          return work.key < probe;
        });
    if (it == pairs_.end() || !(it->key == key)) {
      return nullptr;
    }
    return &it->verdict;
  }

  Status evaluate_obligations() {
    for (const Obligation& obligation : snapshot().obligations) {
      ObligationVerdict verdict;
      verdict.id = obligation.id;
      verdict.kind = obligation.kind;
      ObligationStatus status = ObligationStatus::Satisfied;
      std::vector<Reason> reasons;

      switch (obligation.kind) {
        case ObligationKind::CapacityFloor: {
          if (obligation.requirements.empty()) {
            status = ObligationStatus::Unsupported;
            reasons.push_back(Reason{ReasonCode::EvidenceMissing,
                                     "the capacity obligation names no resource"});
            break;
          }
          for (const CapacityRequirement& requirement : obligation.requirements) {
            const CapacityAggregate* aggregate =
                state_.find_capacity(requirement.resource);
            if (aggregate == nullptr) {
              status = worst_obligation(status, ObligationStatus::Indeterminate);
              reasons.push_back(Reason{ReasonCode::CapacityIncomplete,
                                       "no capacity evidence exists for resource '" +
                                           requirement.resource.token() + "'"});
              continue;
            }
            if (!capacity_is_usable(aggregate->status)) {
              status = worst_obligation(status,
                                        aggregate->status == CapacityStatus::Exhausted
                                            ? ObligationStatus::Violated
                                            : ObligationStatus::Indeterminate);
              reasons.push_back(Reason{ReasonCode::CapacityConflict,
                                       "the aggregate for resource '" +
                                           requirement.resource.token() +
                                           "' is not authoritative"});
              continue;
            }
            const std::uint64_t margin =
                static_cast<std::uint64_t>(requirement.margin_percent == 0
                                               ? policy_.default_margin_percent
                                               : requirement.margin_percent);
            const auto scaled = checked_mul<std::uint64_t>(requirement.minimum, margin);
            if (!scaled.ok()) {
              status = worst_obligation(status, ObligationStatus::Indeterminate);
              reasons.push_back(Reason{ReasonCode::PolicyBoundExceeded,
                                       "obligation margin computation overflowed"});
              continue;
            }
            const auto extra = checked_add<std::uint64_t>(
                requirement.minimum,
                scaled.value() / 100u);
            if (!extra.ok()) {
              status = worst_obligation(status, ObligationStatus::Indeterminate);
              reasons.push_back(Reason{ReasonCode::PolicyBoundExceeded,
                                       "obligation requirement overflowed"});
              continue;
            }
            if (aggregate->headroom < requirement.minimum) {
              status = worst_obligation(status, ObligationStatus::Violated);
              reasons.push_back(Reason{ReasonCode::ObligationUnmet,
                                       "resource '" + requirement.resource.token() +
                                           "' has less headroom than the obligation floor"});
            } else if (aggregate->headroom < extra.value()) {
              status = worst_obligation(status, ObligationStatus::AtRisk);
              reasons.push_back(Reason{ReasonCode::ObligationMargin,
                                       "resource '" + requirement.resource.token() +
                                           "' has less than the requested margin above the floor"});
            }
          }
          break;
        }
        case ObligationKind::DomainDiversity: {
          if (obligation.required_domains == 0) {
            status = ObligationStatus::Unsupported;
            reasons.push_back(Reason{ReasonCode::EvidenceMissing,
                                     "the diversity obligation requires no domains"});
            break;
          }
          const DiversityResult result = diversity(obligation.diversity_kind, false, nullptr);
          if (!result.have_kind) {
            status = ObligationStatus::Unsupported;
            reasons.push_back(
                Reason{ReasonCode::EvidenceMissing,
                       "the pod declares no failure domain of the required kind"});
            break;
          }
          verdict.satisfied_domains = result.count;
          if (result.conflicted) {
            status = worst_obligation(status, ObligationStatus::Indeterminate);
            reasons.push_back(Reason{ReasonCode::DomainOverlap,
                                     "failure-domain membership is in conflict, so diversity "
                                     "cannot be decided"});
          }
          if (result.count < obligation.required_domains) {
            status = worst_obligation(status, ObligationStatus::Violated);
            reasons.push_back(Reason{ReasonCode::DomainDiversityUnmet,
                                     "only " + std::to_string(result.count) +
                                         " rack-disjoint failure domains are serviceable"});
          }
          break;
        }
        case ObligationKind::ProtectedPath: {
          if (obligation.protected_racks.empty()) {
            status = ObligationStatus::Unsupported;
            reasons.push_back(Reason{ReasonCode::EvidenceMissing,
                                     "the protected-path obligation names no rack"});
            break;
          }
          for (const RackId& rack : obligation.protected_racks) {
            const MemberWork* member = find_member(rack);
            if (member == nullptr || !member->established) {
              status = worst_obligation(status, ObligationStatus::Violated);
              reasons.push_back(Reason{ReasonCode::MemberFenced,
                                       "protected rack '" + rack.token() + "' is not established"});
            }
          }
          for (std::size_t i = 0; i < obligation.protected_racks.size(); ++i) {
            for (std::size_t j = i + 1; j < obligation.protected_racks.size(); ++j) {
              const ConnectivityVerdict* pair =
                  find_pair(obligation.protected_racks[i], obligation.protected_racks[j]);
              if (pair == nullptr) {
                status = worst_obligation(status, ObligationStatus::Indeterminate);
                reasons.push_back(
                    Reason{ReasonCode::NoUsablePath, "no connectivity evidence exists between two "
                                                     "protected racks"});
                continue;
              }
              switch (pair->status) {
                case LinkStatus::Usable:
                  break;
                case LinkStatus::Degraded:
                  status = worst_obligation(status, ObligationStatus::AtRisk);
                  reasons.push_back(Reason{ReasonCode::LinkUnderCapacity,
                                           "a protected rack pair is only degraded"});
                  break;
                case LinkStatus::Blocked:
                case LinkStatus::Stale:
                case LinkStatus::Conflicting:
                  status = worst_obligation(status, ObligationStatus::Violated);
                  reasons.push_back(Reason{ReasonCode::NoUsablePath,
                                           "a protected rack pair has no usable connectivity"});
                  break;
                case LinkStatus::Indeterminate:
                case LinkStatus::Unsupported:
                  status = worst_obligation(status, ObligationStatus::Indeterminate);
                  reasons.push_back(Reason{ReasonCode::NoUsablePath,
                                           "a protected rack pair cannot be decided"});
                  break;
              }
            }
          }
          break;
        }
        case ObligationKind::MaintenanceWindow: {
          if (obligation.required_domains == 0) {
            status = ObligationStatus::Unsupported;
            reasons.push_back(Reason{ReasonCode::EvidenceMissing,
                                     "the maintenance obligation requires no domains"});
            break;
          }
          const DiversityResult now = diversity(obligation.diversity_kind, false, nullptr);
          const DiversityResult after = diversity(obligation.diversity_kind, true, nullptr);
          verdict.satisfied_domains = after.count;
          if (!now.have_kind) {
            status = ObligationStatus::Unsupported;
            reasons.push_back(Reason{ReasonCode::EvidenceMissing,
                                     "the pod declares no failure domain of the required kind"});
            break;
          }
          if (now.conflicted || after.conflicted) {
            status = worst_obligation(status, ObligationStatus::Indeterminate);
            reasons.push_back(Reason{ReasonCode::DomainOverlap,
                                     "failure-domain membership is in conflict"});
          }
          if (now.count < obligation.required_domains) {
            status = worst_obligation(status, ObligationStatus::Violated);
            reasons.push_back(Reason{ReasonCode::DomainDiversityUnmet,
                                     "the pod is already below the required domain count"});
          } else if (after.count < obligation.required_domains) {
            status = worst_obligation(status, ObligationStatus::AtRisk);
            reasons.push_back(Reason{ReasonCode::MemberDraining,
                                     "draining every maintenance candidate would drop below the "
                                     "required domain count"});
          }
          break;
        }
      }

      verdict.status = status;
      canonicalise(reasons);
      verdict.reasons = std::move(reasons);
      state_.obligations.push_back(std::move(verdict));
    }
    return Status::success();
  }

  // ---- lifecycle ---------------------------------------------------------

  bool has_degradation() const {
    if (!fences_.empty()) {
      return true;
    }
    for (const MemberWork& work : members_) {
      if (work.verdict.status != MemberStatus::Established) {
        return true;
      }
    }
    for (const PairWork& work : pairs_) {
      if (work.verdict.status != LinkStatus::Usable) {
        return true;
      }
    }
    for (const CapacityAggregate& aggregate : state_.capacity) {
      if (!capacity_is_usable(aggregate.status)) {
        return true;
      }
    }
    for (const DomainWork& work : domains_) {
      if (work.verdict.status != DomainStatus::Ok) {
        return true;
      }
    }
    for (const ObligationVerdict& verdict : state_.obligations) {
      if (verdict.status != ObligationStatus::Satisfied) {
        return true;
      }
    }
    return false;
  }

  Status evaluate_lifecycle() {
    std::size_t established = 0;
    std::size_t in_maintenance = 0;
    std::size_t administratively_moved = 0;
    for (const MemberWork& work : members_) {
      if (!work.established) continue;
      ++established;
      if (work.verdict.admin == AdminState::Maintenance) {
        ++in_maintenance;
      }
      if (work.verdict.admin != AdminState::Enabled) {
        ++administratively_moved;
      }
    }

    bool any_pair_usable = false;
    for (const PairWork& work : pairs_) {
      if (work.verdict.status == LinkStatus::Usable ||
          work.verdict.status == LinkStatus::Degraded) {
        any_pair_usable = true;
        break;
      }
    }
    const bool connectivity_expected =
        !pairs_.empty() || (established >= 2 && policy_.require_full_coverage);

    LifecycleState desired = LifecycleState::Active;
    if (established == 0) {
      desired = LifecycleState::Constructing;
    } else if (established >= 2 && connectivity_expected && !any_pair_usable) {
      desired = LifecycleState::Partitioned;
    } else if (in_maintenance == established) {
      desired = LifecycleState::Maintenance;
    } else if (administratively_moved > 0) {
      desired = LifecycleState::Draining;
    } else if (has_degradation() || established < policy_.min_established_members) {
      desired = LifecycleState::Degraded;
    }

    if (established >= 2 && !connectivity_expected) {
      degradations_.push_back(
          Reason{ReasonCode::EvidenceMissing, "no inter-rack connectivity evidence was supplied"});
      desired = LifecycleState::Degraded;
    }
    if (request_.context.recovered_from_store) {
      degradations_.push_back(Reason{ReasonCode::RecoveredHistorical,
                                     "the pod restarted and has not re-established authority"});
      desired = LifecycleState::Recovering;
    }
    if (request_.context.ambiguous_commit) {
      degradations_.push_back(
          Reason{ReasonCode::AmbiguousCommit,
                 "the last durable commit could not be confirmed, so the pod treats its own "
                 "durable state as indeterminate"});
      desired = LifecycleState::Partitioned;
    }
    if (state_.epoch.value() != request_.context.epoch.value()) {
      degradations_.push_back(
          Reason{ReasonCode::EpochAdvanced,
                 "the pod epoch advanced because a member generation changed"});
      if (desired == LifecycleState::Active) {
        desired = LifecycleState::Degraded;
      }
    }

    if (request_.context.lifecycle == LifecycleState::Retired) {
      state_.lifecycle = LifecycleState::Retired;
    } else {
      state_.lifecycle = next_lifecycle(request_.context.lifecycle, desired);
      if (state_.lifecycle != desired) {
        degradations_.push_back(
            Reason{ReasonCode::PolicyBoundExceeded,
                   "the requested lifecycle transition is not legal from the current state; the "
                   "pod advances through the intermediate state instead"});
      }
    }
    return Status::success();
  }

  // ---- authority ---------------------------------------------------------

  Status evaluate_authority() {
    PodAuthority authority;
    authority.epoch = state_.epoch;
    authority.incarnation = state_.incarnation;
    authority.recovered = request_.context.recovered_from_store;
    Code status = Code::Ok;
    std::vector<Reason> reasons;

    for (const MemberWork& work : members_) {
      switch (work.verdict.status) {
        case MemberStatus::Established:
          ++authority.established_members;
          break;
        case MemberStatus::Conflicting:
          ++authority.conflicting_members;
          break;
        case MemberStatus::Stale:
        case MemberStatus::Missing:
        case MemberStatus::Fenced:
        case MemberStatus::Retiring:
          ++authority.stale_members;
          break;
        case MemberStatus::Joining:
        case MemberStatus::Unsupported:
          break;
      }
      const Code code = member_code(work.verdict.status);
      if (severity(code) > severity(status)) {
        status = code;
        if (!work.verdict.reasons.empty()) {
          reasons.push_back(work.verdict.reasons.front());
        }
      }
      if (work.verdict.status != MemberStatus::Established &&
          work.verdict.status != MemberStatus::Joining) {
        ++authority.fenced_members;
      }
    }

    for (const DomainWork& work : domains_) {
      const Code code = domain_code(work.verdict.status);
      if (severity(code) > severity(status)) {
        status = code;
        if (!work.verdict.reasons.empty()) {
          reasons.push_back(work.verdict.reasons.front());
        }
      }
    }
    for (const CapacityAggregate& aggregate : state_.capacity) {
      const Code code = capacity_code(aggregate.status);
      if (severity(code) > severity(status)) {
        status = code;
        if (!aggregate.reasons.empty()) {
          reasons.push_back(aggregate.reasons.front());
        }
      }
    }
    for (const ObligationVerdict& verdict : state_.obligations) {
      if (verdict.status == ObligationStatus::Violated) {
        const Obligation* source = nullptr;
        for (const Obligation& candidate : snapshot().obligations) {
          if (candidate.id == verdict.id) {
            source = &candidate;
            break;
          }
        }
        const bool mandatory = source == nullptr ? true : source->mandatory;
        const Code code = mandatory ? Code::Refused : Code::Stale;
        if (severity(code) > severity(status)) {
          status = code;
          if (!verdict.reasons.empty()) {
            reasons.push_back(verdict.reasons.front());
          }
        }
      } else if (verdict.status == ObligationStatus::Indeterminate) {
        if (severity(Code::Incomplete) > severity(status)) {
          status = Code::Incomplete;
          if (!verdict.reasons.empty()) {
            reasons.push_back(verdict.reasons.front());
          }
        }
      }
    }

    if (state_.lifecycle == LifecycleState::Retired) {
      status = worst(status, Code::Refused);
      reasons.push_back(Reason{ReasonCode::MemberRetiring, "the pod is retired"});
    } else if (state_.lifecycle == LifecycleState::Constructing) {
      status = worst(status, Code::Incomplete);
    }

    if (request_.context.recovered_from_store) {
      status = worst(status, Code::Indeterminate);
      reasons.push_back(Reason{ReasonCode::RecoveredHistorical,
                               "authority from a previous controller incarnation is not carried over"});
    }
    if (request_.context.ambiguous_commit) {
      status = worst(status, Code::Indeterminate);
      reasons.push_back(Reason{ReasonCode::AmbiguousCommit,
                               "durable state may be torn, so pod authority is indeterminate"});
    }

    AuthorityCheck check;
    check.pod = state_.pod;
    check.epoch = state_.epoch;
    check.incarnation = state_.incarnation;
    check.now = request_.context.now != 0 ? request_.context.now : evaluated_at_;
    check.clock_skew = policy_.clock_skew;
    check.revoked = request_.context.revoked_tokens;

    bool any_valid_pod_token = false;
    for (const AuthorityToken& token : request_.context.granted_tokens) {
      const AuthorityVerdict verdict = validate_token(token, check);
      if (verdict.valid()) {
        if (token.scope == AuthorityScope::Pod) {
          any_valid_pod_token = true;
        }
        continue;
      }
      fence(FenceKind::Token, token.lease.token(),
            Reason{ReasonCode::TokenInvalid, verdict.detail});
      reasons.push_back(Reason{ReasonCode::TokenInvalid, verdict.detail});
      if (verdict.code == AuthorityVerdictCode::Expired) {
        reasons.push_back(Reason{ReasonCode::TokenExpired, verdict.detail});
      }
    }
    if (policy_.require_authority_token && !any_valid_pod_token) {
      status = worst(status, Code::Stale);
      reasons.push_back(Reason{ReasonCode::TokenInvalid,
                               "the pod holds no valid pod-scope authority token for the current "
                               "epoch and incarnation"});
    }

    authority.status = status;
    canonicalise(reasons);
    authority.reasons = std::move(reasons);
    state_.authority = std::move(authority);

    if (!state_.authority.authoritative()) {
      fence(FenceKind::Pod, state_.pod.token(),
            state_.authority.reasons.empty()
                ? Reason{ReasonCode::TokenInvalid, "the pod is not authoritative"}
                : state_.authority.reasons.front());
    }
    return Status::success();
  }

  // ---- decisions ---------------------------------------------------------

  void emit_decisions() {
    state_.members.clear();
    for (const MemberWork& work : members_) {
      state_.members.push_back(work.verdict);
    }
    state_.connectivity.clear();
    for (const PairWork& work : pairs_) {
      state_.connectivity.push_back(work.verdict);
    }
    state_.domains.clear();
    for (const DomainWork& work : domains_) {
      state_.domains.push_back(work.verdict);
    }
    state_.degradations = degradations_;
    state_.fences = fences_;

    for (const MemberWork& work : members_) {
      DecisionRecord record;
      record.id = make_decision_id(DecisionKind::MemberState, work.rack.token());
      record.kind = DecisionKind::MemberState;
      record.status = member_code(work.verdict.status);
      record.deps.push_back(DepKey{DepKind::Member, work.rack.token()});
      record.reasons = work.verdict.reasons;
      state_.decisions.push_back(std::move(record));
    }

    for (const DomainWork& work : domains_) {
      DecisionRecord record;
      record.id = make_decision_id(DecisionKind::DomainState, work.id.token());
      record.kind = DecisionKind::DomainState;
      record.status = domain_code(work.verdict.status);
      record.deps.push_back(DepKey{DepKind::Domain, work.id.token()});
      for (const RackId& rack : work.verdict.declared_members) {
        record.deps.push_back(DepKey{DepKind::Member, rack.token()});
      }
      record.reasons = work.verdict.reasons;
      state_.decisions.push_back(std::move(record));
    }

    for (const CapacityAggregate& aggregate : state_.capacity) {
      DecisionRecord record;
      record.id = make_decision_id(DecisionKind::CapacityAggregate, aggregate.resource.token());
      record.kind = DecisionKind::CapacityAggregate;
      record.status = capacity_code(aggregate.status);
      for (const CapacityContribution& contribution : aggregate.contributions) {
        if (!contribution.owner.is_nil()) {
          record.deps.push_back(DepKey{DepKind::Member, contribution.owner.token()});
        }
      }
      record.reasons = aggregate.reasons;
      state_.decisions.push_back(std::move(record));
    }

    for (const CapacityAggregate& aggregate : state_.domain_capacity) {
      DecisionRecord record;
      record.id = make_decision_id(DecisionKind::DomainCapacity,
                                   aggregate.resource.token() + "." + aggregate.domain.token());
      record.kind = DecisionKind::DomainCapacity;
      record.status = capacity_code(aggregate.status);
      record.deps.push_back(DepKey{DepKind::Domain, aggregate.domain.token()});
      for (const CapacityContribution& contribution : aggregate.contributions) {
        if (!contribution.owner.is_nil()) {
          record.deps.push_back(DepKey{DepKind::Member, contribution.owner.token()});
        }
      }
      record.reasons = aggregate.reasons;
      state_.decisions.push_back(std::move(record));
    }

    for (const PairWork& work : pairs_) {
      DecisionRecord record;
      record.id = make_decision_id(DecisionKind::Connectivity,
                                   work.key.first.token() + "." + work.key.second.token());
      record.kind = DecisionKind::Connectivity;
      switch (work.verdict.status) {
        case LinkStatus::Usable: record.status = Code::Ok; break;
        case LinkStatus::Degraded: record.status = Code::Stale; break;
        case LinkStatus::Blocked: record.status = Code::Unavailable; break;
        case LinkStatus::Stale: record.status = Code::Stale; break;
        case LinkStatus::Conflicting: record.status = Code::Conflicting; break;
        case LinkStatus::Indeterminate: record.status = Code::Indeterminate; break;
        case LinkStatus::Unsupported: record.status = Code::Unsupported; break;
      }
      record.deps.push_back(DepKey{DepKind::Member, work.key.first.token()});
      record.deps.push_back(DepKey{DepKind::Member, work.key.second.token()});
      for (const LinkRecord* link : work.links) {
        record.deps.push_back(DepKey{DepKind::Link, link->id.token()});
      }
      for (const RouteEvidence* route : work.routes) {
        record.deps.push_back(DepKey{DepKind::Route, route->ref.token()});
      }
      record.reasons = work.verdict.reasons;
      state_.decisions.push_back(std::move(record));
    }

    for (const Obligation& obligation : snapshot().obligations) {
      const ObligationVerdict* verdict = nullptr;
      for (const ObligationVerdict& candidate : state_.obligations) {
        if (candidate.id == obligation.id) {
          verdict = &candidate;
          break;
        }
      }
      DecisionRecord record;
      record.id = make_decision_id(DecisionKind::ObligationState, obligation.id.token());
      record.kind = DecisionKind::ObligationState;
      switch (verdict == nullptr ? ObligationStatus::Indeterminate : verdict->status) {
        case ObligationStatus::Satisfied: record.status = Code::Ok; break;
        case ObligationStatus::AtRisk: record.status = Code::Stale; break;
        case ObligationStatus::Violated: record.status = Code::Refused; break;
        case ObligationStatus::Indeterminate: record.status = Code::Indeterminate; break;
        case ObligationStatus::Unsupported: record.status = Code::Unsupported; break;
      }
      record.deps.push_back(DepKey{DepKind::Obligation, obligation.id.token()});
      for (const CapacityRequirement& requirement : obligation.requirements) {
        record.deps.push_back(DepKey{
            DepKind::Decision,
            make_decision_id(DecisionKind::CapacityAggregate, requirement.resource.token()).token()});
      }
      for (const RackId& rack : obligation.protected_racks) {
        record.deps.push_back(DepKey{DepKind::Member, rack.token()});
      }
      for (std::size_t i = 0; i < obligation.protected_racks.size(); ++i) {
        for (std::size_t j = i + 1; j < obligation.protected_racks.size(); ++j) {
          const RackId& lo = obligation.protected_racks[i] < obligation.protected_racks[j]
                                 ? obligation.protected_racks[i]
                                 : obligation.protected_racks[j];
          const RackId& hi = obligation.protected_racks[i] < obligation.protected_racks[j]
                                 ? obligation.protected_racks[j]
                                 : obligation.protected_racks[i];
          record.deps.push_back(DepKey{
              DepKind::Decision,
              make_decision_id(DecisionKind::Connectivity, lo.token() + "." + hi.token()).token()});
        }
      }
      record.reasons = verdict == nullptr ? std::vector<Reason>{} : verdict->reasons;
      state_.decisions.push_back(std::move(record));
    }

    std::vector<DepKey> lifecycle_deps;
    for (const DecisionRecord& record : state_.decisions) {
      lifecycle_deps.push_back(DepKey{DepKind::Decision, record.id.token()});
    }
    lifecycle_deps.push_back(DepKey{DepKind::Epoch, std::to_string(state_.epoch.value())});
    lifecycle_deps.push_back(DepKey{DepKind::Lifecycle,
                                    std::string(to_string(request_.context.lifecycle))});

    DecisionRecord lifecycle;
    lifecycle.id = make_decision_id(DecisionKind::PodLifecycle, "pod");
    lifecycle.kind = DecisionKind::PodLifecycle;
    switch (state_.lifecycle) {
      case LifecycleState::Active: lifecycle.status = Code::Ok; break;
      case LifecycleState::Constructing: lifecycle.status = Code::Incomplete; break;
      case LifecycleState::Degraded: lifecycle.status = Code::Stale; break;
      case LifecycleState::Partitioned: lifecycle.status = Code::Unavailable; break;
      case LifecycleState::Recovering: lifecycle.status = Code::Indeterminate; break;
      case LifecycleState::Maintenance: lifecycle.status = Code::Stale; break;
      case LifecycleState::Draining: lifecycle.status = Code::Stale; break;
      case LifecycleState::Retired: lifecycle.status = Code::Refused; break;
    }
    lifecycle.deps = std::move(lifecycle_deps);
    lifecycle.reasons = state_.degradations;
    state_.decisions.push_back(std::move(lifecycle));

    DecisionRecord authority;
    authority.id = make_decision_id(DecisionKind::PodAuthority, "pod");
    authority.kind = DecisionKind::PodAuthority;
    authority.status = state_.authority.status;
    authority.deps.push_back(
        DepKey{DepKind::Decision, make_decision_id(DecisionKind::PodLifecycle, "pod").token()});
    authority.deps.push_back(DepKey{DepKind::Epoch, std::to_string(state_.epoch.value())});
    authority.deps.push_back(DepKey{DepKind::Incarnation, state_.incarnation.to_string()});
    for (const MemberWork& work : members_) {
      authority.deps.push_back(DepKey{DepKind::Member, work.rack.token()});
    }
    for (const AuthorityToken& token : request_.context.granted_tokens) {
      authority.deps.push_back(DepKey{DepKind::Token, token.lease.token()});
    }
    for (const LeaseId& lease : request_.context.revoked_tokens) {
      authority.deps.push_back(DepKey{DepKind::Token, lease.token()});
    }
    authority.reasons = state_.authority.reasons;
    state_.decisions.push_back(std::move(authority));
  }

  void finalise() {
    canonicalise(state_);
    for (DecisionRecord& record : state_.decisions) {
      record.fingerprint = codec::decision_fingerprint(record);
    }
    // The verdict digest deliberately excludes the decision bookkeeping, so an
    // unchanged pod reports an unchanged digest even when a round consumed a
    // different set of inputs.
    state_.state_digest = codec::state_digest_of(state_);
    state_.decision_digest = codec::digest_of(state_);
  }

  const CompositionRequest& request_;
  Policy policy_;
  Nanos evaluated_at_{0};
  PodState state_{};
  std::vector<MemberWork> members_{};
  std::map<RackId, std::size_t> member_index_{};
  std::vector<DomainWork> domains_{};
  std::map<DomainId, std::size_t> domain_index_{};
  std::vector<PairWork> pairs_{};
  CapacityLedger ledger_;
  std::vector<FenceAction> fences_{};
  std::vector<Reason> degradations_{};
};

}  // namespace

Result<PodState> compose(const CompositionRequest& request) {
  Composer composer(request);
  return composer.run();
}

}  // namespace podfabric
