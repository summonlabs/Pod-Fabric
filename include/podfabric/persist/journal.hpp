// Append-only transaction log framing.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "podfabric/core/digest.hpp"
#include "podfabric/core/status.hpp"
#include "podfabric/model/primitives.hpp"

namespace podfabric::persist {

inline constexpr std::uint16_t journal_version = 1;
inline constexpr std::size_t journal_header_size = 16;
inline constexpr std::size_t journal_record_prefix_size = 20;
// Hard ceiling on a single record, independent of policy: the framing itself
// refuses to describe more than this so a hostile length can never drive an
// allocation.
inline constexpr std::uint32_t journal_hard_max_record = 1u << 24;

enum class RecordType : std::uint16_t {
  Checkpoint = 1,  // a complete durable state, not part of a transaction
  Begin = 2,       // opens a transaction with a complete durable state body
  Commit = 3,      // closes the transaction opened by the matching Begin
};
std::string_view to_string(RecordType type) noexcept;

struct JournalRecord {
  LogSequence sequence{};
  RecordType type{RecordType::Checkpoint};
  std::vector<std::byte> payload{};

  friend bool operator==(const JournalRecord&, const JournalRecord&) noexcept = default;
};

enum class ScanStatus : std::uint8_t {
  Clean = 0,
  TornTail,       // the tail is an incomplete record; it was never committed
  CorruptTail,    // the tail failed its checksum; discarded bytes are reported
  UnsupportedVersion,
  OversizedRecord,
  Corrupt,        // framing is not recoverable at all
};
std::string_view to_string(ScanStatus status) noexcept;

struct JournalScan {
  ScanStatus status{ScanStatus::Clean};
  std::string detail{};
  std::vector<JournalRecord> records{};
  // Offset just past the last record that passed validation. Everything from
  // here on is discarded and reported.
  std::uint64_t valid_bytes{0};
  std::uint64_t discarded_bytes{0};
  std::uint64_t total_bytes{0};
  bool repaired{false};
};

Status encode_journal_header(std::vector<std::byte>& out, std::uint16_t version);
Result<std::uint16_t> decode_journal_header(std::span<const std::byte> bytes);

Status encode_record(const JournalRecord& record, std::size_t max_record_bytes,
                     std::vector<std::byte>& out);

struct RecordPrefix {
  LogSequence sequence{};
  RecordType type{RecordType::Checkpoint};
  std::uint32_t length{0};
  std::uint32_t crc{0};
};

Result<RecordPrefix> parse_record_prefix(std::span<const std::byte> prefix, std::size_t max_record_bytes);

// Validates checksum and framing on a complete record (prefix plus payload).
bool verify_record(std::span<const std::byte> record, const RecordPrefix& prefix) noexcept;

// Scans a complete journal image. Never skips over a damaged record: the scan
// stops at the first one and reports how many bytes were discarded.
JournalScan scan_journal(std::span<const std::byte> bytes, std::size_t max_record_bytes);

}  // namespace podfabric::persist
