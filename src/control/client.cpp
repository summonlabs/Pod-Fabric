// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "podfabric/control/client.hpp"

#include "podfabric/control/server.hpp"

namespace podfabric::control {

Result<PodClient> PodClient::connect(const std::string& endpoint, ClientOptions options) {
  PODFABRIC_TRY(transport::Socket::initialise_runtime());
  PODFABRIC_TRY_ASSIGN(const auto parsed, transport::parse_endpoint(endpoint));
  if (parsed.second == 0) {
    return Status(Code::Invalid, "cannot connect to port zero");
  }
  PODFABRIC_TRY_ASSIGN(
      transport::Socket socket,
      transport::Socket::connect(parsed.first, parsed.second, options.connect_deadline));
  PodClient client;
  client.socket_ = std::move(socket);
  client.options_ = options;
  return client;
}

Result<Response> PodClient::call(const Request& request) {
  if (!socket_.valid()) {
    return Status(Code::Invalid, "the client is not connected");
  }
  std::vector<std::byte> payload;
  PODFABRIC_TRY(encode_request(request, payload));
  transport::Frame outgoing;
  outgoing.type = transport::FrameType::Request;
  outgoing.correlation = next_correlation();
  outgoing.payload = std::move(payload);
  PODFABRIC_TRY(write_frame(socket_, outgoing));
  PODFABRIC_TRY_ASSIGN(
      const transport::Frame incoming,
      read_frame(socket_, options_.io_deadline, options_.max_payload));
  if (incoming.type == transport::FrameType::Error) {
    const std::string text(reinterpret_cast<const char*>(incoming.payload.data()),
                           incoming.payload.size());
    return Status(Code::Corrupt, "the server rejected the frame: " + text);
  }
  if (incoming.type != transport::FrameType::Response) {
    return Status(Code::Invalid, "the server sent an unexpected frame type");
  }
  return decode_response(incoming.payload);
}

Status PodClient::close() { return socket_.close(); }

}  // namespace podfabric::control
