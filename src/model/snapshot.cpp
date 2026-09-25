// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "podfabric/model/snapshot.hpp"

#include <algorithm>
#include <set>

#include "podfabric/core/bytes.hpp"
#include "podfabric/version.hpp"

namespace podfabric {
namespace {

Status bounded(std::size_t count, std::uint32_t limit, const char* what) {
  if (count > limit) {
    return Status(Code::Exhausted,
                  std::string("snapshot declares more ") + what + " than the policy allows");
  }
  return Status::success();
}

template <class T>
bool has_duplicates(const std::vector<T>& items) {
  std::set<T> seen;
  for (const T& item : items) {
    if (!seen.insert(item).second) {
      return true;
    }
  }
  return false;
}

}  // namespace

Status validate(const PodSnapshot& snapshot, const Policy& policy) {
  PODFABRIC_TRY(validate(policy));
  if (snapshot.schema != snapshot_schema) {
    return Status(Code::Unsupported, "snapshot schema revision is not supported");
  }
  if (snapshot.pod.is_nil()) {
    return Status(Code::Invalid, "snapshot has no pod identity");
  }
  if (snapshot.observed_at == 0) {
    return Status(Code::Invalid, "snapshot has no observation time");
  }
  if (!is_valid_utf8(snapshot.provenance.detail)) {
    return Status(Code::Invalid, "snapshot provenance detail is not valid UTF-8");
  }

  PODFABRIC_TRY(bounded(snapshot.members.size(), policy.max_members, "members"));
  PODFABRIC_TRY(bounded(snapshot.links.size(), policy.max_links, "links"));
  PODFABRIC_TRY(bounded(snapshot.routes.size(), policy.max_routes, "routes"));
  PODFABRIC_TRY(bounded(snapshot.domains.size(), policy.max_domains, "failure domains"));
  PODFABRIC_TRY(bounded(snapshot.obligations.size(), policy.max_obligations, "obligations"));

  std::set<RackId> racks;
  std::size_t total_claims = 0;
  for (const MemberRecord& member : snapshot.members) {
    if (member.rack.is_nil()) {
      return Status(Code::Invalid, "member record has no rack identity");
    }
    if (!racks.insert(member.rack).second) {
      // A duplicate rack record is a conflict the composition reports rather
      // than a structural error, so it is not rejected here.
      continue;
    }
    if (member.generation.is_zero()) {
      return Status(Code::Invalid, "member record has a zero generation");
    }
    if (member.digest.is_zero()) {
      return Status(Code::Invalid, "member record has a zero descriptor digest");
    }
    if (member.rnf_schema.empty()) {
      return Status(Code::Invalid, "member record has no Rack Network Fabric schema revision");
    }
    PODFABRIC_TRY(
        bounded(member.failure_domains.size(), policy.max_domains, "member failure domains"));
    PODFABRIC_TRY(bounded(member.ports.size(), policy.max_ports_per_member, "member ports"));
    if (has_duplicates(member.failure_domains)) {
      return Status(Code::Invalid, "member record repeats a failure domain");
    }
    if (has_duplicates(member.ports)) {
      return Status(Code::Invalid, "member record repeats a port reference");
    }
    total_claims += member.capacity.size();
    for (const CapacityClaim& claim : member.capacity) {
      if (claim.resource.is_nil() || claim.exclusivity.is_nil() || claim.domain.is_nil()) {
        return Status(Code::Invalid, "capacity claim is missing a resource, key or domain");
      }
      if (claim.amount > policy.max_capacity_amount) {
        return Status(Code::Invalid, "capacity claim amount exceeds the policy bound");
      }
      if (claim.reserved > claim.amount) {
        return Status(Code::Invalid, "capacity claim reserves more than it offers");
      }
    }
    std::set<std::pair<ResourceClass, ExclusivityKey>> claim_keys;
    for (const CapacityClaim& claim : member.capacity) {
      if (!claim_keys.insert(std::make_pair(claim.resource, claim.exclusivity)).second) {
        return Status(Code::Invalid,
                      "member record repeats a capacity claim for the same physical resource");
      }
    }
  }
  PODFABRIC_TRY(bounded(total_claims, policy.max_capacity_claims, "capacity claims"));

  std::set<LinkId> link_ids;
  for (const LinkRecord& link : snapshot.links) {
    if (link.id.is_nil()) {
      return Status(Code::Invalid, "link record has no identity");
    }
    if (!link_ids.insert(link.id).second) {
      return Status(Code::Invalid, "link identity is duplicated");
    }
    if (link.a.rack.is_nil() || link.b.rack.is_nil()) {
      return Status(Code::Invalid, "link endpoint has no rack identity");
    }
    if (link.a.rack == link.b.rack) {
      return Status(Code::Invalid, "link joins a rack to itself");
    }
    if (link.resource.is_nil()) {
      return Status(Code::Invalid, "link record has no resource class");
    }
    if (link.domain.is_nil()) {
      return Status(Code::Invalid, "link record has no failure-domain attribution");
    }
    if (link.capacity > policy.max_capacity_amount) {
      return Status(Code::Invalid, "link capacity exceeds the policy bound");
    }
  }

  std::set<RouteRef> route_refs;
  for (const RouteEvidence& route : snapshot.routes) {
    if (route.ref.is_nil()) {
      return Status(Code::Invalid, "route record has no reference");
    }
    if (!route_refs.insert(route.ref).second) {
      return Status(Code::Invalid, "route reference is duplicated");
    }
    PODFABRIC_TRY(bounded(route.path.size(), policy.max_members, "route path entries"));
    if (route.path.empty()) {
      return Status(Code::Invalid, "route record has an empty path");
    }
    if (route.path_generations.size() != route.path.size()) {
      return Status(Code::Invalid, "route path and path generations differ in length");
    }
    std::set<RackId> hops;
    for (const RackId& hop : route.path) {
      if (hop.is_nil()) {
        return Status(Code::Invalid, "route path contains a nil rack identity");
      }
      if (!hops.insert(hop).second) {
        return Status(Code::Invalid, "route path revisits a rack");
      }
    }
    if (route.resource.is_nil()) {
      return Status(Code::Invalid, "route record has no resource class");
    }
    if (route.committed_capacity > policy.max_capacity_amount) {
      return Status(Code::Invalid, "route committed capacity exceeds the policy bound");
    }
  }

  std::set<DomainId> domain_ids;
  for (const FailureDomainRecord& domain : snapshot.domains) {
    if (domain.id.is_nil()) {
      return Status(Code::Invalid, "failure domain has no identity");
    }
    if (!domain_ids.insert(domain.id).second) {
      return Status(Code::Invalid, "failure domain identity is duplicated");
    }
    PODFABRIC_TRY(bounded(domain.members.size(), policy.max_members, "failure domain members"));
    if (has_duplicates(domain.members)) {
      return Status(Code::Invalid, "failure domain repeats a member rack");
    }
    if (!is_valid_utf8(domain.description)) {
      return Status(Code::Invalid, "failure domain description is not valid UTF-8");
    }
  }

  std::set<ObligationId> obligation_ids;
  for (const Obligation& obligation : snapshot.obligations) {
    if (obligation.id.is_nil()) {
      return Status(Code::Invalid, "obligation has no identity");
    }
    if (!obligation_ids.insert(obligation.id).second) {
      return Status(Code::Invalid, "obligation identity is duplicated");
    }
    PODFABRIC_TRY(
        bounded(obligation.protected_racks.size(), policy.max_members, "obligation racks"));
    if (has_duplicates(obligation.protected_racks)) {
      return Status(Code::Invalid, "obligation repeats a protected rack");
    }
    PODFABRIC_TRY(
        bounded(obligation.requirements.size(), policy.max_domains, "obligation requirements"));
    for (const CapacityRequirement& requirement : obligation.requirements) {
      if (requirement.resource.is_nil()) {
        return Status(Code::Invalid, "obligation requirement has no resource class");
      }
      if (requirement.minimum > policy.max_capacity_amount) {
        return Status(Code::Invalid, "obligation requirement exceeds the policy bound");
      }
    }
    if (!is_valid_utf8(obligation.description)) {
      return Status(Code::Invalid, "obligation description is not valid UTF-8");
    }
  }

  return Status::success();
}

void canonicalise(PodSnapshot& snapshot) {
  std::sort(snapshot.members.begin(), snapshot.members.end(),
            [](const MemberRecord& a, const MemberRecord& b) { return a.rack < b.rack; });
  std::sort(snapshot.links.begin(), snapshot.links.end(),
            [](const LinkRecord& a, const LinkRecord& b) { return a.id < b.id; });
  std::sort(snapshot.routes.begin(), snapshot.routes.end(),
            [](const RouteEvidence& a, const RouteEvidence& b) { return a.ref < b.ref; });
  std::sort(snapshot.domains.begin(), snapshot.domains.end(),
            [](const FailureDomainRecord& a, const FailureDomainRecord& b) { return a.id < b.id; });
  std::sort(snapshot.obligations.begin(), snapshot.obligations.end(),
            [](const Obligation& a, const Obligation& b) { return a.id < b.id; });
  for (MemberRecord& member : snapshot.members) {
    canonicalise(member.failure_domains);
    canonicalise(member.ports);
    std::sort(member.capacity.begin(), member.capacity.end(),
              [](const CapacityClaim& a, const CapacityClaim& b) {
                if (!(a.resource == b.resource)) return a.resource < b.resource;
                return a.exclusivity < b.exclusivity;
              });
  }
  for (FailureDomainRecord& domain : snapshot.domains) {
    canonicalise(domain.members);
  }
  for (Obligation& obligation : snapshot.obligations) {
    canonicalise(obligation.protected_racks);
    std::sort(obligation.requirements.begin(), obligation.requirements.end(),
              [](const CapacityRequirement& a, const CapacityRequirement& b) {
                return a.resource < b.resource;
              });
  }
}

}  // namespace podfabric
