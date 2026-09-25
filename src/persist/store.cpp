// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "podfabric/persist/store.hpp"

#include <array>
#include <cstring>
#include <vector>

#include "podfabric/core/bytes.hpp"
#include "podfabric/core/time.hpp"

namespace podfabric::persist {
namespace {

constexpr std::array<std::byte, 8> kCheckpointMagic = {
    std::byte{'P'}, std::byte{'O'}, std::byte{'D'}, std::byte{'F'},
    std::byte{'B'}, std::byte{'S'}, std::byte{'N'}, std::byte{'1'}};
constexpr std::size_t kCheckpointHeader = 20;

void put_u16(std::vector<std::byte>& out, std::uint16_t value) {
  out.push_back(static_cast<std::byte>((value >> 8) & 0xFFu));
  out.push_back(static_cast<std::byte>(value & 0xFFu));
}

void put_u32(std::vector<std::byte>& out, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    out.push_back(static_cast<std::byte>((value >> (24 - 8 * i)) & 0xFFu));
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

Status encode_checkpoint(const DurableState& state, std::vector<std::byte>& out) {
  std::vector<std::byte> body;
  PODFABRIC_TRY(encode(state, body));
  out.clear();
  out.reserve(kCheckpointHeader + body.size() + Digest::size);
  out.insert(out.end(), kCheckpointMagic.begin(), kCheckpointMagic.end());
  put_u16(out, durable_state_version);
  put_u16(out, 0);
  put_u32(out, static_cast<std::uint32_t>(body.size()));
  const std::uint32_t crc = crc32c(std::span<const std::byte>(out.data(), out.size()));
  put_u32(out, crc);
  out.insert(out.end(), body.begin(), body.end());
  const Digest digest = Digest::of(body);
  for (std::uint8_t octet : digest.bytes()) {
    out.push_back(static_cast<std::byte>(octet));
  }
  return Status::success();
}

}  // namespace

std::filesystem::path journal_file(const std::filesystem::path& directory) {
  return directory / "pod.journal";
}
std::filesystem::path checkpoint_file(const std::filesystem::path& directory) {
  return directory / "pod.checkpoint";
}
std::filesystem::path lock_file(const std::filesystem::path& directory) {
  return directory / "pod.lock";
}

std::string_view to_string(RecoveryOutcome outcome) noexcept {
  switch (outcome) {
    case RecoveryOutcome::Fresh: return "FRESH";
    case RecoveryOutcome::Clean: return "CLEAN";
    case RecoveryOutcome::TornTailRepaired: return "TORN_TAIL_REPAIRED";
    case RecoveryOutcome::CorruptTailRepaired: return "CORRUPT_TAIL_REPAIRED";
    case RecoveryOutcome::AmbiguousCommitRolledBack: return "AMBIGUOUS_COMMIT_ROLLED_BACK";
    case RecoveryOutcome::UnsupportedVersion: return "UNSUPPORTED_VERSION";
    case RecoveryOutcome::Corrupt: return "CORRUPT";
    case RecoveryOutcome::Refused: return "REFUSED";
  }
  return "CORRUPT";
}

PodStore::PodStore(StoreOptions options) : options_(std::move(options)) {
  journal_path_ = journal_file(options_.directory);
  checkpoint_path_ = checkpoint_file(options_.directory);
  lock_path_ = lock_file(options_.directory);
}

PodStore::~PodStore() { (void)close(); }

Result<std::uint64_t> PodStore::journal_bytes() const {
  if (!platform::path_exists(journal_path_)) {
    return static_cast<std::uint64_t>(0);
  }
  return platform::path_size(journal_path_);
}

Status PodStore::open() {
  if (open_) {
    return Status(Code::Refused, "the pod store is already open in this process");
  }
  PODFABRIC_TRY(validate(options_.policy));
  if (options_.directory.empty()) {
    return Status(Code::Invalid, "the pod store directory is empty");
  }
  PODFABRIC_TRY(platform::ensure_directory(options_.directory));

  auto acquired = platform::FileLock::acquire(lock_path_);
  if (!acquired.ok()) {
    recovery_.outcome = RecoveryOutcome::Refused;
    recovery_.detail = acquired.status().message();
    return acquired.status();
  }
  lock_ = std::move(acquired).value();

  bool present = false;
  std::uint64_t sequence = 0;
  DurableState checkpoint_state;
  const Status loaded = load_checkpoint(checkpoint_state, present, sequence);
  if (!loaded.ok()) {
    recovery_.outcome = loaded.code() == Code::Unsupported ? RecoveryOutcome::UnsupportedVersion
                                                           : RecoveryOutcome::Corrupt;
    recovery_.detail = loaded.message();
    lock_ = platform::FileLock{};
    return loaded;
  }
  have_checkpoint_ = present;
  checkpoint_sequence_ = sequence;
  if (present) {
    state_ = checkpoint_state;
    recovery_.recovered_from_store = true;
  }

  const auto size_before_open = journal_bytes();
  const bool journal_existed = size_before_open.ok() && size_before_open.value() > 0;
  PODFABRIC_TRY_ASSIGN(auto journal, platform::File::open_append(journal_path_));
  journal_ = std::move(journal);

  PODFABRIC_TRY(replay());

  if (!journal_existed && !present) {
    // A brand new store: lay down the framing so the file is self-describing
    // even before the first commit.
    std::vector<std::byte> header;
    PODFABRIC_TRY(encode_journal_header(header, journal_version));
    PODFABRIC_TRY(journal_.write(header));
    PODFABRIC_TRY(journal_.sync());
  }

  open_ = true;
  recovery_.recovered_at = system_now_nanos();
  return Status::success();
}

Status PodStore::load_checkpoint(DurableState& out, bool& present, std::uint64_t& sequence) {
  present = false;
  sequence = 0;
  if (!platform::path_exists(checkpoint_path_)) {
    return Status::success();
  }
  const auto bytes = platform::read_file(checkpoint_path_,
                                         static_cast<std::uint64_t>(options_.policy.max_journal_bytes) *
                                             4u);
  if (!bytes.ok()) {
    return bytes.status();
  }
  if (bytes.value().size() < kCheckpointHeader + Digest::size) {
    return Status(Code::Corrupt, "checkpoint file is shorter than its framing");
  }
  const auto* base = bytes.value().data();
  if (std::memcmp(base, kCheckpointMagic.data(), kCheckpointMagic.size()) != 0) {
    return Status(Code::Corrupt, "checkpoint magic does not match");
  }
  const std::uint16_t version = read_u16(base + 8);
  if (version != durable_state_version) {
    return Status(Code::Unsupported, "checkpoint revision is not supported");
  }
  const std::uint32_t declared_crc = read_u32(base + 16);
  if (declared_crc != crc32c(std::span<const std::byte>(base, 16))) {
    return Status(Code::Corrupt, "checkpoint header checksum mismatch");
  }
  const std::uint32_t body_length = read_u32(base + 12);
  if (body_length > options_.policy.max_journal_bytes * 4u) {
    return Status(Code::Exhausted, "checkpoint body exceeds the configured bound");
  }
  const std::uint64_t expected = kCheckpointHeader + static_cast<std::uint64_t>(body_length) +
                                 Digest::size;
  if (bytes.value().size() != expected) {
    return Status(Code::Corrupt, "checkpoint length does not match its own header");
  }
  const auto body = std::span<const std::byte>(base + kCheckpointHeader, body_length);
  const auto stored = std::span<const std::byte>(base + kCheckpointHeader + body_length,
                                                 Digest::size);
  std::array<std::uint8_t, Digest::size> stored_bytes{};
  for (std::size_t i = 0; i < stored_bytes.size(); ++i) {
    stored_bytes[i] = static_cast<std::uint8_t>(stored[i]);
  }
  if (!(Digest::from_bytes(stored_bytes) == Digest::of(body))) {
    return Status(Code::Corrupt, "checkpoint payload digest mismatch");
  }
  PODFABRIC_TRY_ASSIGN(DurableState decoded, decode(body));
  out = std::move(decoded);
  present = true;
  sequence = out.sequence;
  return Status::success();
}

Status PodStore::replay() {
  const auto size = journal_.size();
  if (!size.ok()) {
    return size.status();
  }
  if (size.value() == 0) {
    recovery_.outcome = have_checkpoint_ ? RecoveryOutcome::Clean : RecoveryOutcome::Fresh;
    recovery_.last_sequence = LogSequence(checkpoint_sequence_);
    return Status::success();
  }
  const std::uint64_t bound =
      static_cast<std::uint64_t>(options_.policy.max_journal_bytes) * 2u;
  if (size.value() > bound) {
    return Status(Code::Exhausted, "journal exceeds the configured bound and was not replayed");
  }
  PODFABRIC_TRY(journal_.seek(0));
  std::vector<std::byte> image(static_cast<std::size_t>(size.value()));
  std::size_t offset = 0;
  while (offset < image.size()) {
    PODFABRIC_TRY_ASSIGN(
        const std::size_t got,
        journal_.read(std::span<std::byte>(image.data() + offset, image.size() - offset)));
    if (got == 0) {
      return Status(Code::Incomplete, "journal ended before its declared size");
    }
    offset += got;
  }

  const JournalScan scan = scan_journal(image, options_.policy.max_journal_record_bytes);
  if (scan.status == ScanStatus::UnsupportedVersion) {
    return Status(Code::Unsupported, scan.detail);
  }
  if (scan.status == ScanStatus::Corrupt || scan.status == ScanStatus::OversizedRecord) {
    recovery_.outcome = RecoveryOutcome::Corrupt;
    recovery_.detail = scan.detail;
    return Status(Code::Corrupt, "journal framing is not recoverable: " + scan.detail);
  }

  recovery_.records_scanned = scan.records.size();
  recovery_.discarded_bytes = scan.discarded_bytes;
  uint64_t open_txn = 0;
  std::vector<std::byte> pending;
  bool ambiguous = false;

  for (const JournalRecord& record : scan.records) {
    if (have_checkpoint_ && record.sequence.value() <= checkpoint_sequence_) {
      ++recovery_.records_skipped;
      continue;
    }
    if (record.type == RecordType::Checkpoint) {
      if (open_txn != 0) {
        ambiguous = true;
        open_txn = 0;
        pending.clear();
      }
      PODFABRIC_TRY_ASSIGN(DurableState decoded, decode(record.payload));
      state_ = std::move(decoded);
      ++recovery_.records_applied;
      recovery_.recovered_from_store = true;
      continue;
    }
    if (record.type == RecordType::Begin) {
      if (open_txn != 0) {
        // A Begin that was never committed is an ambiguous durable commit.
        ambiguous = true;
      }
      open_txn = record.sequence.value();
      pending = record.payload;
      continue;
    }
    // Commit.
    if (open_txn != record.sequence.value()) {
      recovery_.outcome = RecoveryOutcome::Corrupt;
      recovery_.detail = "a commit record does not match the open transaction";
      return Status(Code::Corrupt, recovery_.detail);
    }
    PODFABRIC_TRY_ASSIGN(DurableState decoded, decode(pending));
    state_ = std::move(decoded);
    ++recovery_.records_applied;
    recovery_.recovered_from_store = true;
    open_txn = 0;
    pending.clear();
  }

  if (open_txn != 0) {
    ambiguous = true;
  }
  recovery_.ambiguous_commit = ambiguous;
  recovery_.last_sequence = LogSequence(state_.sequence);

  if (scan.status == ScanStatus::Clean && !ambiguous) {
    recovery_.outcome = have_checkpoint_ || recovery_.records_applied > 0
                            ? RecoveryOutcome::Clean
                            : RecoveryOutcome::Fresh;
    return Status::success();
  }

  if (scan.status != ScanStatus::Clean && options_.repair_torn_tail) {
    PODFABRIC_TRY(journal_.truncate(scan.valid_bytes));
    PODFABRIC_TRY(journal_.sync());
    recovery_.outcome = ambiguous
                            ? RecoveryOutcome::AmbiguousCommitRolledBack
                            : (scan.status == ScanStatus::TornTail
                                   ? RecoveryOutcome::TornTailRepaired
                                   : RecoveryOutcome::CorruptTailRepaired);
    recovery_.detail = scan.detail;
    return Status::success();
  }
  if (scan.status != ScanStatus::Clean) {
    return Status(Code::Corrupt, "journal tail is damaged and repair is disabled: " + scan.detail);
  }
  recovery_.outcome = RecoveryOutcome::AmbiguousCommitRolledBack;
  recovery_.detail = "a transaction was opened but never committed";
  return Status::success();
}

Status PodStore::write_checkpoint() {
  std::vector<std::byte> image;
  PODFABRIC_TRY(encode_checkpoint(state_, image));
  const std::filesystem::path temp = checkpoint_path_.string() + ".tmp";
  PODFABRIC_TRY(platform::write_file(temp, image));
  PODFABRIC_TRY(platform::atomic_replace(temp, checkpoint_path_));
  return platform::sync_directory(options_.directory);
}

Status PodStore::compact() {
  if (!open_) {
    return Status(Code::Invalid, "the pod store is not open");
  }
  PODFABRIC_TRY(write_checkpoint());
  checkpoint_sequence_ = state_.sequence;
  have_checkpoint_ = true;

  // The journal is truncated only after the checkpoint is durable, so a crash
  // between the two leaves a superset of the truth rather than a gap.
  PODFABRIC_TRY(journal_.truncate(0));
  std::vector<std::byte> header;
  PODFABRIC_TRY(encode_journal_header(header, journal_version));
  PODFABRIC_TRY(journal_.write(header));
  PODFABRIC_TRY(journal_.flush());
  JournalRecord record;
  record.sequence = LogSequence(state_.sequence);
  record.type = RecordType::Checkpoint;
  PODFABRIC_TRY(encode(state_, record.payload));
  std::vector<std::byte> frame;
  PODFABRIC_TRY(encode_record(record, options_.policy.max_journal_record_bytes, frame));
  PODFABRIC_TRY(journal_.write(frame));
  return journal_.sync();
}

Status PodStore::commit(DurableState next) {
  if (!open_) {
    return Status(Code::Invalid, "the pod store is not open");
  }
  next.version = durable_state_version;
  next.sequence = state_.sequence + 1;
  next.written_at = next.written_at == 0 ? system_now_nanos() : next.written_at;

  std::vector<std::byte> body;
  PODFABRIC_TRY(encode(next, body));

  JournalRecord begin;
  begin.sequence = LogSequence(next.sequence);
  begin.type = RecordType::Begin;
  begin.payload = body;
  std::vector<std::byte> begin_frame;
  PODFABRIC_TRY(encode_record(begin, options_.policy.max_journal_record_bytes, begin_frame));

  JournalRecord commit_record;
  commit_record.sequence = LogSequence(next.sequence);
  commit_record.type = RecordType::Commit;
  std::vector<std::byte> commit_frame;
  PODFABRIC_TRY(
      encode_record(commit_record, options_.policy.max_journal_record_bytes, commit_frame));

  PODFABRIC_TRY(journal_.write(begin_frame));
  PODFABRIC_TRY(journal_.write(commit_frame));
  if (options_.sync_on_commit) {
    PODFABRIC_TRY(journal_.sync());
  } else {
    PODFABRIC_TRY(journal_.flush());
  }

  state_ = std::move(next);
  recovery_.last_sequence = LogSequence(state_.sequence);

  const auto size = journal_bytes();
  if (size.ok() && size.value() > options_.policy.max_journal_bytes) {
    PODFABRIC_TRY(compact());
  }
  return Status::success();
}

Status PodStore::erase_all() {
  if (!open_) {
    return Status(Code::Invalid, "the pod store is not open");
  }
  PODFABRIC_TRY(journal_.close());
  PODFABRIC_TRY(platform::remove_file(journal_path_));
  if (platform::path_exists(checkpoint_path_)) {
    PODFABRIC_TRY(platform::remove_file(checkpoint_path_));
  }
  state_ = DurableState{};
  checkpoint_sequence_ = 0;
  have_checkpoint_ = false;
  recovery_ = RecoveryReport{};
  PODFABRIC_TRY_ASSIGN(auto journal, platform::File::open_append(journal_path_));
  journal_ = std::move(journal);
  std::vector<std::byte> header;
  PODFABRIC_TRY(encode_journal_header(header, journal_version));
  PODFABRIC_TRY(journal_.write(header));
  return journal_.sync();
}

Status PodStore::close() {
  if (!open_) {
    journal_ = platform::File{};
    lock_ = platform::FileLock{};
    return Status::success();
  }
  open_ = false;
  PODFABRIC_TRY(journal_.close());
  journal_ = platform::File{};
  lock_ = platform::FileLock{};
  return Status::success();
}

}  // namespace podfabric::persist
