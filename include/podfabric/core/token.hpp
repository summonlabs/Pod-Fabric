// Strongly typed canonical identities.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "podfabric/core/status.hpp"

namespace podfabric {

// Maximum length of a canonical identifier token, in bytes.
inline constexpr std::size_t max_id_length = 64;

// Validation of a canonical identifier token.
//
// A canonical token is 1..64 bytes of [a-z0-9] plus '.', '-', '_' and ':',
// starting with a lowercase letter or digit. Non-canonical spellings are
// rejected rather than normalised so that two spellings can never name the
// same object.
bool is_canonical_id(std::string_view token) noexcept;

// A strongly typed, canonically spelled identifier.
//
// The default-constructed value is the nil identifier and never compares equal
// to a valid one.
template <class Tag>
class BasicId {
 public:
  using tag = Tag;

  constexpr BasicId() noexcept = default;

  static Result<BasicId> parse(std::string_view token) {
    if (!is_canonical_id(token)) {
      return Status(Code::Invalid,
                    "identifier is not a canonical token (1..64 bytes of "
                    "[a-z0-9._-:], starting with a letter or digit)");
    }
    BasicId id;
    id.token_.assign(token);
    return id;
  }

  // Unchecked construction for literals known to be canonical.
  static constexpr BasicId from_canonical_literal(const char* literal) noexcept {
    BasicId id;
    id.token_ = literal;
    return id;
  }

  bool is_nil() const noexcept { return token_.empty(); }
  const std::string& token() const noexcept { return token_; }

  friend bool operator==(const BasicId& a, const BasicId& b) noexcept = default;
  friend auto operator<=>(const BasicId& a, const BasicId& b) noexcept = default;

 private:
  std::string token_;
};

// Nil-safe canonical rendering: the nil identifier renders as "-".
template <class Tag>
std::string to_string(const BasicId<Tag>& id) {
  return id.is_nil() ? std::string("-") : id.token();
}

struct PodIdTag;
struct RackIdTag;
struct LinkIdTag;
struct DomainIdTag;
struct RouteRefTag;
struct ObligationIdTag;
struct ResourceClassTag;
struct ExclusivityKeyTag;
struct PortRefTag;
struct TenantIdTag;
struct DecisionIdTag;
struct LeaseIdTag;
struct NodeIdTag;

using PodId = BasicId<PodIdTag>;
using RackId = BasicId<RackIdTag>;
using LinkId = BasicId<LinkIdTag>;
using DomainId = BasicId<DomainIdTag>;
using RouteRef = BasicId<RouteRefTag>;
using ObligationId = BasicId<ObligationIdTag>;
using ResourceClass = BasicId<ResourceClassTag>;
using ExclusivityKey = BasicId<ExclusivityKeyTag>;
using PortRef = BasicId<PortRefTag>;
using TenantId = BasicId<TenantIdTag>;
using DecisionId = BasicId<DecisionIdTag>;
using LeaseId = BasicId<LeaseIdTag>;
using NodeId = BasicId<NodeIdTag>;

// A 128-bit opaque value used for incarnations, generation counters for
// processes, and nonces. Compared and ordered bytewise.
class Uuid {
 public:
  constexpr Uuid() noexcept = default;

  static Uuid from_bytes(const std::array<std::uint8_t, 16>& bytes) noexcept;
  static Result<Uuid> parse(std::string_view text);
  // Operating-system entropy. Returns UNSUPPORTED if no entropy source is
  // available; callers must not fall back to a predictable value for fencing.
  static Result<Uuid> random();

  const std::array<std::uint8_t, 16>& bytes() const noexcept { return bytes_; }
  bool is_nil() const noexcept;

  std::string to_string() const;
  static constexpr std::size_t text_length = 36;

  friend bool operator==(const Uuid&, const Uuid&) noexcept = default;
  friend auto operator<=>(const Uuid&, const Uuid&) noexcept = default;

 private:
  std::array<std::uint8_t, 16> bytes_{};
};

// Rendering helper for diagnostics.
std::string to_string(const Uuid& id);

}  // namespace podfabric
