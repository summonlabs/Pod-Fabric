// Child-process control for the multiprocess proofs.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "podfabric/core/status.hpp"
#include "podfabric/core/time.hpp"

namespace podfabric::test {

// A child process with its standard streams redirected to files, so the parent
// never depends on a pipe that the child could block on.
class Child {
 public:
  Child() = default;
  Child(const Child&) = delete;
  Child& operator=(const Child&) = delete;
  Child(Child&& other) noexcept;
  Child& operator=(Child&& other) noexcept;
  ~Child();

  bool running() const noexcept { return handle_ != nullptr; }
  std::uint32_t pid() const noexcept { return pid_; }

  // Waits up to timeout for the child to exit. A timeout is reported as
  // CANCELLED so the caller can decide; it is never treated as success.
  Result<int> wait(Nanos timeout);
  // Terminates the child without giving it a chance to clean up, which is how
  // the crash-recovery proofs reproduce a hard failure.
  Status kill();
  Status close();

 private:
  friend Result<Child> spawn(const std::string&, const std::vector<std::string>&,
                             const std::string&, const std::string&);
  void* handle_{nullptr};
  std::uint32_t pid_{0};
};

Result<Child> spawn(const std::string& program, const std::vector<std::string>& arguments,
                    const std::string& stdout_path, const std::string& stderr_path);

// Reads a whole file as text; returns an empty string when it does not exist.
std::string read_text(const std::string& path);

// Polls a file until it contains a line matching the needle, or the deadline
// expires. Returns false on timeout so the caller can fail the case.
bool wait_for_text(const std::string& path, const std::string& needle, Nanos timeout);

// Creates a unique scratch directory for one test case and removes any previous
// contents.
std::string make_scratch_directory(const std::string& label);
void remove_directory(const std::string& path);

}  // namespace podfabric::test
