// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "podfabric/core/token.hpp"

#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace podfabric {
namespace {

constexpr bool is_lower_alnum(char c) noexcept {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

constexpr bool is_separator(char c) noexcept {
  return c == '.' || c == '-' || c == '_' || c == ':';
}

constexpr char hex_digit(unsigned v) noexcept {
  return static_cast<char>(v < 10 ? ('0' + v) : ('a' + (v - 10)));
}

constexpr int hex_value(char c) noexcept {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  return -1;
}

}  // namespace

bool is_canonical_id(std::string_view token) noexcept {
  if (token.empty() || token.size() > max_id_length) {
    return false;
  }
  if (!is_lower_alnum(token.front())) {
    return false;
  }
  if (is_separator(token.back())) {
    return false;
  }
  for (char c : token) {
    if (!is_lower_alnum(c) && !is_separator(c)) {
      return false;
    }
  }
  return true;
}

Uuid Uuid::from_bytes(const std::array<std::uint8_t, 16>& bytes) noexcept {
  Uuid id;
  id.bytes_ = bytes;
  return id;
}

bool Uuid::is_nil() const noexcept {
  for (std::uint8_t b : bytes_) {
    if (b != 0) return false;
  }
  return true;
}

std::string Uuid::to_string() const {
  std::string out;
  out.reserve(text_length);
  for (std::size_t i = 0; i < bytes_.size(); ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) {
      out.push_back('-');
    }
    out.push_back(hex_digit(static_cast<unsigned>(bytes_[i] >> 4)));
    out.push_back(hex_digit(static_cast<unsigned>(bytes_[i] & 0x0F)));
  }
  return out;
}

Result<Uuid> Uuid::parse(std::string_view text) {
  if (text.size() != text_length) {
    return Status(Code::Invalid, "uuid: expected 36 characters");
  }
  std::array<std::uint8_t, 16> bytes{};
  std::size_t out_index = 0;
  std::size_t i = 0;
  while (i < text.size()) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (text[i] != '-') {
        return Status(Code::Invalid, "uuid: misplaced separator");
      }
      ++i;
      continue;
    }
    if (i + 1 >= text.size()) {
      return Status(Code::Invalid, "uuid: truncated hex pair");
    }
    const int hi = hex_value(text[i]);
    const int lo = hex_value(text[i + 1]);
    if (hi < 0 || lo < 0) {
      return Status(Code::Invalid, "uuid: non-hexadecimal character");
    }
    if (out_index >= bytes.size()) {
      return Status(Code::Invalid, "uuid: too many octets");
    }
    bytes[out_index++] = static_cast<std::uint8_t>((hi << 4) | lo);
    i += 2;
  }
  if (out_index != bytes.size()) {
    return Status(Code::Invalid, "uuid: wrong number of octets");
  }
  return Uuid::from_bytes(bytes);
}

Result<Uuid> Uuid::random() {
  std::array<std::uint8_t, 16> bytes{};
#if defined(_WIN32)
  const NTSTATUS rc = ::BCryptGenRandom(nullptr, bytes.data(),
                                        static_cast<ULONG>(bytes.size()),
                                        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  if (rc < 0) {
    return Status(Code::Unsupported, "uuid: BCryptGenRandom unavailable");
  }
#else
  const int fd = ::open("/dev/urandom", O_RDONLY);
  if (fd < 0) {
    return Status(Code::Unsupported, "uuid: /dev/urandom unavailable");
  }
  std::size_t got = 0;
  while (got < bytes.size()) {
    const ssize_t n = ::read(fd, bytes.data() + got, bytes.size() - got);
    if (n <= 0) {
      ::close(fd);
      return Status(Code::Unsupported, "uuid: entropy read failed");
    }
    got += static_cast<std::size_t>(n);
  }
  ::close(fd);
#endif
  // Set the RFC 4122 version/variant bits so renderings are self-describing.
  bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0F) | 0x40);
  bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3F) | 0x80);
  return Uuid::from_bytes(bytes);
}

std::string to_string(const Uuid& id) { return id.to_string(); }

}  // namespace podfabric
