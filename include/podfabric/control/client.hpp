// Blocking client for the pod wire protocol.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>

#include "podfabric/control/protocol.hpp"
#include "podfabric/transport/frame.hpp"
#include "podfabric/transport/socket.hpp"

namespace podfabric::control {

struct ClientOptions {
  Nanos connect_deadline{2 * nanos_per_second};
  Nanos io_deadline{10 * nanos_per_second};
  std::size_t max_payload{transport::frame_max_payload};
};

class PodClient {
 public:
  PodClient() = default;
  PodClient(const PodClient&) = delete;
  PodClient& operator=(const PodClient&) = delete;
  PodClient(PodClient&&) noexcept = default;
  PodClient& operator=(PodClient&&) noexcept = default;
  ~PodClient() = default;

  static Result<PodClient> connect(const std::string& endpoint, ClientOptions options = {});

  Result<Response> call(const Request& request);
  Status close();
  bool connected() const noexcept { return socket_.valid(); }
  std::uint64_t next_correlation() noexcept { return ++correlation_; }

 private:
  transport::Socket socket_{};
  ClientOptions options_{};
  std::uint64_t correlation_{0};
};

}  // namespace podfabric::control
