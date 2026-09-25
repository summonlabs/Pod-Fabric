// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "podfabric/codec/codec.hpp"

#include <algorithm>

#include "podfabric/core/checked.hpp"
#include "podfabric/version.hpp"

namespace podfabric::codec {
namespace {

constexpr std::uint32_t kMaxCounts = 1u << 20;

Status put_eq(ByteWriter& out, EvidenceQuality q) { return out.put_u8(static_cast<std::uint8_t>(q)); }
Status put_ec(ByteWriter& out, EvidenceClass c) { return out.put_u8(static_cast<std::uint8_t>(c)); }

Result<EvidenceQuality> get_eq(ByteReader& in) {
  PODFABRIC_TRY_ASSIGN(const std::uint8_t raw, in.u8());
  if (raw > static_cast<std::uint8_t>(EvidenceQuality::Unknown)) {
    return Status(Code::Invalid, "provenance quality value out of range");
  }
  return static_cast<EvidenceQuality>(raw);
}

Result<EvidenceClass> get_ec(ByteReader& in) {
  PODFABRIC_TRY_ASSIGN(const std::uint8_t raw, in.u8());
  if (raw > static_cast<std::uint8_t>(EvidenceClass::Reconstructed)) {
    return Status(Code::Invalid, "provenance class value out of range");
  }
  return static_cast<EvidenceClass>(raw);
}

template <class Enum>
Status put_enum(ByteWriter& out, Enum value) {
  return out.put_u8(static_cast<std::uint8_t>(value));
}

template <class Enum>
Result<Enum> get_enum(ByteReader& in, std::uint8_t max_value, const char* what) {
  PODFABRIC_TRY_ASSIGN(const std::uint8_t raw, in.u8());
  if (raw > max_value) {
    return Status(Code::Invalid, std::string("field '") + what + "' has an out-of-range value");
  }
  return static_cast<Enum>(raw);
}

Status put_claim(ByteWriter& out, const CapacityClaim& claim) {
  PODFABRIC_TRY(put_typed_id(out, claim.resource));
  PODFABRIC_TRY(put_typed_id(out, claim.exclusivity));
  PODFABRIC_TRY(out.put_u64(claim.amount));
  PODFABRIC_TRY(out.put_u64(claim.reserved));
  PODFABRIC_TRY(put_typed_id(out, claim.domain));
  return put_provenance(out, claim.provenance);
}

Result<CapacityClaim> get_claim(ByteReader& in) {
  CapacityClaim claim;
  PODFABRIC_TRY_ASSIGN(claim.resource, get_typed_id<ResourceClassTag>(in, "capacity.resource"));
  PODFABRIC_TRY_ASSIGN(claim.exclusivity,
                       get_typed_id<ExclusivityKeyTag>(in, "capacity.exclusivity"));
  PODFABRIC_TRY_ASSIGN(claim.amount, in.u64());
  PODFABRIC_TRY_ASSIGN(claim.reserved, in.u64());
  PODFABRIC_TRY_ASSIGN(claim.domain, get_typed_id<DomainIdTag>(in, "capacity.domain"));
  PODFABRIC_TRY_ASSIGN(claim.provenance, get_provenance(in));
  return claim;
}

}  // namespace

Status put_id(ByteWriter& out, std::string_view token) { return out.put_string(token); }

Status put_uuid(ByteWriter& out, const Uuid& id) {
  return out.put_raw(std::span<const std::byte>(
      reinterpret_cast<const std::byte*>(id.bytes().data()), id.bytes().size()));
}

Result<Uuid> get_uuid(ByteReader& in) {
  PODFABRIC_TRY_ASSIGN(const auto raw, in.raw(16));
  std::array<std::uint8_t, 16> bytes{};
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] = static_cast<std::uint8_t>(raw[i]);
  }
  return Uuid::from_bytes(bytes);
}

