// Durable store tests: framing, recovery, torn tails, replay.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "podfabric/persist/store.hpp"
#include "podfabric/platform/file.hpp"
#include "process.hpp"
#include "test.hpp"

using namespace podfabric;
using namespace podfabric::persist;

namespace {

DurableState make_state(const char* pod, std::uint64_t epoch, std::uint64_t fencing) {
  DurableState state;
  state.pod = PodId::parse(pod).value();
  state.epoch = PodEpoch(epoch);
  const auto incarnation = Incarnation::fresh();
  state.writer = incarnation.ok() ? incarnation.value() : Incarnation{};
  state.lifecycle = LifecycleState::Active;
  state.fencing = FencingSequence(fencing);
  MemberExpectation expectation;
  expectation.rack = RackId::from_canonical_literal("rack-1");
  expectation.generation = RackGeneration(7);
  expectation.digest = Digest::of("rack-1 descriptor");
  state.expectations.push_back(expectation);
  return state;
}

std::vector<std::byte> read_all(const std::string& path) {
  const auto bytes = platform::read_file(path, 64ull * 1024ull * 1024ull);
  return bytes.ok() ? bytes.value() : std::vector<std::byte>{};
}

void rewrite(const std::string& path, const std::vector<std::byte>& bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}

}  // namespace

PF_TEST(persist, journal_scan_accepts_a_well_formed_image) {
  std::vector<std::byte> image;
  PF_REQUIRE(encode_journal_header(image, journal_version).ok());
  JournalRecord first;
  first.sequence = LogSequence(1);
  first.type = RecordType::Checkpoint;
  first.payload = {std::byte{1}, std::byte{2}};
  JournalRecord second;
  second.sequence = LogSequence(2);
  second.type = RecordType::Commit;
  std::vector<std::byte> frame;
  PF_REQUIRE(encode_record(first, 1024, frame).ok());
  image.insert(image.end(), frame.begin(), frame.end());
  PF_REQUIRE(encode_record(second, 1024, frame).ok());
  image.insert(image.end(), frame.begin(), frame.end());

  const JournalScan scan = scan_journal(image, 1024);
  PF_CHECK_EQ(scan.status, ScanStatus::Clean);
  PF_CHECK_EQ(scan.records.size(), std::size_t(2));
  PF_CHECK_EQ(scan.valid_bytes, image.size());
  PF_CHECK_EQ(scan.discarded_bytes, 0ull);
}

PF_TEST(persist, journal_scan_detects_a_torn_tail) {
  std::vector<std::byte> image;
  PF_REQUIRE(encode_journal_header(image, journal_version).ok());
  JournalRecord record;
  record.sequence = LogSequence(1);
  record.type = RecordType::Checkpoint;
  record.payload.assign(64, std::byte{7});
  std::vector<std::byte> frame;
  PF_REQUIRE(encode_record(record, 1024, frame).ok());
  image.insert(image.end(), frame.begin(), frame.end());
  const std::size_t good = image.size();
  image.insert(image.end(), frame.begin(), frame.begin() + 30);

  const JournalScan scan = scan_journal(image, 1024);
  PF_CHECK_EQ(scan.status, ScanStatus::TornTail);
  PF_CHECK_EQ(scan.records.size(), std::size_t(1));
  PF_CHECK_EQ(scan.valid_bytes, good);
  PF_CHECK_EQ(scan.discarded_bytes, 30ull);
}

PF_TEST(persist, journal_scan_detects_a_flipped_byte) {
  std::vector<std::byte> image;
  PF_REQUIRE(encode_journal_header(image, journal_version).ok());
  JournalRecord record;
  record.sequence = LogSequence(1);
  record.type = RecordType::Checkpoint;
  record.payload.assign(16, std::byte{3});
  std::vector<std::byte> frame;
  PF_REQUIRE(encode_record(record, 1024, frame).ok());
  image.insert(image.end(), frame.begin(), frame.end());
  image[journal_header_size + 24] = std::byte{0xEE};

  const JournalScan scan = scan_journal(image, 1024);
  PF_CHECK_EQ(scan.status, ScanStatus::CorruptTail);
  PF_CHECK(scan.records.empty());
}

PF_TEST(persist, journal_scan_rejects_an_unknown_revision_and_a_bad_magic) {
  std::vector<std::byte> image;
  PF_REQUIRE(encode_journal_header(image, journal_version).ok());
  std::vector<std::byte> foreign = image;
  foreign[0] = std::byte{'X'};
  PF_CHECK_EQ(scan_journal(foreign, 1024).status, ScanStatus::Corrupt);
  std::vector<std::byte> future = image;
  future[9] = std::byte{9};
  // The header checksum covers the revision, so a tampered revision is corrupt
  // rather than silently accepted.
  PF_CHECK_EQ(scan_journal(future, 1024).status, ScanStatus::Corrupt);
  PF_CHECK_EQ(scan_journal(std::span<const std::byte>(), 1024).status, ScanStatus::Corrupt);
  PF_CHECK_EQ(scan_journal(std::span<const std::byte>(image.data(), 4), 1024).status,
              ScanStatus::Corrupt);
}

