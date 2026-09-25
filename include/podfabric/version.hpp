// Pod Fabric — generation-bound pod-level runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string_view>

namespace podfabric {

inline constexpr int version_major = 1;
inline constexpr int version_minor = 0;
inline constexpr int version_patch = 0;

inline constexpr std::string_view version_string = "1.0.0";

// ABI/behaviour revision of the composed pod state document.
inline constexpr std::string_view pod_state_schema = "podfabric.podstate/1";
// Wire/in-memory revision of a composition input snapshot.
inline constexpr std::string_view snapshot_schema = "podfabric.snapshot/1";
// Revision of the human-readable snapshot exchange format.
inline constexpr std::string_view text_schema = "podfabric.text/1";
// Rack Network Fabric descriptor schema revisions this build understands.
// Anything else is reported as UNSUPPORTED, never silently accepted.
inline constexpr std::string_view rnf_schema_v1 = "rnf.descriptor/1";

}  // namespace podfabric
