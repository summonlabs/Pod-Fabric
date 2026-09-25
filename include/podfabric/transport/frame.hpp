// Length-prefixed, checksummed framing for the pod wire protocol.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "podfabric/core/status.hpp"

namespace podfabric::transport {

inline constexpr std::uint32_t frame_magic = 0x50444642u;  // "PDFB"
inline constexpr std::uint16_t frame_version = 1;
inline constexpr std::size_t frame_header_size = 28;
// A frame is refused before its payload is allocated if it declares more than
// this. The value is deliberately smaller than the largest snapshot the store
// accepts so that a hostile length can never drive an unbounded allocation.
inline constexpr std::uint32_t frame_max_payload = 4u * 1024u * 1024u;
// Frames larger than this are rejected outright at the framing layer.
inline constexpr std::uint32_t frame_hard_max_payload = 32u * 1024u * 1024u;

enum class FrameType : std::uint16_t {
  Hello = 1,
  Request = 2,
  Response = 3,
  Error = 4,
  Ping = 5,
  Pong = 6,
  Shutdown = 7,
  Cancel = 8,
};
std::string_view to_string(FrameType type) noexcept;

struct FrameHeader {
  std::uint16_t version{frame_version};
  FrameType type{FrameType::Request};
  std::uint32_t flags{0};
  std::uint32_t length{0};
  std::uint64_t correlation{0};
  std::uint32_t crc{0};
};

struct Frame {
  FrameType type{FrameType::Request};
  std::uint32_t flags{0};
  std::uint64_t correlation{0};
  std::vector<std::byte> payload{};

  friend bool operator==(const Frame&, const Frame&) noexcept = default;
};

Status encode_frame(const Frame& frame, std::vector<std::byte>& out);
Result<FrameHeader> parse_frame_header(std::span<const std::byte> bytes);
// Validates the checksum over the whole frame (header prefix plus payload).
bool verify_frame(std::span<const std::byte> whole, const FrameHeader& header) noexcept;

}  // namespace podfabric::transport
