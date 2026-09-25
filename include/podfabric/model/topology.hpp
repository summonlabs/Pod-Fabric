// Inter-rack links, routing references and failure domains.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "podfabric/core/digest.hpp"
#include "podfabric/core/time.hpp"
#include "podfabric/core/token.hpp"
#include "podfabric/model/primitives.hpp"

namespace podfabric {

struct LinkEndpoint {
  RackId rack{};
  PortRef port{};

  friend bool operator==(const LinkEndpoint&, const LinkEndpoint&) noexcept = default;
  friend auto operator<=>(const LinkEndpoint&, const LinkEndpoint&) noexcept = default;
};

// An inter-rack link. Endpoints are rack-scoped ports; the port itself is
// rack-internal truth owned by Rack Network Fabric and is only referenced here.
struct LinkRecord {
  LinkId id{};
  LinkEndpoint a{};
  LinkEndpoint b{};
  // The failure domain whose loss takes this link down with it, for example the
  // shared switch plane. Used for correlated-failure accounting.
  DomainId domain{};
  ResourceClass resource{};
  std::uint64_t capacity{0};
  AdminState admin{AdminState::Enabled};
  OperationalState oper{OperationalState::Unknown};
  // Rack generations in force when the link was observed. A link observed
  // against a superseded rack generation is STALE: the rack changed underneath
  // it, so the observation no longer describes the current fabric.
  RackGeneration a_generation{};
  RackGeneration b_generation{};
  // Reference to the cluster-wide routing decision that authorises this link.
  // Pod Fabric never computes routes; it validates the reference.
  RouteRef route{};
  Provenance provenance{};
  Nanos observed_at{0};

  friend bool operator==(const LinkRecord&, const LinkRecord&) noexcept = default;
};

enum class RouteState : std::uint8_t { Installed = 0, Pending = 1, Withdrawn = 2, Failed = 3, Unknown = 4 };
std::string_view to_string(RouteState s) noexcept;

// A reference to a routing decision owned by the cluster-wide routing plane.
// Pod Fabric consumes the reference, its generation and its digest; it does not
// derive paths.
struct RouteEvidence {
  RouteRef ref{};
  RouteGeneration generation{};
  Digest authority_digest{};
  std::vector<RackId> path{};
  // Generation of each rack in the path at the time the route was installed,
  // parallel to path. A mismatch makes the route STALE.
  std::vector<RackGeneration> path_generations{};
  ResourceClass resource{};
  std::uint64_t committed_capacity{0};
  RouteState state{RouteState::Unknown};
  Provenance provenance{};
  Nanos observed_at{0};

  friend bool operator==(const RouteEvidence&, const RouteEvidence&) noexcept = default;
};

enum class DomainKind : std::uint8_t {
  Rack = 0,        // the rack itself
  Power = 1,       // shared power feed / bus
  SwitchPlane = 2, // shared switching plane
  Row = 3,
  Site = 4,
  Unknown = 5,
};
std::string_view to_string(DomainKind k) noexcept;

struct FailureDomainRecord {
  DomainId id{};
  DomainKind kind{DomainKind::Unknown};
  std::vector<RackId> members{};
  std::string description{};
  Provenance provenance{};

  friend bool operator==(const FailureDomainRecord&, const FailureDomainRecord&) noexcept = default;
};

}  // namespace podfabric
