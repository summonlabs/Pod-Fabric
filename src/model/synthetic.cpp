// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "podfabric/model/synthetic.hpp"

#include <algorithm>

#include "podfabric/version.hpp"

namespace podfabric {
namespace {

// SplitMix64: tiny, deterministic, and adequate for generating fixtures.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed) {}
  std::uint64_t next() {
    state_ += 0x9E3779B97F4A7C15ull;
    std::uint64_t z = state_;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  std::uint64_t range(std::uint64_t low, std::uint64_t high) {
    if (high <= low) {
      return low;
    }
    return low + next() % (high - low + 1);
  }

 private:
  std::uint64_t state_;
};

std::string numbered(const std::string& prefix, std::uint32_t index) {
  return prefix + "-" + std::to_string(index);
}

Provenance synthetic_provenance(const std::string& detail) {
  Provenance provenance;
  provenance.quality = EvidenceQuality::Synthetic;
  provenance.klass = EvidenceClass::Observed;
  provenance.source = NodeId::from_canonical_literal("podfabric.synthetic");
  provenance.detail = detail;
  return provenance;
}

}  // namespace

PodSnapshot synthesize(const SyntheticOptions& options) {
  Rng rng(options.seed);
  PodSnapshot snapshot;
  snapshot.pod = options.pod;
  snapshot.schema.assign(std::string(snapshot_schema));
  snapshot.observed_at =
      options.observed_at == 0 ? (1700000000LL * nanos_per_second) : options.observed_at;
  snapshot.provenance =
      synthetic_provenance("synthetic pod of " + std::to_string(options.racks) +
                           " racks from seed " + std::to_string(options.seed));

  const std::uint32_t racks = options.racks == 0 ? 1 : options.racks;
  const std::uint32_t domains = options.power_domains == 0 ? 1 : options.power_domains;
  const RackGeneration generation =
      options.generation.is_zero() ? RackGeneration(7) : options.generation;
  const ResourceClass resource = options.resource.is_nil()
                                     ? ResourceClass::from_canonical_literal("fabric.bandwidth")
                                     : options.resource;

  std::vector<RackId> rack_ids;
  for (std::uint32_t i = 1; i <= racks; ++i) {
    const auto parsed = RackId::parse(numbered(options.id_prefix, i));
    if (parsed.ok()) {
      rack_ids.push_back(parsed.value());
    }
  }

  // One shared switching plane spans the whole pod, so a switch-plane failure
  // is a correlated failure for every inter-rack link.
  FailureDomainRecord plane;
  plane.id = DomainId::from_canonical_literal("plane-shared-0");
  plane.kind = DomainKind::SwitchPlane;
  plane.description = "shared switching plane (synthetic)";
  plane.provenance = synthetic_provenance("synthetic switch plane");
  plane.members = rack_ids;
  snapshot.domains.push_back(plane);

  std::vector<DomainId> power_domains;
  for (std::uint32_t d = 0; d < domains; ++d) {
    const auto parsed = DomainId::parse(numbered("power", d));
    if (!parsed.ok()) {
      continue;
    }
    FailureDomainRecord record;
    record.id = parsed.value();
    record.kind = DomainKind::Power;
    record.description = "power feed " + std::to_string(d) + " (synthetic)";
    record.provenance = synthetic_provenance("synthetic power feed");
    power_domains.push_back(record.id);
    snapshot.domains.push_back(std::move(record));
  }

  const std::uint32_t every = options.omit_every;
  for (std::uint32_t i = 0; i < rack_ids.size(); ++i) {
    const RackId& rack = rack_ids[i];
    const DomainId power =
        power_domains.empty() ? DomainId{} : power_domains[i % power_domains.size()];
    if (!power.is_nil()) {
      for (FailureDomainRecord& domain : snapshot.domains) {
        if (domain.id == power) {
          domain.members.push_back(rack);
        }
      }
    }
    if (every != 0 && ((i + 1) % every) == 0) {
      continue;  // deliberately absent: the pod still expects this rack
    }
    MemberRecord member;
    member.rack = rack;
    member.generation = generation;
    member.rnf_schema.assign(std::string(rnf_schema_v1));
    member.membership = options.membership;
    member.admin = AdminState::Enabled;
    member.health = options.health;
    member.failure_domains.push_back(plane.id);
    if (!power.is_nil()) {
      member.failure_domains.push_back(power);
    }
    for (std::uint32_t p = 0; p < 2; ++p) {
      const auto port = PortRef::parse(numbered("port", p));
      if (port.ok()) {
        member.ports.push_back(port.value());
      }
    }
    CapacityClaim claim;
    claim.resource = resource;
    const auto key = ExclusivityKey::parse(numbered("fabric", i + 1));
    if (key.ok()) {
      claim.exclusivity = key.value();
    }
    claim.amount = options.capacity_per_rack;
    claim.reserved = options.reserved_per_rack;
    claim.domain = power.is_nil() ? plane.id : power;
    claim.provenance = synthetic_provenance("synthetic fabric budget");
    member.capacity.push_back(claim);
    member.provenance = synthetic_provenance("synthetic rack descriptor");
    member.provenance.klass = options.klass;
    member.digest = Digest::of("rnf|" + rack.token() + "|" +
                               std::to_string(generation.value()) + "|" +
                               std::to_string(options.capacity_per_rack));
    member.observed_at = 0;
    snapshot.members.push_back(std::move(member));
  }

  std::uint32_t link_index = 0;
  for (std::uint32_t i = 0; i < rack_ids.size(); ++i) {
    for (std::uint32_t j = i + 1; j < rack_ids.size(); ++j) {
      if (!options.full_mesh && !(j == i + 1 || (i == 0 && j + 1 == rack_ids.size()))) {
        continue;
      }
      const auto parsed = LinkId::parse(numbered("link", ++link_index));
      if (!parsed.ok()) {
        continue;
      }
      LinkRecord link;
      link.id = parsed.value();
      link.a.rack = rack_ids[i];
      link.a.port = PortRef::from_canonical_literal("port-0");
      link.b.rack = rack_ids[j];
      link.b.port = PortRef::from_canonical_literal("port-0");
      link.domain = plane.id;
      link.resource = resource;
      link.capacity = rng.range(options.capacity_per_rack / 2 + 1, options.capacity_per_rack);
      link.admin = AdminState::Enabled;
      link.oper = OperationalState::Up;
      link.a_generation = generation;
      link.b_generation = generation;
      link.provenance = synthetic_provenance("synthetic inter-rack link");
      link.observed_at = 0;
      snapshot.links.push_back(std::move(link));
    }
  }

  if (options.with_obligations) {
    if (power_domains.size() >= 2) {
      Obligation diversity;
      diversity.id = ObligationId::from_canonical_literal("obligation.tenant-a-diversity");
      diversity.kind = ObligationKind::DomainDiversity;
      diversity.tenant = TenantId::from_canonical_literal("tenant-a");
      diversity.description = "tenant-a keeps two rack-disjoint power domains";
      diversity.required_domains = 2;
      diversity.diversity_kind = DomainKind::Power;
      diversity.mandatory = true;
      diversity.provenance = synthetic_provenance("synthetic tenant obligation");
      snapshot.obligations.push_back(std::move(diversity));
    }
    Obligation floor;
    floor.id = ObligationId::from_canonical_literal("obligation.protected-floor");
    floor.kind = ObligationKind::CapacityFloor;
    floor.tenant = TenantId::from_canonical_literal("tenant-a");
    floor.description = "protected fabric floor";
    CapacityRequirement requirement;
    requirement.resource = resource;
    requirement.minimum = options.capacity_per_rack / 2;
    requirement.margin_percent = 0;
    floor.requirements.push_back(requirement);
    floor.mandatory = true;
    floor.provenance = synthetic_provenance("synthetic capacity floor");
    snapshot.obligations.push_back(std::move(floor));
    if (rack_ids.size() >= 2) {
      Obligation path;
      path.id = ObligationId::from_canonical_literal("obligation.protected-path");
      path.kind = ObligationKind::ProtectedPath;
      path.tenant = TenantId::from_canonical_literal("tenant-a");
      path.description = "first and last rack stay mutually reachable";
      path.protected_racks.push_back(rack_ids.front());
      path.protected_racks.push_back(rack_ids.back());
      path.mandatory = true;
      path.provenance = synthetic_provenance("synthetic protected path");
      snapshot.obligations.push_back(std::move(path));
    }
  }

  for (FailureDomainRecord& domain : snapshot.domains) {
    std::sort(domain.members.begin(), domain.members.end());
    domain.members.erase(std::unique(domain.members.begin(), domain.members.end()),
                         domain.members.end());
  }
  canonicalise(snapshot);
  return snapshot;
}

std::vector<MemberExpectation> expectations_for(const PodSnapshot& snapshot) {
  std::vector<MemberExpectation> expectations;
  expectations.reserve(snapshot.members.size());
  for (const MemberRecord& member : snapshot.members) {
    MemberExpectation expectation;
    expectation.rack = member.rack;
    expectation.generation = member.generation;
    expectation.digest = member.digest;
    expectation.membership = MembershipState::Established;
    expectations.push_back(std::move(expectation));
  }
  std::sort(expectations.begin(), expectations.end());
  return expectations;
}

}  // namespace podfabric