Status put_digest(ByteWriter& out, const Digest& digest) {
  return out.put_raw(std::span<const std::byte>(
      reinterpret_cast<const std::byte*>(digest.bytes().data()), digest.bytes().size()));
}

Result<Digest> get_digest(ByteReader& in) {
  PODFABRIC_TRY_ASSIGN(const auto raw, in.raw(Digest::size));
  std::array<std::uint8_t, Digest::size> bytes{};
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] = static_cast<std::uint8_t>(raw[i]);
  }
  return Digest::from_bytes(bytes);
}

Status put_provenance(ByteWriter& out, const Provenance& provenance) {
  PODFABRIC_TRY(put_eq(out, provenance.quality));
  PODFABRIC_TRY(put_ec(out, provenance.klass));
  PODFABRIC_TRY(put_typed_id(out, provenance.source));
  PODFABRIC_TRY(out.put_u64(provenance.source_sequence));
  return out.put_string(provenance.detail);
}

Result<Provenance> get_provenance(ByteReader& in) {
  Provenance provenance;
  PODFABRIC_TRY_ASSIGN(provenance.quality, get_eq(in));
  PODFABRIC_TRY_ASSIGN(provenance.klass, get_ec(in));
  PODFABRIC_TRY_ASSIGN(provenance.source, get_typed_id<NodeIdTag>(in, "provenance.source"));
  PODFABRIC_TRY_ASSIGN(provenance.source_sequence, in.u64());
  PODFABRIC_TRY_ASSIGN(const std::string_view detail, in.string());
  provenance.detail.assign(detail);
  return provenance;
}

