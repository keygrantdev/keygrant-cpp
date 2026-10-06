#include "sys.hpp"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <random>
#include <thread>

#include "utf.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>
#else
#include <pwd.h>
#include <unistd.h>
#endif

namespace keygrant::detail {

#ifdef _WIN32

std::wstring widen(const std::string& utf8) {
  // wchar_t is a UTF-16 unit on Windows.
  std::wstring out;
  for (const char16_t unit : utf8ToUtf16(utf8)) out.push_back(static_cast<wchar_t>(unit));
  return out;
}

std::string narrow(const std::wstring& wide) {
  std::u16string units;
  units.reserve(wide.size());
  for (const wchar_t unit : wide) units.push_back(static_cast<char16_t>(unit));
  return utf16ToUtf8(units);
}

std::optional<std::string> environment(const char* name) {
  const std::wstring key = widen(name);
  const DWORD size = GetEnvironmentVariableW(key.c_str(), nullptr, 0);
  if (size == 0) return std::nullopt;
  std::wstring value(size, L'\0');
  const DWORD written = GetEnvironmentVariableW(key.c_str(), value.data(), size);
  if (written == 0 || written >= size) return std::nullopt;
  value.resize(written);
  return narrow(value);
}

namespace {

std::optional<std::string> knownFolder(REFKNOWNFOLDERID id) {
  PWSTR path = nullptr;
  if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &path)) || path == nullptr) {
    if (path != nullptr) CoTaskMemFree(path);
    return std::nullopt;
  }
  const std::string out = narrow(path);
  CoTaskMemFree(path);
  return out;
}

}  // namespace

std::optional<std::string> localAppDataFolder() { return knownFolder(FOLDERID_LocalAppData); }

std::string homeDirectory() {
  if (const auto profile = environment("USERPROFILE"); profile && !profile->empty()) return *profile;
  return knownFolder(FOLDERID_Profile).value_or(std::string());
}

#else

std::optional<std::string> environment(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr) return std::nullopt;
  return std::string(value);
}

std::string homeDirectory() {
  if (const auto home = environment("HOME"); home && !home->empty()) return *home;
  long size = sysconf(_SC_GETPW_R_SIZE_MAX);
  std::string buffer(size > 0 ? static_cast<std::size_t>(size) : 16384, '\0');
  struct passwd entry {};
  struct passwd* found = nullptr;
  if (getpwuid_r(getuid(), &entry, buffer.data(), buffer.size(), &found) == 0 && found != nullptr &&
      found->pw_dir != nullptr) {
    return found->pw_dir;
  }
  return {};
}

#endif

std::string errnoCode(int error) {
  switch (error) {
    case ENOENT: return "ENOENT";
    case EACCES: return "EACCES";
    case EPERM: return "EPERM";
    case ENOTDIR: return "ENOTDIR";
    case EISDIR: return "EISDIR";
    case EBUSY: return "EBUSY";
    case EEXIST: return "EEXIST";
    case EIO: return "EIO";
    case EMFILE: return "EMFILE";
    case ENFILE: return "ENFILE";
    case EAGAIN: return "EAGAIN";
    case EINTR: return "EINTR";
    case ENOSPC: return "ENOSPC";
    case EROFS: return "EROFS";
    case EXDEV: return "EXDEV";
    case ENAMETOOLONG: return "ENAMETOOLONG";
    case ELOOP: return "ELOOP";
    case EINVAL: return "EINVAL";
    case ENOTEMPTY: return "ENOTEMPTY";
    case ENOMEM: return "ENOMEM";
    case EBADF: return "EBADF";
    case ETIMEDOUT: return "ETIMEDOUT";
    default: return "E" + std::to_string(error);
  }
}

std::string win32ErrorCode(unsigned long error) {
  switch (error) {
    case 2: case 3: case 15: case 123: case 126: case 161: return "ENOENT";
    case 5: case 1314: return "EPERM";
    case 740: case 998: case 1920: return "EACCES";
    case 32: case 33: case 170: return "EBUSY";
    case 80: case 183: return "EEXIST";
    case 39: case 112: return "ENOSPC";
    case 145: return "ENOTEMPTY";
    case 267: return "ENOTDIR";
    case 17: return "EXDEV";
    case 19: return "EROFS";
    case 4: return "EMFILE";
    case 8: case 14: return "ENOMEM";
    case 87: return "EINVAL";
    case 23: case 31: case 1117: return "EIO";
    case 206: return "ENAMETOOLONG";
    case 1921: return "ELOOP";
    default: return "EWIN" + std::to_string(error);
  }
}

std::int64_t wallClockMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

long processId() {
#ifdef _WIN32
  return static_cast<long>(GetCurrentProcessId());
#else
  return static_cast<long>(getpid());
#endif
}

std::string randomSuffix() {
  thread_local std::mt19937_64 engine([] {
    std::random_device device;
    std::seed_seq seed{device(), device(), static_cast<unsigned>(wallClockMs()),
                       static_cast<unsigned>(std::hash<std::thread::id>{}(std::this_thread::get_id()))};
    return std::mt19937_64(seed);
  }());
  static constexpr char kDigits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
  std::uniform_int_distribution<int> pick(0, 35);
  std::string out(10, '0');
  for (char& c : out) c = kDigits[pick(engine)];
  return out;
}

}  // namespace keygrant::detail
