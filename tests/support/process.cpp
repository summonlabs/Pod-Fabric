// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "process.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

#if !defined(_WIN32)
#define _getpid getpid
#endif

namespace podfabric::test {
namespace {

std::string quote(const std::string& value) {
  if (value.find_first_of(" \t") == std::string::npos) {
    return value;
  }
  std::string out = "\"";
  out.append(value);
  out.push_back('"');
  return out;
}

}  // namespace

Child::Child(Child&& other) noexcept : handle_(other.handle_), pid_(other.pid_) {
  other.handle_ = nullptr;
  other.pid_ = 0;
}

Child& Child::operator=(Child&& other) noexcept {
  if (this != &other) {
    (void)close();
    handle_ = other.handle_;
    pid_ = other.pid_;
    other.handle_ = nullptr;
    other.pid_ = 0;
  }
  return *this;
}

Child::~Child() { (void)close(); }

Result<int> Child::wait(Nanos timeout) {
  if (handle_ == nullptr) {
    return Status(Code::Invalid, "the child process handle is not open");
  }
#if defined(_WIN32)
  HANDLE handle = static_cast<HANDLE>(handle_);
  const DWORD millis = static_cast<DWORD>(timeout / nanos_per_millisecond);
  const DWORD rc = ::WaitForSingleObject(handle, millis);
  if (rc == WAIT_TIMEOUT) {
    return Status(Code::Cancelled, "the child process did not exit within the deadline");
  }
  if (rc != WAIT_OBJECT_0) {
    return Status(Code::Unavailable, "waiting for the child process failed");
  }
  DWORD code = 0;
  if (!::GetExitCodeProcess(handle, &code)) {
    return Status(Code::Unavailable, "cannot read the child exit code");
  }
  return static_cast<int>(code);
#else
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::nanoseconds(timeout);
  for (;;) {
    int status = 0;
    const pid_t rc = ::waitpid(static_cast<pid_t>(pid_), &status, WNOHANG);
    if (rc == static_cast<pid_t>(pid_)) {
      if (WIFEXITED(status)) {
        return static_cast<int>(WEXITSTATUS(status));
      }
      return 128 + WTERMSIG(status);
    }
    if (rc < 0) {
      return Status(Code::Unavailable, "waiting for the child process failed");
    }
    if (std::chrono::steady_clock::now() > deadline) {
      return Status(Code::Cancelled, "the child process did not exit within the deadline");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
#endif
}

Status Child::kill() {
  if (handle_ == nullptr) {
    return Status(Code::Invalid, "the child process handle is not open");
  }
#if defined(_WIN32)
  HANDLE handle = static_cast<HANDLE>(handle_);
  if (!::TerminateProcess(handle, 0xDEADu)) {
    return Status(Code::Unavailable, "cannot terminate the child process");
  }
  (void)::WaitForSingleObject(handle, 10000);
  return Status::success();
#else
  if (::kill(static_cast<pid_t>(pid_), SIGKILL) != 0) {
    return Status(Code::Unavailable, "cannot kill the child process");
  }
  int status = 0;
  (void)::waitpid(static_cast<pid_t>(pid_), &status, 0);
  return Status::success();
#endif
}

Status Child::close() {
  if (handle_ == nullptr) {
    return Status::success();
  }
#if defined(_WIN32)
  ::CloseHandle(static_cast<HANDLE>(handle_));
#endif
  handle_ = nullptr;
  pid_ = 0;
  return Status::success();
}

Result<Child> spawn(const std::string& program, const std::vector<std::string>& arguments,
                    const std::string& stdout_path, const std::string& stderr_path) {
#if defined(_WIN32)
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;

  HANDLE out = ::CreateFileA(stdout_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &attributes,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (out == INVALID_HANDLE_VALUE) {
    return Status(Code::Unavailable, "cannot create the child stdout file");
  }
  HANDLE err = ::CreateFileA(stderr_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &attributes,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (err == INVALID_HANDLE_VALUE) {
    ::CloseHandle(out);
    return Status(Code::Unavailable, "cannot create the child stderr file");
  }

  std::string command = quote(program);
  for (const std::string& argument : arguments) {
    command.push_back(' ');
    command.append(quote(argument));
  }

  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = out;
  startup.hStdError = err;
  startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
  PROCESS_INFORMATION info{};
  std::vector<char> mutable_command(command.begin(), command.end());
  mutable_command.push_back('\0');
  const BOOL created = ::CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info);
  ::CloseHandle(out);
  ::CloseHandle(err);
  if (!created) {
    return Status(Code::Unavailable,
                  "cannot start the child process (error " +
                      std::to_string(::GetLastError()) + ")");
  }
  ::CloseHandle(info.hThread);
  Child child;
  child.handle_ = info.hProcess;
  child.pid_ = info.dwProcessId;
  return child;
#else
  std::fflush(nullptr);
  const pid_t pid = ::fork();
  if (pid < 0) {
    return Status(Code::Unavailable, "cannot fork");
  }
  if (pid == 0) {
    const int out = ::open(stdout_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    const int err = ::open(stderr_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out >= 0) {
      (void)::dup2(out, STDOUT_FILENO);
    }
    if (err >= 0) {
      (void)::dup2(err, STDERR_FILENO);
    }
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(program.c_str()));
    for (const std::string& argument : arguments) {
      argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    ::execv(program.c_str(), argv.data());
    ::_exit(127);
  }
  Child child;
  child.handle_ = reinterpret_cast<void*>(1);
  child.pid_ = static_cast<std::uint32_t>(pid);
  return child;
#endif
}

std::string read_text(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return {};
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

bool wait_for_text(const std::string& path, const std::string& needle, Nanos timeout) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds(timeout);
  for (;;) {
    const std::string text = read_text(path);
    if (text.find(needle) != std::string::npos) {
      return true;
    }
    if (std::chrono::steady_clock::now() > deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

std::string make_scratch_directory(const std::string& label) {
  std::error_code ec;
  const std::filesystem::path base =
      std::filesystem::temp_directory_path(ec) / "podfabric-tests";
  std::filesystem::create_directories(base, ec);
  // Every call gets its own directory so that a case which needs several
  // scratch locations never destroys an earlier one.
  static std::atomic<std::uint64_t> counter{0};
  const std::uint64_t index = counter.fetch_add(1);
  const std::filesystem::path directory =
      base / (label + "-" + std::to_string(::_getpid()) + "-" + std::to_string(index));
  std::filesystem::remove_all(directory, ec);
  std::filesystem::create_directories(directory, ec);
  return directory.string();
}

void remove_directory(const std::string& path) {
  std::error_code ec;
  std::filesystem::remove_all(path, ec);
}

}  // namespace podfabric::test
