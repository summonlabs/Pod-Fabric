// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "podfabric/persist/durable_state.hpp"

#include <algorithm>

#include "podfabric/codec/codec.hpp"
#include "podfabric/core/bytes.hpp"

namespace podfabric::persist {
namespace {

constexpr std::uint32_t kMaxItems = 1u << 20;

}  // namespace

Status encode(const DurableState& state, std::vector<std::byte>& out) {
  ByteWriter writer;
  PODFABRIC_TRY(writer.put_u16(state.version));
  PODFABRIC_TRY(codec::put_typed_id(writer, state.pod));
  PODFABRIC_TRY(writer.put_u64(state.epoch.value()));
  PODFABRIC_TRY(codec::put_uuid(writer, state.writer.id()));
  PODFABRIC_TRY(writer.put_u8(static_cast<std::uint8_t>(state.lifecycle)));
  PODFABRIC_TRY(writer.put_count(static_cast<std::uint32_t>(state.expectations.size())));
  for (const MemberExpectation& expectation : state.expectations) {
    PODFABRIC_TRY(codec::encode(expectation, writer));
  }
  PODFABRIC_TRY(writer.put_count(static_cast<std::uint32_t>(state.tokens.size())));
  for (const AuthorityToken& token : state.tokens) {
    PODFABRIC_TRY(codec::encode(token, writer));
  }
  PODFABRIC_TRY(writer.put_count(static_cast<std::uint32_t>(state.revoked.size())));
  for (const LeaseId& lease : state.revoked) {
    PODFABRIC_TRY(codec::put_typed_id(writer, lease));
  }
  PODFABRIC_TRY(writer.put_count(static_cast<std::uint32_t>(state.admin.size())));
  for (const auto& entry : state.admin) {
    PODFABRIC_TRY(codec::put_typed_id(writer, entry.first));
    PODFABRIC_TRY(writer.put_u8(static_cast<std::uint8_t>(entry.second)));
  }
  PODFABRIC_TRY(writer.put_u64(state.fencing.value()));
  PODFABRIC_TRY(codec::put_digest(writer, state.last_state_digest));
  PODFABRIC_TRY(writer.put_u64(state.sequence));
  PODFABRIC_TRY(writer.put_i64(state.written_at));
  out = writer.take();
  return Status::success();
}

Result<DurableState> decode(std::span<const std::byte> bytes) {
  ByteReader reader(bytes);
  DurableState state;
  PODFABRIC_TRY_ASSIGN(const std::uint16_t version, reader.u16());
  if (version != durable_state_version) {
    return Status(Code::Unsupported, "durable state revision is not supported");
  }
  state.version = version;
  PODFABRIC_TRY_ASSIGN(state.pod, codec::get_typed_id<PodIdTag>(reader, "durable.pod"));
  PODFABRIC_TRY_ASSIGN(const std::uint64_t epoch, reader.u64());
  state.epoch = PodEpoch(epoch);
  PODFABRIC_TRY_ASSIGN(const Uuid writer, codec::get_uuid(reader));
  state.writer = Incarnation::from_uuid(writer);
  PODFABRIC_TRY_ASSIGN(const std::uint8_t lifecycle, reader.u8());
  if (lifecycle > static_cast<std::uint8_t>(LifecycleState::Retired)) {
    return Status(Code::Invalid, "durable state has an out-of-range lifecycle");
  }
  state.lifecycle = static_cast<LifecycleState>(lifecycle);

  PODFABRIC_TRY_ASSIGN(const std::uint32_t expectations, reader.count(kMaxItems, 8));
  state.expectations.reserve(expectations);
  for (std::uint32_t i = 0; i < expectations; ++i) {
    PODFABRIC_TRY_ASSIGN(const MemberExpectation expectation, codec::decode_expectation(reader));
    state.expectations.push_back(expectation);
  }
  PODFABRIC_TRY_ASSIGN(const std::uint32_t tokens, reader.count(kMaxItems, 16));
  state.tokens.reserve(tokens);
  for (std::uint32_t i = 0; i < tokens; ++i) {
    PODFABRIC_TRY_ASSIGN(const AuthorityToken token, codec::decode_token(reader));
    state.tokens.push_back(token);
  }
  PODFABRIC_TRY_ASSIGN(const std::uint32_t revoked, reader.count(kMaxItems, 1));
  state.revoked.reserve(revoked);
  for (std::uint32_t i = 0; i < revoked; ++i) {
    PODFABRIC_TRY_ASSIGN(const LeaseId lease, codec::get_typed_id<LeaseIdTag>(reader, "durable.revoked"));
    state.revoked.push_back(lease);
  }
  PODFABRIC_TRY_ASSIGN(const std::uint32_t admin, reader.count(kMaxItems, 2));
  state.admin.reserve(admin);
  for (std::uint32_t i = 0; i < admin; ++i) {
    PODFABRIC_TRY_ASSIGN(const RackId rack,
                         codec::get_typed_id<RackIdTag>(reader, "durable.admin.rack"));
    PODFABRIC_TRY_ASSIGN(const std::uint8_t mode, reader.u8());
    if (mode > static_cast<std::uint8_t>(AdminState::Draining)) {
      return Status(Code::Invalid, "durable state has an out-of-range admin mode");
    }
    state.admin.emplace_back(rack, static_cast<AdminState>(mode));
  }
  PODFABRIC_TRY_ASSIGN(const std::uint64_t fencing, reader.u64());
  state.fencing = FencingSequence(fencing);
  PODFABRIC_TRY_ASSIGN(state.last_state_digest, codec::get_digest(reader));
  PODFABRIC_TRY_ASSIGN(state.sequence, reader.u64());
  PODFABRIC_TRY_ASSIGN(state.written_at, reader.i64());
  PODFABRIC_TRY(reader.expect_end());

  std::sort(state.expectations.begin(), state.expectations.end());
  if (std::adjacent_find(state.expectations.begin(), state.expectations.end(),
                         [](const MemberExpectation& a, const MemberExpectation& b) {
                           return a.rack == b.rack;
                         }) != state.expectations.end()) {
    return Status(Code::Invalid, "durable state repeats a member expectation");
  }
  return state;
}

Digest digest_of(const DurableState& state) {
  std::vector<std::byte> bytes;
  if (!encode(state, bytes).ok()) {
    return Digest{};
  }
  return Digest::of(bytes);
}

}  // namespace podfabric::persist