PF_TEST(persist, journal_scan_rejects_an_oversized_record) {
  std::vector<std::byte> image;
  PF_REQUIRE(encode_journal_header(image, journal_version).ok());
  JournalRecord record;
  record.sequence = LogSequence(1);
  record.type = RecordType::Checkpoint;
  record.payload.assign(4096, std::byte{0});
  std::vector<std::byte> frame;
  PF_REQUIRE(encode_record(record, 8192, frame).ok());
  image.insert(image.end(), frame.begin(), frame.end());
  PF_CHECK_EQ(scan_journal(image, 128).status, ScanStatus::OversizedRecord);
}

PF_TEST(persist, a_store_round_trips_through_close_and_reopen) {
  const std::string directory = test::make_scratch_directory("store-roundtrip");
  StoreOptions options;
  options.directory = directory;
  DurableState recovered_state;
  {
    PodStore store(options);
    PF_REQUIRE(store.open().ok());
    PF_CHECK_EQ(store.recovery().outcome, RecoveryOutcome::Fresh);
    PF_REQUIRE(store.commit(make_state("pod-persist", 4, 2)).ok());
    PF_REQUIRE(store.commit(make_state("pod-persist", 5, 3)).ok());
    recovered_state = store.state();
    PF_CHECK(store.close().ok());
  }
  {
    PodStore store(options);
    PF_REQUIRE(store.open().ok());
    PF_CHECK_EQ(store.recovery().outcome, RecoveryOutcome::Clean);
    PF_CHECK_EQ(store.state(), recovered_state);
    PF_CHECK_EQ(store.state().epoch, PodEpoch(5));
    PF_CHECK_EQ(store.recovery().records_applied, 2ull);
    PF_CHECK(store.close().ok());
  }
  test::remove_directory(directory);
}

PF_TEST(persist, compaction_keeps_the_state_and_bounds_the_journal) {
  const std::string directory = test::make_scratch_directory("store-compact");
  StoreOptions options;
  options.directory = directory;
  DurableState latest;
  {
    PodStore store(options);
    PF_REQUIRE(store.open().ok());
    for (std::uint64_t epoch = 1; epoch <= 8; ++epoch) {
      PF_REQUIRE(store.commit(make_state("pod-persist", epoch, epoch)).ok());
    }
    latest = store.state();
    PF_REQUIRE(store.compact().ok());
    const auto size = store.journal_bytes();
    PF_REQUIRE(size.ok());
    PF_CHECK(size.value() < 4096);
    PF_CHECK(platform::path_exists(checkpoint_file(directory)));
    PF_CHECK(store.close().ok());
  }
  {
    PodStore store(options);
    PF_REQUIRE(store.open().ok());
    PF_CHECK_EQ(store.state(), latest);
    PF_CHECK_EQ(store.state().epoch, PodEpoch(8));
    PF_CHECK(store.close().ok());
  }
  test::remove_directory(directory);
}

PF_TEST(persist, a_torn_tail_is_truncated_and_reported) {
  const std::string directory = test::make_scratch_directory("store-torn");
  StoreOptions options;
  options.directory = directory;
  {
    PodStore store(options);
    PF_REQUIRE(store.open().ok());
    PF_REQUIRE(store.commit(make_state("pod-persist", 1, 1)).ok());
    PF_REQUIRE(store.commit(make_state("pod-persist", 2, 2)).ok());
    PF_CHECK(store.close().ok());
  }
  const std::string journal = journal_file(directory).string();
  {
    std::ofstream stream(journal, std::ios::binary | std::ios::app);
    const std::byte junk[37] = {};
    stream.write(reinterpret_cast<const char*>(junk), 37);
  }
  {
    PodStore store(options);
    const Status reopened = store.open();
    PF_REQUIRE_MSG(reopened.ok(), reopened.to_string());
    PF_CHECK_MSG(store.recovery().repaired(),
                 std::string(to_string(store.recovery().outcome)));
    PF_CHECK_EQ(store.recovery().discarded_bytes, 37ull);
    PF_CHECK_EQ(store.state().epoch, PodEpoch(2));
    PF_CHECK(store.close().ok());
  }
  // A clean reopen after the repair is clean again.
  {
    PodStore store(options);
    PF_REQUIRE(store.open().ok());
    PF_CHECK_EQ(store.recovery().outcome, RecoveryOutcome::Clean);
    PF_CHECK(store.close().ok());
  }
  test::remove_directory(directory);
}

