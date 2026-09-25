// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "podfabric/transport/frame.hpp"

#include <algorithm>

#include "podfabric/core/digest.hpp"

namespace podfabric::transport {
namespace {

void put_u16(std::vector<std::byte>& out, std::uint16_t value) {
  out.push_back(static_cast<std::byte>((value >> 8) & 0xFFu));
  out.push_back(static_cast<std::byte>(value & 0xFFu));
}

void put_u32(std::vector<std::byte>& out, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    out.push_back(static_cast<std::byte>((value >> (24 - 8 * i)) & 0xFFu));
  }
}

void put_u64(std::vector<std::byte>& out, std::uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<std::byte>((value >> (56 - 8 * i)) & 0xFFu));
  }
}

std::uint16_t read_u16(const std::byte* p) {
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) |
                                    static_cast<std::uint16_t>(p[1]));
}

std::uint32_t read_u32(const std::byte* p) {
  std::uint32_t value = 0;
  for (int i = 0; i < 4; ++i) {
    value = (value << 8) | static_cast<std::uint32_t>(p[i]);
  }
  return value;
}

std::uint64_t read_u64(const std::byte* p) {
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value = (value << 8) | static_cast<std::uint64_t>(p[i]);
  }
  return value;
}

}  // namespace

std::string_view to_string(FrameType type) noexcept {
  switch (type) {
    case FrameType::Hello: return "hello";
    case FrameType::Request: return "request";
    case FrameType::Response: return "response";
    case FrameType::Error: return "error";
    case FrameType::Ping: return "ping";
    case FrameType::Pong: return "pong";
    case FrameType::Shutdown: return "shutdown";
    case FrameType::Cancel: return "cancel";
  }
  return "unknown";
}

Status encode_frame(const Frame& frame, std::vector<std::byte>& out) {
  if (frame.payload.size() > frame_hard_max_payload) {
    return Status(Code::Exhausted, "frame payload exceeds the hard framing limit");
  }
  out.clear();
  out.reserve(frame_header_size + frame.payload.size());
  put_u32(out, frame_magic);
  put_u16(out, frame_version);
  put_u16(out, static_cast<std::uint16_t>(frame.type));
  put_u32(out, frame.flags);
  put_u32(out, static_cast<std::uint32_t>(frame.payload.size()));
  put_u64(out, frame.correlation);
  // The checksum covers everything except itself: the header prefix and the
  // whole payload.
  Crc32c checksum;
  checksum.update(std::span<const std::byte>(out.data(), out.size()));
  checksum.update(frame.payload);
  put_u32(out, checksum.value());
  out.insert(out.end(), frame.payload.begin(), frame.payload.end());
  return Status::success();
}

Result<FrameHeader> parse_frame_header(std::span<const std::byte> bytes) {
  if (bytes.size() < frame_header_size) {
    return Status(Code::Incomplete, "frame header is truncated");
  }
  if (read_u32(bytes.data()) != frame_magic) {
    return Status(Code::Invalid, "frame magic does not match");
  }
  FrameHeader header;
  header.version = read_u16(bytes.data() + 4);
  if (header.version != frame_version) {
    return Status(Code::Unsupported, "frame revision is not supported");
  }
  const std::uint16_t type = read_u16(bytes.data() + 6);
  if (type < static_cast<std::uint16_t>(FrameType::Hello) ||
      type > static_cast<std::uint16_t>(FrameType::Cancel)) {
    return Status(Code::Invalid, "frame type is not recognised");
  }
  header.type = static_cast<FrameType>(type);
  header.flags = read_u32(bytes.data() + 8);
  header.length = read_u32(bytes.data() + 12);
  if (header.length > frame_hard_max_payload) {
    return Status(Code::Exhausted, "frame declares a payload beyond the hard limit");
  }
  header.correlation = read_u64(bytes.data() + 16);
  header.crc = read_u32(bytes.data() + 24);
  // The checksum covers the payload as well, so it is verified by verify_frame
  // once the whole frame has been read.
  return header;
}

bool verify_frame(std::span<const std::byte> whole, const FrameHeader& header) noexcept {
  if (whole.size() != frame_header_size + static_cast<std::size_t>(header.length)) {
    return false;
  }
  Crc32c checksum;
  checksum.update(whole.first(frame_header_size - 4));
  checksum.update(whole.subspan(frame_header_size));
  return checksum.value() == header.crc;
}

}  // namespace podfabric::transport
