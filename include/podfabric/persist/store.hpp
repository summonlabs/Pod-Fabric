// Durable pod store: checkpoints plus a write-ahead journal.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "podfabric/core/status.hpp"
#include "podfabric/model/policy.hpp"
#include "podfabric/persist/durable_state.hpp"
#include "podfabric/persist/journal.hpp"
#include "podfabric/platform/file.hpp"

namespace podfabric::persist {

struct StoreOptions {
  std::filesystem::path directory{};
  Policy policy{};
  // Ask the operating system to persist every commit to stable storage.
  bool sync_on_commit{true};
  // Truncate a damaged tail at the last record that passed its checksum. The
  // discarded byte count is always reported; nothing is ever silently skipped.
  bool repair_torn_tail{true};
};

enum class RecoveryOutcome : std::uint8_t {
  Fresh = 0,             // no durable state existed
  Clean,                 // every record replayed and the tail was intact
  TornTailRepaired,      // an incomplete tail record was discarded
  CorruptTailRepaired,   // a checksum-failing tail was discarded
  AmbiguousCommitRolledBack,  // a Begin without a Commit was rolled back
  UnsupportedVersion,    // durable state revision is not understood
  Corrupt,               // durable state is damaged beyond conservative recovery
  Refused,               // the store directory is locked or unusable
};
std::string_view to_string(RecoveryOutcome outcome) noexcept;

struct RecoveryReport {
  RecoveryOutcome outcome{RecoveryOutcome::Fresh};
  std::string detail{};
  std::uint64_t records_scanned{0};
  std::uint64_t records_applied{0};
  std::uint64_t records_skipped{0};
  std::uint64_t discarded_bytes{0};
  bool ambiguous_commit{false};
  bool recovered_from_store{false};
  LogSequence last_sequence{};
  Nanos recovered_at{0};

  // A store that refuses to open must never be used; a store that opened with
  // a repaired tail is usable but its history is incomplete.
  bool usable() const noexcept {
    return outcome != RecoveryOutcome::Corrupt && outcome != RecoveryOutcome::Refused &&
           outcome != RecoveryOutcome::UnsupportedVersion;
  }
  bool repaired() const noexcept {
    return outcome == RecoveryOutcome::TornTailRepaired ||
           outcome == RecoveryOutcome::CorruptTailRepaired ||
           outcome == RecoveryOutcome::AmbiguousCommitRolledBack;
  }
};

class PodStore {
 public:
  explicit PodStore(StoreOptions options);
  PodStore(const PodStore&) = delete;
  PodStore& operator=(const PodStore&) = delete;
  ~PodStore();

  // Opens, recovers, and takes the exclusive store lock. Idempotent only in the
  // sense that a second call on an open store is REFUSED.
  Status open();
  Status close();
  bool is_open() const noexcept { return open_; }

  const DurableState& state() const noexcept { return state_; }
  const RecoveryReport& recovery() const noexcept { return recovery_; }
  const std::filesystem::path& directory() const noexcept { return options_.directory; }
  const std::filesystem::path& journal_path() const noexcept { return journal_path_; }
  const std::filesystem::path& checkpoint_path() const noexcept { return checkpoint_path_; }

  // Writes Begin+Commit for the next state and optionally syncs. The caller is
  // responsible for assigning every field except sequence and written_at,
  // which the store owns.
  Status commit(DurableState next);

  // Collapses the journal into a fresh checkpoint.
  Status compact();

  // Removes every durable artefact. Only valid while open; used by explicit
  // operator actions and by tests.
  Status erase_all();

  // Byte size of the journal on disk.
  Result<std::uint64_t> journal_bytes() const;

 private:
  Status load_checkpoint(DurableState& out, bool& present, std::uint64_t& sequence);
  Status write_checkpoint();
  Status replay();

  StoreOptions options_;
  platform::FileLock lock_{};
  platform::File journal_{};
  std::filesystem::path journal_path_{};
  std::filesystem::path checkpoint_path_{};
  std::filesystem::path lock_path_{};
  DurableState state_{};
  RecoveryReport recovery_{};
  std::uint64_t checkpoint_sequence_{0};
  bool have_checkpoint_{false};
  bool open_{false};
};

// Location of the durable artefacts inside a store directory.
std::filesystem::path journal_file(const std::filesystem::path& directory);
std::filesystem::path checkpoint_file(const std::filesystem::path& directory);
std::filesystem::path lock_file(const std::filesystem::path& directory);

}  // namespace podfabric::persist