Status encode(const PodSnapshot& snapshot, ByteWriter& out) {
  PODFABRIC_TRY(out.put_u16(binary_version));
  PODFABRIC_TRY(put_typed_id(out, snapshot.pod));
  PODFABRIC_TRY(out.put_string(snapshot.schema));

  PODFABRIC_TRY(out.put_count(static_cast<std::uint32_t>(snapshot.members.size())));
  for (const MemberRecord& member : snapshot.members) {
    PODFABRIC_TRY(put_typed_id(out, member.rack));
    PODFABRIC_TRY(out.put_u64(member.generation.value()));
    PODFABRIC_TRY(put_digest(out, member.digest));
    PODFABRIC_TRY(out.put_string(member.rnf_schema));
    PODFABRIC_TRY(put_enum(out, member.membership));
    PODFABRIC_TRY(put_enum(out, member.admin));
    PODFABRIC_TRY(put_enum(out, member.health));
    PODFABRIC_TRY(out.put_count(static_cast<std::uint32_t>(member.failure_domains.size())));
    for (const DomainId& domain : member.failure_domains) {
      PODFABRIC_TRY(put_typed_id(out, domain));
    }
    PODFABRIC_TRY(out.put_count(static_cast<std::uint32_t>(member.capacity.size())));
    for (const CapacityClaim& claim : member.capacity) {
      PODFABRIC_TRY(put_claim(out, claim));
    }
    PODFABRIC_TRY(out.put_count(static_cast<std::uint32_t>(member.ports.size())));
    for (const PortRef& port : member.ports) {
      PODFABRIC_TRY(put_typed_id(out, port));
    }
    PODFABRIC_TRY(put_provenance(out, member.provenance));
    PODFABRIC_TRY(out.put_i64(member.observed_at));
  }

  PODFABRIC_TRY(out.put_count(static_cast<std::uint32_t>(snapshot.links.size())));
  for (const LinkRecord& link : snapshot.links) {
    PODFABRIC_TRY(put_typed_id(out, link.id));
    PODFABRIC_TRY(put_typed_id(out, link.a.rack));
    PODFABRIC_TRY(put_typed_id(out, link.a.port));
    PODFABRIC_TRY(put_typed_id(out, link.b.rack));
    PODFABRIC_TRY(put_typed_id(out, link.b.port));
    PODFABRIC_TRY(put_typed_id(out, link.domain));
    PODFABRIC_TRY(put_typed_id(out, link.resource));
    PODFABRIC_TRY(out.put_u64(link.capacity));
    PODFABRIC_TRY(put_enum(out, link.admin));
    PODFABRIC_TRY(put_enum(out, link.oper));
    PODFABRIC_TRY(out.put_u64(link.a_generation.value()));
    PODFABRIC_TRY(out.put_u64(link.b_generation.value()));
    PODFABRIC_TRY(put_typed_id(out, link.route));
    PODFABRIC_TRY(put_provenance(out, link.provenance));
    PODFABRIC_TRY(out.put_i64(link.observed_at));
  }

  PODFABRIC_TRY(out.put_count(static_cast<std::uint32_t>(snapshot.routes.size())));
  for (const RouteEvidence& route : snapshot.routes) {
    PODFABRIC_TRY(put_typed_id(out, route.ref));
    PODFABRIC_TRY(out.put_u64(route.generation.value()));
    PODFABRIC_TRY(put_digest(out, route.authority_digest));
    PODFABRIC_TRY(out.put_count(static_cast<std::uint32_t>(route.path.size())));
    for (const RackId& rack : route.path) {
      PODFABRIC_TRY(put_typed_id(out, rack));
    }
    PODFABRIC_TRY(out.put_count(static_cast<std::uint32_t>(route.path_generations.size())));
    for (const RackGeneration& generation : route.path_generations) {
      PODFABRIC_TRY(out.put_u64(generation.value()));
    }
    PODFABRIC_TRY(put_typed_id(out, route.resource));
    PODFABRIC_TRY(out.put_u64(route.committed_capacity));
    PODFABRIC_TRY(put_enum(out, route.state));
    PODFABRIC_TRY(put_provenance(out, route.provenance));
    PODFABRIC_TRY(out.put_i64(route.observed_at));
  }

  PODFABRIC_TRY(out.put_count(static_cast<std::uint32_t>(snapshot.domains.size())));
  for (const FailureDomainRecord& domain : snapshot.domains) {
    PODFABRIC_TRY(put_typed_id(out, domain.id));
    PODFABRIC_TRY(put_enum(out, domain.kind));
    PODFABRIC_TRY(out.put_count(static_cast<std::uint32_t>(domain.members.size())));
    for (const RackId& rack : domain.members) {
      PODFABRIC_TRY(put_typed_id(out, rack));
    }
    PODFABRIC_TRY(out.put_string(domain.description));
    PODFABRIC_TRY(put_provenance(out, domain.provenance));
  }

  PODFABRIC_TRY(out.put_count(static_cast<std::uint32_t>(snapshot.obligations.size())));
  for (const Obligation& obligation : snapshot.obligations) {
    PODFABRIC_TRY(put_typed_id(out, obligation.id));
    PODFABRIC_TRY(put_enum(out, obligation.kind));
    PODFABRIC_TRY(put_typed_id(out, obligation.tenant));
    PODFABRIC_TRY(out.put_string(obligation.description));
    PODFABRIC_TRY(out.put_count(static_cast<std::uint32_t>(obligation.protected_racks.size())));
    for (const RackId& rack : obligation.protected_racks) {
      PODFABRIC_TRY(put_typed_id(out, rack));
    }
    PODFABRIC_TRY(out.put_u32(obligation.required_domains));
    PODFABRIC_TRY(put_enum(out, obligation.diversity_kind));
    PODFABRIC_TRY(out.put_count(static_cast<std::uint32_t>(obligation.requirements.size())));
    for (const CapacityRequirement& requirement : obligation.requirements) {
      PODFABRIC_TRY(put_typed_id(out, requirement.resource));
      PODFABRIC_TRY(out.put_u64(requirement.minimum));
      PODFABRIC_TRY(out.put_u32(requirement.margin_percent));
    }
    PODFABRIC_TRY(out.put_bool(obligation.mandatory));
    PODFABRIC_TRY(put_provenance(out, obligation.provenance));
  }

  PODFABRIC_TRY(put_provenance(out, snapshot.provenance));
  PODFABRIC_TRY(out.put_i64(snapshot.observed_at));
  return Status::success();
}

