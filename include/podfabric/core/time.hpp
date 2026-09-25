// Clocks and timestamps.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <cstdint>

namespace podfabric {

// Nanoseconds on the coordinator's wall clock. Evidence ages are computed
// against these values; a clock that moves backwards is treated as INVALID
// evidence rather than as an excuse to extend a lease.
using Nanos = std::int64_t;

inline constexpr Nanos nanos_per_second = 1000000000LL;
inline constexpr Nanos nanos_per_millisecond = 1000000LL;

class Clock {
 public:
  Clock() = default;
  Clock(const Clock&) = delete;
  Clock& operator=(const Clock&) = delete;
  virtual ~Clock() = default;
  virtual Nanos now() const = 0;
};

// Wall-clock reading. Not monotonic by construction, which is why every
// consumer compares readings for sanity instead of trusting ordering.
Nanos system_now_nanos() noexcept;

class SystemClock final : public Clock {
 public:
  Nanos now() const override { return system_now_nanos(); }
};

// Deterministic clock for tests and for replay of recorded evidence.
class ManualClock final : public Clock {
 public:
  explicit ManualClock(Nanos start = 1700000000LL * nanos_per_second) : now_(start) {}
  Nanos now() const override { return now_.load(std::memory_order_relaxed); }
  void advance(Nanos delta) { now_.fetch_add(delta, std::memory_order_relaxed); }
  void set(Nanos value) { now_.store(value, std::memory_order_relaxed); }

 private:
  std::atomic<Nanos> now_;
};

}  // namespace podfabric
