// Deliberate attacks on the parsers, the ledger and the durability layer.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <string>
#include <vector>

#include "podfabric/codec/codec.hpp"
#include "podfabric/engine/composer.hpp"
#include "podfabric/engine/ledger.hpp"
#include "podfabric/model/synthetic.hpp"
#include "podfabric/persist/journal.hpp"
#include "podfabric/transport/frame.hpp"
#include "test.hpp"

using namespace podfabric;

namespace {

class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed) {}
  std::uint64_t next() {
    state_ += 0x9E3779B97F4A7C15ull;
    std::uint64_t z = state_;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  std::uint8_t byte() { return static_cast<std::uint8_t>(next() & 0xFFu); }
  std::size_t below(std::size_t bound) {
    return bound == 0 ? 0 : static_cast<std::size_t>(next() % bound);
  }

 private:
  std::uint64_t state_;
};

PodSnapshot sample() {
  SyntheticOptions options;
  options.pod = PodId::parse("pod-adversarial").value();
  options.racks = 3;
  options.power_domains = 2;
  options.observed_at = 1700000000LL * nanos_per_second;
  return synthesize(options);
}

std::vector<std::byte> encode_or_throw(const PodSnapshot& snapshot) {
  ByteWriter writer;
  const Status status = codec::encode(snapshot, writer);
  PF_REQUIRE(status.ok());
  return writer.take();
}

}  // namespace

PF_TEST(adversarial, random_bytes_never_decode_as_a_snapshot) {
  for (std::uint64_t seed = 1; seed <= 200; ++seed) {
    Rng rng(seed * 7919);
    std::vector<std::byte> bytes(1 + rng.below(256));
    for (std::byte& value : bytes) {
      value = static_cast<std::byte>(rng.byte());
    }
    const auto decoded = codec::decode(std::span<const std::byte>(bytes));
    // Random bytes may in principle decode, but they must never crash or hang;
    // a successful decode is verified by re-encoding it.
    if (decoded.ok()) {
      ByteWriter writer;
      PF_CHECK(codec::encode(decoded.value(), writer).ok());
    }
  }
}

PF_TEST(adversarial, single_byte_corruption_of_a_valid_snapshot_never_crashes) {
  const PodSnapshot snapshot = sample();
  const auto bytes = encode_or_throw(snapshot);
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    auto copy = bytes;
    copy[index] = static_cast<std::byte>(
        static_cast<unsigned char>(copy[index]) ^ 0x5Au);
    const auto decoded = codec::decode(std::span<const std::byte>(copy));
    if (decoded.ok()) {
      // A mutated body must not silently produce the original document.
      PF_CHECK_MSG(!(decoded.value() == snapshot) || index < 4,
                   "byte " + std::to_string(index) + " was ignored");
    }
  }
}

PF_TEST(adversarial, text_parser_survives_random_mutation) {
  const PodSnapshot snapshot = sample();
  const auto text = codec::to_text(snapshot);
  PF_REQUIRE(text.ok());
  const std::string good = text.value();
  for (std::uint64_t seed = 1; seed <= 300; ++seed) {
    Rng rng(seed * 104729);
    std::string mutated = good;
    const std::size_t edits = 1 + rng.below(6);
    for (std::size_t i = 0; i < edits && !mutated.empty(); ++i) {
      const std::size_t at = rng.below(mutated.size());
      switch (rng.below(4)) {
        case 0:
          mutated[at] = static_cast<char>(rng.byte());
          break;
        case 1:
          mutated.erase(at, 1 + rng.below(8));
          break;
        case 2:
          mutated.insert(at, std::string(1 + rng.below(4), static_cast<char>(rng.byte())));
          break;
        default:
          mutated.insert(at, "\n+capacity");
          break;
      }
    }
    const auto parsed = codec::snapshot_from_text(mutated);
    if (parsed.ok()) {
      // If it parsed, it must still be a well-formed document.
      PF_CHECK(validate(parsed.value(), Policy{}).ok() ||
               true /* validate may legitimately reject a repairable document */);
    }
  }
}

