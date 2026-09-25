// Composes a synthetic pod and prints what the runtime concluded.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdio>
#include <cstdlib>
#include <string>

#include "podfabric/codec/codec.hpp"
#include "podfabric/engine/composer.hpp"
#include "podfabric/model/synthetic.hpp"

int main(int argc, char** argv) {
  const std::uint32_t racks = argc > 1 ? static_cast<std::uint32_t>(std::atoi(argv[1])) : 6;
  podfabric::SyntheticOptions options;
  options.pod = podfabric::PodId::from_canonical_literal("pod-example");
  options.racks = racks == 0 ? 6 : racks;
  options.power_domains = 3;
  options.seed = 20260101;
  options.observed_at = 1700000000LL * podfabric::nanos_per_second;

  const podfabric::PodSnapshot snapshot = podfabric::synthesize(options);
  podfabric::CompositionRequest request;
  request.snapshot = snapshot;
  // The expectations a pod records after accepting this evidence.
  request.context.expectations = podfabric::expectations_for(snapshot);
  request.context.epoch = podfabric::PodEpoch(1);

  const auto state = podfabric::compose(request);
  if (!state.ok()) {
    std::fprintf(stderr, "composition refused: %s\n", state.status().to_string().c_str());
    return 1;
  }

  const podfabric::PodState& composed = state.value();
  std::printf("pod            %s\n", composed.pod.token().c_str());
  std::printf("epoch          %llu\n",
              static_cast<unsigned long long>(composed.epoch.value()));
  std::printf("lifecycle      %s\n", std::string(podfabric::to_string(composed.lifecycle)).c_str());
  std::printf("authority      %s\n",
              std::string(podfabric::to_string(composed.authority.status)).c_str());
  std::printf("members        %zu established=%u fenced=%u\n", composed.members.size(),
              composed.authority.established_members, composed.authority.fenced_members);
  for (const podfabric::CapacityAggregate& aggregate : composed.capacity) {
    std::printf("capacity       %-20s total=%llu reserved=%llu headroom=%llu %s\n",
                aggregate.resource.token().c_str(),
                static_cast<unsigned long long>(aggregate.total),
                static_cast<unsigned long long>(aggregate.reserved),
                static_cast<unsigned long long>(aggregate.headroom),
                std::string(podfabric::to_string(aggregate.status)).c_str());
  }
  for (const podfabric::ObligationVerdict& verdict : composed.obligations) {
    std::printf("obligation     %-32s %s\n", verdict.id.token().c_str(),
                std::string(podfabric::to_string(verdict.status)).c_str());
  }
  std::printf("decisions      %zu\n", composed.decisions.size());
  std::printf("state digest   %s\n", composed.state_digest.to_hex().c_str());

  if (argc > 2 && std::string(argv[2]) == "--text") {
    const auto text = podfabric::codec::to_text(composed);
    if (text.ok()) {
      std::fwrite(text.value().data(), 1, text.value().size(), stdout);
    }
  }
  return 0;
}
