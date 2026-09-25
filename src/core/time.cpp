// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "podfabric/core/time.hpp"

#include <chrono>

namespace podfabric {

Nanos system_now_nanos() noexcept {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
}

}  // namespace podfabric
