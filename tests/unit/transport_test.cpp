// Framing and loopback transport tests.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <string>
#include <thread>
#include <vector>

#include "podfabric/control/client.hpp"
#include "podfabric/control/controller.hpp"
#include "podfabric/control/server.hpp"
#include "podfabric/model/synthetic.hpp"
#include "podfabric/transport/frame.hpp"
#include "podfabric/transport/socket.hpp"
#include "test.hpp"

using namespace podfabric;

namespace {

transport::Frame make_frame(transport::FrameType type, std::size_t payload) {
  transport::Frame frame;
  frame.type = type;
  frame.correlation = 42;
  frame.payload.assign(payload, std::byte{9});
  return frame;
}

struct Harness {
  PodController controller;
  control::PodServer server;

  explicit Harness(control::ServerOptions options)
      : controller([] {
          ControllerOptions controller_options;
          controller_options.pod = PodId::parse("pod-transport").value();
          return controller_options;
        }()),
        server(controller, options) {}
};

}  // namespace

PF_TEST(transport, frames_round_trip_through_the_codec) {
  for (std::size_t size : {std::size_t(0), std::size_t(1), std::size_t(255),
                           std::size_t(65536)}) {
    const transport::Frame frame = make_frame(transport::FrameType::Request, size);
    std::vector<std::byte> encoded;
    PF_REQUIRE(transport::encode_frame(frame, encoded).ok());
    PF_CHECK_EQ(encoded.size(), transport::frame_header_size + size);
    const auto header =
        transport::parse_frame_header(std::span<const std::byte>(encoded.data(),
                                                                 transport::frame_header_size));
    PF_REQUIRE(header.ok());
    PF_CHECK_EQ(header.value().length, static_cast<std::uint32_t>(size));
    PF_CHECK_EQ(header.value().correlation, 42ull);
    PF_CHECK(transport::verify_frame(encoded, header.value()));
  }
}

PF_TEST(transport, framing_rejects_hostile_input) {
  const transport::Frame frame = make_frame(transport::FrameType::Request, 32);
  std::vector<std::byte> encoded;
  PF_REQUIRE(transport::encode_frame(frame, encoded).ok());

  {
    auto copy = encoded;
    copy[0] = std::byte{0};
    PF_CHECK(transport::parse_frame_header(
                  std::span<const std::byte>(copy.data(), transport::frame_header_size))
                  .code() == Code::Invalid);
  }
  {
    auto copy = encoded;
    copy[5] = std::byte{9};
    PF_CHECK(transport::parse_frame_header(
                  std::span<const std::byte>(copy.data(), transport::frame_header_size))
                  .code() == Code::Unsupported);
  }
  {
    auto copy = encoded;
    copy[7] = std::byte{0x7F};
    PF_CHECK(transport::parse_frame_header(
                  std::span<const std::byte>(copy.data(), transport::frame_header_size))
                  .code() == Code::Invalid);
  }
  {
    auto copy = encoded;
    // A declared length beyond the hard limit is refused before allocation.
    copy[12] = std::byte{0x7F};
    copy[13] = std::byte{0xFF};
    copy[14] = std::byte{0xFF};
    copy[15] = std::byte{0xFF};
    PF_CHECK(transport::parse_frame_header(
                  std::span<const std::byte>(copy.data(), transport::frame_header_size))
                  .code() == Code::Exhausted);
  }
  {
    // A corrupted checksum is caught by whole-frame verification, and a
    // corrupted payload is caught by the same check.
    auto copy = encoded;
    copy[24] = static_cast<std::byte>(static_cast<unsigned char>(copy[24]) ^ 0x01u);
    const auto header = transport::parse_frame_header(copy);
    PF_REQUIRE(header.ok());
    PF_CHECK(!transport::verify_frame(copy, header.value()));
    auto body = encoded;
    body[transport::frame_header_size + 3] =
        static_cast<std::byte>(static_cast<unsigned char>(body[transport::frame_header_size + 3]) ^
                               0x01u);
    const auto body_header = transport::parse_frame_header(body);
    PF_REQUIRE(body_header.ok());
    PF_CHECK(!transport::verify_frame(body, body_header.value()));
  }
  PF_CHECK(transport::parse_frame_header(
                std::span<const std::byte>(encoded.data(), 8)).code() == Code::Incomplete);
}

PF_TEST(transport, endpoints_parse_and_reject_bad_ports) {
  const auto parsed = transport::parse_endpoint("127.0.0.1:8080");
  PF_REQUIRE(parsed.ok());
  PF_CHECK_EQ(parsed.value().first, std::string("127.0.0.1"));
  PF_CHECK_EQ(parsed.value().second, static_cast<std::uint16_t>(8080));
  PF_CHECK(transport::parse_endpoint("127.0.0.1").code() == Code::Invalid);
  // Port zero asks the operating system for a free port, which is what a
  // listener wants; a client refuses to connect to it.
  const auto ephemeral = transport::parse_endpoint("127.0.0.1:0");
  PF_REQUIRE(ephemeral.ok());
  PF_CHECK_EQ(ephemeral.value().second, static_cast<std::uint16_t>(0));
  PF_CHECK(podfabric::control::PodClient::connect("127.0.0.1:0").code() == Code::Invalid);
  PF_CHECK(transport::parse_endpoint("127.0.0.1:70000").code() == Code::Invalid);
  PF_CHECK(transport::parse_endpoint(":80").code() == Code::Invalid);
  PF_CHECK(transport::parse_endpoint("host:abc").code() == Code::Invalid);
}

