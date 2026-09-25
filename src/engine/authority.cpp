// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "podfabric/engine/authority.hpp"

#include <algorithm>

namespace podfabric {
namespace {

AuthorityVerdict make(AuthorityVerdictCode code, Code status, std::string detail) {
  AuthorityVerdict verdict;
  verdict.code = code;
  verdict.status = status;
  verdict.detail = std::move(detail);
  return verdict;
}

}  // namespace

AuthorityVerdict validate_token(const AuthorityToken& token, const AuthorityCheck& check) {
  if (token.lease.is_nil() || token.pod.is_nil()) {
    return make(AuthorityVerdictCode::Malformed, Code::Invalid,
                "token has no lease identity or no pod identity");
  }
  if (token.expires_at <= token.issued_at) {
    return make(AuthorityVerdictCode::Malformed, Code::Invalid,
                "token lease window is empty or inverted");
  }
  if (!(token.pod == check.pod)) {
    return make(AuthorityVerdictCode::WrongPod, Code::Refused,
                "token was minted for a different pod");
  }
  if (std::find(check.revoked.begin(), check.revoked.end(), token.lease) != check.revoked.end()) {
    return make(AuthorityVerdictCode::Revoked, Code::Refused, "token lease has been revoked");
  }
  // Incident order matters: a token minted by a process incarnation that no
  // longer exists is fenced on that ground even if the epoch also moved, and a
  // token minted by the live incarnation at a superseded epoch is fenced as an
  // epoch artefact.
  if (!(token.incarnation == check.incarnation)) {
    return make(AuthorityVerdictCode::StaleIncarnation, Code::Stale,
                "token was minted by a controller incarnation that is no longer live");
  }
  if (!(token.epoch == check.epoch)) {
    return make(AuthorityVerdictCode::StaleEpoch, Code::Stale,
                "token was minted at a superseded pod epoch");
  }
  if (check.now < token.issued_at - check.clock_skew) {
    return make(AuthorityVerdictCode::NotYetValid, Code::Stale,
                "token is not yet valid on the coordinator clock");
  }
  if (check.now > token.expires_at) {
    return make(AuthorityVerdictCode::Expired, Code::Stale, "token lease has expired");
  }
  if (check.enforce_scope && token.scope != AuthorityScope::Pod &&
      token.scope != check.required) {
    return make(AuthorityVerdictCode::WrongScope, Code::Refused,
                "token scope does not cover the requested operation");
  }
  return make(AuthorityVerdictCode::Valid, Code::Ok, {});
}

}  // namespace podfabric