PF_TEST(persist, a_begin_without_a_commit_is_rolled_back_and_reported) {
  const std::string directory = test::make_scratch_directory("store-ambiguous");
  StoreOptions options;
  options.directory = directory;
  {
    PodStore store(options);
    PF_REQUIRE(store.open().ok());
    PF_REQUIRE(store.commit(make_state("pod-persist", 1, 1)).ok());
    PF_CHECK(store.close().ok());
  }
  const std::string journal = journal_file(directory).string();
  {
    // Append a Begin frame with no matching Commit, exactly as an interrupted
    // commit would leave it.
    std::vector<std::byte> body;
    PF_REQUIRE(encode(make_state("pod-persist", 2, 2), body).ok());
    JournalRecord record;
    record.sequence = LogSequence(2);
    record.type = RecordType::Begin;
    record.payload = body;
    std::vector<std::byte> frame;
    PF_REQUIRE(encode_record(record, 1 << 20, frame).ok());
    std::ofstream stream(journal, std::ios::binary | std::ios::app);
    stream.write(reinterpret_cast<const char*>(frame.data()),
                 static_cast<std::streamsize>(frame.size()));
  }
  {
    PodStore store(options);
    PF_REQUIRE(store.open().ok());
    PF_CHECK_EQ(store.recovery().outcome, RecoveryOutcome::AmbiguousCommitRolledBack);
    PF_CHECK(store.recovery().ambiguous_commit);
    PF_CHECK_EQ(store.state().epoch, PodEpoch(1));
    PF_CHECK(store.close().ok());
  }
  test::remove_directory(directory);
}

PF_TEST(persist, a_durable_state_from_an_unknown_revision_is_unsupported) {
  const std::string directory = test::make_scratch_directory("store-version");
  StoreOptions options;
  options.directory = directory;
  DurableState state = make_state("pod-persist", 1, 1);
  state.version = 999;
  std::vector<std::byte> bytes;
  // encode() writes the caller's revision, which the decoder then rejects.
  PF_REQUIRE(encode(state, bytes).ok());
  PF_CHECK(decode(std::span<const std::byte>(bytes)).code() == Code::Unsupported);
  test::remove_directory(directory);
}

PF_TEST(persist, a_second_store_on_the_same_directory_is_refused) {
  const std::string directory = test::make_scratch_directory("store-lock");
  StoreOptions options;
  options.directory = directory;
  PodStore first(options);
  PF_REQUIRE(first.open().ok());
  PodStore second(options);
  const Status locked = second.open();
  PF_CHECK_EQ(locked.code(), Code::Refused);
  PF_CHECK_EQ(second.recovery().outcome, RecoveryOutcome::Refused);
  PF_CHECK(first.commit(make_state("pod-persist", 1, 1)).ok());
  PF_CHECK(first.close().ok());
  PF_CHECK(second.open().ok());
  PF_CHECK(second.close().ok());
  test::remove_directory(directory);
}

PF_TEST(persist, a_corrupt_checkpoint_is_refused_rather_than_guessed) {
  const std::string directory = test::make_scratch_directory("store-checkpoint");
  StoreOptions options;
  options.directory = directory;
  {
    PodStore store(options);
    PF_REQUIRE(store.open().ok());
    PF_REQUIRE(store.commit(make_state("pod-persist", 3, 3)).ok());
    PF_REQUIRE(store.compact().ok());
    PF_CHECK(store.close().ok());
  }
  {
    auto bytes = read_all(checkpoint_file(directory).string());
    PF_REQUIRE(bytes.size() > 40);
    bytes[30] = static_cast<std::byte>(static_cast<unsigned char>(bytes[30]) ^ 0xFFu);
    rewrite(checkpoint_file(directory).string(), bytes);
  }
  PodStore store(options);
  const Status opened = store.open();
  PF_CHECK_EQ(opened.code(), Code::Corrupt);
  PF_CHECK_EQ(store.recovery().outcome, RecoveryOutcome::Corrupt);
  test::remove_directory(directory);
}

PF_TEST(persist, an_oversized_journal_is_refused_conservatively) {
  const std::string directory = test::make_scratch_directory("store-oversize");
  StoreOptions options;
  options.directory = directory;
  options.policy.max_journal_bytes = 512;
  options.policy.max_journal_record_bytes = 256;
  {
    PodStore store(options);
    PF_REQUIRE(store.open().ok());
    PF_REQUIRE(store.commit(make_state("pod-persist", 1, 1)).ok());
    PF_CHECK(store.close().ok());
  }
  {
    std::ofstream stream(journal_file(directory).string(), std::ios::binary | std::ios::app);
    std::vector<char> filler(4096, '\0');
    stream.write(filler.data(), static_cast<std::streamsize>(filler.size()));
  }
  PodStore store(options);
  const Status opened = store.open();
  PF_CHECK(!opened.ok());
  test::remove_directory(directory);
}

PF_TEST(persist, erase_all_returns_the_store_to_a_fresh_state) {
  const std::string directory = test::make_scratch_directory("store-erase");
  StoreOptions options;
  options.directory = directory;
  PodStore store(options);
  PF_REQUIRE(store.open().ok());
  PF_REQUIRE(store.commit(make_state("pod-persist", 7, 7)).ok());
  PF_REQUIRE(store.erase_all().ok());
  PF_CHECK(store.state().pod.is_nil());
  PF_CHECK(store.close().ok());
  PodStore reopened(options);
  PF_REQUIRE(reopened.open().ok());
  PF_CHECK_EQ(reopened.recovery().outcome, RecoveryOutcome::Fresh);
  PF_CHECK(reopened.close().ok());
  test::remove_directory(directory);
}
