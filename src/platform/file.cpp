// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "podfabric/platform/file.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <system_error>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace podfabric::platform {
namespace {

std::string describe_errno(int code) {
  return "operating-system error " + std::to_string(code);
}

// Byte-range locks are owned by the process on some platforms, so a second
// store in the same process would otherwise be admitted. The registry makes
// ownership exclusive per process as well as across processes.
std::mutex& lock_registry_mutex() {
  static std::mutex mutex;
  return mutex;
}

std::set<std::string>& lock_registry() {
  static std::set<std::string> registry;
  return registry;
}

std::string lock_key(const std::filesystem::path& path) {
  std::error_code ec;
  const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
  return ec ? path.string() : absolute.lexically_normal().string();
}

}  // namespace

File::File(File&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }

File& File::operator=(File&& other) noexcept {
  if (this != &other) {
    (void)close();
    handle_ = other.handle_;
    other.handle_ = nullptr;
  }
  return *this;
}

File::~File() { (void)close(); }

Result<File> File::open_read(const std::filesystem::path& path) {
#if defined(_WIN32)
  std::FILE* handle = nullptr;
  if (::_wfopen_s(&handle, path.wstring().c_str(), L"rb") != 0 || handle == nullptr) {
    return Status(Code::Unavailable, "cannot open file for reading: " + path.string());
  }
#else
  std::FILE* handle = std::fopen(path.string().c_str(), "rb");
  if (handle == nullptr) {
    return Status(Code::Unavailable, "cannot open file for reading: " + path.string());
  }
#endif
  return File(handle);
}

Result<File> File::open_append(const std::filesystem::path& path) {
#if defined(_WIN32)
  std::FILE* handle = nullptr;
  if (::_wfopen_s(&handle, path.wstring().c_str(), L"ab+") != 0 || handle == nullptr) {
    return Status(Code::Unavailable, "cannot open file for appending: " + path.string());
  }
#else
  std::FILE* handle = std::fopen(path.string().c_str(), "ab+");
  if (handle == nullptr) {
    return Status(Code::Unavailable, "cannot open file for appending: " + path.string());
  }
#endif
  return File(handle);
}

Result<File> File::open_write_truncate(const std::filesystem::path& path) {
#if defined(_WIN32)
  std::FILE* handle = nullptr;
  if (::_wfopen_s(&handle, path.wstring().c_str(), L"wb+") != 0 || handle == nullptr) {
    return Status(Code::Unavailable, "cannot open file for writing: " + path.string());
  }
#else
  std::FILE* handle = std::fopen(path.string().c_str(), "wb+");
  if (handle == nullptr) {
    return Status(Code::Unavailable, "cannot open file for writing: " + path.string());
  }
#endif
  return File(handle);
}

Status File::close() {
  if (handle_ == nullptr) {
    return Status::success();
  }
  std::FILE* handle = static_cast<std::FILE*>(handle_);
  handle_ = nullptr;
  if (std::fclose(handle) != 0) {
    return Status(Code::Unavailable, "failed to close file");
  }
  return Status::success();
}

Result<std::uint64_t> File::size() const {
  if (handle_ == nullptr) {
    return Status(Code::Invalid, "file handle is not open");
  }
  std::FILE* handle = static_cast<std::FILE*>(handle_);
  const long current = std::ftell(handle);
  if (current < 0) {
    return Status(Code::Unavailable, "failed to query file position");
  }
  if (std::fseek(handle, 0, SEEK_END) != 0) {
    return Status(Code::Unavailable, "failed to seek to the end of the file");
  }
  const long end = std::ftell(handle);
  if (std::fseek(handle, current, SEEK_SET) != 0) {
    return Status(Code::Unavailable, "failed to restore the file position");
  }
  if (end < 0) {
    return Status(Code::Unavailable, "failed to query file size");
  }
  return static_cast<std::uint64_t>(end);
}

Status File::seek(std::uint64_t offset) {
  if (handle_ == nullptr) {
    return Status(Code::Invalid, "file handle is not open");
  }
  if (offset > 0x7FFFFFFFFFFFFFFFull) {
    return Status(Code::Invalid, "seek offset is out of range");
  }
  if (std::fseek(static_cast<std::FILE*>(handle_), static_cast<long>(offset), SEEK_SET) != 0) {
    return Status(Code::Unavailable, "failed to seek");
  }
  return Status::success();
}