Result<PodSnapshot> decode(ByteReader& in) {
  PODFABRIC_TRY_ASSIGN(const std::uint16_t version, in.u16());
  if (version != binary_version) {
    return Status(Code::Unsupported, "snapshot encoding version is not supported");
  }
  PodSnapshot snapshot;
  PODFABRIC_TRY_ASSIGN(snapshot.pod, get_typed_id<PodIdTag>(in, "pod"));
  PODFABRIC_TRY_ASSIGN(const std::string_view schema, in.string());
  snapshot.schema.assign(schema);

  PODFABRIC_TRY_ASSIGN(const std::uint32_t member_count, in.count(kMaxCounts, 8));
  snapshot.members.reserve(member_count);
  for (std::uint32_t i = 0; i < member_count; ++i) {
    MemberRecord member;
    PODFABRIC_TRY_ASSIGN(member.rack, get_typed_id<RackIdTag>(in, "member.rack"));
    PODFABRIC_TRY_ASSIGN(const std::uint64_t generation, in.u64());
    member.generation = RackGeneration(generation);
    PODFABRIC_TRY_ASSIGN(member.digest, get_digest(in));
    PODFABRIC_TRY_ASSIGN(const std::string_view rnf, in.string());
    member.rnf_schema.assign(rnf);
    PODFABRIC_TRY_ASSIGN(member.membership,
                         get_enum<MembershipState>(in, 4, "member.membership"));
    PODFABRIC_TRY_ASSIGN(member.admin, get_enum<AdminState>(in, 3, "member.admin"));
    PODFABRIC_TRY_ASSIGN(member.health, get_enum<HealthState>(in, 3, "member.health"));
    PODFABRIC_TRY_ASSIGN(const std::uint32_t domain_count, in.count(kMaxCounts, 1));
    member.failure_domains.reserve(domain_count);
    for (std::uint32_t d = 0; d < domain_count; ++d) {
      PODFABRIC_TRY_ASSIGN(const DomainId id, get_typed_id<DomainIdTag>(in, "member.domain"));
      member.failure_domains.push_back(id);
    }
    PODFABRIC_TRY_ASSIGN(const std::uint32_t claim_count, in.count(kMaxCounts, 24));
    member.capacity.reserve(claim_count);
    for (std::uint32_t c = 0; c < claim_count; ++c) {
      PODFABRIC_TRY_ASSIGN(const CapacityClaim claim, get_claim(in));
      member.capacity.push_back(claim);
    }
    PODFABRIC_TRY_ASSIGN(const std::uint32_t port_count, in.count(kMaxCounts, 1));
    member.ports.reserve(port_count);
    for (std::uint32_t p = 0; p < port_count; ++p) {
      PODFABRIC_TRY_ASSIGN(const PortRef port, get_typed_id<PortRefTag>(in, "member.port"));
      member.ports.push_back(port);
    }
    PODFABRIC_TRY_ASSIGN(member.provenance, get_provenance(in));
    PODFABRIC_TRY_ASSIGN(member.observed_at, in.i64());
    snapshot.members.push_back(std::move(member));
  }

  PODFABRIC_TRY_ASSIGN(const std::uint32_t link_count, in.count(kMaxCounts, 16));
  snapshot.links.reserve(link_count);
  for (std::uint32_t i = 0; i < link_count; ++i) {
    LinkRecord link;
    PODFABRIC_TRY_ASSIGN(link.id, get_typed_id<LinkIdTag>(in, "link.id"));
    PODFABRIC_TRY_ASSIGN(link.a.rack, get_typed_id<RackIdTag>(in, "link.a.rack"));
    PODFABRIC_TRY_ASSIGN(link.a.port, get_typed_id<PortRefTag>(in, "link.a.port"));
    PODFABRIC_TRY_ASSIGN(link.b.rack, get_typed_id<RackIdTag>(in, "link.b.rack"));
    PODFABRIC_TRY_ASSIGN(link.b.port, get_typed_id<PortRefTag>(in, "link.b.port"));
    PODFABRIC_TRY_ASSIGN(link.domain, get_typed_id<DomainIdTag>(in, "link.domain"));
    PODFABRIC_TRY_ASSIGN(link.resource, get_typed_id<ResourceClassTag>(in, "link.resource"));
    PODFABRIC_TRY_ASSIGN(link.capacity, in.u64());
    PODFABRIC_TRY_ASSIGN(link.admin, get_enum<AdminState>(in, 3, "link.admin"));
    PODFABRIC_TRY_ASSIGN(link.oper, get_enum<OperationalState>(in, 3, "link.oper"));
    PODFABRIC_TRY_ASSIGN(const std::uint64_t a_generation, in.u64());
    link.a_generation = RackGeneration(a_generation);
    PODFABRIC_TRY_ASSIGN(const std::uint64_t b_generation, in.u64());
    link.b_generation = RackGeneration(b_generation);
    PODFABRIC_TRY_ASSIGN(link.route, get_typed_id<RouteRefTag>(in, "link.route"));
    PODFABRIC_TRY_ASSIGN(link.provenance, get_provenance(in));
    PODFABRIC_TRY_ASSIGN(link.observed_at, in.i64());
    snapshot.links.push_back(std::move(link));
  }

  PODFABRIC_TRY_ASSIGN(const std::uint32_t route_count, in.count(kMaxCounts, 12));
  snapshot.routes.reserve(route_count);
  for (std::uint32_t i = 0; i < route_count; ++i) {
    RouteEvidence route;
    PODFABRIC_TRY_ASSIGN(route.ref, get_typed_id<RouteRefTag>(in, "route.ref"));
    PODFABRIC_TRY_ASSIGN(const std::uint64_t generation, in.u64());
    route.generation = RouteGeneration(generation);
    PODFABRIC_TRY_ASSIGN(route.authority_digest, get_digest(in));
    PODFABRIC_TRY_ASSIGN(const std::uint32_t path_count, in.count(kMaxCounts, 1));
    route.path.reserve(path_count);
    for (std::uint32_t p = 0; p < path_count; ++p) {
      PODFABRIC_TRY_ASSIGN(const RackId rack, get_typed_id<RackIdTag>(in, "route.path"));
      route.path.push_back(rack);
    }
    PODFABRIC_TRY_ASSIGN(const std::uint32_t generation_count, in.count(kMaxCounts, 8));
    route.path_generations.reserve(generation_count);
    for (std::uint32_t g = 0; g < generation_count; ++g) {
      PODFABRIC_TRY_ASSIGN(const std::uint64_t value, in.u64());
      route.path_generations.push_back(RackGeneration(value));
    }
    PODFABRIC_TRY_ASSIGN(route.resource, get_typed_id<ResourceClassTag>(in, "route.resource"));
    PODFABRIC_TRY_ASSIGN(route.committed_capacity, in.u64());
    PODFABRIC_TRY_ASSIGN(route.state, get_enum<RouteState>(in, 4, "route.state"));
    PODFABRIC_TRY_ASSIGN(route.provenance, get_provenance(in));
    PODFABRIC_TRY_ASSIGN(route.observed_at, in.i64());
    snapshot.routes.push_back(std::move(route));
  }

  PODFABRIC_TRY_ASSIGN(const std::uint32_t domain_count, in.count(kMaxCounts, 8));
  snapshot.domains.reserve(domain_count);
  for (std::uint32_t i = 0; i < domain_count; ++i) {
    FailureDomainRecord domain;
    PODFABRIC_TRY_ASSIGN(domain.id, get_typed_id<DomainIdTag>(in, "domain.id"));
    PODFABRIC_TRY_ASSIGN(domain.kind, get_enum<DomainKind>(in, 5, "domain.kind"));
    PODFABRIC_TRY_ASSIGN(const std::uint32_t members, in.count(kMaxCounts, 1));
    domain.members.reserve(members);
    for (std::uint32_t m = 0; m < members; ++m) {
      PODFABRIC_TRY_ASSIGN(const RackId rack, get_typed_id<RackIdTag>(in, "domain.member"));
      domain.members.push_back(rack);
    }
    PODFABRIC_TRY_ASSIGN(const std::string_view description, in.string());
    domain.description.assign(description);
    PODFABRIC_TRY_ASSIGN(domain.provenance, get_provenance(in));
    snapshot.domains.push_back(std::move(domain));
  }

  PODFABRIC_TRY_ASSIGN(const std::uint32_t obligation_count, in.count(kMaxCounts, 12));
  snapshot.obligations.reserve(obligation_count);
  for (std::uint32_t i = 0; i < obligation_count; ++i) {
    Obligation obligation;
    PODFABRIC_TRY_ASSIGN(obligation.id, get_typed_id<ObligationIdTag>(in, "obligation.id"));
    PODFABRIC_TRY_ASSIGN(obligation.kind, get_enum<ObligationKind>(in, 3, "obligation.kind"));
    PODFABRIC_TRY_ASSIGN(obligation.tenant, get_typed_id<TenantIdTag>(in, "obligation.tenant"));
    PODFABRIC_TRY_ASSIGN(const std::string_view description, in.string());
    obligation.description.assign(description);
    PODFABRIC_TRY_ASSIGN(const std::uint32_t racks, in.count(kMaxCounts, 1));
    obligation.protected_racks.reserve(racks);
    for (std::uint32_t r = 0; r < racks; ++r) {
      PODFABRIC_TRY_ASSIGN(const RackId rack, get_typed_id<RackIdTag>(in, "obligation.rack"));
      obligation.protected_racks.push_back(rack);
    }
    PODFABRIC_TRY_ASSIGN(obligation.required_domains, in.u32());
    PODFABRIC_TRY_ASSIGN(obligation.diversity_kind, get_enum<DomainKind>(in, 5, "obligation.kind"));
    PODFABRIC_TRY_ASSIGN(const std::uint32_t requirements, in.count(kMaxCounts, 16));
    obligation.requirements.reserve(requirements);
    for (std::uint32_t r = 0; r < requirements; ++r) {
      CapacityRequirement requirement;
      PODFABRIC_TRY_ASSIGN(requirement.resource,
                           get_typed_id<ResourceClassTag>(in, "obligation.resource"));
      PODFABRIC_TRY_ASSIGN(requirement.minimum, in.u64());
      PODFABRIC_TRY_ASSIGN(requirement.margin_percent, in.u32());
      obligation.requirements.push_back(requirement);
    }
    PODFABRIC_TRY_ASSIGN(obligation.mandatory, in.boolean());
    PODFABRIC_TRY_ASSIGN(obligation.provenance, get_provenance(in));
    snapshot.obligations.push_back(std::move(obligation));
  }

  PODFABRIC_TRY_ASSIGN(snapshot.provenance, get_provenance(in));
  PODFABRIC_TRY_ASSIGN(snapshot.observed_at, in.i64());
  return snapshot;
}

