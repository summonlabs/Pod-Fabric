// Authority token validation and fencing.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vector>

#include "podfabric/core/status.hpp"
#include "podfabric/core/time.hpp"
#include "podfabric/core/token.hpp"
#include "podfabric/model/primitives.hpp"

namespace podfabric {

struct AuthorityCheck {
  PodId pod{};
  PodEpoch epoch{};
  Incarnation incarnation{};
  Nanos now{0};
  Nanos clock_skew{0};
  std::vector<LeaseId> revoked{};
  // When enforce_scope is set, a token of a different scope is WRONG_SCOPE.
  // Pod-scope tokens satisfy any requirement.
  AuthorityScope required{AuthorityScope::Pod};
  bool enforce_scope{false};
};

// Validity is a conjunction of pod identity, minting incarnation, epoch,
// revocation status and lease window. The first failing predicate decides.
AuthorityVerdict validate_token(const AuthorityToken& token, const AuthorityCheck& check);

}  // namespace podfabric
