// Request and response messages for the pod wire protocol.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "podfabric/control/controller.hpp"
#include "podfabric/core/status.hpp"
#include "podfabric/model/snapshot.hpp"
#include "podfabric/persist/store.hpp"

namespace podfabric::control {

inline constexpr std::uint16_t protocol_version = 1;

enum class MessageKind : std::uint16_t {
  Hello = 1,
  Status = 2,
  Describe = 3,
  Apply = 4,
  Mint = 5,
  Revoke = 6,
  CheckAuthority = 7,
  SetAdmin = 8,
  Retire = 9,
  Shutdown = 10,
  Ping = 11,
  Erase = 12,
};
std::string_view to_string(MessageKind kind) noexcept;

struct Request {
  MessageKind kind{MessageKind::Status};
  PodSnapshot snapshot{};
  RackId rack{};
  // SetAdmin carries the requested mode; ClearAdmin removes the override.
  AdminState admin{AdminState::Enabled};
  bool clear_admin{false};
  AuthorityScope scope{AuthorityScope::Pod};
  Nanos lifetime{0};
  AuthorityToken token{};
  LeaseId lease{};
  std::uint64_t nonce{0};

  friend bool operator==(const Request&, const Request&) noexcept = default;
};

struct Response {
  Code code{Code::Ok};
  std::string message{};
  PodEpoch epoch{};
  Incarnation incarnation{};
  LifecycleState lifecycle{LifecycleState::Constructing};
  Digest state_digest{};
  ApplyOutcome outcome{ApplyOutcome::Unchanged};
  bool revalidation_sound{true};
  std::string revalidation{};
  std::string text{};
  bool text_truncated{false};
  persist::RecoveryOutcome recovery{persist::RecoveryOutcome::Fresh};
  bool ambiguous_commit{false};
  std::uint32_t established_members{0};
  std::uint32_t fenced_members{0};
  std::uint32_t stale_members{0};
  std::uint32_t conflicting_members{0};
  AuthorityToken token{};
  AuthorityVerdictCode verdict{AuthorityVerdictCode::NoToken};
  std::string verdict_detail{};
  std::uint64_t nonce{0};

  friend bool operator==(const Response&, const Response&) noexcept = default;
};

Status encode_request(const Request& request, std::vector<std::byte>& out);
Result<Request> decode_request(std::span<const std::byte> bytes);
Status encode_response(const Response& response, std::vector<std::byte>& out);
Result<Response> decode_response(std::span<const std::byte> bytes);

// Renders a state document for the wire, bounded and explicitly marked when it
// had to be cut.
std::string bounded_text(std::string text, std::size_t max_bytes, bool& truncated);

}  // namespace podfabric::control
