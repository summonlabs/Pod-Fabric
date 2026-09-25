// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "podfabric/control/server.hpp"

#include <algorithm>
#include <utility>

namespace podfabric::control {
namespace {

Response error_response(Code code, std::string message) {
  Response response;
  response.code = code;
  response.message = std::move(message);
  return response;
}

}  // namespace

Result<transport::Frame> read_frame(transport::Socket& socket, Nanos deadline,
                                    std::size_t max_payload) {
  std::vector<std::byte> header_bytes(transport::frame_header_size);
  std::size_t filled = 0;
  while (filled < header_bytes.size()) {
    PODFABRIC_TRY_ASSIGN(
        const std::size_t got,
        socket.recv_some(
            std::span<std::byte>(header_bytes.data() + filled, header_bytes.size() - filled),
            deadline));
    if (got == 0) {
      return Status(Code::Cancelled, "the peer closed the connection");
    }
    filled += got;
  }
  PODFABRIC_TRY_ASSIGN(const transport::FrameHeader header,
                       transport::parse_frame_header(header_bytes));
  if (header.length > max_payload) {
    return Status(Code::Exhausted, "frame payload exceeds the connection limit");
  }
  std::vector<std::byte> whole(transport::frame_header_size + header.length);
  std::copy(header_bytes.begin(), header_bytes.end(), whole.begin());
  std::size_t offset = transport::frame_header_size;
  while (offset < whole.size()) {
    PODFABRIC_TRY_ASSIGN(
        const std::size_t got,
        socket.recv_some(std::span<std::byte>(whole.data() + offset, whole.size() - offset),
                         deadline));
    if (got == 0) {
      return Status(Code::Incomplete, "the peer closed inside a frame payload");
    }
    offset += got;
  }
  PODFABRIC_TRY_ASSIGN(const transport::FrameHeader verified,
                       transport::parse_frame_header(whole));
  if (!transport::verify_frame(whole, verified)) {
    return Status(Code::Corrupt, "frame checksum mismatch");
  }
  transport::Frame frame;
  frame.type = verified.type;
  frame.flags = verified.flags;
  frame.correlation = verified.correlation;
  frame.payload.assign(whole.begin() + static_cast<std::ptrdiff_t>(transport::frame_header_size),
                       whole.end());
  return frame;
}

Status write_frame(transport::Socket& socket, const transport::Frame& frame) {
  std::vector<std::byte> encoded;
  PODFABRIC_TRY(transport::encode_frame(frame, encoded));
  return socket.send_all_bounded(encoded);
}

PodServer::PodServer(PodController& controller, ServerOptions options)
    : controller_(controller), options_(std::move(options)) {}

PodServer::~PodServer() { (void)stop(); }

Status PodServer::start() {
  if (running_.load()) {
    return Status(Code::Refused, "the pod server is already running");
  }
  PODFABRIC_TRY(transport::Socket::initialise_runtime());
  if (options_.workers == 0 || options_.workers > 64) {
    return Status(Code::Invalid, "the worker count must be between 1 and 64");
  }
  if (options_.max_pending_connections == 0 || options_.max_pending_connections > 4096) {
    return Status(Code::Invalid, "the pending-connection bound must be between 1 and 4096");
  }
  PODFABRIC_TRY_ASSIGN(
      transport::Socket listener,
      transport::Socket::listen(options_.host, options_.port,
                                static_cast<int>(options_.max_pending_connections)));
  port_ = listener.bound_port();
  if (port_ == 0) {
    return Status(Code::Unavailable, "the listener did not receive a port");
  }
  listener_ = std::move(listener);
  stopping_.store(false);
  running_.store(true);
  workers_.reserve(options_.workers);
  for (std::size_t i = 0; i < options_.workers; ++i) {
    workers_.emplace_back([this] { worker_loop(); });
  }
  acceptor_ = std::thread([this] { accept_loop(); });
  return Status::success();
}

Status PodServer::stop() {
  if (!running_.load() && !acceptor_.joinable() && workers_.empty()) {
    return Status::success();
  }
  stopping_.store(true);
  queue_cv_.notify_all();
  if (acceptor_.joinable()) {
    acceptor_.join();
  }
  for (std::thread& worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  workers_.clear();
  {
    std::lock_guard<std::mutex> guard(queue_mutex_);
    queue_.clear();  // sockets close on destruction, after every worker stopped
  }
  const Status closed = listener_.close();
  running_.store(false);
  return closed;
}

std::string PodServer::endpoint() const {
  return transport::format_endpoint(options_.host, port_);
}

void PodServer::accept_loop() {
  while (!stopping_.load()) {
    auto accepted = listener_.accept(options_.accept_poll_interval);
    if (!accepted.ok()) {
      continue;
    }
    std::unique_lock<std::mutex> lock(queue_mutex_);
    queue_cv_.wait(lock, [this] {
      return stopping_.load() || queue_.size() < options_.max_pending_connections;
    });
    if (stopping_.load()) {
      return;
    }
    queue_.push_back(std::move(accepted.value()));
    lock.unlock();
    queue_cv_.notify_one();
  }
}

void PodServer::worker_loop() {
  for (;;) {
    transport::Socket connection;
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      queue_cv_.wait(lock, [this] { return stopping_.load() || !queue_.empty(); });
      if (queue_.empty()) {
        if (stopping_.load()) {
          return;
        }
        continue;
      }
      connection = std::move(queue_.front());
      queue_.pop_front();
    }
    queue_cv_.notify_all();
    serve(std::move(connection));
  }
}

void PodServer::serve(transport::Socket connection) {
  connections_.fetch_add(1);
  for (;;) {
    if (stopping_.load()) {
      return;
    }
    auto frame = read_frame(connection, options_.io_deadline, transport::frame_max_payload);
    if (!frame.ok()) {
      if (frame.code() == Code::Corrupt || frame.code() == Code::Exhausted ||
          frame.code() == Code::Invalid || frame.code() == Code::Unsupported) {
        protocol_errors_.fetch_add(1);
        const std::string text = frame.status().to_string();
        transport::Frame error;
        error.type = transport::FrameType::Error;
        error.payload.assign(reinterpret_cast<const std::byte*>(text.data()),
                             reinterpret_cast<const std::byte*>(text.data()) + text.size());
        (void)write_frame(connection, error);
      }
      return;
    }
    requests_.fetch_add(1);
    const transport::Frame& incoming = frame.value();
    if (incoming.type == transport::FrameType::Ping) {
      transport::Frame pong;
      pong.type = transport::FrameType::Pong;
      pong.correlation = incoming.correlation;
      if (!write_frame(connection, pong).ok()) {
        return;
      }
      continue;
    }
    if (incoming.type != transport::FrameType::Request) {
      protocol_errors_.fetch_add(1);
      return;
    }
    const auto decoded = decode_request(incoming.payload);
    Response response;
    if (!decoded.ok()) {
      protocol_errors_.fetch_add(1);
      response = error_response(decoded.code(), decoded.status().message());
    } else {
      response = handle(decoded.value());
      response.nonce = decoded.value().nonce;
    }
    std::vector<std::byte> body;
    if (!encode_response(response, body).ok()) {
      return;
    }
    transport::Frame outgoing;
    outgoing.type = transport::FrameType::Response;
    outgoing.correlation = incoming.correlation;
    outgoing.payload = std::move(body);
    if (!write_frame(connection, outgoing).ok()) {
      return;
    }
    if (decoded.ok() && decoded.value().kind == MessageKind::Shutdown) {
      stopping_.store(true);
      queue_cv_.notify_all();
      return;
    }
  }
}

Response PodServer::handle(const Request& request) {
  Response response;
  const PodState current = controller_.state();
  response.epoch = current.epoch;
  response.incarnation = current.incarnation;
  response.lifecycle = current.lifecycle;
  response.state_digest = current.state_digest;
  response.established_members = current.authority.established_members;
  response.fenced_members = current.authority.fenced_members;
  response.stale_members = current.authority.stale_members;
  response.conflicting_members = current.authority.conflicting_members;
  response.recovery = controller_.recovery().outcome;
  response.ambiguous_commit = controller_.ambiguous_commit();
  bool truncated = false;
  response.text = bounded_text(controller_.describe(), options_.max_text_bytes, truncated);
  response.text_truncated = truncated;

  switch (request.kind) {
    case MessageKind::Hello:
    case MessageKind::Ping:
    case MessageKind::Status:
    case MessageKind::Describe:
      response.code = Code::Ok;
      response.message = std::string(to_string(request.kind));
      return response;
    case MessageKind::Apply: {
      auto applied = controller_.apply(request.snapshot);
      if (!applied.ok()) {
        response.code = applied.code();
        response.message = applied.status().message();
        return response;
      }
      const ApplyReport& report = applied.value();
      response.code = Code::Ok;
      response.message = std::string(to_string(report.outcome));
      response.outcome = report.outcome;
      response.epoch = report.state.epoch;
      response.lifecycle = report.state.lifecycle;
      response.state_digest = report.state.state_digest;
      response.revalidation = report.detail;
      response.revalidation_sound = report.revalidation.sound();
      response.established_members = report.state.authority.established_members;
      response.fenced_members = report.state.authority.fenced_members;
      response.stale_members = report.state.authority.stale_members;
      response.conflicting_members = report.state.authority.conflicting_members;
      bool apply_truncated = false;
      response.text = bounded_text(controller_.describe(), options_.max_text_bytes, apply_truncated);
      response.text_truncated = apply_truncated;
      return response;
    }
    case MessageKind::Mint: {
      auto minted = controller_.mint_authority(request.scope, request.rack, request.lifetime);
      if (!minted.ok()) {
        response.code = minted.code();
        response.message = minted.status().message();
        return response;
      }
      response.code = Code::Ok;
      response.message = "granted";
      response.token = minted.value();
      response.epoch = minted.value().epoch;
      return response;
    }
    case MessageKind::Revoke: {
      const Status revoked = controller_.revoke_authority(request.lease);
      response.code = revoked.code();
      response.message = revoked.ok() ? std::string("revoked") : revoked.message();
      return response;
    }
    case MessageKind::CheckAuthority: {
      const AuthorityVerdict verdict =
          controller_.check_authority(request.token, request.scope, request.rack);
      response.code = verdict.status;
      response.message = verdict.detail;
      response.verdict = verdict.code;
      response.verdict_detail = verdict.detail;
      return response;
    }
    case MessageKind::SetAdmin: {
      const Status changed = request.clear_admin
                                 ? controller_.clear_admin_state(request.rack)
                                 : controller_.set_admin_state(request.rack, request.admin);
      response.code = changed.code();
      response.message = changed.ok() ? std::string("ok") : changed.message();
      if (changed.ok()) {
        // The mutation recomposes the pod, so the response must report the
        // state that now holds rather than the one it started from.
        const PodState updated = controller_.state();
        response.epoch = updated.epoch;
        response.lifecycle = updated.lifecycle;
        response.state_digest = updated.state_digest;
        bool refreshed_truncated = false;
        response.text = bounded_text(controller_.describe(), options_.max_text_bytes,
                                     refreshed_truncated);
        response.text_truncated = refreshed_truncated;
      }
      return response;
    }
    case MessageKind::Retire: {
      const Status retired = controller_.retire();
      response.code = retired.code();
      response.message = retired.ok() ? std::string("retired") : retired.message();
      response.lifecycle = controller_.lifecycle();
      return response;
    }
    case MessageKind::Erase:
      response.code = Code::Unsupported;
      response.message = "the store is not erasable over the protocol";
      return response;
    case MessageKind::Shutdown:
      response.code = Code::Ok;
      response.message = "stopping";
      return response;
  }
  response.code = Code::Invalid;
  response.message = "unhandled request kind";
  return response;
}

}  // namespace podfabric::control
