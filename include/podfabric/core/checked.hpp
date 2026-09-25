// Checked arithmetic helpers.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <limits>
#include <type_traits>

#include "podfabric/core/status.hpp"

namespace podfabric {

// All arithmetic on externally supplied magnitudes goes through these helpers.
// Overflow and underflow are reported as INVALID (malformed input) rather than
// wrapping, and saturation is never used where it could hide a defect.

template <class T>
  requires std::is_integral_v<T>
Result<T> checked_add(T a, T b) {
  if constexpr (std::is_unsigned_v<T>) {
    if (b > static_cast<T>(std::numeric_limits<T>::max() - a)) {
      return Status(Code::Invalid, "checked_add: unsigned overflow");
    }
  } else {
    if (b > 0 && a > static_cast<T>(std::numeric_limits<T>::max() - b)) {
      return Status(Code::Invalid, "checked_add: signed overflow");
    }
    if (b < 0 && a < static_cast<T>(std::numeric_limits<T>::min() - b)) {
      return Status(Code::Invalid, "checked_add: signed underflow");
    }
  }
  return static_cast<T>(a + b);
}

template <class T>
  requires std::is_integral_v<T>
Result<T> checked_sub(T a, T b) {
  if constexpr (std::is_unsigned_v<T>) {
    if (b > a) {
      return Status(Code::Invalid, "checked_sub: unsigned underflow");
    }
  } else {
    if (b < 0 && a > static_cast<T>(std::numeric_limits<T>::max() + b)) {
      return Status(Code::Invalid, "checked_sub: signed overflow");
    }
    if (b > 0 && a < static_cast<T>(std::numeric_limits<T>::min() + b)) {
      return Status(Code::Invalid, "checked_sub: signed underflow");
    }
  }
  return static_cast<T>(a - b);
}

template <class T>
  requires std::is_integral_v<T>
Result<T> checked_mul(T a, T b) {
  if (a == 0 || b == 0) {
    return static_cast<T>(0);
  }
  if constexpr (std::is_unsigned_v<T>) {
    if (a > static_cast<T>(std::numeric_limits<T>::max() / b)) {
      return Status(Code::Invalid, "checked_mul: unsigned overflow");
    }
  } else {
    const T limit = std::numeric_limits<T>::max();
    if (a > 0) {
      if (b > 0) {
        if (a > limit / b) return Status(Code::Invalid, "checked_mul: signed overflow");
      } else {
        if (b < std::numeric_limits<T>::min() / a) {
          return Status(Code::Invalid, "checked_mul: signed overflow");
        }
      }
    } else {
      if (b > 0) {
        if (a < std::numeric_limits<T>::min() / b) {
          return Status(Code::Invalid, "checked_mul: signed overflow");
        }
      } else {
        if (a != 0 && b < limit / a) {
          return Status(Code::Invalid, "checked_mul: signed overflow");
        }
      }
    }
  }
  return static_cast<T>(a * b);
}

// Narrowing conversion that rejects out-of-range values instead of truncating.
template <class To, class From>
  requires std::is_integral_v<To> && std::is_integral_v<From>
Result<To> checked_narrow(From value) {
  if constexpr (std::is_signed_v<From> == std::is_signed_v<To>) {
    if (value < static_cast<From>(std::numeric_limits<To>::min()) ||
        value > static_cast<From>(std::numeric_limits<To>::max())) {
      return Status(Code::Invalid, "checked_narrow: value out of range");
    }
  } else if constexpr (std::is_signed_v<From>) {
    if (value < 0 || static_cast<std::make_unsigned_t<From>>(value) >
                         static_cast<std::make_unsigned_t<From>>(std::numeric_limits<To>::max())) {
      return Status(Code::Invalid, "checked_narrow: value out of range");
    }
  } else {
    if (value > static_cast<From>(std::numeric_limits<To>::max())) {
      return Status(Code::Invalid, "checked_narrow: value out of range");
    }
  }
  return static_cast<To>(value);
}

}  // namespace podfabric
