// Core primitive tests: identities, digests, checked arithmetic, codecs.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <string>

#include "podfabric/core/bytes.hpp"
#include "podfabric/core/checked.hpp"
#include "podfabric/core/digest.hpp"
#include "podfabric/core/status.hpp"
#include "podfabric/core/token.hpp"
#include "podfabric/model/primitives.hpp"
#include "test.hpp"

using namespace podfabric;

namespace {
struct ProbeTag;
}  // namespace

PF_TEST(core, sha256_matches_fips_vectors) {
  PF_CHECK_EQ(Digest::of("").to_hex(),
              std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  PF_CHECK_EQ(Digest::of("abc").to_hex(),
              std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  PF_CHECK_EQ(Digest::of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq").to_hex(),
              std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
}

PF_TEST(core, sha256_handles_block_boundaries) {
  // 55, 56, 63 and 64 byte inputs exercise every padding path.
  for (std::size_t size : {std::size_t(55), std::size_t(56), std::size_t(63), std::size_t(64),
                           std::size_t(65), std::size_t(119), std::size_t(120)}) {
    const std::string input(size, 'a');
    Sha256 hasher;
    hasher.update(input);
    const auto one_shot = hasher.finish();
    // Streaming in two halves must agree with a single update.
    Sha256 split;
    split.update(std::string_view(input).substr(0, size / 2));
    split.update(std::string_view(input).substr(size / 2));
    const auto streamed = split.finish();
    PF_CHECK_EQ(Digest::from_bytes(one_shot), Digest::from_bytes(streamed));
  }
  const std::string million(1000000, 'a');
  PF_CHECK_EQ(Digest::of(million).to_hex(),
              std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

PF_TEST(core, crc32c_matches_the_reference_value) {
  PF_CHECK_EQ(crc32c(std::string_view("123456789")), 0xE3069283u);
  PF_CHECK_EQ(crc32c(std::string_view("")), 0u);
}

PF_TEST(core, canonical_identifiers_are_strict) {
  PF_CHECK(is_canonical_id("rack-1"));
  PF_CHECK(is_canonical_id("a"));
  PF_CHECK(is_canonical_id("fabric.bandwidth.v2"));
  PF_CHECK(is_canonical_id("plane:shared_0"));
  PF_CHECK(!is_canonical_id(""));
  PF_CHECK(!is_canonical_id("Rack-1"));
  PF_CHECK(!is_canonical_id("-rack"));
  PF_CHECK(!is_canonical_id("rack-"));
  PF_CHECK(!is_canonical_id(std::string(65, 'a')));
  PF_CHECK(!is_canonical_id("rack 1"));
  PF_CHECK(RackId::parse("Rack-1").code() == Code::Invalid);
  const auto rack = RackId::parse("rack-1");
  PF_REQUIRE(rack.ok());
  PF_CHECK_EQ(rack.value().token(), std::string("rack-1"));
  PF_CHECK(RackId{}.is_nil());
  PF_CHECK_EQ(to_string(RackId{}), std::string("-"));
}

PF_TEST(core, uuid_round_trips_and_rejects_malformed_text) {
  const auto generated = Uuid::random();
  PF_REQUIRE(generated.ok());
  const std::string text = generated.value().to_string();
  PF_CHECK_EQ(text.size(), Uuid::text_length);
  const auto parsed = Uuid::parse(text);
  PF_REQUIRE(parsed.ok());
  PF_CHECK_EQ(parsed.value(), generated.value());
  PF_CHECK(Uuid::parse("").code() == Code::Invalid);
  PF_CHECK(Uuid::parse("00000000-0000-0000-0000-00000000000").code() == Code::Invalid);
  PF_CHECK(Uuid::parse("zzzzzzzz-0000-0000-0000-000000000000").code() == Code::Invalid);
  PF_CHECK(Uuid::parse("000000000000000000000000000000000000").code() == Code::Invalid);
  PF_CHECK(Uuid{}.is_nil());
  PF_CHECK(!generated.value().is_nil());
}

PF_TEST(core, checked_arithmetic_reports_overflow_instead_of_wrapping) {
  PF_CHECK_EQ(checked_add<std::uint64_t>(1, 2).value(), 3ull);
  PF_CHECK(checked_add<std::uint64_t>(0xFFFFFFFFFFFFFFFFull, 1).code() == Code::Invalid);
  PF_CHECK(checked_sub<std::uint64_t>(0, 1).code() == Code::Invalid);
  PF_CHECK_EQ(checked_sub<std::uint64_t>(5, 5).value(), 0ull);
  PF_CHECK(checked_mul<std::uint64_t>(0xFFFFFFFFFFFFFFFFull, 2).code() == Code::Invalid);
  PF_CHECK_EQ(checked_mul<std::uint64_t>(0, 0xFFFFFFFFFFFFFFFFull).value(), 0ull);
  PF_CHECK(checked_narrow<std::uint8_t>(std::uint32_t(256)).code() == Code::Invalid);
  PF_CHECK_EQ(checked_narrow<std::uint8_t>(std::uint32_t(255)).value(), 255u);
}

PF_TEST(core, utf8_validation_rejects_hostile_sequences) {
  PF_CHECK(is_valid_utf8("plain ascii"));
  PF_CHECK(is_valid_utf8("caf\xC3\xA9"));
  PF_CHECK(is_valid_utf8("\xE2\x82\xAC"));
  PF_CHECK(!is_valid_utf8("\xC3"));
  PF_CHECK(!is_valid_utf8("\xC0\xAF"));            // overlong solidus
  PF_CHECK(!is_valid_utf8("\xED\xA0\x80"));        // surrogate half
  PF_CHECK(!is_valid_utf8("\xF5\x80\x80\x80"));    // beyond U+10FFFF
  PF_CHECK(!is_valid_utf8("\x80"));                // stray continuation
  PF_CHECK(!is_valid_utf8(std::string("ab\0cd", 5).substr(0, 2) + std::string("\xFF")));
}

PF_TEST(core, byte_writer_enforces_its_limit) {
  ByteWriter writer(8);
  PF_CHECK(writer.put_u32(1).ok());
  PF_CHECK(writer.put_u32(2).ok());
  PF_CHECK(writer.put_u8(3).code() == Code::Exhausted);
  PF_CHECK(writer.overflowed());
  PF_CHECK_EQ(writer.size(), std::size_t(8));
}

PF_TEST(core, byte_reader_rejects_truncation_and_oversized_lengths) {
  const std::vector<std::byte> empty;
  ByteReader short_reader(empty);
  PF_CHECK(short_reader.u32().code() == Code::Incomplete);
  PF_CHECK(short_reader.at_end());

  ByteWriter writer;
  PF_CHECK(writer.put_u32(0xFFFFFFFFu).ok());
  const auto bytes = writer.take();
  ByteReader reader(bytes, CodecLimits{});
  PF_CHECK(reader.string().code() == Code::Invalid);
  ByteReader counting(bytes, CodecLimits{});
  PF_CHECK(counting.count(16, 1).code() == Code::Invalid);

  ByteWriter counted;
  PF_CHECK(counted.put_u32(1000).ok());
  const auto counted_bytes = counted.take();
  ByteReader counted_reader(counted_bytes);
  PF_CHECK(counted_reader.count(2000, 8).code() == Code::Invalid);
}

PF_TEST(core, severity_ordering_is_total_and_distinguishes_outcomes) {
  PF_CHECK(severity(Code::Ok) < severity(Code::Unknown));
  PF_CHECK(severity(Code::Incomplete) < severity(Code::Stale));
  PF_CHECK(severity(Code::Stale) < severity(Code::Indeterminate));
  PF_CHECK(severity(Code::Indeterminate) < severity(Code::Conflicting));
  PF_CHECK(severity(Code::Conflicting) < severity(Code::Refused));
  PF_CHECK(worst(Code::Ok, Code::Conflicting) == Code::Conflicting);
  PF_CHECK(worst(Code::Refused, Code::Ok) == Code::Refused);
  PF_CHECK_EQ(std::string(to_string(Code::Conflicting)), std::string("CONFLICTING"));
  PF_CHECK_EQ(std::string(to_string(Code::Indeterminate)), std::string("INDETERMINATE"));
  PF_CHECK_EQ(std::string(to_string(Code::Unsupported)), std::string("UNSUPPORTED"));
}

PF_TEST(core, sequences_never_wrap) {
  const Sequence<ProbeTag> low(1);
  PF_CHECK_EQ(low.next().value().value(), 2ull);
  const Sequence<ProbeTag> max(std::numeric_limits<std::uint64_t>::max());
  PF_CHECK(max.next().code() == Code::Invalid);
}
