// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "podfabric/core/bytes.hpp"

#include <algorithm>

namespace podfabric {

bool is_valid_utf8(std::string_view text) noexcept {
  const auto* p = reinterpret_cast<const unsigned char*>(text.data());
  std::size_t i = 0;
  const std::size_t n = text.size();
  while (i < n) {
    const unsigned char c = p[i];
    std::size_t extra = 0;
    std::uint32_t cp = 0;
    std::uint32_t lowest = 0;
    if (c < 0x80) {
      ++i;
      continue;
    } else if ((c & 0xE0) == 0xC0) {
      extra = 1;
      cp = c & 0x1Fu;
      lowest = 0x80u;
    } else if ((c & 0xF0) == 0xE0) {
      extra = 2;
      cp = c & 0x0Fu;
      lowest = 0x800u;
    } else if ((c & 0xF8) == 0xF0) {
      extra = 3;
      cp = c & 0x07u;
      lowest = 0x10000u;
    } else {
      return false;
    }
    if (i + extra >= n) {
      return false;
    }
    for (std::size_t k = 1; k <= extra; ++k) {
      const unsigned char cc = p[i + k];
      if ((cc & 0xC0) != 0x80) {
        return false;
      }
      cp = (cp << 6) | static_cast<std::uint32_t>(cc & 0x3Fu);
    }
    if (cp < lowest) {
      return false;  // overlong encoding
    }
    if (cp > 0x10FFFFu) {
      return false;
    }
    if (cp >= 0xD800u && cp <= 0xDFFFu) {
      return false;  // surrogate half
    }
    i += extra + 1;
  }
  return true;
}

Status ByteWriter::reserve(std::size_t extra) {
  const std::size_t room = limit_ - std::min(limit_, buffer_.size());
  if (extra > room) {
    overflowed_ = true;
    return Status(Code::Exhausted, "encoded document exceeds the configured byte limit");
  }
  buffer_.resize(buffer_.size() + extra);
  return Status::success();
}

Status ByteWriter::put_u8(std::uint8_t v) {
  const std::size_t at = buffer_.size();
  PODFABRIC_TRY(reserve(1));
  buffer_[at] = static_cast<std::byte>(v);
  return Status::success();
}

Status ByteWriter::put_u16(std::uint16_t v) {
  const std::size_t at = buffer_.size();
  PODFABRIC_TRY(reserve(2));
  buffer_[at] = static_cast<std::byte>((v >> 8) & 0xFFu);
  buffer_[at + 1] = static_cast<std::byte>(v & 0xFFu);
  return Status::success();
}

Status ByteWriter::put_u32(std::uint32_t v) {
  const std::size_t at = buffer_.size();
  PODFABRIC_TRY(reserve(4));
  for (int i = 0; i < 4; ++i) {
    buffer_[at + static_cast<std::size_t>(i)] =
        static_cast<std::byte>((v >> (24 - 8 * i)) & 0xFFu);
  }
  return Status::success();
}

Status ByteWriter::put_u64(std::uint64_t v) {
  const std::size_t at = buffer_.size();
  PODFABRIC_TRY(reserve(8));
  for (int i = 0; i < 8; ++i) {
    buffer_[at + static_cast<std::size_t>(i)] =
        static_cast<std::byte>((v >> (56 - 8 * i)) & 0xFFu);
  }
  return Status::success();
}

Status ByteWriter::put_i64(std::int64_t v) { return put_u64(static_cast<std::uint64_t>(v)); }

Status ByteWriter::put_bool(bool v) { return put_u8(v ? 1u : 0u); }

Status ByteWriter::put_raw(std::span<const std::byte> data) {
  const std::size_t at = buffer_.size();
  PODFABRIC_TRY(reserve(data.size()));
  for (std::size_t i = 0; i < data.size(); ++i) {
    buffer_[at + i] = data[i];
  }
  return Status::success();
}

Status ByteWriter::put_raw(std::string_view data) {
  return put_raw(std::span<const std::byte>(reinterpret_cast<const std::byte*>(data.data()),
                                            data.size()));
}

Status ByteWriter::put_string(std::string_view text) {
  if (!is_valid_utf8(text)) {
    return Status(Code::Invalid, "string field is not valid UTF-8");
  }
  if (text.size() > CodecLimits{}.max_string_bytes) {
    return Status(Code::Exhausted, "string field exceeds the maximum length");
  }
  PODFABRIC_TRY(put_u32(static_cast<std::uint32_t>(text.size())));
  return put_raw(text);
}

Status ByteWriter::put_text(std::string_view text) {
  if (!is_valid_utf8(text)) {
    return Status(Code::Invalid, "text field is not valid UTF-8");
  }
  if (text.size() > CodecLimits{}.max_blob_bytes) {
    return Status(Code::Exhausted, "text field exceeds the maximum length");
  }
  PODFABRIC_TRY(put_u32(static_cast<std::uint32_t>(text.size())));
  return put_raw(text);
}

Status ByteWriter::put_blob(std::span<const std::byte> data) {
  if (data.size() > CodecLimits{}.max_blob_bytes) {
    return Status(Code::Exhausted, "blob exceeds the maximum length");
  }
  PODFABRIC_TRY(put_u32(static_cast<std::uint32_t>(data.size())));
  return put_raw(data);
}

Status ByteWriter::put_count(std::uint32_t count_items) { return put_u32(count_items); }

Result<std::uint8_t> ByteReader::u8() {
  if (remaining() < 1) {
    return Status(Code::Incomplete, "truncated document: expected one byte");
  }
  return static_cast<std::uint8_t>(data_[cursor_++]);
}

Result<std::uint16_t> ByteReader::u16() {
  if (remaining() < 2) {
    return Status(Code::Incomplete, "truncated document: expected two bytes");
  }
  std::uint16_t v = 0;
  for (int i = 0; i < 2; ++i) {
    v = static_cast<std::uint16_t>((v << 8) | static_cast<std::uint8_t>(data_[cursor_++]));
  }
  return v;
}

Result<std::uint32_t> ByteReader::u32() {
  if (remaining() < 4) {
    return Status(Code::Incomplete, "truncated document: expected four bytes");
  }
  std::uint32_t v = 0;
  for (int i = 0; i < 4; ++i) {
    v = (v << 8) | static_cast<std::uint8_t>(data_[cursor_++]);
  }
  return v;
}

Result<std::uint64_t> ByteReader::u64() {
  if (remaining() < 8) {
    return Status(Code::Incomplete, "truncated document: expected eight bytes");
  }
  std::uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v = (v << 8) | static_cast<std::uint8_t>(data_[cursor_++]);
  }
  return v;
}

