// Completed-work benchmark for deterministic composition.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Reports wall-clock cost per composition for several pod sizes, together with
// the work actually completed (members, links, decisions) so the numbers can be
// checked rather than trusted.

#include <chrono>
#include <cstdio>
#include <cstdlib>

#include "podfabric/engine/composer.hpp"
#include "podfabric/model/synthetic.hpp"

namespace {

struct Result {
  std::size_t members{0};
  std::size_t links{0};
  std::size_t decisions{0};
  double microseconds{0};
};

Result measure(std::uint32_t racks, std::uint32_t domains, int iterations) {
  podfabric::SyntheticOptions options;
  options.pod = podfabric::PodId::from_canonical_literal("pod-benchmark");
  options.racks = racks;
  options.power_domains = domains;
  options.seed = 4242;
  options.observed_at = 1700000000LL * podfabric::nanos_per_second;
  // A ring keeps the fixture realistic for larger pods instead of quadratic.
  options.full_mesh = racks <= 8;

  const podfabric::PodSnapshot snapshot = podfabric::synthesize(options);
  podfabric::CompositionRequest request;
  request.snapshot = snapshot;
  request.context.expectations = podfabric::expectations_for(snapshot);
  request.context.epoch = podfabric::PodEpoch(1);

  const auto warmup = podfabric::compose(request);
  if (!warmup.ok()) {
    std::fprintf(stderr, "benchmark fixture is invalid: %s\n",
                 warmup.status().to_string().c_str());
    std::exit(1);
  }

  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < iterations; ++i) {
    const auto state = podfabric::compose(request);
    if (!state.ok()) {
      std::fprintf(stderr, "composition failed mid-benchmark\n");
      std::exit(1);
    }
  }
  const auto stop = std::chrono::steady_clock::now();

  Result result;
  result.members = snapshot.members.size();
  result.links = snapshot.links.size();
  result.decisions = warmup.value().decisions.size();
  const double total = std::chrono::duration<double, std::micro>(stop - start).count();
  result.microseconds = total / static_cast<double>(iterations);
  return result;
}

}  // namespace

int main(int argc, char** argv) {
  const int requested = argc > 1 ? std::atoi(argv[1]) : 200;
  const int rounds = requested <= 0 ? 200 : requested;
  std::printf("%8s %8s %8s %12s %14s %14s\n", "members", "links", "decisions", "us/compose",
              "members/ms", "decisions/ms");
  for (std::uint32_t racks : {1u, 4u, 8u, 16u, 64u, 256u, 1024u}) {
    const Result result = measure(racks, 4, rounds);
    const double per_millisecond = result.microseconds / 1000.0;
    std::printf("%8zu %8zu %8zu %12.2f %14.1f %14.1f\n", result.members, result.links,
                result.decisions, result.microseconds,
                static_cast<double>(result.members) / per_millisecond,
                static_cast<double>(result.decisions) / per_millisecond);
  }
  return 0;
}