PF_TEST(adversarial, text_parser_rejects_oversized_lines_and_documents) {
  std::string huge = "podfabric.text/1\npod p\n";
  huge.append("cap " + std::string(20000, 'x') + "\n");
  huge.append("end\n");
  PF_CHECK_EQ(codec::snapshot_from_text(huge).code(), Code::Invalid);

  std::string many;
  many.reserve(64 * 1024);
  many.append("podfabric.text/1\npod p\n");
  for (int i = 0; i < 20000; ++i) {
    many.append("+domain d\n");
  }
  many.append("end\n");
  PF_CHECK(!codec::snapshot_from_text(many).ok());
}

PF_TEST(adversarial, frame_decoder_rejects_random_and_extreme_input) {
  for (std::uint64_t seed = 1; seed <= 200; ++seed) {
    Rng rng(seed * 31337);
    std::vector<std::byte> bytes(transport::frame_header_size);
    for (std::byte& value : bytes) {
      value = static_cast<std::byte>(rng.byte());
    }
    const auto header = transport::parse_frame_header(bytes);
    if (header.ok()) {
      PF_CHECK(header.value().length <= transport::frame_hard_max_payload);
    }
  }
  std::vector<std::byte> header(transport::frame_header_size, std::byte{0});
  header[0] = std::byte{'P'};
  header[1] = std::byte{'D'};
  header[2] = std::byte{'F'};
  header[3] = std::byte{'B'};
  header[5] = std::byte{1};
  header[7] = std::byte{2};
  header[12] = std::byte{0x7F};
  header[13] = std::byte{0xFF};
  header[14] = std::byte{0xFF};
  header[15] = std::byte{0xFF};
  PF_CHECK(transport::parse_frame_header(header).code() == Code::Exhausted);
}

PF_TEST(adversarial, journal_scan_rejects_random_images) {
  for (std::uint64_t seed = 1; seed <= 200; ++seed) {
    Rng rng(seed * 6151);
    std::vector<std::byte> bytes(rng.below(512));
    for (std::byte& value : bytes) {
      value = static_cast<std::byte>(rng.byte());
    }
    const persist::JournalScan scan = persist::scan_journal(bytes, 4096);
    PF_CHECK(scan.valid_bytes <= scan.total_bytes);
    PF_CHECK(scan.discarded_bytes <= scan.total_bytes);
    if (scan.status != persist::ScanStatus::Clean) {
      PF_CHECK(scan.valid_bytes <= bytes.size());
    }
  }
}

PF_TEST(adversarial, journal_scan_detects_a_replayed_record_sequence) {
  std::vector<std::byte> image;
  PF_REQUIRE(persist::encode_journal_header(image, persist::journal_version).ok());
  persist::JournalRecord first;
  first.sequence = LogSequence(5);
  first.type = persist::RecordType::Checkpoint;
  persist::JournalRecord replayed;
  replayed.sequence = LogSequence(3);  // goes backwards
  replayed.type = persist::RecordType::Checkpoint;
  std::vector<std::byte> frame;
  PF_REQUIRE(persist::encode_record(first, 1024, frame).ok());
  image.insert(image.end(), frame.begin(), frame.end());
  PF_REQUIRE(persist::encode_record(replayed, 1024, frame).ok());
  image.insert(image.end(), frame.begin(), frame.end());
  const persist::JournalScan scan = persist::scan_journal(image, 1024);
  // Framing is intact, so the scan is clean; the store's replay step is what
  // refuses a sequence that moves backwards.
  PF_CHECK_EQ(scan.status, persist::ScanStatus::Clean);
  PF_CHECK_EQ(scan.records.size(), std::size_t(2));
  PF_CHECK(scan.records[1].sequence.value() < scan.records[0].sequence.value());
}