PF_TEST(transport, a_request_round_trips_over_loopback_tcp) {
  control::ServerOptions options;
  options.workers = 2;
  Harness harness(options);
  PF_REQUIRE(harness.controller.open().ok());
  PF_REQUIRE(harness.server.start().ok());
  PF_CHECK(harness.server.port() != 0);

  auto client = control::PodClient::connect(harness.server.endpoint());
  PF_REQUIRE(client.ok());
  control::Request request;
  request.kind = control::MessageKind::Status;
  request.nonce = 1234;
  const auto response = client.value().call(request);
  PF_REQUIRE(response.ok());
  PF_CHECK_EQ(response.value().code, Code::Ok);
  PF_CHECK_EQ(response.value().nonce, 1234ull);
  PF_CHECK_EQ(response.value().lifecycle, LifecycleState::Constructing);
  PF_CHECK(response.value().text.find("podfabric.podstate/1") != std::string::npos);
  PF_CHECK(client.value().close().ok());
  PF_CHECK(harness.server.stop().ok());
  PF_CHECK(harness.controller.close().ok());
}

PF_TEST(transport, a_hostile_frame_is_rejected_without_stopping_the_server) {
  control::ServerOptions options;
  options.workers = 2;
  Harness harness(options);
  PF_REQUIRE(harness.controller.open().ok());
  PF_REQUIRE(harness.server.start().ok());

  {
    auto socket = transport::Socket::connect("127.0.0.1", harness.server.port(),
                                             2 * nanos_per_second);
    PF_REQUIRE(socket.ok());
    std::vector<std::byte> garbage(64, std::byte{0xAB});
    PF_CHECK(socket.value().send_all_bounded(garbage).ok());
    std::vector<std::byte> reply(128);
    const auto got = socket.value().recv_some_bounded(reply);
    // The server answers with an error frame or closes; either way it must not
    // crash and must keep serving.
    PF_CHECK(got.ok());
    (void)socket.value().close();
  }
  auto client = control::PodClient::connect(harness.server.endpoint());
  PF_REQUIRE(client.ok());
  control::Request request;
  request.kind = control::MessageKind::Ping;
  const auto response = client.value().call(request);
  PF_REQUIRE(response.ok());
  PF_CHECK_EQ(response.value().code, Code::Ok);
  PF_CHECK(harness.server.protocol_errors() >= 1);
  PF_CHECK(client.value().close().ok());
  PF_CHECK(harness.server.stop().ok());
  PF_CHECK(harness.controller.close().ok());
}

PF_TEST(transport, connecting_to_a_closed_port_fails_cleanly) {
  const auto socket = transport::Socket::connect("127.0.0.1", 1, 500 * nanos_per_millisecond);
  PF_CHECK(!socket.ok());
  PF_CHECK(socket.code() == Code::Unavailable);
}

PF_TEST(transport, an_apply_over_the_wire_moves_the_pod_epoch) {
  control::ServerOptions options;
  options.workers = 2;
  Harness harness(options);
  PF_REQUIRE(harness.controller.open().ok());
  PF_REQUIRE(harness.server.start().ok());

  SyntheticOptions synthetic;
  synthetic.pod = PodId::parse("pod-transport").value();
  synthetic.racks = 3;
  synthetic.power_domains = 2;
  synthetic.observed_at = 1700000000LL * nanos_per_second;
  const PodSnapshot snapshot = synthesize(synthetic);

  auto client = control::PodClient::connect(harness.server.endpoint());
  PF_REQUIRE(client.ok());
  control::Request request;
  request.kind = control::MessageKind::Apply;
  request.snapshot = snapshot;
  const auto response = client.value().call(request);
  PF_REQUIRE_MSG(response.ok(), response.status().to_string());
  PF_CHECK_EQ(response.value().code, Code::Ok);
  PF_CHECK_EQ(response.value().outcome, ApplyOutcome::EpochAdvanced);
  PF_CHECK(response.value().revalidation_sound);
  PF_CHECK_EQ(response.value().established_members, 3u);

  // The round after the epoch advance settles the pod, and the round after
  // that is idempotent.
  const auto settled = client.value().call(request);
  PF_REQUIRE(settled.ok());
  PF_CHECK_EQ(settled.value().outcome, ApplyOutcome::Applied);
  PF_CHECK_EQ(settled.value().lifecycle, LifecycleState::Active);
  const auto again = client.value().call(request);
  PF_REQUIRE(again.ok());
  PF_CHECK_EQ(again.value().outcome, ApplyOutcome::Unchanged);
  PF_CHECK_EQ(again.value().state_digest, settled.value().state_digest);
  PF_CHECK(client.value().close().ok());
  PF_CHECK(harness.server.stop().ok());
  PF_CHECK(harness.controller.close().ok());
}

PF_TEST(transport, a_shutdown_request_stops_the_server) {
  control::ServerOptions options;
  options.workers = 2;
  Harness harness(options);
  PF_REQUIRE(harness.controller.open().ok());
  PF_REQUIRE(harness.server.start().ok());
  auto client = control::PodClient::connect(harness.server.endpoint());
  PF_REQUIRE(client.ok());
  control::Request request;
  request.kind = control::MessageKind::Shutdown;
  const auto response = client.value().call(request);
  PF_REQUIRE(response.ok());
  PF_CHECK_EQ(response.value().code, Code::Ok);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!harness.server.stopping() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  PF_CHECK(harness.server.stopping());
  PF_CHECK(harness.server.stop().ok());
  PF_CHECK(harness.controller.close().ok());
}
