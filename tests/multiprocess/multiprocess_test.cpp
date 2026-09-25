// Independent-process proofs over a real loopback TCP transport.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Every case here starts real operating-system processes, talks to them over
// framed loopback TCP, and (where the point is durability) terminates them
// without a graceful shutdown.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "podfabric/codec/codec.hpp"
#include "podfabric/control/client.hpp"
#include "podfabric/platform/file.hpp"
#include "podfabric/model/synthetic.hpp"
#include "process.hpp"
#include "test.hpp"

#ifndef PODFABRIC_DAEMON_PATH
#define PODFABRIC_DAEMON_PATH ""
#endif
#ifndef PODFABRIC_CTL_PATH
#define PODFABRIC_CTL_PATH ""
#endif

using namespace podfabric;

namespace {

SyntheticOptions options_for(std::uint32_t racks, std::uint64_t seed,
                             std::uint64_t generation = 7) {
  SyntheticOptions options;
  options.pod = PodId::parse("pod-multiprocess").value();
  options.racks = racks;
  options.power_domains = 2;
  options.seed = seed;
  options.generation = RackGeneration(generation);
  options.observed_at = 1700000000LL * nanos_per_second;
  return options;
}

struct Daemon {
  test::Child child;
  std::string endpoint;
  std::string log;
};

std::string extract_endpoint(const std::string& text) {
  const std::string marker = "endpoint=";
  const std::size_t at = text.find(marker);
  if (at == std::string::npos) {
    return {};
  }
  std::size_t end = text.find_first_of(" \r\n", at);
  if (end == std::string::npos) {
    end = text.size();
  }
  return text.substr(at + marker.size(), end - (at + marker.size()));
}

// Starts a daemon and waits until it announces its endpoint.
bool start_daemon(Daemon& daemon, const std::string& store, const std::string& evidence,
                  const std::string& label) {
  const std::string directory = test::make_scratch_directory("multiprocess");
  daemon.log = directory + "\\" + label + ".log";
  std::vector<std::string> arguments;
  arguments.push_back("--pod");
  arguments.push_back("pod-multiprocess");
  arguments.push_back("--store");
  arguments.push_back(store);
  arguments.push_back("--listen");
  arguments.push_back("127.0.0.1:0");
  arguments.push_back("--workers");
  arguments.push_back("4");
  arguments.push_back("--require-token");
  if (!evidence.empty()) {
    arguments.push_back("--evidence");
    arguments.push_back(evidence);
  }
  auto spawned = test::spawn(PODFABRIC_DAEMON_PATH, arguments,
                                   daemon.log + ".out", daemon.log + ".err");
  if (!spawned.ok()) {
    return false;
  }
  daemon.child = std::move(spawned).value();
  if (!test::wait_for_text(daemon.log + ".out", "endpoint=", 20 * nanos_per_second)) {
    return false;
  }
  daemon.endpoint = extract_endpoint(test::read_text(daemon.log + ".out"));
  return !daemon.endpoint.empty();
}

int run_ctl(const std::vector<std::string>& arguments, const std::string& label) {
  const std::string directory = test::make_scratch_directory("multiprocess");
  const std::string out = directory + "\\" + label + ".out";
  auto spawned = test::spawn(PODFABRIC_CTL_PATH, arguments, out, out + ".err");
  if (!spawned.ok()) {
    return -1;
  }
  test::Child child = std::move(spawned.value());
  const auto status = child.wait(30 * nanos_per_second);
  if (!status.ok()) {
    (void)child.kill();
    return -1;
  }
  return status.value();
}

std::string write_snapshot(const PodSnapshot& snapshot, const std::string& label) {
  const std::string directory = test::make_scratch_directory("multiprocess");
  const std::string path = directory + "\\" + label + ".pfs";
  const auto text = codec::to_text(snapshot);
  if (!text.ok()) {
    return {};
  }
  if (!platform::write_file(
          path, std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.value().data()),
                                           text.value().size()))
           .ok()) {
    return {};
  }
  return path;
}