Result<PodSnapshot> decode(std::span<const std::byte> bytes) {
  ByteReader reader(bytes);
  PODFABRIC_TRY_ASSIGN(PodSnapshot snapshot, decode(reader));
  PODFABRIC_TRY(reader.expect_end());
  return snapshot;
}

Status encode(const MemberExpectation& expectation, ByteWriter& out) {
  PODFABRIC_TRY(put_typed_id(out, expectation.rack));
  PODFABRIC_TRY(out.put_u64(expectation.generation.value()));
  PODFABRIC_TRY(put_digest(out, expectation.digest));
  return put_enum(out, expectation.membership);
}

Result<MemberExpectation> decode_expectation(ByteReader& in) {
  MemberExpectation expectation;
  PODFABRIC_TRY_ASSIGN(expectation.rack, get_typed_id<RackIdTag>(in, "expectation.rack"));
  PODFABRIC_TRY_ASSIGN(const std::uint64_t generation, in.u64());
  expectation.generation = RackGeneration(generation);
  PODFABRIC_TRY_ASSIGN(expectation.digest, get_digest(in));
  PODFABRIC_TRY_ASSIGN(expectation.membership,
                       get_enum<MembershipState>(in, 4, "expectation.membership"));
  return expectation;
}

Status encode(const AuthorityToken& token, ByteWriter& out) {
  PODFABRIC_TRY(put_typed_id(out, token.lease));
  PODFABRIC_TRY(put_typed_id(out, token.pod));
  PODFABRIC_TRY(out.put_u64(token.epoch.value()));
  PODFABRIC_TRY(put_uuid(out, token.incarnation.id()));
  PODFABRIC_TRY(put_enum(out, token.scope));
  PODFABRIC_TRY(put_typed_id(out, token.subject));
  PODFABRIC_TRY(out.put_i64(token.issued_at));
  PODFABRIC_TRY(out.put_i64(token.expires_at));
  PODFABRIC_TRY(out.put_u64(token.fencing.value()));
  return put_digest(out, token.payload_digest);
}

