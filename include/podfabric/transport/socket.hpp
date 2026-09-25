// Minimal blocking TCP with explicit deadlines.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <span>
#include <string>

#include "podfabric/core/status.hpp"
#include "podfabric/core/time.hpp"

namespace podfabric::transport {

#if defined(_WIN32)
using native_socket = std::uintptr_t;
inline constexpr native_socket invalid_socket = static_cast<native_socket>(~0ull);
#else
using native_socket = int;
inline constexpr native_socket invalid_socket = -1;
#endif

enum class WaitResult : std::uint8_t { Ready = 0, TimedOut = 1, Closed = 2 };

// A blocking TCP endpoint. Every operation is bounded by a caller-supplied
// deadline rather than by a global timeout, so no code path can block forever
// on a peer that stops responding.
class Socket {
 public:
  Socket() = default;
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  Socket(Socket&& other) noexcept;
  Socket& operator=(Socket&& other) noexcept;
  ~Socket();

  static Status initialise_runtime();
  static Result<Socket> connect(const std::string& host, std::uint16_t port, Nanos deadline);
  static Result<Socket> listen(const std::string& host, std::uint16_t port, int backlog);
  // Waits up to timeout for an inbound connection. CLOSED means the listener is
  // no longer able to accept, which is how shutdown is observed.
  Result<Socket> accept(Nanos timeout);
  Result<WaitResult> wait_readable(Nanos timeout) const;

  Status send_all(std::span<const std::byte> data, Nanos deadline);
  // Reads at most data.size() bytes. A zero result means the peer closed.
  Result<std::size_t> recv_some(std::span<std::byte> data, Nanos deadline);
  Status send_all_bounded(std::span<const std::byte> data);
  Result<std::size_t> recv_some_bounded(std::span<std::byte> data);

  Status set_recv_timeout(Nanos timeout);
  Status set_send_timeout(Nanos timeout);
  Status shutdown_both();
  Status close();
  bool valid() const noexcept { return handle_ != invalid_socket; }

  std::uint16_t bound_port() const;
  std::string peer_description() const;

 private:
  explicit Socket(native_socket handle) noexcept : handle_(handle) {}
  native_socket handle_{invalid_socket};
  std::string peer_{};
};

// Formats and parses the "host:port" endpoint notation used by the tooling.
std::string format_endpoint(const std::string& host, std::uint16_t port);
Result<std::pair<std::string, std::uint16_t>> parse_endpoint(const std::string& text);

}  // namespace podfabric::transport