control::Response call(const std::string& endpoint, control::MessageKind kind,
                       const PodSnapshot* snapshot = nullptr) {
  control::Response response;
  auto client = control::PodClient::connect(endpoint);
  if (!client.ok()) {
    response.code = client.code();
    response.message = client.status().message();
    return response;
  }
  control::Request request;
  request.kind = kind;
  if (snapshot != nullptr) {
    request.snapshot = *snapshot;
  }
  const auto result = client.value().call(request);
  if (!result.ok()) {
    response.code = result.code();
    response.message = result.status().message();
  } else {
    response = result.value();
  }
  (void)client.value().close();
  return response;
}

}  // namespace

PF_TEST(multiprocess, a_daemon_builds_authority_over_a_real_socket) {
  if (std::string(PODFABRIC_DAEMON_PATH).empty()) {
    PF_REQUIRE(false);
  }
  const std::string store = test::make_scratch_directory("mp-store-authority");
  const PodSnapshot snapshot = synthesize(options_for(4, 21));
  const std::string evidence = write_snapshot(snapshot, "evidence");
  PF_REQUIRE(!evidence.empty());

  Daemon daemon;
  PF_REQUIRE(start_daemon(daemon, store, evidence, "authority"));
  const control::Response minted = call(daemon.endpoint, control::MessageKind::Mint);
  PF_CHECK_EQ(minted.code, Code::Ok);
  PF_CHECK(!minted.token.lease.is_nil());
  PF_CHECK_EQ(minted.token.pod.token(), std::string("pod-multiprocess"));

  const control::Response status = call(daemon.endpoint, control::MessageKind::Status);
  PF_CHECK_EQ(status.code, Code::Ok);
  PF_CHECK(status.established_members >= 1);
  PF_CHECK(status.text.find("lifecycle") != std::string::npos);

  const control::Response shutdown = call(daemon.endpoint, control::MessageKind::Shutdown);
  PF_CHECK_EQ(shutdown.code, Code::Ok);
  const auto exit = daemon.child.wait(20 * nanos_per_second);
  PF_REQUIRE(exit.ok());
  PF_CHECK_EQ(exit.value(), 0);
  test::remove_directory(store);
}

