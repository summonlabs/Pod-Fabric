// podfabd - the pod fabric daemon.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "cli.hpp"
#include "podfabric/codec/codec.hpp"
#include "podfabric/control/controller.hpp"
#include "podfabric/control/protocol.hpp"
#include "podfabric/control/server.hpp"
#include "podfabric/model/synthetic.hpp"
#include "podfabric/platform/file.hpp"
#include "podfabric/version.hpp"

namespace {

std::atomic<bool> g_interrupted{false};

void on_signal(int) { g_interrupted.store(true); }

const char* kUsageLines[] = {
    "podfabd --pod <id> [--store <dir>] [--listen <host:port>] [--workers <n>]",
    "        [--require-token] [--lease-ms <n>] [--evidence <file>]",
    "        [--warm-up] [--max-text-bytes <n>] [--help]",
    "",
    "--store is optional; without it the daemon is memory-only and reports so.",
    "--listen defaults to 127.0.0.1:0, which asks the operating system for a free port.",
    "--evidence applies a synthetic snapshot at start-up, which makes the daemon",
    "  immediately useful for inspection and for the multiprocess proofs.",
};

}  // namespace

int main(int argc, char** argv) {
  const std::set<std::string> value_flags{"--pod", "--store", "--listen", "--workers",
                                          "--lease-ms", "--evidence", "--max-text-bytes"};
  const std::set<std::string> bool_flags{"--require-token", "--help", "--version"};
  const auto parsed = podfabric::cli::parse(argc, argv, value_flags, bool_flags, 0);
  if (!parsed.ok()) {
    std::fprintf(stderr, "podfabd: %s\n", parsed.status().to_string().c_str());
    return 2;
  }
  const auto& options = parsed.value();
  if (podfabric::cli::has_flag(options, "--help")) {
    for (const char* line : kUsageLines) {
      std::printf("%s\n", line);
    }
    return 0;
  }
  if (podfabric::cli::has_flag(options, "--version")) {
    std::printf("podfabd %s\n", std::string(podfabric::version_string).c_str());
    return 0;
  }
  const auto pod_text = podfabric::cli::required(options, "--pod");
  if (!pod_text.ok()) {
    std::fprintf(stderr, "podfabd: %s\n", pod_text.status().to_string().c_str());
    return 2;
  }
  const auto pod = podfabric::PodId::parse(pod_text.value());
  if (!pod.ok()) {
    std::fprintf(stderr, "podfabd: --pod is not a canonical identifier\n");
    return 2;
  }

  podfabric::ControllerOptions controller_options;
  controller_options.pod = pod.value();
  controller_options.store_directory = podfabric::cli::value_or(options, "--store", {});
  controller_options.require_authority_token = podfabric::cli::has_flag(options, "--require-token");
  const auto lease_ms = podfabric::cli::signed_or(options, "--lease-ms", 120000);
  if (!lease_ms.ok()) {
    std::fprintf(stderr, "podfabd: %s\n", lease_ms.status().to_string().c_str());
    return 2;
  }
  controller_options.default_lease_duration =
      lease_ms.value() * podfabric::nanos_per_millisecond;

  podfabric::control::ServerOptions server_options;
  const std::string listen = podfabric::cli::value_or(options, "--listen", "127.0.0.1:0");
  const auto endpoint = podfabric::transport::parse_endpoint(listen);
  if (!endpoint.ok()) {
    std::fprintf(stderr, "podfabd: --listen %s\n", endpoint.status().to_string().c_str());
    return 2;
  }
  server_options.host = endpoint.value().first;
  server_options.port = endpoint.value().second;
  const auto workers = podfabric::cli::unsigned_or(options, "--workers", 4);
  const auto max_text = podfabric::cli::unsigned_or(options, "--max-text-bytes", 512 * 1024);
  if (!workers.ok() || !max_text.ok()) {
    std::fprintf(stderr, "podfabd: invalid numeric option\n");
    return 2;
  }
  server_options.workers = static_cast<std::size_t>(workers.value());
  server_options.max_text_bytes = static_cast<std::size_t>(max_text.value());

  podfabric::PodController controller(controller_options);
  const podfabric::Status opened = controller.open();
  if (!opened.ok()) {
    std::fprintf(stderr, "podfabd: open failed: %s\n", opened.to_string().c_str());
    return 1;
  }

  const std::string evidence = podfabric::cli::value_or(options, "--evidence", {});
  if (!evidence.empty()) {
    const auto bytes = podfabric::platform::read_file(evidence, 16ull * 1024ull * 1024ull);
    if (!bytes.ok()) {
      std::fprintf(stderr, "podfabd: %s\n", bytes.status().to_string().c_str());
      return 1;
    }
    const std::string text(reinterpret_cast<const char*>(bytes.value().data()),
                           bytes.value().size());
    const auto snapshot = podfabric::codec::snapshot_from_text(text);
    if (!snapshot.ok()) {
      std::fprintf(stderr, "podfabd: evidence is not a valid snapshot: %s\n",
                   snapshot.status().to_string().c_str());
      return 1;
    }
    const auto applied = controller.apply(snapshot.value());
    if (!applied.ok()) {
      std::fprintf(stderr, "podfabd: the initial snapshot was refused: %s\n",
                   applied.status().to_string().c_str());
      return 1;
    }
    std::printf("podfabd initial-apply=%s lifecycle=%s\n",
                std::string(podfabric::to_string(applied.value().outcome)).c_str(),
                std::string(podfabric::to_string(applied.value().state.lifecycle)).c_str());
  }

  podfabric::control::PodServer server(controller, server_options);
  const podfabric::Status started = server.start();
  if (!started.ok()) {
    std::fprintf(stderr, "podfabd: listen failed: %s\n", started.to_string().c_str());
    return 1;
  }
  const podfabric::persist::RecoveryReport recovery = controller.recovery();
  std::printf("podfabd pod=%s endpoint=%s recovery=%s epoch=%llu incarnation=%s\n",
              pod.value().token().c_str(), server.endpoint().c_str(),
              std::string(podfabric::persist::to_string(recovery.outcome)).c_str(),
              static_cast<unsigned long long>(controller.epoch().value()),
              controller.incarnation().to_string().c_str());
  std::fflush(stdout);

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  while (server.running() && !server.stopping() && !g_interrupted.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  const podfabric::Status stopped = server.stop();
  const podfabric::Status closed = controller.close();
  std::printf("podfabd stopping connections=%llu requests=%llu protocol-errors=%llu\n",
              static_cast<unsigned long long>(server.connections_served()),
              static_cast<unsigned long long>(server.requests_served()),
              static_cast<unsigned long long>(server.protocol_errors()));
  std::fflush(stdout);
  if (!stopped.ok()) {
    std::fprintf(stderr, "podfabd: shutdown reported %s\n", stopped.to_string().c_str());
    return 1;
  }
  if (!closed.ok()) {
    std::fprintf(stderr, "podfabd: close reported %s\n", closed.to_string().c_str());
    return 1;
  }
  return 0;
}
