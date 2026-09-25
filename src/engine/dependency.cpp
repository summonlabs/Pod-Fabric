// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "podfabric/engine/dependency.hpp"

#include <algorithm>
#include <deque>
#include <set>

namespace podfabric {
namespace {

std::string describe_ids(const std::vector<DecisionId>& ids) {
  std::string out;
  for (std::size_t i = 0; i < ids.size() && i < 8; ++i) {
    if (i > 0) out.append(", ");
    out.append(ids[i].token());
  }
  if (ids.size() > 8) {
    out.append(", ...");
  }
  return out;
}

}  // namespace

bool RevalidationPlan::contains(const DecisionId& id) const {
  return std::binary_search(recompute.begin(), recompute.end(), id);
}

void DecisionIndex::clear() {
  edges_.clear();
  records_.clear();
}

void DecisionIndex::publish(const DecisionRecord& record) {
  DecisionRecord canonical = record;
  std::sort(canonical.deps.begin(), canonical.deps.end());
  canonical.deps.erase(std::unique(canonical.deps.begin(), canonical.deps.end()),
                       canonical.deps.end());
  records_[record.id] = canonical;
  for (const DepKey& dep : canonical.deps) {
    auto& bucket = edges_[dep];
    if (std::find(bucket.begin(), bucket.end(), record.id) == bucket.end()) {
      bucket.push_back(record.id);
      std::sort(bucket.begin(), bucket.end());
    }
  }
}

const DecisionRecord* DecisionIndex::find(const DecisionId& id) const {
  const auto it = records_.find(id);
  return it == records_.end() ? nullptr : &it->second;
}

const std::vector<DecisionId>* DecisionIndex::dependents_of(const DepKey& key) const {
  const auto it = edges_.find(key);
  return it == edges_.end() ? nullptr : &it->second;
}

std::vector<DecisionId> DecisionIndex::all_ids() const {
  std::vector<DecisionId> ids;
  ids.reserve(records_.size());
  for (const auto& entry : records_) {
    ids.push_back(entry.first);
  }
  return ids;
}

RevalidationPlan DecisionIndex::plan(const std::vector<DepKey>& changed) const {
  RevalidationPlan result;
  std::set<DecisionId> visited;
  std::deque<DecisionId> queue;

  for (const DepKey& key : changed) {
    const auto it = edges_.find(key);
    if (it == edges_.end()) {
      continue;
    }
    for (const DecisionId& id : it->second) {
      if (visited.insert(id).second) {
        queue.push_back(id);
      }
    }
  }

  while (!queue.empty()) {
    const DecisionId current = queue.front();
    queue.pop_front();
    ++result.closure_steps;
    // Decisions that depend on this decision are found through the same edge
    // map, using a decision-typed key.
    const DepKey key{DepKind::Decision, current.token()};
    const auto it = edges_.find(key);
    if (it == edges_.end()) {
      continue;
    }
    for (const DecisionId& id : it->second) {
      if (visited.insert(id).second) {
        queue.push_back(id);
      }
    }
  }

  result.recompute.assign(visited.begin(), visited.end());
  std::sort(result.recompute.begin(), result.recompute.end());
  return result;
}

std::string RevalidationReport::describe() const {
  std::string out;
  out.append("plan=");
  out.append(std::to_string(plan.size()));
  out.append(" total=");
  out.append(std::to_string(total_decisions));
  out.append(" changed-in-plan=");
  out.append(std::to_string(recomputed_changed.size()));
  out.append(" unchanged-in-plan=");
  out.append(std::to_string(recomputed_unchanged.size()));
  if (!changed_outside_plan.empty()) {
    out.append(" UNPREDICTED-CHANGE=[");
    out.append(describe_ids(changed_outside_plan));
    out.append("]");
  }
  if (!added_outside_plan.empty()) {
    out.append(" UNPREDICTED-ADD=[");
    out.append(describe_ids(added_outside_plan));
    out.append("]");
  }
  if (!removed_outside_plan.empty()) {
    out.append(" UNPREDICTED-REMOVE=[");
    out.append(describe_ids(removed_outside_plan));
    out.append("]");
  }
  return out;
}

RevalidationReport revalidate(const DecisionIndex& before, const PodState& after,
                              const std::vector<DepKey>& changed) {
  RevalidationReport report;
  if (before.empty()) {
    // Nothing was published before, so every decision is new by construction
    // and the plan is the whole document.
    report.plan.recompute.reserve(after.decisions.size());
    for (const DecisionRecord& record : after.decisions) {
      report.plan.recompute.push_back(record.id);
    }
    std::sort(report.plan.recompute.begin(), report.plan.recompute.end());
  } else {
    report.plan = before.plan(changed);
  }
  report.total_decisions = after.decisions.size();

  std::map<DecisionId, const DecisionRecord*> now;
  for (const DecisionRecord& record : after.decisions) {
    now[record.id] = &record;
  }

  for (const DecisionRecord& record : after.decisions) {
    const DecisionRecord* previous = before.find(record.id);
    if (previous == nullptr) {
      if (!report.plan.contains(record.id)) {
        report.added_outside_plan.push_back(record.id);
      }
      continue;
    }
    const bool in_plan = report.plan.contains(record.id);
    if (previous->fingerprint == record.fingerprint) {
      if (in_plan) {
        report.recomputed_unchanged.push_back(record.id);
      }
    } else if (in_plan) {
      report.recomputed_changed.push_back(record.id);
    } else {
      report.changed_outside_plan.push_back(record.id);
    }
  }

  for (const DecisionId& id : before.all_ids()) {
    if (now.find(id) == now.end() && !report.plan.contains(id)) {
      report.removed_outside_plan.push_back(id);
    }
  }

  std::sort(report.recomputed_changed.begin(), report.recomputed_changed.end());
  std::sort(report.recomputed_unchanged.begin(), report.recomputed_unchanged.end());
  std::sort(report.changed_outside_plan.begin(), report.changed_outside_plan.end());
  std::sort(report.added_outside_plan.begin(), report.added_outside_plan.end());
  std::sort(report.removed_outside_plan.begin(), report.removed_outside_plan.end());
  return report;
}

namespace {

// Diffs two identity-indexed collections and reports the keys whose value moved
// or whose presence changed.
template <class Item, class Key, class Identity>
std::vector<DepKey> diff_collection(const std::vector<Item>& before,
                                    const std::vector<Item>& after, DepKind kind,
                                    Identity identity) {
  std::map<Key, const Item*> left;
  std::map<Key, const Item*> right;
  for (const Item& item : before) left[identity(item)] = &item;
  for (const Item& item : after) right[identity(item)] = &item;
  std::set<Key> keys;
  for (const auto& entry : left) keys.insert(entry.first);
  for (const auto& entry : right) keys.insert(entry.first);
  std::vector<DepKey> changed;
  for (const Key& key : keys) {
    const auto l = left.find(key);
    const auto r = right.find(key);
    const bool same = (l == left.end()) == (r == right.end()) &&
                      (l == left.end() || *l->second == *r->second);
    if (!same) {
      changed.push_back(DepKey{kind, key.token()});
    }
  }
  return changed;
}

}  // namespace

std::vector<DepKey> snapshot_change_keys(const PodSnapshot& before, const PodSnapshot& after) {
  std::vector<DepKey> changed;
  auto append = [&changed](std::vector<DepKey> more) {
    changed.insert(changed.end(), more.begin(), more.end());
  };
  append(diff_collection<MemberRecord, RackId>(
      before.members, after.members, DepKind::Member,
      [](const MemberRecord& m) { return m.rack; }));
  append(diff_collection<LinkRecord, LinkId>(
      before.links, after.links, DepKind::Link, [](const LinkRecord& l) { return l.id; }));
  append(diff_collection<RouteEvidence, RouteRef>(
      before.routes, after.routes, DepKind::Route, [](const RouteEvidence& r) { return r.ref; }));
  append(diff_collection<FailureDomainRecord, DomainId>(
      before.domains, after.domains, DepKind::Domain,
      [](const FailureDomainRecord& d) { return d.id; }));
  append(diff_collection<Obligation, ObligationId>(
      before.obligations, after.obligations, DepKind::Obligation,
      [](const Obligation& o) { return o.id; }));
  std::sort(changed.begin(), changed.end());
  changed.erase(std::unique(changed.begin(), changed.end()), changed.end());
  return changed;
}

std::vector<DepKey> token_change_keys(const std::vector<AuthorityToken>& before,
                                      const std::vector<LeaseId>& before_revoked,
                                      const std::vector<AuthorityToken>& after,
                                      const std::vector<LeaseId>& after_revoked) {
  std::map<LeaseId, const AuthorityToken*> left;
  std::map<LeaseId, const AuthorityToken*> right;
  for (const AuthorityToken& token : before) left[token.lease] = &token;
  for (const AuthorityToken& token : after) right[token.lease] = &token;
  std::set<LeaseId> leases;
  for (const auto& entry : left) leases.insert(entry.first);
  for (const auto& entry : right) leases.insert(entry.first);
  for (const LeaseId& lease : before_revoked) leases.insert(lease);
  for (const LeaseId& lease : after_revoked) leases.insert(lease);

  auto revoked = [](const std::vector<LeaseId>& list, const LeaseId& lease) {
    return std::find(list.begin(), list.end(), lease) != list.end();
  };
  std::vector<DepKey> changed;
  for (const LeaseId& lease : leases) {
    const auto l = left.find(lease);
    const auto r = right.find(lease);
    // Both iterators must be dereferenceable before their values can be
    // compared; a lease present on only one side is a change by itself.
    const bool same_value = (l == left.end() && r == right.end()) ||
                            (l != left.end() && r != right.end() && *l->second == *r->second);
    const bool same_revocation = revoked(before_revoked, lease) == revoked(after_revoked, lease);
    const bool same_presence = (l == left.end()) == (r == right.end());
    if (!same_presence || !same_value || !same_revocation) {
      changed.push_back(DepKey{DepKind::Token, lease.token()});
    }
  }
  return changed;
}

std::vector<DepKey> membership_change_keys(const std::vector<MemberExpectation>& before,
                                           const std::vector<MemberExpectation>& after) {
  std::map<RackId, MemberExpectation> left;
  std::map<RackId, MemberExpectation> right;
  for (const auto& entry : before) left[entry.rack] = entry;
  for (const auto& entry : after) right[entry.rack] = entry;

  std::set<RackId> racks;
  for (const auto& entry : left) racks.insert(entry.first);
  for (const auto& entry : right) racks.insert(entry.first);

  std::vector<DepKey> keys;
  for (const RackId& rack : racks) {
    const auto l = left.find(rack);
    const auto r = right.find(rack);
    const bool present_before = l != left.end();
    const bool present_after = r != right.end();
    const bool same = present_before == present_after &&
                      (!present_before ||
                       (l->second.generation == r->second.generation &&
                        l->second.digest == r->second.digest &&
                        l->second.membership == r->second.membership));
    if (!same) {
      keys.push_back(DepKey{DepKind::Member, rack.token()});
    }
  }
  return keys;
}

}  // namespace podfabric