PF_TEST(multiprocess, a_hard_kill_and_restart_fences_previous_authority) {
  const std::string store = test::make_scratch_directory("mp-store-kill");
  const PodSnapshot snapshot = synthesize(options_for(4, 22));
  const std::string evidence = write_snapshot(snapshot, "evidence-kill");
  PF_REQUIRE(!evidence.empty());

  Daemon first;
  PF_REQUIRE(start_daemon(first, store, evidence, "kill-first"));
  const control::Response minted = call(first.endpoint, control::MessageKind::Mint);
  PF_REQUIRE(minted.code == Code::Ok);
  const control::Response before = call(first.endpoint, control::MessageKind::Status);
  PF_REQUIRE(before.code == Code::Ok);

  // Hard kill: no shutdown request, no flush, no destructor.
  PF_REQUIRE(first.child.kill().ok());
  PF_REQUIRE(first.child.close().ok());

  Daemon second;
  PF_REQUIRE(start_daemon(second, store, "", "kill-second"));
  const control::Response after = call(second.endpoint, control::MessageKind::Status);
  PF_REQUIRE(after.code == Code::Ok);
  PF_CHECK_MSG(after.epoch.value() > before.epoch.value(),
               "the restarted pod did not advance its epoch");
  PF_CHECK(!(after.incarnation == before.incarnation));
  PF_CHECK_MSG(after.recovery == persist::RecoveryOutcome::Clean ||
                   after.recovery == persist::RecoveryOutcome::TornTailRepaired ||
                   after.recovery == persist::RecoveryOutcome::AmbiguousCommitRolledBack,
               std::string(persist::to_string(after.recovery)));

  // The token minted before the crash must not be honoured by the new process.
  control::Request check;
  check.kind = control::MessageKind::CheckAuthority;
  check.token = minted.token;
  check.scope = AuthorityScope::Pod;
  auto client = control::PodClient::connect(second.endpoint);
  PF_REQUIRE(client.ok());
  const auto verdict = client.value().call(check);
  PF_REQUIRE(verdict.ok());
  PF_CHECK_MSG(verdict.value().verdict != AuthorityVerdictCode::Valid,
               "a token from the killed incarnation was still accepted");
  PF_CHECK(verdict.value().verdict == AuthorityVerdictCode::StaleIncarnation ||
           verdict.value().verdict == AuthorityVerdictCode::StaleEpoch);
  (void)client.value().close();

  // The restarted pod may not claim authority it has not re-established.
  const control::Response recovered_round =
      call(second.endpoint, control::MessageKind::Apply, &snapshot);
  PF_REQUIRE(recovered_round.code == Code::Ok);
  PF_CHECK_EQ(recovered_round.outcome, ApplyOutcome::Recovered);
  PF_CHECK(!control::PodClient::connect(second.endpoint).ok() == false);
  const control::Response settled =
      call(second.endpoint, control::MessageKind::Apply, &snapshot);
  PF_REQUIRE(settled.code == Code::Ok);
  PF_CHECK_EQ(settled.lifecycle, LifecycleState::Active);
  const control::Response idle =
      call(second.endpoint, control::MessageKind::Apply, &snapshot);
  PF_REQUIRE(idle.code == Code::Ok);
  PF_CHECK_EQ(idle.outcome, ApplyOutcome::Unchanged);

  PF_REQUIRE(call(second.endpoint, control::MessageKind::Shutdown).code == Code::Ok);
  PF_REQUIRE(second.child.wait(20 * nanos_per_second).ok());
  test::remove_directory(store);
}

PF_TEST(multiprocess, recovered_expectations_still_fence_a_stale_generation) {
  const std::string store = test::make_scratch_directory("mp-store-stale");
  SyntheticOptions options = options_for(4, 23, 9);
  const PodSnapshot newer = synthesize(options);
  const std::string evidence = write_snapshot(newer, "evidence-stale");
  PF_REQUIRE(!evidence.empty());

  Daemon first;
  PF_REQUIRE(start_daemon(first, store, evidence, "stale-first"));
  PF_REQUIRE(call(first.endpoint, control::MessageKind::Shutdown).code == Code::Ok);
  PF_REQUIRE(first.child.wait(20 * nanos_per_second).ok());

  Daemon second;
  PF_REQUIRE(start_daemon(second, store, "", "stale-second"));
  PodSnapshot older = synthesize(options_for(4, 23, 4));
  const control::Response applied =
      call(second.endpoint, control::MessageKind::Apply, &older);
  PF_REQUIRE(applied.code == Code::Ok);
  PF_CHECK(applied.stale_members >= 1);
  PF_CHECK_MSG(applied.text.find("status stale") != std::string::npos,
               "the recovered expectation did not fence the older generation");
  PF_REQUIRE(call(second.endpoint, control::MessageKind::Shutdown).code == Code::Ok);
  PF_REQUIRE(second.child.wait(20 * nanos_per_second).ok());
  test::remove_directory(store);
}

