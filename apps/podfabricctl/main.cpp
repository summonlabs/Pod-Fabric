// podfabricctl - inspection and control for a pod fabric.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

#include "cli.hpp"
#include "podfabric/codec/codec.hpp"
#include "podfabric/core/bytes.hpp"
#include "podfabric/control/client.hpp"
#include "podfabric/control/controller.hpp"
#include "podfabric/core/digest.hpp"
#include "podfabric/engine/composer.hpp"
#include "podfabric/model/synthetic.hpp"
#include "podfabric/persist/store.hpp"
#include "podfabric/platform/file.hpp"
#include "podfabric/version.hpp"

namespace {

const std::vector<std::string> kUsage = {
    "podfabricctl <command> [options]",
    "",
    "Commands:",
    "  compose      --snapshot <file> [--out <file>] [--expectations <file>]",
    "  synthesize   --out <file> [--pod <id>] [--racks <n>] [--domains <n>] [--seed <n>]",
    "               [--generation <n>] [--capacity <n>] [--reserved <n>] [--omit-every <n>]",
    "               [--ring] [--observed-at <n>] [--prefix <text>]",
    "  status       --endpoint <host:port>",
    "  describe     --endpoint <host:port>",
    "  apply        --endpoint <host:port> --snapshot <file>",
    "  drain        --endpoint <host:port> --rack <id> [--clear]",
    "  maintenance  --endpoint <host:port> --rack <id> [--clear]",
    "  retire       --endpoint <host:port>",
    "  mint         --endpoint <host:port> [--scope pod|member] [--rack <id>] [--lease-ms <n>]",
    "               [--out <file>]",
    "  revoke       --endpoint <host:port> --lease <id>",
    "  check        --endpoint <host:port> --token <file> [--scope pod|member] [--rack <id>]",
    "  shutdown     --endpoint <host:port>",
    "  selftest",
    "  version",
    "",
    "Exit codes: 0 success, 1 operation refused or failed, 2 usage error.",
};

int usage_error(const std::string& message) {
  std::fprintf(stderr, "podfabricctl: %s\n", message.c_str());
  std::fprintf(stderr, "run 'podfabricctl --help' for usage\n");
  return 2;
}

int fail(const podfabric::Status& status) {
  std::fprintf(stderr, "podfabricctl: %s\n", status.to_string().c_str());
  return 1;
}

podfabric::Result<std::string> read_text_file(const std::string& path, std::uint64_t limit) {
  const auto bytes = podfabric::platform::read_file(path, limit);
  if (!bytes.ok()) {
    return bytes.status();
  }
  return std::string(reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size());
}

int write_text_file(const std::string& path, const std::string& text) {
  const auto status = podfabric::platform::write_file(
      path, std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                                       text.size()));
  return status.ok() ? 0 : fail(status);
}

podfabric::Result<podfabric::control::PodClient> connect(
    const podfabric::cli::Options& options) {
  const auto endpoint = podfabric::cli::required(options, "--endpoint");
  if (!endpoint.ok()) {
    return endpoint.status();
  }
  return podfabric::control::PodClient::connect(endpoint.value());
}

podfabric::control::Request base_request(podfabric::control::MessageKind kind) {
  podfabric::control::Request request;
  request.kind = kind;
  return request;
}

int command_compose(const podfabric::cli::Options& options) {
  const auto snapshot_path = podfabric::cli::required(options, "--snapshot");
  if (!snapshot_path.ok()) {
    return fail(snapshot_path.status());
  }
  const auto text = read_text_file(snapshot_path.value(), 16ull * 1024ull * 1024ull);
  if (!text.ok()) {
    return fail(text.status());
  }
  const auto snapshot = podfabric::codec::snapshot_from_text(text.value());
  if (!snapshot.ok()) {
    return fail(snapshot.status());
  }
  podfabric::CompositionRequest request;
  request.snapshot = snapshot.value();
  const std::string expectations_path = podfabric::cli::value_or(options, "--expectations", {});
  if (!expectations_path.empty()) {
    const auto expectations_text = read_text_file(expectations_path, 4ull * 1024ull * 1024ull);
    if (!expectations_text.ok()) {
      return fail(expectations_text.status());
    }
    const auto expected = podfabric::codec::snapshot_from_text(expectations_text.value());
    if (!expected.ok()) {
      return fail(expected.status());
    }
    request.context.expectations = podfabric::expectations_for(expected.value());
    request.context.epoch = podfabric::PodEpoch(1);
  }
  const auto state = podfabric::compose(request);
  if (!state.ok()) {
    return fail(state.status());
  }
  const auto rendered = podfabric::codec::to_text(state.value());
  if (!rendered.ok()) {
    return fail(rendered.status());
  }
  const std::string out_path = podfabric::cli::value_or(options, "--out", {});
  if (out_path.empty()) {
    std::fwrite(rendered.value().data(), 1, rendered.value().size(), stdout);
    return 0;
  }
  return write_text_file(out_path, rendered.value());
}

