// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "podfabric/model/policy.hpp"

namespace podfabric {

Status validate(const Policy& policy) {
  if (policy.max_capacity_claims > (1u << 24)) {
    return Status(Code::Invalid, "policy: max_capacity_claims is unreasonably large");
  }
  if (policy.max_capacity_amount == 0) {
    return Status(Code::Invalid, "policy: max_capacity_amount must be positive");
  }
  if (policy.max_capacity_total < policy.max_capacity_amount) {
    return Status(Code::Invalid,
                  "policy: max_capacity_total must not be smaller than max_capacity_amount");
  }
  if (policy.evidence_ttl < 0) {
    return Status(Code::Invalid, "policy: evidence_ttl must not be negative");
  }
  if (policy.clock_skew < 0) {
    return Status(Code::Invalid, "policy: clock_skew must not be negative");
  }
  if (policy.max_journal_record_bytes < 64) {
    return Status(Code::Invalid, "policy: max_journal_record_bytes is too small");
  }
  if (policy.max_journal_bytes < policy.max_journal_record_bytes) {
    return Status(Code::Invalid, "policy: max_journal_bytes must cover at least one record");
  }
  if (policy.supported_rnf_schemas.empty()) {
    return Status(Code::Invalid, "policy: at least one Rack Network Fabric schema must be accepted");
  }
  return Status::success();
}

}  // namespace podfabric