Result<std::int64_t> ByteReader::i64() {
  PODFABRIC_TRY_ASSIGN(const std::uint64_t raw, u64());
  return static_cast<std::int64_t>(raw);
}

Result<bool> ByteReader::boolean() {
  PODFABRIC_TRY_ASSIGN(const std::uint8_t raw, u8());
  if (raw > 1) {
    return Status(Code::Invalid, "boolean field is neither 0 nor 1");
  }
  return raw == 1;
}

Result<std::span<const std::byte>> ByteReader::raw(std::size_t count_bytes) {
  if (count_bytes > remaining()) {
    return Status(Code::Incomplete, "truncated document: field extents exceed the buffer");
  }
  const auto view = data_.subspan(cursor_, count_bytes);
  cursor_ += count_bytes;
  return view;
}

Result<std::string_view> ByteReader::string() {
  PODFABRIC_TRY_ASSIGN(const std::uint32_t length, u32());
  if (length > limits_.max_string_bytes) {
    return Status(Code::Invalid, "string field declares an oversized length");
  }
  PODFABRIC_TRY_ASSIGN(const auto view, raw(length));
  const std::string_view text(reinterpret_cast<const char*>(view.data()), view.size());
  if (!is_valid_utf8(text)) {
    return Status(Code::Invalid, "string field is not valid UTF-8");
  }
  return text;
}

Result<std::string_view> ByteReader::text() {
  PODFABRIC_TRY_ASSIGN(const std::uint32_t length, u32());
  if (length > limits_.max_blob_bytes) {
    return Status(Code::Invalid, "text field declares an oversized length");
  }
  PODFABRIC_TRY_ASSIGN(const auto view, raw(length));
  const std::string_view value(reinterpret_cast<const char*>(view.data()), view.size());
  if (!is_valid_utf8(value)) {
    return Status(Code::Invalid, "text field is not valid UTF-8");
  }
  return value;
}

Result<std::span<const std::byte>> ByteReader::blob() {
  PODFABRIC_TRY_ASSIGN(const std::uint32_t length, u32());
  if (length > limits_.max_blob_bytes) {
    return Status(Code::Invalid, "blob declares an oversized length");
  }
  return raw(length);
}

Result<std::uint32_t> ByteReader::count(std::uint32_t max_items, std::size_t min_item_bytes) {
  PODFABRIC_TRY_ASSIGN(const std::uint32_t declared, u32());
  if (declared > max_items || declared > limits_.max_sequence_items) {
    return Status(Code::Invalid, "sequence declares more items than the configured bound");
  }
  const std::uint64_t widest =
      static_cast<std::uint64_t>(declared) *
      static_cast<std::uint64_t>(min_item_bytes == 0 ? 1 : min_item_bytes);
  if (widest > static_cast<std::uint64_t>(remaining())) {
    return Status(Code::Invalid, "sequence declares more items than the remaining bytes allow");
  }
  return declared;
}

Status ByteReader::expect_end() const {
  if (!at_end()) {
    return Status(Code::Invalid, "trailing bytes after the end of the document");
  }
  return Status::success();
}

}  // namespace podfabric