Result<AuthorityToken> decode_token(ByteReader& in) {
  AuthorityToken token;
  PODFABRIC_TRY_ASSIGN(token.lease, get_typed_id<LeaseIdTag>(in, "token.lease"));
  PODFABRIC_TRY_ASSIGN(token.pod, get_typed_id<PodIdTag>(in, "token.pod"));
  PODFABRIC_TRY_ASSIGN(const std::uint64_t epoch, in.u64());
  token.epoch = PodEpoch(epoch);
  PODFABRIC_TRY_ASSIGN(const Uuid incarnation, get_uuid(in));
  token.incarnation = Incarnation::from_uuid(incarnation);
  PODFABRIC_TRY_ASSIGN(token.scope, get_enum<AuthorityScope>(in, 4, "token.scope"));
  PODFABRIC_TRY_ASSIGN(token.subject, get_typed_id<RackIdTag>(in, "token.subject"));
  PODFABRIC_TRY_ASSIGN(token.issued_at, in.i64());
  PODFABRIC_TRY_ASSIGN(token.expires_at, in.i64());
  PODFABRIC_TRY_ASSIGN(const std::uint64_t fencing, in.u64());
  token.fencing = FencingSequence(fencing);
  PODFABRIC_TRY_ASSIGN(token.payload_digest, get_digest(in));
  return token;
}