Status File::write(std::span<const std::byte> data) {
  if (handle_ == nullptr) {
    return Status(Code::Invalid, "file handle is not open");
  }
  if (data.empty()) {
    return Status::success();
  }
  const std::size_t written =
      std::fwrite(data.data(), 1, data.size(), static_cast<std::FILE*>(handle_));
  if (written != data.size()) {
    return Status(Code::Unavailable, "short write to file");
  }
  return Status::success();
}

Result<std::size_t> File::read(std::span<std::byte> data) {
  if (handle_ == nullptr) {
    return Status(Code::Invalid, "file handle is not open");
  }
  if (data.empty()) {
    return static_cast<std::size_t>(0);
  }
  const std::size_t got = std::fread(data.data(), 1, data.size(), static_cast<std::FILE*>(handle_));
  if (got < data.size() && std::ferror(static_cast<std::FILE*>(handle_)) != 0) {
    return Status(Code::Unavailable, "read failure");
  }
  return got;
}

Status File::flush() {
  if (handle_ == nullptr) {
    return Status(Code::Invalid, "file handle is not open");
  }
  if (std::fflush(static_cast<std::FILE*>(handle_)) != 0) {
    return Status(Code::Unavailable, "flush failed");
  }
  return Status::success();
}

Status File::sync() {
  PODFABRIC_TRY(flush());
  std::FILE* handle = static_cast<std::FILE*>(handle_);
#if defined(_WIN32)
  if (::_commit(::_fileno(handle)) != 0) {
    return Status(Code::Unavailable, describe_errno(errno));
  }
#else
  if (::fsync(::fileno(handle)) != 0) {
    return Status(Code::Unavailable, describe_errno(errno));
  }
#endif
  return Status::success();
}

Status File::truncate(std::uint64_t new_size) {
  if (handle_ == nullptr) {
    return Status(Code::Invalid, "file handle is not open");
  }
  PODFABRIC_TRY(flush());
  std::FILE* handle = static_cast<std::FILE*>(handle_);
  if (new_size > 0x7FFFFFFFFFFFFFFFull) {
    return Status(Code::Invalid, "truncate size is out of range");
  }
#if defined(_WIN32)
  if (::_chsize_s(::_fileno(handle), static_cast<long long>(new_size)) != 0) {
    return Status(Code::Unavailable, "failed to truncate journal");
  }
#else
  if (::ftruncate(::fileno(handle), static_cast<off_t>(new_size)) != 0) {
    return Status(Code::Unavailable, describe_errno(errno));
  }
#endif
  return Status::success();
}

bool path_exists(const std::filesystem::path& path) {
  std::error_code ec;
  return std::filesystem::exists(path, ec) && !ec;
}

Result<std::uint64_t> path_size(const std::filesystem::path& path) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec) {
    return Status(Code::Unavailable, "cannot determine file size: " + path.string());
  }
  return static_cast<std::uint64_t>(size);
}

Status ensure_directory(const std::filesystem::path& path) {
  std::error_code ec;
  if (std::filesystem::exists(path, ec)) {
    if (!std::filesystem::is_directory(path, ec)) {
      return Status(Code::Invalid, "path exists and is not a directory: " + path.string());
    }
    return Status::success();
  }
  if (!std::filesystem::create_directories(path, ec) || ec) {
    return Status(Code::Unavailable, "cannot create directory: " + path.string());
  }
  return Status::success();
}

Status remove_file(const std::filesystem::path& path) {
  std::error_code ec;
  std::filesystem::remove(path, ec);
  if (ec) {
    return Status(Code::Unavailable, "cannot remove file: " + path.string());
  }
  return Status::success();
}

