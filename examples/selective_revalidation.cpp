// Shows how a rack reincarnation invalidates only the decisions that depend on
// that rack, and how the runtime proves it.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdio>
#include <string>
#include <vector>

#include "podfabric/engine/composer.hpp"
#include "podfabric/engine/dependency.hpp"
#include "podfabric/model/synthetic.hpp"

namespace {

// The expectations are what the pod already recorded, not what the new
// evidence says. Supplying the new evidence's own expectations would hide the
// generation advance this example is about.
podfabric::CompositionRequest request_for(const podfabric::PodSnapshot& snapshot,
                                          const podfabric::PodSnapshot& recorded) {
  podfabric::CompositionRequest request;
  request.snapshot = snapshot;
  request.context.epoch = podfabric::PodEpoch(1);
  request.context.expectations = podfabric::expectations_for(recorded);
  return request;
}

podfabric::DecisionIndex index_of(const podfabric::PodState& state) {
  podfabric::DecisionIndex index;
  for (const podfabric::DecisionRecord& record : state.decisions) {
    index.publish(record);
  }
  return index;
}

}  // namespace

int main() {
  podfabric::SyntheticOptions options;
  options.pod = podfabric::PodId::from_canonical_literal("pod-example");
  options.racks = 6;
  options.power_domains = 3;
  options.observed_at = 1700000000LL * podfabric::nanos_per_second;

  podfabric::PodSnapshot before = podfabric::synthesize(options);
  const auto first = podfabric::compose(request_for(before, before));
  if (!first.ok()) {
    std::fprintf(stderr, "refused: %s\n", first.status().to_string().c_str());
    return 1;
  }
  const podfabric::DecisionIndex index = index_of(first.value());
  std::printf("decisions before: %zu\n", index.size());

  // Rack 3 is reincarnated: a new generation with a new descriptor digest, and
  // every link observed against the old incarnation is re-stamped too.
  const podfabric::RackId target = podfabric::RackId::from_canonical_literal("rack-3");
  podfabric::PodSnapshot after = before;
  for (podfabric::MemberRecord& record : after.members) {
    if (record.rack == target) {
      record.generation = podfabric::RackGeneration(record.generation.value() + 1);
      record.digest = podfabric::Digest::of("rack-3 reincarnated");
    }
  }
  for (podfabric::LinkRecord& link : after.links) {
    if (link.a.rack == target) {
      link.a_generation = podfabric::RackGeneration(link.a_generation.value() + 1);
    }
    if (link.b.rack == target) {
      link.b_generation = podfabric::RackGeneration(link.b_generation.value() + 1);
    }
  }
  podfabric::canonicalise(after);

  const auto second = podfabric::compose(request_for(after, before));
  if (!second.ok()) {
    std::fprintf(stderr, "refused: %s\n", second.status().to_string().c_str());
    return 1;
  }

  const std::vector<podfabric::DepKey> changed =
      podfabric::snapshot_change_keys(before, after);
  const podfabric::RevalidationReport report =
      podfabric::revalidate(index, second.value(), changed);
  std::printf("epoch            %llu -> %llu\n",
              static_cast<unsigned long long>(first.value().epoch.value()),
              static_cast<unsigned long long>(second.value().epoch.value()));
  std::printf("plan             %zu decision(s) must be recomputed\n", report.plan.size());
  std::printf("changed in plan  %zu\n", report.recomputed_changed.size());
  std::printf("reused unchanged %zu\n", report.recomputed_unchanged.size());
  std::printf("sound            %s\n", report.sound() ? "yes" : "NO - dependency gap");
  std::printf("%s\n", report.describe().c_str());
  for (const podfabric::DecisionId& id : report.recomputed_changed) {
    std::printf("  recomputed %s\n", id.token().c_str());
  }
  return report.sound() ? 0 : 1;
}
