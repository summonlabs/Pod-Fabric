// Deterministic pod-level composition.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "podfabric/core/status.hpp"
#include "podfabric/model/pod_state.hpp"
#include "podfabric/model/snapshot.hpp"

namespace podfabric {

// Composes the authoritative pod state from evidence and the pod's recorded
// expectations.
//
// The function is pure: identical requests produce byte-identical states, and
// no ordering of the input collections can change the result. Structural
// problems in the request are reported as a failing Status; conflicts inside
// otherwise well-formed evidence are reported as CONFLICTING verdicts inside a
// successful result, because a conflicted pod still has to say what it knows.
Result<PodState> compose(const CompositionRequest& request);

}  // namespace podfabric
