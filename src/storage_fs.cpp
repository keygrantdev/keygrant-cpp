// The real file system under FileStorage: POSIX, or Win32 with UTF-8 paths.
// Every failure is an IoError named as Node names it (libuv's mapping on
// Windows), so the storage's retry rules read the same on every platform.
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "keygrant/storage.hpp"
#include "sys.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace keygrant {

namespace {

#ifdef _WIN32

[[noreturn]] void fail(const char* what, const std::string& path) {
  const DWORD error = GetLastError();
  throw IoError(detail::win32ErrorCode(error), std::string(what) + " " + path);
}

class WinFile final : public StorageFile {
 public:
  WinFile(HANDLE handle, std::string path) : handle_(handle), path_(std::move(path)) {}
  ~WinFile() override {
    if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
  }
  void write(const std::string& data) override {
    std::size_t done = 0;
    while (done < data.size()) {
      const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(data.size() - done, 1u << 30));
      DWORD written = 0;
      if (!WriteFile(handle_, data.data() + done, chunk, &written, nullptr)) fail("write", path_);
      done += written;
    }
  }
  void sync() override {
    if (!FlushFileBuffers(handle_)) fail("flush", path_);
  }
  void close() override {
    const HANDLE handle = handle_;
    handle_ = INVALID_HANDLE_VALUE;
    if (handle != INVALID_HANDLE_VALUE && !CloseHandle(handle)) fail("close", path_);
  }

 private:
  HANDLE handle_;
  std::string path_;
};

class WinFs final : public StorageFs {
 public:
  std::string readFile(const std::string& path) override {
    const HANDLE handle = CreateFileW(detail::widen(path).c_str(), GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) fail("open", path);
    std::string text;
    char buffer[16384];
    for (;;) {
      DWORD read = 0;
      if (!ReadFile(handle, buffer, static_cast<DWORD>(sizeof buffer), &read, nullptr)) {
        const DWORD error = GetLastError();
        CloseHandle(handle);
        throw IoError(detail::win32ErrorCode(error), "read " + path);
      }
      if (read == 0) break;
      text.append(buffer, read);
    }
    CloseHandle(handle);
    return text;
  }

  std::unique_ptr<StorageFile> create(const std::string& path) override {
    const HANDLE handle = CreateFileW(detail::widen(path).c_str(), GENERIC_WRITE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) fail("create", path);
    return std::make_unique<WinFile>(handle, path);
  }

  void rename(const std::string& from, const std::string& to) override {
    // As libuv renames: replacing the target, written through.
    if (!MoveFileExW(detail::widen(from).c_str(), detail::widen(to).c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      const DWORD error = GetLastError();  // before the message is built
      throw IoError(detail::win32ErrorCode(error), "rename " + from + " -> " + to);
    }
  }

  void rm(const std::string& path) override {
    if (DeleteFileW(detail::widen(path).c_str())) return;
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return;
    throw IoError(detail::win32ErrorCode(error), "remove " + path);
  }

  std::vector<std::string> readdir(const std::string& dir) override {
    WIN32_FIND_DATAW entry;
    const std::wstring pattern = detail::widen(dir + "\\*");
    const HANDLE find = FindFirstFileW(pattern.c_str(), &entry);
    if (find == INVALID_HANDLE_VALUE) fail("readdir", dir);
    std::vector<std::string> names;
    do {
      const std::string name = detail::narrow(entry.cFileName);
      if (name != "." && name != "..") names.push_back(name);
    } while (FindNextFileW(find, &entry));
    FindClose(find);
    return names;
  }

  std::int64_t mtimeMs(const std::string& path) override {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(detail::widen(path).c_str(), GetFileExInfoStandard, &data)) fail("stat", path);
    ULARGE_INTEGER ticks;
    ticks.LowPart = data.ftLastWriteTime.dwLowDateTime;
    ticks.HighPart = data.ftLastWriteTime.dwHighDateTime;
    // 100 ns ticks since 1601-01-01, to ms since 1970-01-01.
    return static_cast<std::int64_t>(ticks.QuadPart / 10'000) - 11'644'473'600'000LL;
  }

  void mkdir(const std::string& dir) override {
    // Every prefix is tried, and what a prefix cannot be (a drive, a UNC
    // server or share, a folder there already) is passed over: only whether
    // the whole folder is there in the end is the answer.
    DWORD error = ERROR_SUCCESS;
    for (std::size_t i = 1; i <= dir.size(); ++i) {
      if (i < dir.size() && dir[i] != '\\' && dir[i] != '/') continue;
      const std::string path = dir.substr(0, i);
      if (path.back() == '\\' || path.back() == '/' || (path.size() == 2 && path[1] == ':')) continue;
      if (!CreateDirectoryW(detail::widen(path).c_str(), nullptr)) error = GetLastError();
    }
    const DWORD attributes = GetFileAttributesW(detail::widen(dir).c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return;
    throw IoError(detail::win32ErrorCode(error == ERROR_SUCCESS ? GetLastError() : error), "mkdir " + dir);
  }

  // Windows opens no handle on a folder to flush; NTFS journals the rename itself.
  void syncDir(const std::string&) override {}
};

#else

[[noreturn]] void fail(const char* what, const std::string& path) {
  const int error = errno;
  throw IoError(detail::errnoCode(error), std::string(what) + " " + path + ": " + std::strerror(error));
}

/** fsync, and on macOS F_FULLFSYNC, which flushes the drive's own cache too. */
int flush(int fd) {
#ifdef __APPLE__
  if (fcntl(fd, F_FULLFSYNC) == 0) return 0;
#endif
  return fsync(fd);
}

class PosixFile final : public StorageFile {
 public:
  PosixFile(int fd, std::string path) : fd_(fd), path_(std::move(path)) {}
  ~PosixFile() override {
    if (fd_ >= 0) ::close(fd_);
  }
  void write(const std::string& data) override {
    std::size_t done = 0;
    while (done < data.size()) {
      const ssize_t written = ::write(fd_, data.data() + done, data.size() - done);
      if (written < 0) {
        if (errno == EINTR) continue;
        fail("write", path_);
      }
      done += static_cast<std::size_t>(written);
    }
  }
  void sync() override {
    if (flush(fd_) != 0) fail("fsync", path_);
  }
  void close() override {
    const int fd = fd_;
    fd_ = -1;
    if (fd >= 0 && ::close(fd) != 0 && errno != EINTR) fail("close", path_);
  }

 private:
  int fd_;
  std::string path_;
};

class PosixFs final : public StorageFs {
 public:
  std::string readFile(const std::string& path) override {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) fail("open", path);
    std::string text;
    char buffer[16384];
    for (;;) {
      const ssize_t got = ::read(fd, buffer, sizeof buffer);
      if (got < 0) {
        if (errno == EINTR) continue;
        const int error = errno;
        ::close(fd);
        throw IoError(detail::errnoCode(error), "read " + path);
      }
      if (got == 0) break;
      text.append(buffer, static_cast<std::size_t>(got));
    }
    ::close(fd);
    return text;
  }

  std::unique_ptr<StorageFile> create(const std::string& path) override {
    // Owner-only: the file holds the licence key.
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) fail("create", path);
    return std::make_unique<PosixFile>(fd, path);
  }

