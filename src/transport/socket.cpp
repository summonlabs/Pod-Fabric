// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "podfabric/transport/socket.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace podfabric::transport {
namespace {

#if defined(_WIN32)
struct WinsockRuntime {
  WinsockRuntime() {
    WSADATA data{};
    started = ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }
  ~WinsockRuntime() {
    if (started) {
      ::WSACleanup();
    }
  }
  bool started{false};
};

Status ensure_runtime() {
  static WinsockRuntime runtime;
  if (!runtime.started) {
    return Status(Code::Unsupported, "the Winsock runtime could not be initialised");
  }
  return Status::success();
}

void close_native(native_socket handle) { ::closesocket(static_cast<SOCKET>(handle)); }
int last_error() { return ::WSAGetLastError(); }
bool would_block(int code) { return code == WSAEWOULDBLOCK || code == WSAETIMEDOUT; }
#else
Status ensure_runtime() { return Status::success(); }
void close_native(native_socket handle) { ::close(handle); }
int last_error() { return errno; }
bool would_block(int code) { return code == EAGAIN || code == EWOULDBLOCK || code == EINTR; }
#endif

Result<std::pair<std::string, std::uint16_t>> resolve_target(const std::string& host,
                                                             std::uint16_t port) {
  (void)host;
  (void)port;
  return Status(Code::Internal, "unused");
}

std::string describe_socket_error(int code) {
  return "socket error " + std::to_string(code);
}

}  // namespace

Status Socket::initialise_runtime() { return ensure_runtime(); }

Socket::Socket(Socket&& other) noexcept : handle_(other.handle_), peer_(std::move(other.peer_)) {
  other.handle_ = invalid_socket;
  other.peer_.clear();
}

Socket& Socket::operator=(Socket&& other) noexcept {
  if (this != &other) {
    (void)close();
    handle_ = other.handle_;
    peer_ = std::move(other.peer_);
    other.handle_ = invalid_socket;
    other.peer_.clear();
  }
  return *this;
}

Socket::~Socket() { (void)close(); }

Status Socket::close() {
  if (handle_ == invalid_socket) {
    return Status::success();
  }
  const native_socket handle = handle_;
  handle_ = invalid_socket;
  close_native(handle);
  return Status::success();
}

Status Socket::shutdown_both() {
  if (handle_ == invalid_socket) {
    return Status::success();
  }
#if defined(_WIN32)
  ::shutdown(static_cast<SOCKET>(handle_), SD_BOTH);
#else
  ::shutdown(handle_, SHUT_RDWR);
#endif
  return Status::success();
}

Result<Socket> Socket::connect(const std::string& host, std::uint16_t port, Nanos deadline) {
  PODFABRIC_TRY(ensure_runtime());
  if (host.empty()) {
    return Status(Code::Invalid, "connect requires a host");
  }
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  addrinfo* results = nullptr;
  const std::string service = std::to_string(port);
  const int rc = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &results);
  if (rc != 0 || results == nullptr) {
    return Status(Code::Unavailable, "cannot resolve endpoint '" + host + "'");
  }
  Status failure(Code::Unavailable, "no usable address for endpoint '" + host + "'");
  for (addrinfo* candidate = results; candidate != nullptr; candidate = candidate->ai_next) {
    const native_socket handle =
        static_cast<native_socket>(::socket(candidate->ai_family, candidate->ai_socktype,
                                            candidate->ai_protocol));
    if (handle == invalid_socket) {
      failure = Status(Code::Unavailable, describe_socket_error(last_error()));
      continue;
    }
    Socket socket(handle);
    socket.peer_ = host + ":" + service;
    if (deadline > 0) {
      (void)socket.set_send_timeout(deadline);
      (void)socket.set_recv_timeout(deadline);
    }
    if (::connect(handle, candidate->ai_addr,
                  static_cast<int>(candidate->ai_addrlen)) == 0) {
      ::freeaddrinfo(results);
      return socket;
    }
    failure = Status(Code::Unavailable, describe_socket_error(last_error()));
    (void)socket.close();
  }
  ::freeaddrinfo(results);
  return failure;
}

