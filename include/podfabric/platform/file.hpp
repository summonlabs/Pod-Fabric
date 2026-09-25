// Minimal, explicit file primitives used by the durable store.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "podfabric/core/bytes.hpp"
#include "podfabric/core/status.hpp"

namespace podfabric::platform {

// An owned file handle. Reads and writes report failures instead of throwing,
// and every write path offers an explicit durability barrier.
class File {
 public:
  File() = default;
  File(const File&) = delete;
  File& operator=(const File&) = delete;
  File(File&& other) noexcept;
  File& operator=(File&& other) noexcept;
  ~File();

  static Result<File> open_read(const std::filesystem::path& path);
  static Result<File> open_append(const std::filesystem::path& path);
  static Result<File> open_write_truncate(const std::filesystem::path& path);

  bool valid() const noexcept { return handle_ != nullptr; }
  Status close();

  Result<std::uint64_t> size() const;
  Status seek(std::uint64_t offset);
  Status write(std::span<const std::byte> data);
  // Reads up to data.size() bytes; returns the number actually read.
  Result<std::size_t> read(std::span<std::byte> data);
  // Flushes user-space buffers to the operating system.
  Status flush();
  // Flushes and asks the operating system to persist to stable storage.
  Status sync();
  Status truncate(std::uint64_t size);

 private:
  friend class FileLock;
  explicit File(void* handle) noexcept : handle_(handle) {}
  void* handle_{nullptr};
};

bool path_exists(const std::filesystem::path& path);
Result<std::uint64_t> path_size(const std::filesystem::path& path);
Status ensure_directory(const std::filesystem::path& path);
Status remove_file(const std::filesystem::path& path);
// Replaces the destination atomically where the platform supports it.
Status atomic_replace(const std::filesystem::path& source, const std::filesystem::path& destination);
// Reads an entire file into memory, refusing files larger than max_bytes.
Result<std::vector<std::byte>> read_file(const std::filesystem::path& path, std::uint64_t max_bytes);
// Writes a whole file and syncs it.
Status write_file(const std::filesystem::path& path, std::span<const std::byte> data);
Status sync_directory(const std::filesystem::path& path);

// Advisory exclusive lock on a file, held for the lifetime of the object. A
// second holder is refused rather than queued, so two runtimes can never share
// a store directory.
class FileLock {
 public:
  FileLock() = default;
  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;
  FileLock(FileLock&& other) noexcept;
  FileLock& operator=(FileLock&& other) noexcept;
  ~FileLock();

  static Result<FileLock> acquire(const std::filesystem::path& path);

  bool held() const noexcept { return handle_ != nullptr; }

 private:
  explicit FileLock(void* handle) noexcept : handle_(handle) {}
  void release();
  void* handle_{nullptr};
  std::string key_{};
};

}  // namespace podfabric::platform
