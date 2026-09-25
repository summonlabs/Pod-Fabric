// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "podfabric/control/protocol.hpp"

#include "podfabric/codec/codec.hpp"
#include "podfabric/core/bytes.hpp"

namespace podfabric::control {
namespace {

constexpr std::uint32_t kMaxItems = 1u << 16;

Status check_kind(const Request& request) {
  if (static_cast<std::uint16_t>(request.kind) < static_cast<std::uint16_t>(MessageKind::Hello) ||
      static_cast<std::uint16_t>(request.kind) > static_cast<std::uint16_t>(MessageKind::Erase)) {
    return Status(Code::Invalid, "request kind is not recognised");
  }
  return Status::success();
}

}  // namespace

std::string_view to_string(MessageKind kind) noexcept {
  switch (kind) {
    case MessageKind::Hello: return "hello";
    case MessageKind::Status: return "status";
    case MessageKind::Describe: return "describe";
    case MessageKind::Apply: return "apply";
    case MessageKind::Mint: return "mint";
    case MessageKind::Revoke: return "revoke";
    case MessageKind::CheckAuthority: return "check-authority";
    case MessageKind::SetAdmin: return "set-admin";
    case MessageKind::Retire: return "retire";
    case MessageKind::Shutdown: return "shutdown";
    case MessageKind::Ping: return "ping";
    case MessageKind::Erase: return "erase";
  }
  return "unknown";
}

Status encode_request(const Request& request, std::vector<std::byte>& out) {
  PODFABRIC_TRY(check_kind(request));
  ByteWriter writer;
  PODFABRIC_TRY(writer.put_u16(protocol_version));
  PODFABRIC_TRY(writer.put_u16(static_cast<std::uint16_t>(request.kind)));
  PODFABRIC_TRY(writer.put_u64(request.nonce));
  PODFABRIC_TRY(codec::put_typed_id(writer, request.rack));
  PODFABRIC_TRY(writer.put_u8(static_cast<std::uint8_t>(request.admin)));
  PODFABRIC_TRY(writer.put_bool(request.clear_admin));
  PODFABRIC_TRY(writer.put_u8(static_cast<std::uint8_t>(request.scope)));
  PODFABRIC_TRY(writer.put_i64(request.lifetime));
  PODFABRIC_TRY(codec::encode(request.token, writer));
  PODFABRIC_TRY(codec::put_typed_id(writer, request.lease));
  PODFABRIC_TRY(writer.put_bool(request.kind == MessageKind::Apply));
  if (request.kind == MessageKind::Apply) {
    PODFABRIC_TRY(codec::encode(request.snapshot, writer));
  }
  out = writer.take();
  return Status::success();
}

Result<Request> decode_request(std::span<const std::byte> bytes) {
  ByteReader reader(bytes);
  PODFABRIC_TRY_ASSIGN(const std::uint16_t version, reader.u16());
  if (version != protocol_version) {
    return Status(Code::Unsupported, "protocol revision is not supported");
  }
  Request request;
  PODFABRIC_TRY_ASSIGN(const std::uint16_t kind, reader.u16());
  if (kind < static_cast<std::uint16_t>(MessageKind::Hello) ||
      kind > static_cast<std::uint16_t>(MessageKind::Erase)) {
    return Status(Code::Invalid, "request kind is not recognised");
  }
  request.kind = static_cast<MessageKind>(kind);
  PODFABRIC_TRY_ASSIGN(request.nonce, reader.u64());
  PODFABRIC_TRY_ASSIGN(request.rack, codec::get_typed_id<RackIdTag>(reader, "request.rack"));
  PODFABRIC_TRY_ASSIGN(const std::uint8_t admin, reader.u8());
  if (admin > static_cast<std::uint8_t>(AdminState::Draining)) {
    return Status(Code::Invalid, "request admin mode is out of range");
  }
  request.admin = static_cast<AdminState>(admin);
  PODFABRIC_TRY_ASSIGN(request.clear_admin, reader.boolean());
  PODFABRIC_TRY_ASSIGN(const std::uint8_t scope, reader.u8());
  if (scope > static_cast<std::uint8_t>(AuthorityScope::MaintenanceWindow)) {
    return Status(Code::Invalid, "request authority scope is out of range");
  }
  request.scope = static_cast<AuthorityScope>(scope);
  PODFABRIC_TRY_ASSIGN(request.lifetime, reader.i64());
  PODFABRIC_TRY_ASSIGN(request.token, codec::decode_token(reader));
  PODFABRIC_TRY_ASSIGN(request.lease, codec::get_typed_id<LeaseIdTag>(reader, "request.lease"));
  PODFABRIC_TRY_ASSIGN(const bool has_snapshot, reader.boolean());
  if (has_snapshot) {
    PODFABRIC_TRY_ASSIGN(PodSnapshot snapshot, codec::decode(reader));
    request.snapshot = std::move(snapshot);
  }
  PODFABRIC_TRY(reader.expect_end());
  return request;
}

Status encode_response(const Response& response, std::vector<std::byte>& out) {
  ByteWriter writer;
  PODFABRIC_TRY(writer.put_u16(protocol_version));
  PODFABRIC_TRY(writer.put_u8(static_cast<std::uint8_t>(response.code)));
  PODFABRIC_TRY(writer.put_string(response.message));
  PODFABRIC_TRY(writer.put_u64(response.epoch.value()));
  PODFABRIC_TRY(codec::put_uuid(writer, response.incarnation.id()));
  PODFABRIC_TRY(writer.put_u8(static_cast<std::uint8_t>(response.lifecycle)));
  PODFABRIC_TRY(codec::put_digest(writer, response.state_digest));
  PODFABRIC_TRY(writer.put_u8(static_cast<std::uint8_t>(response.outcome)));
  PODFABRIC_TRY(writer.put_bool(response.revalidation_sound));
  PODFABRIC_TRY(writer.put_string(response.revalidation));
  PODFABRIC_TRY(writer.put_text(response.text));
  PODFABRIC_TRY(writer.put_bool(response.text_truncated));
  PODFABRIC_TRY(writer.put_u8(static_cast<std::uint8_t>(response.recovery)));
  PODFABRIC_TRY(writer.put_bool(response.ambiguous_commit));
  PODFABRIC_TRY(writer.put_u32(response.established_members));
  PODFABRIC_TRY(writer.put_u32(response.fenced_members));
  PODFABRIC_TRY(writer.put_u32(response.stale_members));
  PODFABRIC_TRY(writer.put_u32(response.conflicting_members));
  PODFABRIC_TRY(codec::encode(response.token, writer));
  PODFABRIC_TRY(writer.put_u8(static_cast<std::uint8_t>(response.verdict)));
  PODFABRIC_TRY(writer.put_string(response.verdict_detail));
  PODFABRIC_TRY(writer.put_u64(response.nonce));
  out = writer.take();
  return Status::success();
}

Result<Response> decode_response(std::span<const std::byte> bytes) {
  ByteReader reader(bytes);
  PODFABRIC_TRY_ASSIGN(const std::uint16_t version, reader.u16());
  if (version != protocol_version) {
    return Status(Code::Unsupported, "protocol revision is not supported");
  }
  Response response;
  PODFABRIC_TRY_ASSIGN(const std::uint8_t code, reader.u8());
  if (code > static_cast<std::uint8_t>(Code::Internal)) {
    return Status(Code::Invalid, "response code is out of range");
  }
  response.code = static_cast<Code>(code);
  PODFABRIC_TRY_ASSIGN(const std::string_view message, reader.string());
  response.message.assign(message);
  PODFABRIC_TRY_ASSIGN(const std::uint64_t epoch, reader.u64());
  response.epoch = PodEpoch(epoch);
  PODFABRIC_TRY_ASSIGN(const Uuid incarnation, codec::get_uuid(reader));
  response.incarnation = Incarnation::from_uuid(incarnation);
  PODFABRIC_TRY_ASSIGN(const std::uint8_t lifecycle, reader.u8());
  if (lifecycle > static_cast<std::uint8_t>(LifecycleState::Retired)) {
    return Status(Code::Invalid, "response lifecycle is out of range");
  }
  response.lifecycle = static_cast<LifecycleState>(lifecycle);
  PODFABRIC_TRY_ASSIGN(response.state_digest, codec::get_digest(reader));
  PODFABRIC_TRY_ASSIGN(const std::uint8_t outcome, reader.u8());
  if (outcome > static_cast<std::uint8_t>(ApplyOutcome::Retired)) {
    return Status(Code::Invalid, "response outcome is out of range");
  }
  response.outcome = static_cast<ApplyOutcome>(outcome);
  PODFABRIC_TRY_ASSIGN(response.revalidation_sound, reader.boolean());
  PODFABRIC_TRY_ASSIGN(const std::string_view revalidation, reader.string());
  response.revalidation.assign(revalidation);
  PODFABRIC_TRY_ASSIGN(const std::string_view text, reader.text());
  response.text.assign(text);
  PODFABRIC_TRY_ASSIGN(response.text_truncated, reader.boolean());
  PODFABRIC_TRY_ASSIGN(const std::uint8_t recovery, reader.u8());
  if (recovery > static_cast<std::uint8_t>(persist::RecoveryOutcome::Refused)) {
    return Status(Code::Invalid, "response recovery outcome is out of range");
  }
  response.recovery = static_cast<persist::RecoveryOutcome>(recovery);
  PODFABRIC_TRY_ASSIGN(response.ambiguous_commit, reader.boolean());
  PODFABRIC_TRY_ASSIGN(response.established_members, reader.u32());
  PODFABRIC_TRY_ASSIGN(response.fenced_members, reader.u32());
  PODFABRIC_TRY_ASSIGN(response.stale_members, reader.u32());
  PODFABRIC_TRY_ASSIGN(response.conflicting_members, reader.u32());
  PODFABRIC_TRY_ASSIGN(response.token, codec::decode_token(reader));
  PODFABRIC_TRY_ASSIGN(const std::uint8_t verdict, reader.u8());
  if (verdict > static_cast<std::uint8_t>(AuthorityVerdictCode::ClockAnomaly)) {
    return Status(Code::Invalid, "response authority verdict is out of range");
  }
  response.verdict = static_cast<AuthorityVerdictCode>(verdict);
  PODFABRIC_TRY_ASSIGN(const std::string_view detail, reader.string());
  response.verdict_detail.assign(detail);
  PODFABRIC_TRY_ASSIGN(response.nonce, reader.u64());
  PODFABRIC_TRY(reader.expect_end());
  return response;
}

std::string bounded_text(std::string text, std::size_t max_bytes, bool& truncated) {
  truncated = false;
  if (text.size() <= max_bytes) {
    return text;
  }
  truncated = true;
  std::string out = text.substr(0, max_bytes);
  out.append("\ntruncated true\nend\n");
  return out;
}

}  // namespace podfabric::control
