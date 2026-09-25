// Authority token fencing.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "podfabric/engine/authority.hpp"
#include "test.hpp"

using namespace podfabric;

namespace {

AuthorityToken make_token(const Incarnation& incarnation, PodEpoch epoch, Nanos issued,
                          Nanos expires) {
  AuthorityToken token;
  token.lease = LeaseId::from_canonical_literal("lease-1");
  token.pod = PodId::from_canonical_literal("pod-1");
  token.epoch = epoch;
  token.incarnation = incarnation;
  token.scope = AuthorityScope::Pod;
  token.issued_at = issued;
  token.expires_at = expires;
  return token;
}

AuthorityCheck make_check(const Incarnation& incarnation, PodEpoch epoch, Nanos now) {
  AuthorityCheck check;
  check.pod = PodId::from_canonical_literal("pod-1");
  check.epoch = epoch;
  check.incarnation = incarnation;
  check.now = now;
  check.clock_skew = nanos_per_second;
  return check;
}

}  // namespace

PF_TEST(authority, a_fresh_token_is_valid) {
  const auto incarnation = Incarnation::fresh();
  PF_REQUIRE(incarnation.ok());
  const AuthorityToken token =
      make_token(incarnation.value(), PodEpoch(3), 1000, 1000 + nanos_per_second);
  const AuthorityVerdict verdict =
      validate_token(token, make_check(incarnation.value(), PodEpoch(3), 1500));
  PF_CHECK(verdict.valid());
  PF_CHECK_EQ(verdict.status, Code::Ok);
}

PF_TEST(authority, a_token_from_a_dead_incarnation_is_fenced_even_if_unexpired) {
  const auto old_incarnation = Incarnation::fresh();
  const auto new_incarnation = Incarnation::fresh();
  PF_REQUIRE(old_incarnation.ok() && new_incarnation.ok());
  const AuthorityToken token =
      make_token(old_incarnation.value(), PodEpoch(3), 1000, 1000 + 3600 * nanos_per_second);
  const AuthorityVerdict verdict =
      validate_token(token, make_check(new_incarnation.value(), PodEpoch(3), 2000));
  PF_CHECK(!verdict.valid());
  PF_CHECK_EQ(verdict.code, AuthorityVerdictCode::StaleIncarnation);
  PF_CHECK_EQ(verdict.status, Code::Stale);
}

PF_TEST(authority, incarnation_fencing_outranks_epoch_fencing) {
  const auto old_incarnation = Incarnation::fresh();
  const auto new_incarnation = Incarnation::fresh();
  PF_REQUIRE(old_incarnation.ok() && new_incarnation.ok());
  const AuthorityToken token =
      make_token(old_incarnation.value(), PodEpoch(3), 1000, 1000 + 3600 * nanos_per_second);
  const AuthorityVerdict verdict =
      validate_token(token, make_check(new_incarnation.value(), PodEpoch(9), 2000));
  PF_CHECK_EQ(verdict.code, AuthorityVerdictCode::StaleIncarnation);
}

PF_TEST(authority, a_superseded_epoch_is_fenced) {
  const auto incarnation = Incarnation::fresh();
  PF_REQUIRE(incarnation.ok());
  const AuthorityToken token =
      make_token(incarnation.value(), PodEpoch(3), 1000, 1000 + 3600 * nanos_per_second);
  const AuthorityVerdict verdict =
      validate_token(token, make_check(incarnation.value(), PodEpoch(4), 2000));
  PF_CHECK_EQ(verdict.code, AuthorityVerdictCode::StaleEpoch);
}

PF_TEST(authority, expiry_and_revocation_and_scope_are_distinct_outcomes) {
  const auto incarnation = Incarnation::fresh();
  PF_REQUIRE(incarnation.ok());
  const AuthorityToken token =
      make_token(incarnation.value(), PodEpoch(3), 1000, 1000 + nanos_per_second);
  PF_CHECK_EQ(validate_token(token, make_check(incarnation.value(), PodEpoch(3), 1000 +
                                                                         2 * nanos_per_second))
                  .code,
              AuthorityVerdictCode::Expired);

  AuthorityCheck revoked = make_check(incarnation.value(), PodEpoch(3), 1500);
  revoked.revoked.push_back(token.lease);
  PF_CHECK_EQ(validate_token(token, revoked).code, AuthorityVerdictCode::Revoked);

  // A pod-scope token satisfies every requirement.
  AuthorityCheck scoped = make_check(incarnation.value(), PodEpoch(3), 1500);
  scoped.enforce_scope = true;
  scoped.required = AuthorityScope::Member;
  PF_CHECK_EQ(validate_token(token, scoped).code, AuthorityVerdictCode::Valid);
  // A member-scope token does not satisfy a link-scope requirement.
  AuthorityToken narrowed =
      make_token(incarnation.value(), PodEpoch(3), 1000, 1000 + nanos_per_second);
  narrowed.scope = AuthorityScope::Member;
  scoped.required = AuthorityScope::Link;
  PF_CHECK_EQ(validate_token(narrowed, scoped).code, AuthorityVerdictCode::WrongScope);

  AuthorityCheck future = make_check(incarnation.value(), PodEpoch(3), 0);
  future.clock_skew = 0;
  PF_CHECK_EQ(validate_token(token, future).code, AuthorityVerdictCode::NotYetValid);
}

PF_TEST(authority, malformed_tokens_are_rejected_before_anything_else) {
  const auto incarnation = Incarnation::fresh();
  PF_REQUIRE(incarnation.ok());
  AuthorityToken token =
      make_token(incarnation.value(), PodEpoch(3), 1000, 1000 + nanos_per_second);
  token.lease = LeaseId{};
  PF_CHECK_EQ(validate_token(token, make_check(incarnation.value(), PodEpoch(3), 1500)).code,
              AuthorityVerdictCode::Malformed);
  AuthorityToken inverted =
      make_token(incarnation.value(), PodEpoch(3), 2000, 1000);
  PF_CHECK_EQ(validate_token(inverted, make_check(incarnation.value(), PodEpoch(3), 1500)).code,
              AuthorityVerdictCode::Malformed);
  AuthorityToken foreign =
      make_token(incarnation.value(), PodEpoch(3), 1000, 1000 + nanos_per_second);
  foreign.pod = PodId::from_canonical_literal("pod-other");
  PF_CHECK_EQ(validate_token(foreign, make_check(incarnation.value(), PodEpoch(3), 1500)).code,
              AuthorityVerdictCode::WrongPod);
}
