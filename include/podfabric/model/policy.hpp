// Composition policy and its hard bounds.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "podfabric/core/status.hpp"
#include "podfabric/core/time.hpp"

namespace podfabric {

// Every externally supplied collection is validated against these bounds before
// it is walked. The defaults are sized for a rack-scale pod; deployments raise
// them deliberately rather than by accident.
struct Policy {
  std::uint32_t max_members = 4096;
  std::uint32_t max_links = 65536;
  std::uint32_t max_routes = 65536;
  std::uint32_t max_domains = 4096;
  std::uint32_t max_obligations = 4096;
  std::uint32_t max_capacity_claims = 262144;
  std::uint32_t max_ports_per_member = 4096;

  // Largest single capacity amount accepted for one claim.
  std::uint64_t max_capacity_amount = 1ull << 48;
  // Largest aggregate the ledger will report before declaring EXHAUSTED.
  std::uint64_t max_capacity_total = 1ull << 56;

  // Evidence older than this is STALE. Zero disables the age check, which is
  // only appropriate when the caller supplies its own freshness evidence.
  Nanos evidence_ttl = 30LL * nanos_per_second;
  // Tolerance for evidence stamped slightly in the future.
  Nanos clock_skew = 5LL * nanos_per_second;

  // Minimum number of established members for the pod to be considered whole.
  std::uint32_t min_established_members = 1;
  // When true, a connectivity verdict is produced for every pair of
  // established members and a pair with no evidence at all is INDETERMINATE.
  // When false (the default) only pairs with declared links or routes are
  // evaluated, which is what a sparse pod needs.
  bool require_full_coverage = false;
  // A member whose first sighting or generation advance is only Declared (or
  // carries UNKNOWN provenance quality) is not adopted: it stays Joining and
  // the gap is reported rather than assumed away.
  bool require_attested_join = true;
  // When true the pod is only authoritative while it holds at least one valid
  // authority token for the current epoch and controller incarnation. A
  // controller running with persistence enables this; a stateless composition
  // leaves it off.
  bool require_authority_token = false;
  // Failure-domain kind used for diversity accounting when a pod has exactly
  // one domain kind declared. Overridden per obligation.
  std::string default_diversity_kind = "power";

  // Rack Network Fabric descriptor schema revisions this pod accepts.
  std::vector<std::string> supported_rnf_schemas{std::string("rnf.descriptor/1")};

  // Percentage of an obligation minimum that must additionally remain free for
  // the obligation to be reported Satisfied rather than AtRisk.
  std::uint32_t default_margin_percent = 0;

  // Bounds on persisted state.
  std::size_t max_journal_record_bytes = 1u * 1024u * 1024u;
  std::size_t max_journal_bytes = 64u * 1024u * 1024u;
  std::uint32_t max_recovered_members = 65536;

  friend bool operator==(const Policy&, const Policy&) noexcept = default;
};

// Validates internal consistency of the policy itself.
Status validate(const Policy& policy);

}  // namespace podfabric
