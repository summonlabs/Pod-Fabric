// Decision dependency tracking and selective revalidation.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include "podfabric/model/member.hpp"
#include "podfabric/model/snapshot.hpp"
#include "podfabric/model/pod_state.hpp"

namespace podfabric {

// The set of decisions that must be recomputed after a dependency changed.
struct RevalidationPlan {
  std::vector<DecisionId> recompute{};
  // Decisions examined while closing the graph transitively.
  std::size_t closure_steps{0};

  bool contains(const DecisionId& id) const;
  std::size_t size() const noexcept { return recompute.size(); }
};

// Index over the decisions produced by the last composition. It answers one
// question: given a set of changed inputs, which decisions can possibly change?
class DecisionIndex {
 public:
  DecisionIndex() = default;

  void clear();
  void publish(const DecisionRecord& record);

  bool empty() const noexcept { return records_.empty(); }
  std::size_t size() const noexcept { return records_.size(); }

  const DecisionRecord* find(const DecisionId& id) const;
  const std::vector<DecisionId>* dependents_of(const DepKey& key) const;
  std::vector<DecisionId> all_ids() const;

  // Transitive closure over decision-to-decision edges. Terminates because the
  // visited set is monotone and the graph is finite.
  RevalidationPlan plan(const std::vector<DepKey>& changed) const;

 private:
  std::map<DepKey, std::vector<DecisionId>> edges_{};
  std::map<DecisionId, DecisionRecord> records_{};
};

// Outcome of applying a plan to a freshly composed state.
struct RevalidationReport {
  RevalidationPlan plan{};
  std::size_t total_decisions{0};
  std::vector<DecisionId> recomputed_changed{};
  std::vector<DecisionId> recomputed_unchanged{};
  std::vector<DecisionId> changed_outside_plan{};
  std::vector<DecisionId> added_outside_plan{};
  std::vector<DecisionId> removed_outside_plan{};

  // True when every decision that actually changed was predicted. A false
  // value means the dependency declarations are incomplete, which is an
  // internal defect rather than an input problem.
  bool sound() const noexcept {
    return changed_outside_plan.empty() && added_outside_plan.empty() &&
           removed_outside_plan.empty();
  }
  std::string describe() const;
};

// Compares the previously published decisions with a freshly composed state.
// The changed list holds the dependency keys known to have moved.
RevalidationReport revalidate(const DecisionIndex& before, const PodState& after,
                              const std::vector<DepKey>& changed);

// Convenience: every dependency key that differs between two expectation sets.
std::vector<DepKey> membership_change_keys(const std::vector<MemberExpectation>& before,
                                           const std::vector<MemberExpectation>& after);

// Every dependency key whose evidence differs between two snapshots. A change
// that is not reported here would be invisible to the revalidation plan, so the
// comparison covers members, links, routes, failure domains and obligations.
std::vector<DepKey> snapshot_change_keys(const PodSnapshot& before, const PodSnapshot& after);

// Every dependency key whose authority differs between two token sets.
std::vector<DepKey> token_change_keys(const std::vector<AuthorityToken>& before,
                                      const std::vector<LeaseId>& before_revoked,
                                      const std::vector<AuthorityToken>& after,
                                      const std::vector<LeaseId>& after_revoked);

}  // namespace podfabric