Result<Socket> Socket::listen(const std::string& host, std::uint16_t port, int backlog) {
  PODFABRIC_TRY(ensure_runtime());
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  hints.ai_flags = AI_PASSIVE;
  addrinfo* results = nullptr;
  const std::string service = std::to_string(port);
  const int rc = ::getaddrinfo(host.empty() ? nullptr : host.c_str(), service.c_str(), &hints,
                               &results);
  if (rc != 0 || results == nullptr) {
    return Status(Code::Unavailable, "cannot resolve listen address '" + host + "'");
  }
  Status failure(Code::Unavailable, "no usable listen address");
  for (addrinfo* candidate = results; candidate != nullptr; candidate = candidate->ai_next) {
    const native_socket handle =
        static_cast<native_socket>(::socket(candidate->ai_family, candidate->ai_socktype,
                                            candidate->ai_protocol));
    if (handle == invalid_socket) {
      failure = Status(Code::Unavailable, describe_socket_error(last_error()));
      continue;
    }
    Socket socket(handle);
    const int one = 1;
    (void)::setsockopt(handle, SOL_SOCKET, SO_REUSEADDR,
                       reinterpret_cast<const char*>(&one), sizeof(one));
    if (::bind(handle, candidate->ai_addr, static_cast<int>(candidate->ai_addrlen)) != 0) {
      failure = Status(Code::Unavailable,
                       "cannot bind listen address: " + describe_socket_error(last_error()));
      (void)socket.close();
      continue;
    }
    if (::listen(handle, backlog) != 0) {
      failure = Status(Code::Unavailable, describe_socket_error(last_error()));
      (void)socket.close();
      continue;
    }
    ::freeaddrinfo(results);
    socket.peer_ = "listener";
    return socket;
  }
  ::freeaddrinfo(results);
  return failure;
}

Result<WaitResult> Socket::wait_readable(Nanos timeout) const {
  if (handle_ == invalid_socket) {
    return WaitResult::Closed;
  }
  const Nanos bounded = std::clamp<Nanos>(timeout, 0, 5 * nanos_per_second);
  fd_set read_set;
  FD_ZERO(&read_set);
#if defined(_WIN32)
  FD_SET(static_cast<SOCKET>(handle_), &read_set);
  timeval tv{};
  tv.tv_sec = static_cast<long>(bounded / nanos_per_second);
  tv.tv_usec = static_cast<long>((bounded % nanos_per_second) / 1000);
  const int rc = ::select(0, &read_set, nullptr, nullptr, &tv);
#else
  FD_SET(handle_, &read_set);
  timeval tv{};
  tv.tv_sec = static_cast<time_t>(bounded / nanos_per_second);
  tv.tv_usec = static_cast<suseconds_t>((bounded % nanos_per_second) / 1000);
  const int rc = ::select(handle_ + 1, &read_set, nullptr, nullptr, &tv);
#endif
  if (rc == 0) {
    return WaitResult::TimedOut;
  }
  if (rc < 0) {
    return would_block(last_error()) ? WaitResult::TimedOut : WaitResult::Closed;
  }
  return WaitResult::Ready;
}

Result<Socket> Socket::accept(Nanos timeout) {
  if (handle_ == invalid_socket) {
    return Status(Code::Cancelled, "the listener is closed");
  }
  PODFABRIC_TRY_ASSIGN(const WaitResult ready, wait_readable(timeout));
  if (ready == WaitResult::TimedOut) {
    return Status(Code::Cancelled, "no connection arrived within the polling interval");
  }
  if (ready == WaitResult::Closed) {
    return Status(Code::Cancelled, "the listener is no longer readable");
  }
  sockaddr_storage address{};
  socklen_t length = sizeof(address);
  const native_socket handle =
      static_cast<native_socket>(::accept(handle_, reinterpret_cast<sockaddr*>(&address), &length));
  if (handle == invalid_socket) {
    return Status(Code::Cancelled, "accept did not complete");
  }
  Socket socket(handle);
  socket.peer_ = "peer";
  return socket;
}

Status Socket::set_recv_timeout(Nanos timeout) {
  if (handle_ == invalid_socket) {
    return Status(Code::Invalid, "socket is not open");
  }
#if defined(_WIN32)
  const DWORD millis = static_cast<DWORD>(std::max<Nanos>(1, timeout / nanos_per_millisecond));
  if (::setsockopt(static_cast<SOCKET>(handle_), SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&millis), sizeof(millis)) != 0) {
    return Status(Code::Unavailable, describe_socket_error(last_error()));
  }
#else
  timeval tv{};
  tv.tv_sec = static_cast<time_t>(timeout / nanos_per_second);
  tv.tv_usec = static_cast<suseconds_t>((timeout % nanos_per_second) / 1000);
  if (::setsockopt(handle_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
    return Status(Code::Unavailable, describe_socket_error(last_error()));
  }
#endif
  return Status::success();
}