int command_synthesize(const podfabric::cli::Options& options) {
  const auto out_path = podfabric::cli::required(options, "--out");
  if (!out_path.ok()) {
    return fail(out_path.status());
  }
  podfabric::SyntheticOptions synthetic;
  const auto pod_text = podfabric::cli::value_or(options, "--pod", "pod-demo");
  const auto pod = podfabric::PodId::parse(pod_text);
  if (!pod.ok()) {
    return fail(pod.status());
  }
  synthetic.pod = pod.value();
  synthetic.id_prefix = podfabric::cli::value_or(options, "--prefix", "rack");
  const auto racks = podfabric::cli::unsigned_or(options, "--racks", 4);
  const auto domains = podfabric::cli::unsigned_or(options, "--domains", 2);
  const auto seed = podfabric::cli::unsigned_or(options, "--seed", 1);
  const auto generation = podfabric::cli::unsigned_or(options, "--generation", 7);
  const auto capacity = podfabric::cli::unsigned_or(options, "--capacity", 100);
  const auto reserved = podfabric::cli::unsigned_or(options, "--reserved", 10);
  const auto omit = podfabric::cli::unsigned_or(options, "--omit-every", 0);
  const auto observed = podfabric::cli::signed_or(options, "--observed-at", 0);
  if (!racks.ok() || !domains.ok() || !seed.ok() || !generation.ok() || !capacity.ok() ||
      !reserved.ok() || !omit.ok() || !observed.ok()) {
    return usage_error("a numeric option is out of range");
  }
  if (racks.value() == 0 || racks.value() > 4096) {
    return usage_error("--racks must be between 1 and 4096");
  }
  if (domains.value() == 0 || domains.value() > 4096) {
    return usage_error("--domains must be between 1 and 4096");
  }
  if (reserved.value() > capacity.value()) {
    return usage_error("--reserved must not exceed --capacity");
  }
  synthetic.racks = static_cast<std::uint32_t>(racks.value());
  synthetic.power_domains = static_cast<std::uint32_t>(domains.value());
  synthetic.seed = seed.value();
  synthetic.generation = podfabric::RackGeneration(generation.value());
  synthetic.capacity_per_rack = capacity.value();
  synthetic.reserved_per_rack = reserved.value();
  synthetic.omit_every = static_cast<std::uint32_t>(omit.value());
  synthetic.full_mesh = !podfabric::cli::has_flag(options, "--ring");
  if (observed.value() > 0) {
    synthetic.observed_at = observed.value();
  }
  const auto snapshot = podfabric::synthesize(synthetic);
  const auto rendered = podfabric::codec::to_text(snapshot);
  if (!rendered.ok()) {
    return fail(rendered.status());
  }
  return write_text_file(out_path.value(), rendered.value());
}

int command_inspect(const podfabric::cli::Options& options, bool full) {
  auto client = connect(options);
  if (!client.ok()) {
    return fail(client.status());
  }
  auto request = base_request(full ? podfabric::control::MessageKind::Describe
                                   : podfabric::control::MessageKind::Status);
  const auto response = client.value().call(request);
  if (!response.ok()) {
    return fail(response.status());
  }
  const auto& body = response.value();
  if (!full) {
    std::printf(
        "code=%s epoch=%llu lifecycle=%s established=%u fenced=%u stale=%u conflicting=%u\n",
        std::string(podfabric::to_string(body.code)).c_str(),
        static_cast<unsigned long long>(body.epoch.value()),
        std::string(podfabric::to_string(body.lifecycle)).c_str(), body.established_members,
        body.fenced_members, body.stale_members, body.conflicting_members);
    std::printf("digest=%s recovery=%s ambiguous-commit=%d incarnation=%s\n",
                body.state_digest.to_hex().c_str(),
                std::string(podfabric::persist::to_string(body.recovery)).c_str(),
                body.ambiguous_commit ? 1 : 0, body.incarnation.to_string().c_str());
  }
  std::fwrite(body.text.data(), 1, body.text.size(), stdout);
  return body.code == podfabric::Code::Ok ? 0 : 1;
}

