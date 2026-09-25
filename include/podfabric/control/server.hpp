// Framed request/response server in front of a PodController.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "podfabric/control/controller.hpp"
#include "podfabric/control/protocol.hpp"
#include "podfabric/transport/frame.hpp"
#include "podfabric/transport/socket.hpp"

namespace podfabric::control {

struct ServerOptions {
  std::string host{"127.0.0.1"};
  std::uint16_t port{0};  // zero asks the operating system for a free port
  std::size_t workers{4};
  std::size_t max_pending_connections{64};
  Nanos accept_poll_interval{50 * nanos_per_millisecond};
  Nanos io_deadline{5 * nanos_per_second};
  std::size_t max_text_bytes{512 * 1024};
};

// A bounded connection pool. Shutdown is cooperative: the listener is closed
// only after every worker has been joined, so no thread can observe a closed
// handle it is still using.
class PodServer {
 public:
  PodServer(PodController& controller, ServerOptions options);
  PodServer(const PodServer&) = delete;
  PodServer& operator=(const PodServer&) = delete;
  ~PodServer();

  Status start();
  Status stop();
  bool running() const noexcept { return running_.load(); }
  bool stopping() const noexcept { return stopping_.load(); }
  std::uint16_t port() const noexcept { return port_; }
  std::string endpoint() const;
  std::uint64_t connections_served() const noexcept { return connections_.load(); }
  std::uint64_t requests_served() const noexcept { return requests_.load(); }
  std::uint64_t protocol_errors() const noexcept { return protocol_errors_.load(); }

 private:
  void accept_loop();
  void worker_loop();
  void serve(transport::Socket connection);
  Response handle(const Request& request);

  PodController& controller_;
  ServerOptions options_;
  transport::Socket listener_{};
  std::thread acceptor_{};
  std::vector<std::thread> workers_{};
  mutable std::mutex queue_mutex_{};
  std::condition_variable queue_cv_{};
  std::deque<transport::Socket> queue_{};
  std::atomic<bool> running_{false};
  std::atomic<bool> stopping_{false};
  std::uint16_t port_{0};
  std::atomic<std::uint64_t> connections_{0};
  std::atomic<std::uint64_t> requests_{0};
  std::atomic<std::uint64_t> protocol_errors_{0};
};

// Reads exactly one frame from a connection, enforcing the framing limits before
// any payload is allocated.
Result<transport::Frame> read_frame(transport::Socket& socket, Nanos deadline,
                                    std::size_t max_payload);

// Sends one frame on a connection.
Status write_frame(transport::Socket& socket, const transport::Frame& frame);

}  // namespace podfabric::control
