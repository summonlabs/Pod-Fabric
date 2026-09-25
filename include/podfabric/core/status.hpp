// Typed outcomes.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace podfabric {

// Outcome codes. These are deliberately kept distinct: absence of evidence
// (UNKNOWN), inapplicability (UNSUPPORTED), an aged-out but authentic record
// (STALE), two authentic records that disagree (CONFLICTING), missing required
// inputs (INCOMPLETE), inputs that cannot decide the question (INDETERMINATE),
// a policy denial (REFUSED), an aborted operation (CANCELLED), malformed input
// (INVALID) and a storage integrity failure (CORRUPT) are never collapsed.
enum class Code : std::uint8_t {
  Ok = 0,
  Unknown,
  Unsupported,
  Stale,
  Conflicting,
  Incomplete,
  Indeterminate,
  Refused,
  Cancelled,
  Invalid,
  NotFound,
  AlreadyExists,
  Exhausted,
  Corrupt,
  Unavailable,
  Internal,
};

std::string_view to_string(Code code) noexcept;
bool is_ok(Code code) noexcept;

// Total severity order used when several independent findings must be reduced
// to one reported code. Higher means "more disqualifying"; OK is zero.
int severity(Code code) noexcept;
// Returns whichever of the two codes is more severe.
Code worst(Code a, Code b) noexcept;

// A code plus a bounded human-readable explanation. Explanations are part of
// the observable contract: they are copied into decision records so that every
// verdict can be explained after the fact.
class Status {
 public:
  Status() noexcept = default;
  Status(Code code, std::string message) : code_(code), message_(std::move(message)) {}

  // Named success() rather than ok() because ok() is the observer.
  static Status success() { return Status(); }

  Code code() const noexcept { return code_; }
  const std::string& message() const noexcept { return message_; }

  bool ok() const noexcept { return code_ == Code::Ok; }
  explicit operator bool() const noexcept { return ok(); }

  std::string to_string() const;

 private:
  Code code_{Code::Ok};
  std::string message_{};
};

// Either a value or a Status. There is no "value plus error" state.
template <class T>
class Result {
 public:
  Result(T value) : storage_(std::move(value)) {}
  Result(Status status) : storage_(std::move(status)) {}

  bool ok() const noexcept { return storage_.index() == 0; }
  explicit operator bool() const noexcept { return ok(); }

  const Status& status() const noexcept {
    return ok() ? ok_status_ : std::get<Status>(storage_);
  }
  Code code() const noexcept { return status().code(); }

  T& value() & { return std::get<T>(storage_); }
  const T& value() const& { return std::get<T>(storage_); }
  T&& value() && { return std::get<T>(std::move(storage_)); }

  T* operator->() { return &std::get<T>(storage_); }
  const T* operator->() const { return &std::get<T>(storage_); }
  T& operator*() { return std::get<T>(storage_); }
  const T& operator*() const { return std::get<T>(storage_); }

 private:
  static inline const Status ok_status_{};
  std::variant<T, Status> storage_;
};

#define PODFABRIC_CONCAT_INNER_(a, b) a##b
#define PODFABRIC_CONCAT_(a, b) PODFABRIC_CONCAT_INNER_(a, b)

// Evaluates the expression, returns its Status on failure, and otherwise
// binds the value to a fresh name. The first argument is written exactly as it
// should appear before the initialiser, so it may be a declaration
// ("const std::uint64_t raw") or an assignment target ("record.field").
#define PODFABRIC_TRY_ASSIGN(decl, expression)                                 \
  auto PODFABRIC_CONCAT_(podfabric_try_tmp_, __LINE__) = (expression);         \
  if (!PODFABRIC_CONCAT_(podfabric_try_tmp_, __LINE__).ok()) {                 \
    return PODFABRIC_CONCAT_(podfabric_try_tmp_, __LINE__).status();           \
  }                                                                            \
  decl = std::move(PODFABRIC_CONCAT_(podfabric_try_tmp_, __LINE__)).value()

#define PODFABRIC_TRY(expression)                             \
  do {                                                        \
    auto&& podfabric_try_tmp_ = (expression);                 \
    if (!podfabric_try_tmp_.ok()) {                           \
      return podfabric_try_tmp_;                              \
    }                                                         \
  } while (false)

}  // namespace podfabric