int command_apply(const podfabric::cli::Options& options) {
  const auto snapshot_path = podfabric::cli::required(options, "--snapshot");
  if (!snapshot_path.ok()) {
    return fail(snapshot_path.status());
  }
  const auto text = read_text_file(snapshot_path.value(), 16ull * 1024ull * 1024ull);
  if (!text.ok()) {
    return fail(text.status());
  }
  const auto snapshot = podfabric::codec::snapshot_from_text(text.value());
  if (!snapshot.ok()) {
    return fail(snapshot.status());
  }
  auto client = connect(options);
  if (!client.ok()) {
    return fail(client.status());
  }
  auto request = base_request(podfabric::control::MessageKind::Apply);
  request.snapshot = snapshot.value();
  const auto response = client.value().call(request);
  if (!response.ok()) {
    return fail(response.status());
  }
  const auto& body = response.value();
  std::printf("outcome=%s code=%s epoch=%llu lifecycle=%s digest=%s\n",
              std::string(podfabric::to_string(body.outcome)).c_str(),
              std::string(podfabric::to_string(body.code)).c_str(),
              static_cast<unsigned long long>(body.epoch.value()),
              std::string(podfabric::to_string(body.lifecycle)).c_str(),
              body.state_digest.to_hex().c_str());
  std::printf("revalidation-sound=%d %s\n", body.revalidation_sound ? 1 : 0,
              body.revalidation.c_str());
  if (!body.revalidation_sound) {
    std::fprintf(stderr, "podfabricctl: the server reported an unsound revalidation plan\n");
    return 1;
  }
  return body.code == podfabric::Code::Ok ? 0 : 1;
}

int command_admin(const podfabric::cli::Options& options, podfabric::AdminState state) {
  const auto rack_text = podfabric::cli::required(options, "--rack");
  if (!rack_text.ok()) {
    return fail(rack_text.status());
  }
  const auto rack = podfabric::RackId::parse(rack_text.value());
  if (!rack.ok()) {
    return fail(rack.status());
  }
  auto client = connect(options);
  if (!client.ok()) {
    return fail(client.status());
  }
  auto request = base_request(podfabric::control::MessageKind::SetAdmin);
  request.rack = rack.value();
  request.admin = state;
  request.clear_admin = podfabric::cli::has_flag(options, "--clear");
  const auto response = client.value().call(request);
  if (!response.ok()) {
    return fail(response.status());
  }
  std::printf("%s %s\n", response.value().message.c_str(), rack.value().token().c_str());
  return response.value().code == podfabric::Code::Ok ? 0 : 1;
}

int command_retire(const podfabric::cli::Options& options) {
  auto client = connect(options);
  if (!client.ok()) {
    return fail(client.status());
  }
  const auto response =
      client.value().call(base_request(podfabric::control::MessageKind::Retire));
  if (!response.ok()) {
    return fail(response.status());
  }
  std::printf("%s\n", response.value().message.c_str());
  return response.value().code == podfabric::Code::Ok ? 0 : 1;
}

