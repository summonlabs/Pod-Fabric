// Binary and text codec tests, including malformed input.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <string>
#include <vector>

#include "podfabric/codec/codec.hpp"
#include "podfabric/model/synthetic.hpp"
#include "test.hpp"

using namespace podfabric;

namespace {

PodSnapshot sample() {
  SyntheticOptions options;
  const auto pod = PodId::parse("pod-codec");
  options.pod = pod.ok() ? pod.value() : PodId{};
  options.racks = 3;
  options.power_domains = 2;
  options.seed = 5;
  return synthesize(options);
}

// Reports the first line that differs between two renderings.
std::string first_difference(const std::string& left, const std::string& right) {
  std::size_t start = 0;
  std::size_t line = 1;
  while (start <= left.size() && start <= right.size()) {
    const std::size_t left_end = left.find('\n', start);
    const std::size_t right_end = right.find('\n', start);
    const std::string left_line = left.substr(start, left_end - start);
    const std::string right_line = right.substr(start, right_end - start);
    if (left_line != right_line) {
      return "line " + std::to_string(line) + "\n  left:  " + left_line +
             "\n  right: " + right_line;
    }
    if (left_end == std::string::npos || right_end == std::string::npos) {
      break;
    }
    start = left_end + 1;
    ++line;
  }
  return "no line differs; lengths " + std::to_string(left.size()) + " vs " +
         std::to_string(right.size());
}

// Field-by-field comparison used to localise a round-trip failure.
std::string field_difference(const PodSnapshot& a, const PodSnapshot& b) {
  auto flag = [](bool same, const char* name, std::size_t index) {
    return same ? std::string() : (std::string(name) + "[" + std::to_string(index) + "]");
  };
  if (a.schema != b.schema) return "schema";
  if (!(a.pod == b.pod)) return "pod";
  if (a.observed_at != b.observed_at) return "observed_at";
  if (!(a.provenance == b.provenance)) return "provenance";
  if (a.members.size() != b.members.size()) return "members.size";
  for (std::size_t i = 0; i < a.members.size(); ++i) {
    const std::string which = flag(a.members[i] == b.members[i], "member", i);
    if (!which.empty()) return which;
  }
  if (a.links.size() != b.links.size()) return "links.size";
  for (std::size_t i = 0; i < a.links.size(); ++i) {
    const std::string which = flag(a.links[i] == b.links[i], "link", i);
    if (!which.empty()) return which;
  }
  if (a.routes.size() != b.routes.size()) return "routes.size";
  for (std::size_t i = 0; i < a.routes.size(); ++i) {
    const std::string which = flag(a.routes[i] == b.routes[i], "route", i);
    if (!which.empty()) return which;
  }
  if (a.domains.size() != b.domains.size()) return "domains.size";
  for (std::size_t i = 0; i < a.domains.size(); ++i) {
    const std::string which = flag(a.domains[i] == b.domains[i], "domain", i);
    if (!which.empty()) return which;
  }
  if (a.obligations.size() != b.obligations.size()) return "obligations.size";
  for (std::size_t i = 0; i < a.obligations.size(); ++i) {
    const std::string which = flag(a.obligations[i] == b.obligations[i], "obligation", i);
    if (!which.empty()) return which;
  }
  return "equal";
}

std::vector<std::byte> encode_or_throw(const PodSnapshot& snapshot) {
  ByteWriter writer;
  const Status status = codec::encode(snapshot, writer);
  PF_REQUIRE(status.ok());
  return writer.take();
}

}  // namespace

PF_TEST(codec, binary_round_trip_is_lossless) {
  const PodSnapshot snapshot = sample();
  const auto bytes = encode_or_throw(snapshot);
  const auto decoded = codec::decode(std::span<const std::byte>(bytes));
  PF_REQUIRE(decoded.ok());
  PF_CHECK(decoded.value() == snapshot);
  PF_CHECK_EQ(codec::digest_of(decoded.value()), codec::digest_of(snapshot));
}

PF_TEST(codec, binary_rejects_version_truncation_and_trailing_bytes) {
  const PodSnapshot snapshot = sample();
  const auto bytes = encode_or_throw(snapshot);
  {
    auto copy = bytes;
    copy[0] = std::byte{0};
    copy[1] = std::byte{99};
    PF_CHECK(codec::decode(std::span<const std::byte>(copy)).code() == Code::Unsupported);
  }
  for (std::size_t cut = 1; cut < bytes.size(); cut += 7) {
    const auto truncated =
        codec::decode(std::span<const std::byte>(bytes.data(), bytes.size() - cut));
    PF_CHECK_MSG(!truncated.ok(), "truncation of " + std::to_string(cut) + " bytes was accepted");
  }
  {
    auto extended = bytes;
    extended.push_back(std::byte{0});
    PF_CHECK(codec::decode(std::span<const std::byte>(extended)).code() == Code::Invalid);
  }
  PF_CHECK(codec::decode(std::span<const std::byte>()).code() == Code::Incomplete);
}

PF_TEST(codec, binary_rejects_oversized_declared_counts) {
  const PodSnapshot snapshot = sample();
  auto bytes = encode_or_throw(snapshot);
  // The member count follows the version, pod id and schema string. Locate it
  // by re-encoding the prefix so the test does not depend on hard-coded offsets.
  ByteWriter prefix;
  PF_REQUIRE(prefix.put_u16(codec::binary_version).ok());
  PF_REQUIRE(codec::put_typed_id(prefix, snapshot.pod).ok());
  PF_REQUIRE(prefix.put_string(snapshot.schema).ok());
  const std::size_t offset = prefix.size();
  PF_REQUIRE(offset + 4 <= bytes.size());
  bytes[offset] = std::byte{0x7F};
  bytes[offset + 1] = std::byte{0xFF};
  bytes[offset + 2] = std::byte{0xFF};
  bytes[offset + 3] = std::byte{0xFF};
  const auto decoded = codec::decode(std::span<const std::byte>(bytes));
  PF_CHECK(decoded.code() == Code::Invalid);
}

