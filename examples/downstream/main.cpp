// An independent consumer of the installed PodFabric package.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Deliberately small: it includes the installed public headers, composes a pod,
// inspects the result and prints it. Everything it needs must come from the
// installed prefix.

#include <cstdio>
#include <string>

#include "podfabric/codec/codec.hpp"
#include "podfabric/control/controller.hpp"
#include "podfabric/engine/composer.hpp"
#include "podfabric/model/synthetic.hpp"
#include "podfabric/version.hpp"

#include "podfabric/control/client.hpp"

int main() {
  std::printf("PodFabric %s (downstream consumer)\n", std::string(podfabric::version_string).c_str());

  podfabric::SyntheticOptions options;
  options.pod = podfabric::PodId::from_canonical_literal("pod-downstream");
  options.racks = 4;
  options.power_domains = 2;
  options.observed_at = 1700000000LL * podfabric::nanos_per_second;

  const podfabric::PodSnapshot snapshot = podfabric::synthesize(options);
  podfabric::CompositionRequest request;
  request.snapshot = snapshot;
  request.context.expectations = podfabric::expectations_for(snapshot);
  request.context.epoch = podfabric::PodEpoch(1);

  const auto state = podfabric::compose(request);
  if (!state.ok()) {
    std::fprintf(stderr, "compose refused: %s\n", state.status().to_string().c_str());
    return 1;
  }
  std::printf("lifecycle=%s members=%zu decisions=%zu\n",
              std::string(podfabric::to_string(state.value().lifecycle)).c_str(),
              state.value().members.size(), state.value().decisions.size());

  // The controller and the wire client are part of the installed surface too.
  podfabric::ControllerOptions controller_options;
  controller_options.pod = podfabric::PodId::from_canonical_literal("pod-downstream");
  podfabric::PodController controller(controller_options);
  if (!controller.open().ok()) {
    std::fprintf(stderr, "controller did not open\n");
    return 1;
  }
  const auto applied = controller.apply(snapshot);
  if (!applied.ok()) {
    std::fprintf(stderr, "apply refused: %s\n", applied.status().to_string().c_str());
    return 1;
  }
  std::printf("apply outcome=%s epoch=%llu\n",
              std::string(podfabric::to_string(applied.value().outcome)).c_str(),
              static_cast<unsigned long long>(applied.value().state.epoch.value()));
  if (!controller.close().ok()) {
    std::fprintf(stderr, "controller did not close\n");
    return 1;
  }
  std::printf("downstream consumer completed\n");
  return 0;
}