int command_mint(const podfabric::cli::Options& options) {
  auto client = connect(options);
  if (!client.ok()) {
    return fail(client.status());
  }
  auto request = base_request(podfabric::control::MessageKind::Mint);
  const std::string scope = podfabric::cli::value_or(options, "--scope", "pod");
  if (scope == "pod") {
    request.scope = podfabric::AuthorityScope::Pod;
  } else if (scope == "member") {
    request.scope = podfabric::AuthorityScope::Member;
  } else if (scope == "link") {
    request.scope = podfabric::AuthorityScope::Link;
  } else if (scope == "capacity-reservation") {
    request.scope = podfabric::AuthorityScope::CapacityReservation;
  } else if (scope == "maintenance-window") {
    request.scope = podfabric::AuthorityScope::MaintenanceWindow;
  } else {
    return usage_error("--scope is not a known authority scope");
  }
  const std::string rack_text = podfabric::cli::value_or(options, "--rack", {});
  if (!rack_text.empty()) {
    const auto rack = podfabric::RackId::parse(rack_text);
    if (!rack.ok()) {
      return fail(rack.status());
    }
    request.rack = rack.value();
  }
  const auto lease_ms = podfabric::cli::signed_or(options, "--lease-ms", 0);
  if (!lease_ms.ok()) {
    return usage_error(lease_ms.status().message());
  }
  request.lifetime = lease_ms.value() * podfabric::nanos_per_millisecond;
  const auto response = client.value().call(request);
  if (!response.ok()) {
    return fail(response.status());
  }
  const auto& body = response.value();
  if (body.code != podfabric::Code::Ok) {
    std::fprintf(stderr, "podfabricctl: %s\n", body.message.c_str());
    return 1;
  }
  std::printf("lease=%s epoch=%llu expires-at=%lld scope=%s\n", body.token.lease.token().c_str(),
              static_cast<unsigned long long>(body.token.epoch.value()),
              static_cast<long long>(body.token.expires_at),
              std::string(podfabric::to_string(body.token.scope)).c_str());
  const std::string out_path = podfabric::cli::value_or(options, "--out", {});
  if (!out_path.empty()) {
    podfabric::ByteWriter writer;
    const auto status = podfabric::codec::encode(body.token, writer);
    if (!status.ok()) {
      return fail(status);
    }
    const auto written = podfabric::platform::write_file(out_path, writer.buffer());
    if (!written.ok()) {
      return fail(written);
    }
  }
  return 0;
}

int command_revoke(const podfabric::cli::Options& options) {
  const auto lease_text = podfabric::cli::required(options, "--lease");
  if (!lease_text.ok()) {
    return fail(lease_text.status());
  }
  const auto lease = podfabric::LeaseId::parse(lease_text.value());
  if (!lease.ok()) {
    return fail(lease.status());
  }
  auto client = connect(options);
  if (!client.ok()) {
    return fail(client.status());
  }
  auto request = base_request(podfabric::control::MessageKind::Revoke);
  request.lease = lease.value();
  const auto response = client.value().call(request);
  if (!response.ok()) {
    return fail(response.status());
  }
  std::printf("%s\n", response.value().message.c_str());
  return response.value().code == podfabric::Code::Ok ? 0 : 1;
}

int command_check(const podfabric::cli::Options& options) {
  const auto token_path = podfabric::cli::required(options, "--token");
  if (!token_path.ok()) {
    return fail(token_path.status());
  }
  const auto bytes = podfabric::platform::read_file(token_path.value(), 64ull * 1024ull);
  if (!bytes.ok()) {
    return fail(bytes.status());
  }
  podfabric::ByteReader reader(std::span<const std::byte>(bytes.value()));
  const auto token = podfabric::codec::decode_token(reader);
  if (!token.ok()) {
    return fail(token.status());
  }
  auto client = connect(options);
  if (!client.ok()) {
    return fail(client.status());
  }
  auto request = base_request(podfabric::control::MessageKind::CheckAuthority);
  request.token = token.value();
  const std::string scope = podfabric::cli::value_or(options, "--scope", "pod");
  request.scope = scope == "member" ? podfabric::AuthorityScope::Member
                                    : podfabric::AuthorityScope::Pod;
  const std::string rack_text = podfabric::cli::value_or(options, "--rack", {});
  if (!rack_text.empty()) {
    const auto rack = podfabric::RackId::parse(rack_text);
    if (!rack.ok()) {
      return fail(rack.status());
    }
    request.rack = rack.value();
  }
  const auto response = client.value().call(request);
  if (!response.ok()) {
    return fail(response.status());
  }
  std::printf("verdict=%s code=%s detail=%s\n",
              std::string(podfabric::to_string(response.value().verdict)).c_str(),
              std::string(podfabric::to_string(response.value().code)).c_str(),
              response.value().verdict_detail.c_str());
  return response.value().verdict == podfabric::AuthorityVerdictCode::Valid ? 0 : 1;
}