Status encode(const CompositionContext& context, ByteWriter& out) {
  PODFABRIC_TRY(out.put_u64(context.epoch.value()));
  PODFABRIC_TRY(put_uuid(out, context.incarnation.id()));
  PODFABRIC_TRY(out.put_count(static_cast<std::uint32_t>(context.expectations.size())));
  for (const MemberExpectation& expectation : context.expectations) {
    PODFABRIC_TRY(encode(expectation, out));
  }
  PODFABRIC_TRY(out.put_count(static_cast<std::uint32_t>(context.granted_tokens.size())));
  for (const AuthorityToken& token : context.granted_tokens) {
    PODFABRIC_TRY(encode(token, out));
  }
  PODFABRIC_TRY(out.put_count(static_cast<std::uint32_t>(context.revoked_tokens.size())));
  for (const LeaseId& lease : context.revoked_tokens) {
    PODFABRIC_TRY(put_typed_id(out, lease));
  }
  PODFABRIC_TRY(out.put_i64(context.now));
  PODFABRIC_TRY(out.put_bool(context.recovered_from_store));
  PODFABRIC_TRY(out.put_i64(context.recovered_at));
  PODFABRIC_TRY(out.put_bool(context.ambiguous_commit));
  return put_enum(out, context.lifecycle);
}