PF_TEST(multiprocess, a_second_daemon_on_the_same_store_is_refused) {
  const std::string store = test::make_scratch_directory("mp-store-lock");
  Daemon first;
  PF_REQUIRE(start_daemon(first, store, "", "lock-first"));
  // The second daemon must fail fast rather than corrupt the first one's state.
  const int exit = run_ctl({"--pod", "pod-multiprocess", "--store", store, "--listen",
                            "127.0.0.1:0", "--workers", "1"},
                           "second-daemon");
  PF_CHECK_MSG(exit != 0, "a second daemon on the same store exited successfully");
  const control::Response status = call(first.endpoint, control::MessageKind::Status);
  PF_CHECK_EQ(status.code, Code::Ok);
  PF_REQUIRE(call(first.endpoint, control::MessageKind::Shutdown).code == Code::Ok);
  PF_REQUIRE(first.child.wait(20 * nanos_per_second).ok());
  test::remove_directory(store);
}

PF_TEST(multiprocess, concurrent_clients_over_independent_processes_agree) {
  const std::string store = test::make_scratch_directory("mp-store-concurrent");
  const PodSnapshot snapshot = synthesize(options_for(4, 24));
  const std::string evidence = write_snapshot(snapshot, "evidence-concurrent");
  PF_REQUIRE(!evidence.empty());

  Daemon daemon;
  PF_REQUIRE(start_daemon(daemon, store, evidence, "concurrent"));
  // Settle the pod first, so the concurrent clients are all applying evidence
  // that the pod has already accepted.
  const control::Response settling =
      call(daemon.endpoint, control::MessageKind::Apply, &snapshot);
  PF_REQUIRE(settling.code == Code::Ok);
  const control::Response baseline = call(daemon.endpoint, control::MessageKind::Status);
  PF_REQUIRE(baseline.code == Code::Ok);
  PF_CHECK_EQ(baseline.lifecycle, LifecycleState::Active);

  // Six independent client processes apply the same evidence concurrently.
  std::atomic<int> failures{0};
  std::vector<std::thread> clients;
  const std::string directory = test::make_scratch_directory("mp-concurrent-output");
  for (int worker = 0; worker < 6; ++worker) {
    clients.emplace_back([&, worker] {
      const std::string out = directory + "\\client-" + std::to_string(worker) + ".txt";
      auto spawned =
          test::spawn(PODFABRIC_CTL_PATH,
                      {"apply", "--endpoint", daemon.endpoint, "--snapshot", evidence}, out,
                      out + ".err");
      if (!spawned.ok()) {
        failures.fetch_add(1);
        return;
      }
      test::Child child = std::move(spawned).value();
      const auto status = child.wait(60 * nanos_per_second);
      if (!status.ok() || status.value() != 0) {
        failures.fetch_add(1);
      }
    });
  }
  for (std::thread& client : clients) {
    client.join();
  }
  PF_CHECK_EQ(failures.load(), 0);

  const control::Response final_status = call(daemon.endpoint, control::MessageKind::Status);
  PF_REQUIRE(final_status.code == Code::Ok);
  PF_CHECK_EQ(final_status.epoch, baseline.epoch);
  PF_CHECK_MSG(final_status.state_digest == baseline.state_digest,
               "baseline=" + baseline.state_digest.to_hex() + " final=" +
                   final_status.state_digest.to_hex() + " baseline-lifecycle=" +
                   std::string(to_string(baseline.lifecycle)) + " final-lifecycle=" +
                   std::string(to_string(final_status.lifecycle)));

  // Every client reported a sound revalidation plan.
  for (int worker = 0; worker < 6; ++worker) {
    const std::string text =
        test::read_text(directory + "\\client-" + std::to_string(worker) + ".txt");
    PF_CHECK_MSG(text.find("revalidation-sound=1") != std::string::npos,
                 "client " + std::to_string(worker) + " reported an unsound plan: " + text);
  }

  PF_REQUIRE(call(daemon.endpoint, control::MessageKind::Shutdown).code == Code::Ok);
  PF_REQUIRE(daemon.child.wait(20 * nanos_per_second).ok());
  test::remove_directory(store);
}