Status Socket::set_send_timeout(Nanos timeout) {
  if (handle_ == invalid_socket) {
    return Status(Code::Invalid, "socket is not open");
  }
#if defined(_WIN32)
  const DWORD millis = static_cast<DWORD>(std::max<Nanos>(1, timeout / nanos_per_millisecond));
  if (::setsockopt(static_cast<SOCKET>(handle_), SOL_SOCKET, SO_SNDTIMEO,
                   reinterpret_cast<const char*>(&millis), sizeof(millis)) != 0) {
    return Status(Code::Unavailable, describe_socket_error(last_error()));
  }
#else
  timeval tv{};
  tv.tv_sec = static_cast<time_t>(timeout / nanos_per_second);
  tv.tv_usec = static_cast<suseconds_t>((timeout % nanos_per_second) / 1000);
  if (::setsockopt(handle_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) {
    return Status(Code::Unavailable, describe_socket_error(last_error()));
  }
#endif
  return Status::success();
}

Status Socket::send_all(std::span<const std::byte> data, Nanos deadline) {
  if (handle_ == invalid_socket) {
    return Status(Code::Invalid, "socket is not open");
  }
  PODFABRIC_TRY(set_send_timeout(deadline));
  std::size_t sent = 0;
  while (sent < data.size()) {
#if defined(_WIN32)
    const int written = ::send(static_cast<SOCKET>(handle_),
                               reinterpret_cast<const char*>(data.data() + sent),
                               static_cast<int>(data.size() - sent), 0);
#else
    const ssize_t written = ::send(handle_, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
#endif
    if (written <= 0) {
      return Status(Code::Unavailable, "send failed: " + describe_socket_error(last_error()));
    }
    sent += static_cast<std::size_t>(written);
  }
  return Status::success();
}

Status Socket::send_all_bounded(std::span<const std::byte> data) {
  return send_all(data, 5 * nanos_per_second);
}

Result<std::size_t> Socket::recv_some(std::span<std::byte> data, Nanos deadline) {
  if (handle_ == invalid_socket) {
    return Status(Code::Invalid, "socket is not open");
  }
  PODFABRIC_TRY(set_recv_timeout(deadline));
#if defined(_WIN32)
  const int got = ::recv(static_cast<SOCKET>(handle_), reinterpret_cast<char*>(data.data()),
                         static_cast<int>(data.size()), 0);
#else
  const ssize_t got = ::recv(handle_, data.data(), data.size(), 0);
#endif
  if (got == 0) {
    return static_cast<std::size_t>(0);
  }
  if (got < 0) {
    return Status(Code::Unavailable, "receive failed: " + describe_socket_error(last_error()));
  }
  return static_cast<std::size_t>(got);
}

Result<std::size_t> Socket::recv_some_bounded(std::span<std::byte> data) {
  return recv_some(data, 5 * nanos_per_second);
}

std::uint16_t Socket::bound_port() const {
  if (handle_ == invalid_socket) {
    return 0;
  }
  sockaddr_storage address{};
  socklen_t length = sizeof(address);
  if (::getsockname(handle_, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
    return 0;
  }
  if (address.ss_family == AF_INET) {
    return ::ntohs(reinterpret_cast<sockaddr_in*>(&address)->sin_port);
  }
  if (address.ss_family == AF_INET6) {
    return ::ntohs(reinterpret_cast<sockaddr_in6*>(&address)->sin6_port);
  }
  return 0;
}

std::string Socket::peer_description() const { return peer_; }

std::string format_endpoint(const std::string& host, std::uint16_t port) {
  std::string out = host;
  out.push_back(':');
  out.append(std::to_string(port));
  return out;
}

Result<std::pair<std::string, std::uint16_t>> parse_endpoint(const std::string& text) {
  const std::size_t colon = text.rfind(':');
  if (colon == std::string::npos || colon == 0 || colon + 1 >= text.size()) {
    return Status(Code::Invalid, "endpoint must be written as host:port");
  }
  const std::string host = text.substr(0, colon);
  const std::string service = text.substr(colon + 1);
  std::uint32_t port = 0;
  for (char c : service) {
    if (c < '0' || c > '9') {
      return Status(Code::Invalid, "endpoint port is not numeric");
    }
    port = port * 10u + static_cast<std::uint32_t>(c - '0');
    if (port > 65535u) {
      return Status(Code::Invalid, "endpoint port is out of range");
    }
  }
  // Port zero is a valid request for an operating-system-assigned port, which
  // is what a listener uses. A client that tries to connect to it is refused by
  // connect() rather than here.
  return std::make_pair(host, static_cast<std::uint16_t>(port));
}

}  // namespace podfabric::transport
