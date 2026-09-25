// Canonical binary and text encodings of pod documents.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "podfabric/core/bytes.hpp"
#include "podfabric/core/digest.hpp"
#include "podfabric/core/status.hpp"
#include "podfabric/model/pod_state.hpp"
#include "podfabric/model/snapshot.hpp"

namespace podfabric::codec {

// Binary revision of the wire/persistence encoding. Bumping it makes older
// documents UNSUPPORTED rather than misparsed.
inline constexpr std::uint16_t binary_version = 1;

// ---- primitives -----------------------------------------------------------

Status put_id(ByteWriter& out, std::string_view token);
template <class Tag>
Status put_typed_id(ByteWriter& out, const BasicId<Tag>& id) {
  return put_id(out, id.token());
}
template <class Tag>
Result<BasicId<Tag>> get_typed_id(ByteReader& in, const char* what) {
  PODFABRIC_TRY_ASSIGN(const std::string_view token, in.string());
  if (token.empty()) {
    return BasicId<Tag>{};
  }
  const auto parsed = BasicId<Tag>::parse(token);
  if (!parsed.ok()) {
    return Status(Code::Invalid, std::string("field '") + what + "' is not a canonical identity");
  }
  return parsed.value();
}

Status put_uuid(ByteWriter& out, const Uuid& id);
Result<Uuid> get_uuid(ByteReader& in);
Status put_digest(ByteWriter& out, const Digest& digest);
Result<Digest> get_digest(ByteReader& in);
Status put_provenance(ByteWriter& out, const Provenance& provenance);
Result<Provenance> get_provenance(ByteReader& in);

// ---- documents ------------------------------------------------------------

Status encode(const PodSnapshot& snapshot, ByteWriter& out);
Result<PodSnapshot> decode(ByteReader& in);
Result<PodSnapshot> decode(std::span<const std::byte> bytes);

Status encode(const MemberExpectation& expectation, ByteWriter& out);
Result<MemberExpectation> decode_expectation(ByteReader& in);

Status encode(const AuthorityToken& token, ByteWriter& out);
Result<AuthorityToken> decode_token(ByteReader& in);

Status encode(const CompositionContext& context, ByteWriter& out);
Result<CompositionContext> decode_context(ByteReader& in);
Result<CompositionContext> decode_context(std::span<const std::byte> bytes);

// ---- digests --------------------------------------------------------------

// Digest of the canonical binary encoding. Two snapshots with equal digests
// are byte-identical documents.
Digest digest_of(const PodSnapshot& snapshot);
Digest digest_of(const CompositionContext& context);

// Digest of the canonical text rendering of a composed pod state.
Digest digest_of(const PodState& state);
// Digest of the verdicts alone, with the decision bookkeeping excluded.
Digest state_digest_of(const PodState& state);
// Digest of the rendered state document. include_decisions selects whether the
// decision records are part of the rendering.
Digest digest_of(const PodState& state, bool include_decisions);

// Fingerprint of a single decision, used to prove that unchanged fingerprints
// imply unchanged decisions.
Digest decision_fingerprint(const DecisionRecord& record);

// ---- canonical text -------------------------------------------------------

Result<std::string> to_text(const PodSnapshot& snapshot);
Result<PodSnapshot> snapshot_from_text(std::string_view text);
Result<std::string> to_text(const PodState& state, bool include_decisions = true);

// Renders one decision record; used by the inspection tooling and by the
// fingerprint computation.
std::string decision_text(const DecisionRecord& record);

}  // namespace podfabric::codec
