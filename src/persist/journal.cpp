// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "podfabric/persist/journal.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace podfabric::persist {
namespace {

constexpr std::array<std::byte, 8> kMagic = {
    std::byte{'P'}, std::byte{'O'}, std::byte{'D'}, std::byte{'F'},
    std::byte{'B'}, std::byte{'J'}, std::byte{'0'}, std::byte{'1'}};

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

std::string_view to_string(RecordType type) noexcept {
  switch (type) {
    case RecordType::Checkpoint: return "checkpoint";
    case RecordType::Begin: return "begin";
    case RecordType::Commit: return "commit";
  }
  return "unknown";
}

std::string_view to_string(ScanStatus status) noexcept {
  switch (status) {
    case ScanStatus::Clean: return "CLEAN";
    case ScanStatus::TornTail: return "TORN_TAIL";
    case ScanStatus::CorruptTail: return "CORRUPT_TAIL";
    case ScanStatus::UnsupportedVersion: return "UNSUPPORTED_VERSION";
    case ScanStatus::OversizedRecord: return "OVERSIZED_RECORD";
    case ScanStatus::Corrupt: return "CORRUPT";
  }
  return "CORRUPT";
}

Status encode_journal_header(std::vector<std::byte>& out, std::uint16_t version) {
  out.clear();
  out.reserve(journal_header_size);
  out.insert(out.end(), kMagic.begin(), kMagic.end());
  put_u16(out, version);
  put_u16(out, 0);
  const std::uint32_t crc = crc32c(std::span<const std::byte>(out.data(), out.size()));
  put_u32(out, crc);
  return Status::success();
}

Result<std::uint16_t> decode_journal_header(std::span<const std::byte> bytes) {
  if (bytes.size() < journal_header_size) {
    return Status(Code::Incomplete, "journal header is truncated");
  }
  if (std::memcmp(bytes.data(), kMagic.data(), kMagic.size()) != 0) {
    return Status(Code::Corrupt, "journal magic does not match");
  }
  const std::uint32_t crc = read_u32(bytes.data() + 12);
  const std::uint32_t actual = crc32c(bytes.subspan(0, 12));
  if (crc != actual) {
    return Status(Code::Corrupt, "journal header checksum mismatch");
  }
  const std::uint16_t version = read_u16(bytes.data() + 8);
  if (version != journal_version) {
    return Status(Code::Unsupported, "journal revision is not supported");
  }
  return version;
}

Status encode_record(const JournalRecord& record, std::size_t max_record_bytes,
                     std::vector<std::byte>& out) {
  if (record.payload.size() > journal_hard_max_record) {
    return Status(Code::Exhausted, "journal record exceeds the hard framing limit");
  }
  if (record.payload.size() > max_record_bytes) {
    return Status(Code::Exhausted, "journal record exceeds the configured limit");
  }
  out.clear();
  out.reserve(journal_record_prefix_size + record.payload.size());
  put_u64(out, record.sequence.value());
  put_u16(out, static_cast<std::uint16_t>(record.type));
  put_u16(out, 0);
  put_u32(out, static_cast<std::uint32_t>(record.payload.size()));
  // The checksum covers the framing prefix and the whole payload.
  Crc32c checksum;
  checksum.update(std::span<const std::byte>(out.data(), out.size()));
  checksum.update(record.payload);
  put_u32(out, checksum.value());
  out.insert(out.end(), record.payload.begin(), record.payload.end());
  return Status::success();
}

Result<RecordPrefix> parse_record_prefix(std::span<const std::byte> prefix,
                                         std::size_t max_record_bytes) {
  if (prefix.size() < journal_record_prefix_size) {
    return Status(Code::Incomplete, "record prefix is truncated");
  }
  RecordPrefix parsed;
  parsed.sequence = LogSequence(read_u64(prefix.data()));
  const std::uint16_t type = read_u16(prefix.data() + 8);
  if (type < static_cast<std::uint16_t>(RecordType::Checkpoint) ||
      type > static_cast<std::uint16_t>(RecordType::Commit)) {
    return Status(Code::Corrupt, "record type is not recognised");
  }
  parsed.type = static_cast<RecordType>(type);
  parsed.length = read_u32(prefix.data() + 12);
  parsed.crc = read_u32(prefix.data() + 16);
  const std::size_t limit = std::min(max_record_bytes, static_cast<std::size_t>(journal_hard_max_record));
  if (parsed.length > limit) {
    return Status(Code::Exhausted, "record declares a payload larger than the configured limit");
  }
  return parsed;
}

bool verify_record(std::span<const std::byte> record, const RecordPrefix& prefix) noexcept {
  if (record.size() < journal_record_prefix_size) {
    return false;
  }
  Crc32c checksum;
  checksum.update(record.first(journal_record_prefix_size - 4));
  checksum.update(record.subspan(journal_record_prefix_size));
  return checksum.value() == prefix.crc;
}

JournalScan scan_journal(std::span<const std::byte> bytes, std::size_t max_record_bytes) {
  JournalScan scan;
  scan.total_bytes = bytes.size();
  if (bytes.size() < journal_header_size) {
    scan.status = ScanStatus::Corrupt;
    scan.detail = "journal is shorter than its header";
    return scan;
  }
  const auto header = decode_journal_header(bytes.subspan(0, journal_header_size));
  if (!header.ok()) {
    scan.status = header.code() == Code::Unsupported ? ScanStatus::UnsupportedVersion
                                                     : ScanStatus::Corrupt;
    scan.detail = header.status().message();
    return scan;
  }

  std::uint64_t offset = journal_header_size;
  while (offset < bytes.size()) {
    if (bytes.size() - offset < journal_record_prefix_size) {
      scan.status = ScanStatus::TornTail;
      scan.detail = "the journal ends inside a record prefix";
      break;
    }
    const auto prefix = parse_record_prefix(
        bytes.subspan(static_cast<std::size_t>(offset), journal_record_prefix_size),
        max_record_bytes);
    if (!prefix.ok()) {
      const bool oversized = prefix.code() == Code::Exhausted;
      // Damage that follows at least one intact record is tail damage: the
      // records before it are still authoritative, and everything from here on
      // is discarded and reported. Damage at the very start is unrecoverable.
      if (!scan.records.empty()) {
        scan.status = ScanStatus::CorruptTail;
      } else {
        scan.status = oversized ? ScanStatus::OversizedRecord : ScanStatus::Corrupt;
      }
      scan.detail = prefix.status().message();
      break;
    }
    const std::uint64_t frame = journal_record_prefix_size +
                                static_cast<std::uint64_t>(prefix.value().length);
    if (static_cast<std::uint64_t>(bytes.size()) - offset < frame) {
      scan.status = ScanStatus::TornTail;
      scan.detail = "the journal ends inside a record payload";
      break;
    }
    const auto record = bytes.subspan(static_cast<std::size_t>(offset),
                                      static_cast<std::size_t>(frame));
    if (!verify_record(record, prefix.value())) {
      scan.status = ScanStatus::CorruptTail;
      scan.detail = "record checksum mismatch";
      break;
    }
    JournalRecord entry;
    entry.sequence = prefix.value().sequence;
    entry.type = prefix.value().type;
    entry.payload.assign(record.begin() + static_cast<std::ptrdiff_t>(journal_record_prefix_size),
                         record.end());
    scan.records.push_back(std::move(entry));
    offset += frame;
    scan.valid_bytes = offset;
  }

  if (scan.status == ScanStatus::Clean && offset != bytes.size()) {
    scan.status = ScanStatus::TornTail;
    scan.detail = "trailing bytes after the last record";
  }
  scan.discarded_bytes = scan.total_bytes - scan.valid_bytes;
  if (scan.status != ScanStatus::Clean && scan.records.empty() && scan.valid_bytes == 0) {
    // Nothing at all could be trusted; the caller must not treat this as a
    // recoverable store.
    scan.valid_bytes = journal_header_size;
    scan.discarded_bytes = scan.total_bytes - journal_header_size;
  }
  return scan;
}

}  // namespace podfabric::persist