PF_TEST(codec, context_round_trips) {
  CompositionContext context;
  context.epoch = PodEpoch(9);
  const auto incarnation = Incarnation::fresh();
  PF_REQUIRE(incarnation.ok());
  context.incarnation = incarnation.value();
  context.expectations = expectations_for(sample());
  context.recovered_from_store = true;
  context.ambiguous_commit = true;
  context.lifecycle = LifecycleState::Recovering;
  context.revoked_tokens.push_back(LeaseId::from_canonical_literal("lease-1"));
  ByteWriter writer;
  PF_REQUIRE(codec::encode(context, writer).ok());
  const auto decoded = codec::decode_context(writer.buffer());
  PF_REQUIRE(decoded.ok());
  PF_CHECK(decoded.value() == context);
}

PF_TEST(codec, text_round_trip_preserves_the_document) {
  PodSnapshot snapshot = sample();
  canonicalise(snapshot);
  const auto text = codec::to_text(snapshot);
  PF_REQUIRE(text.ok());
  const auto parsed = codec::snapshot_from_text(text.value());
  PF_REQUIRE(parsed.ok());
  const auto again = codec::to_text(parsed.value());
  PF_REQUIRE(again.ok());
  PF_CHECK_MSG(parsed.value() == snapshot,
               field_difference(snapshot, parsed.value()) + " / " +
                   first_difference(text.value(), again.value()));
  PF_CHECK_EQ(again.value(), text.value());
}

PF_TEST(codec, text_parser_rejects_malformed_documents) {
  const PodSnapshot snapshot = sample();
  const auto text = codec::to_text(snapshot);
  PF_REQUIRE(text.ok());
  const std::string good = text.value();

  PF_CHECK(codec::snapshot_from_text("").code() == Code::Invalid);
  PF_CHECK(codec::snapshot_from_text("podfabric.other/1\nend\n").code() == Code::Unsupported);
  PF_CHECK(codec::snapshot_from_text("podfabric.text/1\n").code() == Code::Incomplete);
  PF_CHECK(codec::snapshot_from_text("podfabric.text/1\nbogus 1\nend\n").code() == Code::Invalid);
  PF_CHECK(codec::snapshot_from_text("podfabric.text/1\npod p\n+domain x\nend\n").code() ==
           Code::Invalid);
  PF_CHECK(codec::snapshot_from_text("podfabric.text/1\npod \nend\n").code() == Code::Invalid);
  PF_CHECK(codec::snapshot_from_text(
               "podfabric.text/1\npod p\nobserved-at 99999999999999999999999\nend\n")
               .code() == Code::Invalid);
  PF_CHECK(codec::snapshot_from_text("podfabric.text/1\npod p\nobserved-at -abc\nend\n").code() ==
           Code::Invalid);
  PF_CHECK(codec::snapshot_from_text(
               "podfabric.text/1\npod p\nmember rack-1 gen 1 digest zz rnf r state established "
               "admin enabled health ok observed 0 prov REAL observed - 0\nend\n")
               .code() == Code::Invalid);
  // Invalid UTF-8 in a free-text continuation line.
  std::string hostile = good;
  const std::size_t end_at = hostile.rfind("end\n");
  PF_REQUIRE(end_at != std::string::npos);
  hostile.insert(end_at, "+detail \xFF\xFE\n");
  PF_CHECK(codec::snapshot_from_text(hostile).code() == Code::Invalid);
  // Nothing may follow the end marker.
  PF_CHECK(codec::snapshot_from_text(good + "+detail trailing\n").code() == Code::Invalid);
  // A member line with trailing junk is refused rather than ignored.
  PF_CHECK(codec::snapshot_from_text(
               "podfabric.text/1\npod p\nmember rack-1 gen 1 digest "
               "0000000000000000000000000000000000000000000000000000000000000000 rnf r state "
               "established admin enabled health ok observed 0 prov REAL observed - 0 junk\nend\n")
               .code() == Code::Invalid);
}

PF_TEST(codec, text_renderer_never_emits_raw_control_characters) {
  PodSnapshot snapshot = sample();
  snapshot.provenance.detail = std::string("line\nbreak\ttab") + std::string(1, '\x01');
  const auto text = codec::to_text(snapshot);
  PF_REQUIRE(text.ok());
  // The document still has exactly one header line and parses back.
  PF_CHECK(text.value().find("line break tab") != std::string::npos);
  const auto parsed = codec::snapshot_from_text(text.value());
  PF_REQUIRE(parsed.ok());
}

PF_TEST(codec, decision_fingerprints_track_content) {
  DecisionRecord record;
  record.id = make_decision_id(DecisionKind::MemberState, "rack-1");
  record.kind = DecisionKind::MemberState;
  record.status = Code::Ok;
  record.deps.push_back(DepKey{DepKind::Member, "rack-1"});
  const Digest first = codec::decision_fingerprint(record);
  PF_CHECK(!first.is_zero());
  PF_CHECK_EQ(codec::decision_fingerprint(record), first);
  record.status = Code::Stale;
  PF_CHECK(!(codec::decision_fingerprint(record) == first));
  record.status = Code::Ok;
  record.reasons.push_back(Reason{ReasonCode::EvidenceStale, "aged out"});
  PF_CHECK(!(codec::decision_fingerprint(record) == first));
}
