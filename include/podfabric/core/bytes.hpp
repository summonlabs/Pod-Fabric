// Bounded binary codec primitives.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "podfabric/core/checked.hpp"
#include "podfabric/core/status.hpp"

namespace podfabric {

// Bounds applied to every externally supplied document before anything is
// allocated. A decoder that is handed a length larger than the enclosing
// buffer is INVALID, never "allocate and hope".
struct CodecLimits {
  std::size_t max_document_bytes = 16u * 1024u * 1024u;
  std::size_t max_string_bytes = 4096;
  std::size_t max_blob_bytes = 1u * 1024u * 1024u;
  std::uint32_t max_sequence_items = 1u << 20;
};

// Strict UTF-8 validation. Rejects overlong encodings, surrogate code points,
// values above U+10FFFF and truncated sequences.
bool is_valid_utf8(std::string_view text) noexcept;

// Appends a byte-wise writer with a hard output limit.
class ByteWriter {
 public:
  explicit ByteWriter(std::size_t limit = CodecLimits{}.max_document_bytes) : limit_(limit) {}

  Status put_u8(std::uint8_t v);
  Status put_u16(std::uint16_t v);
  Status put_u32(std::uint32_t v);
  Status put_u64(std::uint64_t v);
  Status put_i64(std::int64_t v);
  Status put_bool(bool v);
  Status put_raw(std::span<const std::byte> data);
  Status put_raw(std::string_view data);
  Status put_string(std::string_view text);
  // A longer free-text field, bounded by the blob limit rather than the string
  // limit. Used for rendered documents that are legitimately large.
  Status put_text(std::string_view text);
  Status put_blob(std::span<const std::byte> data);
  // Length-prefixed sequence header. The caller must then write exactly the
  // declared number of items. Bounded before use.
  Status put_count(std::uint32_t count);

  std::size_t size() const noexcept { return buffer_.size(); }
  bool overflowed() const noexcept { return overflowed_; }
  const std::vector<std::byte>& buffer() const noexcept { return buffer_; }
  std::vector<std::byte> take() { return std::move(buffer_); }

 private:
  Status reserve(std::size_t extra);

  std::vector<std::byte> buffer_{};
  std::size_t limit_;
  bool overflowed_{false};
};

// A cursor over a bounded buffer. Every read validates that the requested
// extent fits inside the remaining bytes.
class ByteReader {
 public:
  explicit ByteReader(std::span<const std::byte> data, CodecLimits limits = {})
      : data_(data), limits_(limits) {}

  Result<std::uint8_t> u8();
  Result<std::uint16_t> u16();
  Result<std::uint32_t> u32();
  Result<std::uint64_t> u64();
  Result<std::int64_t> i64();
  Result<bool> boolean();
  Result<std::span<const std::byte>> raw(std::size_t count_bytes);
  Result<std::string_view> string();
  Result<std::string_view> text();
  Result<std::span<const std::byte>> blob();
  // Reads a length prefix and validates it against max_items and against the
  // number of bytes that could possibly remain.
  Result<std::uint32_t> count(std::uint32_t max_items, std::size_t min_item_bytes);

  std::size_t remaining() const noexcept { return data_.size() - cursor_; }
  std::size_t cursor() const noexcept { return cursor_; }
  bool at_end() const noexcept { return cursor_ == data_.size(); }
  Status expect_end() const;

 private:
  std::span<const std::byte> data_;
  CodecLimits limits_;
  std::size_t cursor_{0};
};

}  // namespace podfabric
