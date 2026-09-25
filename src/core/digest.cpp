// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "podfabric/core/digest.hpp"

#include <cstring>

namespace podfabric {
namespace {

constexpr std::array<std::uint32_t, 64> kSha256K = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

constexpr std::uint32_t rotr(std::uint32_t v, unsigned n) noexcept {
  return (v >> n) | (v << (32u - n));
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

std::array<std::uint32_t, 256> make_crc32c_table() noexcept {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t i = 0; i < 256; ++i) {
    std::uint32_t crc = i;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1u) ? ((crc >> 1) ^ 0x82F63B78u) : (crc >> 1);
    }
    table[i] = crc;
  }
  return table;
}

const std::array<std::uint32_t, 256>& crc32c_table() noexcept {
  static const std::array<std::uint32_t, 256> table = make_crc32c_table();
  return table;
}

}  // namespace

Sha256::Sha256() noexcept {
  state_ = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
            0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
}

void Sha256::compress(const std::uint8_t* block) noexcept {
  std::uint32_t w[64];
  for (int i = 0; i < 16; ++i) {
    w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
           (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
           (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
           (static_cast<std::uint32_t>(block[i * 4 + 3]));
  }
  for (int i = 16; i < 64; ++i) {
    const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
  std::uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
  for (int i = 0; i < 64; ++i) {
    const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const std::uint32_t ch = (e & f) ^ ((~e) & g);
    const std::uint32_t temp1 = h + s1 + ch + kSha256K[static_cast<std::size_t>(i)] + w[i];
    const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = s0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }
  state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
  state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
}

void Sha256::update(std::span<const std::byte> data) noexcept {
  const auto* p = reinterpret_cast<const std::uint8_t*>(data.data());
  std::size_t remaining = data.size();
  total_ += static_cast<std::uint64_t>(remaining);
  if (buffered_ > 0) {
    while (remaining > 0 && buffered_ < block_size) {
      buffer_[buffered_++] = *p++;
      --remaining;
    }
    if (buffered_ == block_size) {
      compress(buffer_.data());
      buffered_ = 0;
    }
  }
  while (remaining >= block_size) {
    compress(p);
    p += block_size;
    remaining -= block_size;
  }
  while (remaining > 0) {
    buffer_[buffered_++] = *p++;
    --remaining;
  }
}

void Sha256::update(std::string_view data) noexcept {
  update(std::span<const std::byte>(reinterpret_cast<const std::byte*>(data.data()), data.size()));
}

std::array<std::uint8_t, Sha256::digest_size> Sha256::finish() noexcept {
  const std::uint64_t bit_length = total_ * 8u;
  const std::uint8_t pad = 0x80;
  update(std::span<const std::byte>(reinterpret_cast<const std::byte*>(&pad), 1));
  const std::uint8_t zero = 0x00;
  while (buffered_ != 56) {
    update(std::span<const std::byte>(reinterpret_cast<const std::byte*>(&zero), 1));
  }
  std::uint8_t length_bytes[8];
  for (int i = 0; i < 8; ++i) {
    length_bytes[i] = static_cast<std::uint8_t>((bit_length >> (56 - 8 * i)) & 0xFFu);
  }
  // Bypass total_ accounting for the length field itself.
  for (int i = 0; i < 8; ++i) {
    buffer_[buffered_++] = length_bytes[i];
  }
  compress(buffer_.data());
  buffered_ = 0;

  std::array<std::uint8_t, digest_size> out{};
  for (std::size_t i = 0; i < 8; ++i) {
    out[i * 4] = static_cast<std::uint8_t>(state_[i] >> 24);
    out[i * 4 + 1] = static_cast<std::uint8_t>(state_[i] >> 16);
    out[i * 4 + 2] = static_cast<std::uint8_t>(state_[i] >> 8);
    out[i * 4 + 3] = static_cast<std::uint8_t>(state_[i]);
  }
  state_ = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
            0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
  buffer_.fill(0);
  buffered_ = 0;
  total_ = 0;
  return out;
}

Digest Digest::of(std::span<const std::byte> data) noexcept {
  Sha256 h;
  h.update(data);
  return Digest::from_bytes(h.finish());
}

Digest Digest::of(std::string_view data) noexcept {
  Sha256 h;
  h.update(data);
  return Digest::from_bytes(h.finish());
}

bool Digest::is_zero() const noexcept {
  for (std::uint8_t b : bytes_) {
    if (b != 0) return false;
  }
  return true;
}

std::string Digest::to_hex() const {
  std::string out;
  out.reserve(hex_size);
  for (std::uint8_t b : bytes_) {
    out.push_back(hex_digit(static_cast<unsigned>(b >> 4)));
    out.push_back(hex_digit(static_cast<unsigned>(b & 0x0F)));
  }
  return out;
}

Result<Digest> Digest::parse(std::string_view hex) {
  if (hex.size() != hex_size) {
    return Status(Code::Invalid, "digest: expected 64 hexadecimal characters");
  }
  std::array<std::uint8_t, size> bytes{};
  for (std::size_t i = 0; i < size; ++i) {
    const int hi = hex_value(hex[i * 2]);
    const int lo = hex_value(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) {
      return Status(Code::Invalid, "digest: non-hexadecimal character");
    }
    bytes[i] = static_cast<std::uint8_t>((hi << 4) | lo);
  }
  return Digest::from_bytes(bytes);
}

std::string to_string(const Digest& d) { return d.to_hex(); }

void Crc32c::update(std::span<const std::byte> data) noexcept {
  const auto& table = crc32c_table();
  for (std::byte b : data) {
    state_ = table[(state_ ^ static_cast<std::uint8_t>(b)) & 0xFFu] ^ (state_ >> 8);
  }
}

void Crc32c::update(std::string_view data) noexcept {
  update(std::span<const std::byte>(reinterpret_cast<const std::byte*>(data.data()),
                                    data.size()));
}

std::uint32_t Crc32c::value() const noexcept { return state_ ^ 0xFFFFFFFFu; }

std::uint32_t crc32c(std::span<const std::byte> data) noexcept {
  const auto& table = crc32c_table();
  std::uint32_t crc = 0xFFFFFFFFu;
  for (std::byte b : data) {
    crc = table[(crc ^ static_cast<std::uint8_t>(b)) & 0xFFu] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

std::uint32_t crc32c(std::string_view data) noexcept {
  return crc32c(std::span<const std::byte>(reinterpret_cast<const std::byte*>(data.data()),
                                            data.size()));
}

}  // namespace podfabric