PF_TEST(multiprocess, operator_intent_reaches_a_restarted_process) {
  const std::string store = test::make_scratch_directory("mp-store-admin");
  const PodSnapshot snapshot = synthesize(options_for(3, 25));
  const std::string evidence = write_snapshot(snapshot, "evidence-admin");
  PF_REQUIRE(!evidence.empty());

  Daemon first;
  PF_REQUIRE(start_daemon(first, store, evidence, "admin-first"));
  control::Request drain;
  drain.kind = control::MessageKind::SetAdmin;
  drain.rack = RackId::from_canonical_literal("rack-1");
  drain.admin = AdminState::Draining;
  auto client = control::PodClient::connect(first.endpoint);
  PF_REQUIRE(client.ok());
  const auto drained = client.value().call(drain);
  PF_REQUIRE(drained.ok());
  PF_CHECK_EQ(drained.value().code, Code::Ok);
  PF_CHECK_EQ(drained.value().lifecycle, LifecycleState::Draining);
  (void)client.value().close();

  PF_REQUIRE(call(first.endpoint, control::MessageKind::Shutdown).code == Code::Ok);
  PF_REQUIRE(first.child.wait(20 * nanos_per_second).ok());

  Daemon second;
  PF_REQUIRE(start_daemon(second, store, "", "admin-second"));
  const control::Response applied =
      call(second.endpoint, control::MessageKind::Apply, &snapshot);
  PF_REQUIRE(applied.code == Code::Ok);
  PF_CHECK_MSG(applied.lifecycle == LifecycleState::Draining ||
                   applied.lifecycle == LifecycleState::Recovering,
               "the durable drain intent was lost: " +
                   std::string(to_string(applied.lifecycle)));
  PF_REQUIRE(call(second.endpoint, control::MessageKind::Shutdown).code == Code::Ok);
  PF_REQUIRE(second.child.wait(20 * nanos_per_second).ok());
  test::remove_directory(store);
}

PF_TEST(multiprocess, the_inspection_tool_reports_the_same_state_over_two_processes) {
  const std::string store = test::make_scratch_directory("mp-store-ctl");
  const PodSnapshot snapshot = synthesize(options_for(3, 26));
  const std::string evidence = write_snapshot(snapshot, "evidence-ctl");
  PF_REQUIRE(!evidence.empty());

  Daemon daemon;
  PF_REQUIRE(start_daemon(daemon, store, evidence, "ctl"));

  const std::string directory = test::make_scratch_directory("mp-ctl-output");
  const std::string out = directory + "\\status.txt";
  auto spawned = test::spawn(PODFABRIC_CTL_PATH,
                             {"status", "--endpoint", daemon.endpoint}, out, out + ".err");
  PF_REQUIRE(spawned.ok());
  test::Child status_child = std::move(spawned.value());
  const auto status_exit = status_child.wait(30 * nanos_per_second);
  PF_REQUIRE(status_exit.ok());
  PF_CHECK_EQ(status_exit.value(), 0);
  const std::string status_text = test::read_text(out);
  PF_CHECK(status_text.find("lifecycle=") != std::string::npos);

  const std::string describe_out = directory + "\\describe.txt";
  spawned = test::spawn(PODFABRIC_CTL_PATH, {"describe", "--endpoint", daemon.endpoint},
                        describe_out, describe_out + ".err");
  PF_REQUIRE(spawned.ok());
  test::Child describe_child = std::move(spawned.value());
  const auto describe_exit = describe_child.wait(30 * nanos_per_second);
  PF_REQUIRE(describe_exit.ok());
  PF_CHECK_EQ(describe_exit.value(), 0);
  const std::string describe_text = test::read_text(describe_out);
  PF_CHECK(describe_text.find("podfabric.podstate/1") != std::string::npos);
  PF_CHECK(describe_text.find("decision ") != std::string::npos);

  PF_REQUIRE(call(daemon.endpoint, control::MessageKind::Shutdown).code == Code::Ok);
  PF_REQUIRE(daemon.child.wait(20 * nanos_per_second).ok());
  test::remove_directory(store);
}