Result<CompositionContext> decode_context(ByteReader& in) {
  CompositionContext context;
  PODFABRIC_TRY_ASSIGN(const std::uint64_t epoch, in.u64());
  context.epoch = PodEpoch(epoch);
  PODFABRIC_TRY_ASSIGN(const Uuid incarnation, get_uuid(in));
  context.incarnation = Incarnation::from_uuid(incarnation);
  PODFABRIC_TRY_ASSIGN(const std::uint32_t expectations, in.count(kMaxCounts, 8));
  context.expectations.reserve(expectations);
  for (std::uint32_t i = 0; i < expectations; ++i) {
    PODFABRIC_TRY_ASSIGN(const MemberExpectation expectation, decode_expectation(in));
    context.expectations.push_back(expectation);
  }
  PODFABRIC_TRY_ASSIGN(const std::uint32_t tokens, in.count(kMaxCounts, 16));
  context.granted_tokens.reserve(tokens);
  for (std::uint32_t i = 0; i < tokens; ++i) {
    PODFABRIC_TRY_ASSIGN(const AuthorityToken token, decode_token(in));
    context.granted_tokens.push_back(token);
  }
  PODFABRIC_TRY_ASSIGN(const std::uint32_t revoked, in.count(kMaxCounts, 1));
  context.revoked_tokens.reserve(revoked);
  for (std::uint32_t i = 0; i < revoked; ++i) {
    PODFABRIC_TRY_ASSIGN(const LeaseId lease, get_typed_id<LeaseIdTag>(in, "revoked.lease"));
    context.revoked_tokens.push_back(lease);
  }
  PODFABRIC_TRY_ASSIGN(context.now, in.i64());
  PODFABRIC_TRY_ASSIGN(context.recovered_from_store, in.boolean());
  PODFABRIC_TRY_ASSIGN(context.recovered_at, in.i64());
  PODFABRIC_TRY_ASSIGN(context.ambiguous_commit, in.boolean());
  PODFABRIC_TRY_ASSIGN(context.lifecycle, get_enum<LifecycleState>(in, 7, "context.lifecycle"));
  return context;
}

Result<CompositionContext> decode_context(std::span<const std::byte> bytes) {
  ByteReader reader(bytes);
  PODFABRIC_TRY_ASSIGN(CompositionContext context, decode_context(reader));
  PODFABRIC_TRY(reader.expect_end());
  return context;
}

Digest digest_of(const PodSnapshot& snapshot) {
  ByteWriter writer;
  if (!encode(snapshot, writer).ok()) {
    return Digest{};
  }
  return Digest::of(writer.buffer());
}

Digest digest_of(const CompositionContext& context) {
  ByteWriter writer;
  if (!encode(context, writer).ok()) {
    return Digest{};
  }
  return Digest::of(writer.buffer());
}

std::string decision_text(const DecisionRecord& record) {
  std::string out;
  out.reserve(256);
  out.append(record.id.token());
  out.push_back('|');
  out.append(to_string(record.kind));
  out.push_back('|');
  out.append(to_string(record.status));
  for (const DepKey& dep : record.deps) {
    out.push_back('|');
    out.append(to_string(dep.kind));
    out.push_back('=');
    out.append(dep.subject);
  }
  for (const Reason& reason : record.reasons) {
    out.push_back('|');
    out.append(to_string(reason.code));
    out.push_back('=');
    out.append(reason.detail);
  }
  return out;
}

Digest decision_fingerprint(const DecisionRecord& record) {
  return Digest::of(decision_text(record));
}

Digest digest_of(const PodState& state) { return digest_of(state, true); }

Digest digest_of(const PodState& state, bool include_decisions) {
  const auto text = to_text(state, include_decisions);
  if (!text.ok()) {
    return Digest{};
  }
  return Digest::of(text.value());
}

Digest state_digest_of(const PodState& state) { return digest_of(state, false); }

}  // namespace podfabric::codec
