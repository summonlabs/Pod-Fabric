// Content digests and integrity checksums.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "podfabric/core/status.hpp"

namespace podfabric {

// SHA-256 (FIPS 180-4). Used to bind a pod-level decision to the exact bytes of
// the evidence it consumed, and to integrity-check persisted snapshots.
class Sha256 {
 public:
  static constexpr std::size_t digest_size = 32;
  static constexpr std::size_t block_size = 64;

  Sha256() noexcept;

  void update(std::span<const std::byte> data) noexcept;
  void update(std::string_view data) noexcept;
  // Finalises the digest and resets the object to its initial state.
  std::array<std::uint8_t, digest_size> finish() noexcept;

 private:
  void compress(const std::uint8_t* block) noexcept;

  std::array<std::uint32_t, 8> state_{};
  std::array<std::uint8_t, block_size> buffer_{};
  std::size_t buffered_{0};
  std::uint64_t total_{0};
};

// A 32-byte content digest with canonical lowercase hex rendering.
class Digest {
 public:
  constexpr Digest() noexcept = default;

  static constexpr std::size_t size = Sha256::digest_size;
  static constexpr std::size_t hex_size = size * 2;

  static Digest from_bytes(const std::array<std::uint8_t, size>& bytes) noexcept {
    Digest d;
    d.bytes_ = bytes;
    return d;
  }
  static Result<Digest> parse(std::string_view hex);
  static Digest of(std::span<const std::byte> data) noexcept;
  static Digest of(std::string_view data) noexcept;

  const std::array<std::uint8_t, size>& bytes() const noexcept { return bytes_; }
  bool is_zero() const noexcept;

  std::string to_hex() const;

  friend bool operator==(const Digest&, const Digest&) noexcept = default;
  friend auto operator<=>(const Digest&, const Digest&) noexcept = default;

 private:
  std::array<std::uint8_t, size> bytes_{};
};

std::string to_string(const Digest& d);

// CRC-32C (Castagnoli). Used for per-record integrity in the write-ahead log
// and on the wire, where a full digest would be wasteful; snapshot files use
// SHA-256.
std::uint32_t crc32c(std::span<const std::byte> data) noexcept;
std::uint32_t crc32c(std::string_view data) noexcept;

// Incremental form, for a checksum that covers fields on both sides of the
// stored value.
class Crc32c {
 public:
  void update(std::span<const std::byte> data) noexcept;
  void update(std::string_view data) noexcept;
  std::uint32_t value() const noexcept;
  void reset() noexcept { state_ = 0xFFFFFFFFu; }

 private:
  std::uint32_t state_{0xFFFFFFFFu};
};

// Incremental SHA-256 over a caller-supplied byte producer is not needed here;
// all call sites hash materialised buffers so bounds are validated first.

}  // namespace podfabric
