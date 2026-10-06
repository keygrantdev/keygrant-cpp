// The machine id's parsing and decisions, as portable functions (tested on
// every platform), and the read sequence over a MachineIdSource
// (sdk/src/machine-id.ts).
#pragma once

#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <string_view>

#include "keygrant/fingerprint.hpp"

namespace keygrant::detail {

/// Where Linux keeps it: systemd's file, else the D-Bus one older systems have.
inline constexpr const char* kMachineIdFile = "/etc/machine-id";
inline constexpr const char* kDbusMachineIdFile = "/var/lib/dbus/machine-id";

/// Win32 status codes the registry read classifies.
inline constexpr long kWinSuccess = 0;
inline constexpr long kWinFileNotFound = 2;
inline constexpr long kWinPathNotFound = 3;
inline constexpr long kWinAccessDenied = 5;
inline constexpr long kWinMoreData = 234;
inline constexpr long kWinUnsupportedType = 1630;

/**
 * The MachineGuid in a REG_SZ value as RegGetValueW returns it: the data up to
 * its NUL, and of that the first run of non-space characters, as the
 * reference's `MachineGuid\s+REG_SZ\s+(\S+)` takes it out of `reg query`.
 * Not found, access denied or another type is unavailable; anything else
 * (an error nobody expected) is transient.
 */
MachineIdReading machineGuidFrom(const RegistryRead& read);

/**
 * The IOPlatformUUID as IOKit returns it: up to any `"`, trimmed, as the
 * reference's `"IOPlatformUUID"\s*=\s*"([^"]+)"` takes it out of `ioreg`.
 * No service, no property or not a string is unavailable; a failed call transient.
 */
MachineIdReading platformUuidFrom(const IoKitRead& read);

/**
 * The type of the filesystem /etc/machine-id is on, from /proc/self/mountinfo:
 * the mount whose mount point is the longest prefix of the path on a path
 * boundary; of several at that point, the one no other names as its parent,
 * else the last listed. None when no line can be read so.
 */
std::optional<std::string> machineIdFilesystem(const std::string& mountinfo);

/**
 * A host name as Node's os.hostname() gives it, from the bytes the system
 * returned: read as UTF-8, each ill-formed sequence one U+FFFD (as V8 and the
 * WHATWG decoder read them), so a machine has one host-name fingerprint
 * whichever SDK reads it.
 */
std::string hostNameFrom(std::string_view bytes);

/// The boot time from /proc/stat (`btime <seconds>` on a line of its own), in ms.
std::optional<std::int64_t> bootTimeMs(const std::string& procStat);

/// Whether a failed read is for good (unavailable: ENOENT, EACCES, EPERM, ENOTDIR, EISDIR) or this time only.
MachineIdReading failureOf(std::exception_ptr error);

/// The read itself, on this thread, with no bound: what readMachineId bounds.
MachineIdReading readMachineIdNow(MachineIdSource& source);

}  // namespace keygrant::detail