Status atomic_replace(const std::filesystem::path& source,
                      const std::filesystem::path& destination) {
#if defined(_WIN32)
  if (!::MoveFileExW(source.wstring().c_str(), destination.wstring().c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    return Status(Code::Unavailable,
                  "atomic replace failed with error " + std::to_string(::GetLastError()));
  }
  return Status::success();
#else
  std::error_code ec;
  std::filesystem::rename(source, destination, ec);
  if (ec) {
    return Status(Code::Unavailable, "atomic replace failed: " + ec.message());
  }
  return Status::success();
#endif
}

Result<std::vector<std::byte>> read_file(const std::filesystem::path& path,
                                         std::uint64_t max_bytes) {
  if (!path_exists(path)) {
    return Status(Code::NotFound, "file does not exist: " + path.string());
  }
  PODFABRIC_TRY_ASSIGN(const std::uint64_t size, path_size(path));
  if (size > max_bytes) {
    return Status(Code::Exhausted, "file exceeds the configured maximum size");
  }
  PODFABRIC_TRY_ASSIGN(File file, File::open_read(path));
  std::vector<std::byte> buffer(static_cast<std::size_t>(size));
  std::size_t offset = 0;
  while (offset < buffer.size()) {
    PODFABRIC_TRY_ASSIGN(const std::size_t got,
                         file.read(std::span<std::byte>(buffer.data() + offset,
                                                        buffer.size() - offset)));
    if (got == 0) {
      return Status(Code::Incomplete, "file ended before the declared size");
    }
    offset += got;
  }
  PODFABRIC_TRY(file.close());
  return buffer;
}

Status write_file(const std::filesystem::path& path, std::span<const std::byte> data) {
  PODFABRIC_TRY_ASSIGN(File file, File::open_write_truncate(path));
  PODFABRIC_TRY(file.write(data));
  PODFABRIC_TRY(file.sync());
  return file.close();
}

Status sync_directory(const std::filesystem::path& path) {
#if defined(_WIN32)
  (void)path;
  return Status::success();
#else
  const int fd = ::open(path.string().c_str(), O_RDONLY);
  if (fd < 0) {
    return Status(Code::Unavailable, "cannot open directory for sync");
  }
  const int rc = ::fsync(fd);
  ::close(fd);
  if (rc != 0) {
    return Status(Code::Unavailable, describe_errno(errno));
  }
  return Status::success();
#endif
}

FileLock::FileLock(FileLock&& other) noexcept
    : handle_(other.handle_), key_(std::move(other.key_)) {
  other.handle_ = nullptr;
  other.key_.clear();
}

FileLock& FileLock::operator=(FileLock&& other) noexcept {
  if (this != &other) {
    release();
    handle_ = other.handle_;
    key_ = std::move(other.key_);
    other.handle_ = nullptr;
    other.key_.clear();
  }
  return *this;
}

FileLock::~FileLock() { release(); }

void FileLock::release() {
  if (handle_ == nullptr) {
    return;
  }
  std::FILE* handle = static_cast<std::FILE*>(handle_);
  handle_ = nullptr;
  (void)std::fclose(handle);
  if (!key_.empty()) {
    std::lock_guard<std::mutex> guard(lock_registry_mutex());
    lock_registry().erase(key_);
    key_.clear();
  }
}

Result<FileLock> FileLock::acquire(const std::filesystem::path& path) {
  const std::string key = lock_key(path);
  {
    std::lock_guard<std::mutex> guard(lock_registry_mutex());
    if (lock_registry().find(key) != lock_registry().end()) {
      return Status(Code::Refused, "the pod store is already locked by this process: " + key);
    }
    lock_registry().insert(key);
  }
  struct RegistryGuard {
    std::string key;
    bool armed{true};
    ~RegistryGuard() {
      if (armed) {
        std::lock_guard<std::mutex> guard(lock_registry_mutex());
        lock_registry().erase(key);
      }
    }
  } guard{key, true};

  PODFABRIC_TRY_ASSIGN(File file, File::open_append(path));
  std::FILE* handle = static_cast<std::FILE*>(file.handle_);
#if defined(_WIN32)
  HANDLE os = reinterpret_cast<HANDLE>(::_get_osfhandle(::_fileno(handle)));
  if (os == INVALID_HANDLE_VALUE) {
    return Status(Code::Unavailable, "cannot obtain a lock handle");
  }
  OVERLAPPED overlapped{};
  if (!::LockFileEx(os, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &overlapped)) {
    return Status(Code::Refused,
                  "the pod store is already locked by another process (error " +
                      std::to_string(::GetLastError()) + ")");
  }
#else
  if (::flock(::fileno(handle), LOCK_EX | LOCK_NB) != 0) {
    return Status(Code::Refused, "the pod store is already locked by another process");
  }
#endif
  guard.armed = false;
  FileLock lock(handle);
  lock.key_ = key;
  file.handle_ = nullptr;  // ownership transferred to the lock
  return lock;
}

}  // namespace podfabric::platform