int command_shutdown(const podfabric::cli::Options& options) {
  auto client = connect(options);
  if (!client.ok()) {
    return fail(client.status());
  }
  const auto response =
      client.value().call(base_request(podfabric::control::MessageKind::Shutdown));
  if (!response.ok()) {
    return fail(response.status());
  }
  std::printf("%s\n", response.value().message.c_str());
  return 0;
}

int command_selftest() {
  int failures = 0;
  const auto check = [&failures](bool condition, const char* label) {
    std::printf("%-56s %s\n", label, condition ? "ok" : "FAILED");
    if (!condition) {
      ++failures;
    }
  };

  // FIPS 180-4 reference vectors.
  check(podfabric::Digest::of("").to_hex() ==
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        "sha256 of the empty message matches the reference vector");
  check(podfabric::Digest::of("abc").to_hex() ==
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "sha256 of abc matches the reference vector");

  podfabric::SyntheticOptions synthetic;
  const auto pod = podfabric::PodId::parse("pod-selftest");
  if (!pod.ok()) {
    return fail(pod.status());
  }
  synthetic.pod = pod.value();
  synthetic.racks = 4;
  synthetic.power_domains = 2;
  synthetic.with_obligations = false;
  const auto snapshot = podfabric::synthesize(synthetic);
  podfabric::CompositionRequest request;
  request.snapshot = snapshot;
  request.context.expectations = podfabric::expectations_for(snapshot);
  request.context.epoch = podfabric::PodEpoch(1);
  const auto first = podfabric::compose(request);
  check(first.ok(), "a synthetic four-rack pod composes");
  if (first.ok()) {
    check(first.value().lifecycle == podfabric::LifecycleState::Active,
          "a healthy pod reports active");
    const auto second = podfabric::compose(request);
    check(second.ok() && second.value().state_digest == first.value().state_digest,
          "composition is deterministic");
    const auto aggregate = first.value().find_capacity(
        podfabric::ResourceClass::from_canonical_literal("fabric.bandwidth"));
    check(aggregate != nullptr && aggregate->total == 400,
          "aggregate capacity counts every rack exactly once");
  }
  std::printf("\n%d check(s) failed\n", failures);
  return failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  const std::set<std::string> value_flags{
      "--snapshot", "--out",           "--expectations", "--endpoint", "--rack",
      "--scope",    "--lease-ms",      "--lease",        "--token",    "--pod",
      "--prefix",   "--racks",         "--domains",      "--seed",     "--generation",
      "--capacity", "--reserved",      "--omit-every",   "--observed-at"};
  const std::set<std::string> bool_flags{"--help", "--version", "--clear", "--ring"};
  if (argc >= 2 && std::string(argv[1]) == "--help") {
    for (const std::string& line : kUsage) {
      std::printf("%s\n", line.c_str());
    }
    return 0;
  }
  if (argc >= 2 && std::string(argv[1]) == "--version") {
    std::printf("podfabricctl %s\n", std::string(podfabric::version_string).c_str());
    return 0;
  }
  const auto parsed = podfabric::cli::parse(argc, argv, value_flags, bool_flags, 1);
  if (!parsed.ok()) {
    return usage_error(parsed.status().message());
  }
  const auto& options = parsed.value();
  if (options.positional.empty()) {
    for (const std::string& line : kUsage) {
      std::printf("%s\n", line.c_str());
    }
    return 2;
  }
  const std::string command = options.positional.front();
  if (command == "compose") return command_compose(options);
  if (command == "synthesize") return command_synthesize(options);
  if (command == "status") return command_inspect(options, false);
  if (command == "describe") return command_inspect(options, true);
  if (command == "apply") return command_apply(options);
  if (command == "drain") return command_admin(options, podfabric::AdminState::Draining);
  if (command == "maintenance") return command_admin(options, podfabric::AdminState::Maintenance);
  if (command == "retire") return command_retire(options);
  if (command == "mint") return command_mint(options);
  if (command == "revoke") return command_revoke(options);
  if (command == "check") return command_check(options);
  if (command == "shutdown") return command_shutdown(options);
  if (command == "selftest") return command_selftest();
  if (command == "version") {
    std::printf("podfabricctl %s\n", std::string(podfabric::version_string).c_str());
    return 0;
  }
  return usage_error("unrecognised command '" + command + "'");
}
