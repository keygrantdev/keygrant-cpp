// The little the SDK asks of the operating system directly.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace keygrant::detail {

/// An environment variable as UTF-8; none when unset.
std::optional<std::string> environment(const char* name);

/// The user's home folder as Node's os.homedir() finds it (HOME / USERPROFILE first); empty when unknown.
std::string homeDirectory();

/// The errno-style name of a POSIX errno value ("ENOENT", "EBUSY", ...).
std::string errnoCode(int error);

/// The errno-style name libuv gives a Win32 error code (ERROR_SHARING_VIOLATION is "EBUSY", ...).
std::string win32ErrorCode(unsigned long error);

/// The wall clock in ms since the epoch.
std::int64_t wallClockMs();

/// This process's id.
long processId();

/// Ten random base-36 characters, for a temp file's name.
std::string randomSuffix();

#ifdef _WIN32
/// FOLDERID_LocalAppData, as UTF-8.
std::optional<std::string> localAppDataFolder();
/// UTF-8 to the UTF-16 Win32 wants, and back.
std::wstring widen(const std::string& utf8);
std::string narrow(const std::wstring& wide);
#endif

}  // namespace keygrant::detail