  void rename(const std::string& from, const std::string& to) override {
    if (::rename(from.c_str(), to.c_str()) == 0) return;
    const int error = errno;  // before the message is built
    throw IoError(detail::errnoCode(error), "rename " + from + " -> " + to + ": " + std::strerror(error));
  }

  void rm(const std::string& path) override {
    if (::unlink(path.c_str()) != 0 && errno != ENOENT) fail("unlink", path);
  }

  std::vector<std::string> readdir(const std::string& dir) override {
    DIR* handle = ::opendir(dir.c_str());
    if (handle == nullptr) fail("opendir", dir);
    std::vector<std::string> names;
    while (const dirent* entry = ::readdir(handle)) {
      const std::string name = entry->d_name;
      if (name != "." && name != "..") names.push_back(name);
    }
    ::closedir(handle);
    return names;
  }

  std::int64_t mtimeMs(const std::string& path) override {
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0) fail("stat", path);
#ifdef __APPLE__
    const auto& at = info.st_mtimespec;
#else
    const auto& at = info.st_mtim;
#endif
    // Held to a range that cannot overflow in milliseconds (a file can carry any time).
    constexpr std::int64_t kLimit = 9'000'000'000'000'000;
    const std::int64_t seconds = std::clamp<std::int64_t>(static_cast<std::int64_t>(at.tv_sec), -kLimit, kLimit);
    return seconds * 1000 + static_cast<std::int64_t>(at.tv_nsec) / 1'000'000;
  }

  void mkdir(const std::string& dir) override {
    for (std::size_t i = 1; i <= dir.size(); ++i) {
      if (i < dir.size() && dir[i] != '/') continue;
      const std::string path = dir.substr(0, i);
      if (path.back() == '/') continue;
      if (::mkdir(path.c_str(), 0700) == 0) continue;
      const int error = errno;
      // There already (or just made by another process): only a folder will do.
      struct stat info {};
      if (::stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode)) continue;
      errno = error;
      fail("mkdir", path);
    }
  }

  void syncDir(const std::string& dir) override {
    const int fd = ::open(dir.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) fail("open", dir);
    const int rc = flush(fd);
    const int error = errno;
    ::close(fd);
    if (rc != 0) throw IoError(detail::errnoCode(error), "fsync " + dir);
  }
};

#endif

}  // namespace

std::shared_ptr<StorageFs> systemStorageFs() {
  // Never destroyed: a machine-id read abandoned at its bound may still use it at exit.
#ifdef _WIN32
  static const auto* fs = new std::shared_ptr<StorageFs>(std::make_shared<WinFs>());
#else
  static const auto* fs = new std::shared_ptr<StorageFs>(std::make_shared<PosixFs>());
#endif
  return *fs;
}

}  // namespace keygrant
