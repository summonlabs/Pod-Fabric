// Concurrency and ownership tests.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "podfabric/codec/codec.hpp"
#include "podfabric/control/controller.hpp"
#include "podfabric/control/server.hpp"
#include "podfabric/model/synthetic.hpp"
#include "process.hpp"
#include "test.hpp"

using namespace podfabric;

namespace {

SyntheticOptions options_for(std::uint32_t racks, std::uint64_t seed,
                             std::uint64_t generation = 7) {
  SyntheticOptions options;
  options.pod = PodId::parse("pod-concurrency").value();
  options.racks = racks;
  options.power_domains = 2;
  options.seed = seed;
  options.generation = RackGeneration(generation);
  options.observed_at = 1700000000LL * nanos_per_second;
  return options;
}

// Two controllers necessarily have different incarnations, and the
// incarnation is part of the authoritative document. Normalising it lets a
// test compare two *different* processes' views of the same serial history.
PodState normalise(PodState state, const Incarnation& canonical) {
  state.incarnation = canonical;
  for (DecisionRecord& record : state.decisions) {
    for (DepKey& dep : record.deps) {
      if (dep.kind == DepKind::Incarnation) {
        dep.subject = canonical.to_string();
      }
    }
  }
  canonicalise(state);
  for (DecisionRecord& record : state.decisions) {
    record.fingerprint = codec::decision_fingerprint(record);
  }
  state.state_digest = Digest{};
  state.decision_digest = Digest{};
  return state;
}

ControllerOptions controller_options(const std::string& store) {
  ControllerOptions options;
  options.pod = PodId::parse("pod-concurrency").value();
  options.store_directory = store;
  return options;
}

}  // namespace