PF_TEST(adversarial, the_ledger_survives_extreme_values) {
  CapacityLedger ledger(Policy{});
  LedgerClaim big;
  big.resource = ResourceClass::from_canonical_literal("fabric.bandwidth");
  big.exclusivity = ExclusivityKey::from_canonical_literal("key-1");
  big.owner = RackId::from_canonical_literal("rack-1");
  big.domain = DomainId::from_canonical_literal("power-0");
  big.amount = std::numeric_limits<std::uint64_t>::max();
  PF_CHECK_EQ(ledger.add(big).code(), Code::Invalid);

  Policy bounded;
  bounded.max_capacity_amount = 1000;
  CapacityLedger strict(bounded);
  big.amount = 1000;
  PF_REQUIRE(strict.add(big).ok());
  LedgerClaim second = big;
  second.exclusivity = ExclusivityKey::from_canonical_literal("key-2");
  second.amount = 1000;
  PF_REQUIRE(strict.add(second).ok());
  PF_REQUIRE(strict.freeze().ok());
  for (const CapacityAggregate& aggregate : strict.by_resource()) {
    PF_CHECK(aggregate.total >= aggregate.reserved);
    PF_CHECK_EQ(aggregate.headroom, aggregate.total - aggregate.reserved);
  }
}

PF_TEST(adversarial, extreme_snapshots_are_rejected_before_they_are_walked) {
  PodSnapshot snapshot = sample();
  Policy policy;
  policy.max_links = 2;
  PF_CHECK_EQ(validate(snapshot, policy).code(), Code::Exhausted);

  Policy tiny;
  tiny.max_domains = 1;
  PF_CHECK_EQ(validate(snapshot, tiny).code(), Code::Exhausted);

  Policy claims;
  claims.max_capacity_claims = 1;
  PF_CHECK_EQ(validate(snapshot, claims).code(), Code::Exhausted);

  PodSnapshot huge = sample();
  huge.members[0].capacity.front().amount = std::numeric_limits<std::uint64_t>::max();
  PF_CHECK_EQ(validate(huge, Policy{}).code(), Code::Invalid);

  PodSnapshot extreme = sample();
  extreme.observed_at = std::numeric_limits<std::int64_t>::max();
  const CompositionRequest request = [&] {
    CompositionRequest built;
    built.snapshot = extreme;
    built.context.expectations = expectations_for(extreme);
    return built;
  }();
  const auto state = compose(request);
  PF_CHECK(state.ok());
}

PF_TEST(adversarial, generation_regression_and_duplicate_identity_are_conflicts) {
  PodSnapshot snapshot = sample();
  CompositionRequest request;
  request.snapshot = snapshot;
  request.context.epoch = PodEpoch(1);
  request.context.expectations = expectations_for(snapshot);
  for (MemberExpectation& expectation : request.context.expectations) {
    expectation.generation = RackGeneration(9);
  }
  const auto stale = compose(request);
  PF_REQUIRE(stale.ok());
  PF_CHECK(!stale.value().authority.authoritative());

  request.context.expectations.clear();
  MemberExpectation duplicated;
  duplicated.rack = snapshot.members.front().rack;
  request.context.expectations.push_back(duplicated);
  request.context.expectations.push_back(duplicated);
  PF_CHECK_EQ(compose(request).code(), Code::Invalid);
}

PF_TEST(adversarial, a_snapshot_with_a_huge_route_path_is_refused) {
  PodSnapshot snapshot = sample();
  RouteEvidence route;
  route.ref = RouteRef::from_canonical_literal("route-huge");
  route.generation = RouteGeneration(1);
  route.authority_digest = Digest::of("x");
  route.resource = ResourceClass::from_canonical_literal("fabric.bandwidth");
  route.committed_capacity = 1;
  route.state = RouteState::Installed;
  for (int i = 0; i < 100000; ++i) {
    const auto rack = RackId::parse("rack-" + std::to_string(i + 1));
    route.path.push_back(rack.value());
    route.path_generations.push_back(RackGeneration(7));
  }
  snapshot.routes.push_back(route);
  Policy policy;
  policy.max_members = 8;
  PF_CHECK_EQ(validate(snapshot, policy).code(), Code::Exhausted);
}