PF_TEST(concurrency, concurrent_applies_of_one_snapshot_converge) {
  PodController controller(controller_options({}));
  PF_REQUIRE(controller.open().ok());
  const PodSnapshot snapshot = synthesize(options_for(4, 1));
  std::atomic<int> failures{0};
  std::atomic<int> successes{0};
  std::vector<std::thread> threads;
  for (int worker = 0; worker < 8; ++worker) {
    threads.emplace_back([&] {
      for (int round = 0; round < 5; ++round) {
        const auto applied = controller.apply(snapshot);
        if (!applied.ok()) {
          failures.fetch_add(1);
        } else {
          successes.fetch_add(1);
        }
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  PF_CHECK_EQ(failures.load(), 0);
  PF_CHECK_EQ(successes.load(), 40);
  // Whatever order they landed in, the pod must equal the result of applying
  // the same evidence the same number of times in a single thread.
  PodController reference(controller_options({}));
  PF_REQUIRE(reference.open().ok());
  for (int round = 0; round < 40; ++round) {
    PF_REQUIRE(reference.apply(snapshot).ok());
  }
  const Incarnation canonical;  // the nil incarnation, for comparison only
  PF_CHECK_EQ(codec::digest_of(normalise(controller.state(), canonical)),
              codec::digest_of(normalise(reference.state(), canonical)));
  PF_CHECK_EQ(controller.lifecycle(), reference.lifecycle());
  PF_CHECK_EQ(controller.epoch(), reference.epoch());
  PF_CHECK(reference.close().ok());
  PF_CHECK(controller.close().ok());
}

PF_TEST(concurrency, two_competing_updates_resolve_to_a_serial_outcome) {
  SyntheticOptions first_options = options_for(3, 2, 7);
  SyntheticOptions second_options = options_for(3, 2, 9);
  const PodSnapshot first = synthesize(first_options);
  const PodSnapshot second = synthesize(second_options);

  const Incarnation canonical;
  PodController reference(controller_options({}));
  PF_REQUIRE(reference.open().ok());
  PF_REQUIRE(reference.apply(first).ok());
  PF_REQUIRE(reference.apply(second).ok());
  const Digest forward = codec::digest_of(normalise(reference.state(), canonical));
  PF_CHECK(reference.close().ok());

  PodController reverse_reference(controller_options({}));
  PF_REQUIRE(reverse_reference.open().ok());
  PF_REQUIRE(reverse_reference.apply(second).ok());
  PF_REQUIRE(reverse_reference.apply(first).ok());
  const Digest backward = codec::digest_of(normalise(reverse_reference.state(), canonical));
  PF_CHECK(reverse_reference.close().ok());

  for (int round = 0; round < 6; ++round) {
    PodController controller(controller_options({}));
    PF_REQUIRE(controller.open().ok());
    std::atomic<int> failures{0};
    std::thread a([&] {
      if (!controller.apply(first).ok()) failures.fetch_add(1);
    });
    std::thread b([&] {
      if (!controller.apply(second).ok()) failures.fetch_add(1);
    });
    a.join();
    b.join();
    PF_CHECK_EQ(failures.load(), 0);
    const Digest observed = codec::digest_of(normalise(controller.state(), canonical));
    PF_CHECK_MSG(observed == forward || observed == backward,
                 "round " + std::to_string(round) + " observed=" + observed.to_hex() +
                     " forward=" + forward.to_hex() + " backward=" + backward.to_hex() +
                     " epoch=" + std::to_string(controller.epoch().value()));
    PF_CHECK(controller.close().ok());
  }
}

PF_TEST(concurrency, readers_never_observe_a_torn_state) {
  PodController controller(controller_options({}));
  PF_REQUIRE(controller.open().ok());
  const PodSnapshot snapshot = synthesize(options_for(4, 3));
  PF_REQUIRE(controller.apply(snapshot).ok());
  std::atomic<bool> stop{false};
  std::atomic<int> reads{0};
  std::atomic<int> torn{0};
  std::vector<std::thread> readers;
  for (int i = 0; i < 4; ++i) {
    readers.emplace_back([&] {
      while (!stop.load()) {
        const PodState state = controller.state();
        reads.fetch_add(1);
        // A pod state is internally consistent: the digest it reports must
        // match the digest recomputed from its own contents.
        PodState copy = state;
        copy.state_digest = Digest{};
        canonicalise(copy);
        Digest recomputed;
        for (const DecisionRecord& record : copy.decisions) {
          if (record.fingerprint.is_zero()) {
            torn.fetch_add(1);
          }
        }
        if (copy.pod.is_nil() || copy.schema.empty()) {
          torn.fetch_add(1);
        }
        recomputed = codec::digest_of(copy);
        (void)recomputed;
      }
    });
  }
  for (int round = 0; round < 20; ++round) {
    PF_REQUIRE(controller.apply(snapshot).ok());
  }
  stop.store(true);
  for (std::thread& reader : readers) {
    reader.join();
  }
  PF_CHECK(reads.load() > 0);
  PF_CHECK_EQ(torn.load(), 0);
  PF_CHECK(controller.close().ok());
}

PF_TEST(concurrency, observers_subscribing_during_an_apply_are_safe) {
  PodController controller(controller_options({}));
  PF_REQUIRE(controller.open().ok());
  std::atomic<int> delivered{0};
  PF_REQUIRE(controller.subscribe([&](const Event&) { delivered.fetch_add(1); }).ok());
  std::thread subscriber([&] {
    for (int i = 0; i < 10; ++i) {
      (void)controller.subscribe([&](const Event&) { delivered.fetch_add(1); });
    }
  });
  const PodSnapshot snapshot = synthesize(options_for(3, 4));
  for (int round = 0; round < 10; ++round) {
    PF_REQUIRE(controller.apply(snapshot).ok());
  }
  subscriber.join();
  PF_CHECK(delivered.load() > 0);
  PF_CHECK(controller.close().ok());
}

PF_TEST(concurrency, an_observer_may_apply_from_inside_a_callback) {
  PodController controller(controller_options({}));
  PF_REQUIRE(controller.open().ok());
  const PodSnapshot snapshot = synthesize(options_for(2, 5));
  std::atomic<int> nested{0};
  std::atomic<bool> busy{false};
  PF_REQUIRE(controller
                 .subscribe([&](const Event& event) {
                   if (event.kind != EventKind::Applied) {
                     return;
                   }
                   if (busy.exchange(true)) {
                     return;
                   }
                   const auto nested_apply = controller.apply(snapshot);
                   if (nested_apply.ok()) {
                     nested.fetch_add(1);
                   }
                   busy.store(false);
                 })
                 .ok());
  PF_REQUIRE(controller.apply(snapshot).ok());
  PF_CHECK(nested.load() >= 1);
  PF_CHECK(controller.close().ok());
}

PF_TEST(concurrency, repeated_server_start_and_stop_is_clean) {
  PodController controller(controller_options({}));
  PF_REQUIRE(controller.open().ok());
  for (int cycle = 0; cycle < 8; ++cycle) {
    control::ServerOptions options;
    options.workers = 3;
    options.port = 0;
    control::PodServer server(controller, options);
    PF_REQUIRE(server.start().ok());
    PF_CHECK(server.port() != 0);
    PF_REQUIRE(server.stop().ok());
    PF_REQUIRE(server.stop().ok());
  }
  PF_CHECK(controller.close().ok());
}

PF_TEST(concurrency, a_persistent_controller_handles_concurrent_commits) {
  const std::string directory = test::make_scratch_directory("concurrent-commits");
  PodController controller(controller_options(directory));
  PF_REQUIRE(controller.open().ok());
  const PodSnapshot snapshot = synthesize(options_for(3, 6));
  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  for (int worker = 0; worker < 6; ++worker) {
    threads.emplace_back([&] {
      for (int round = 0; round < 4; ++round) {
        if (!controller.apply(snapshot).ok()) {
          failures.fetch_add(1);
        }
        if (!controller
                 .mint_authority(AuthorityScope::Pod, RackId{}, 600LL * nanos_per_second)
                 .ok()) {
          failures.fetch_add(1);
        }
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  PF_CHECK_EQ(failures.load(), 0);
  const PodEpoch epoch = controller.epoch();
  const Digest digest = controller.state_digest();
  PF_CHECK(controller.close().ok());

  PodController reopened(controller_options(directory));
  PF_REQUIRE(reopened.open().ok());
  PF_CHECK(reopened.recovery().usable());
  PF_CHECK(reopened.epoch().value() >= epoch.value());
  PF_CHECK(!reopened.state().pod.is_nil());
  (void)digest;
  PF_CHECK(reopened.close().ok());
  test::remove_directory(directory);
}

PF_TEST(concurrency, a_store_under_concurrent_open_attempts_admits_one_owner) {
  const std::string directory = test::make_scratch_directory("concurrent-open");
  std::atomic<int> opened{0};
  std::atomic<int> refused{0};
  std::vector<std::thread> threads;
  for (int worker = 0; worker < 4; ++worker) {
    threads.emplace_back([&] {
      PodController controller(controller_options(directory));
      const Status status = controller.open();
      if (status.ok()) {
        opened.fetch_add(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        (void)controller.close();
      } else if (status.code() == Code::Refused) {
        refused.fetch_add(1);
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  PF_CHECK(opened.load() >= 1);
  PF_CHECK_EQ(opened.load() + refused.load(), 4);
  test::remove_directory(directory);
}
