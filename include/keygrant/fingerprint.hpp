// Who this device is: the default fingerprinter and the machine id it hashes
// (sdk/src/adapters.ts nodeFingerprinter, sdk/src/machine-id.ts).
//
// A machine has ONE fingerprint whichever KeyGrant SDK reads it: the same id,
// the same host name, platform and architecture names, the same hashes.
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "keygrant/adapters.hpp"

namespace keygrant {

/// How long status() may spend reading the machine id.
inline constexpr std::chrono::milliseconds kMachineIdTimeout{2'000};
/// How long activate() and startTrial() may: a person is waiting, and a seat is at stake.
inline constexpr std::chrono::milliseconds kActivateMachineIdTimeout{10'000};
/// How far before the boot time an id file may have been written and still count as written at this boot.
inline constexpr std::int64_t kBootSlackMs = 5'000;

/// What is hashed when not even the host name can be read.
inline constexpr const char* kUnknownHost = "unknown-host";

/** A machine's fingerprint from what identifies it: lowercase hex SHA-256 of "keygrant|" + raw. */
std::string hashFingerprint(const std::string& raw);

/** The fingerprint of a machine id as it is read: hashFingerprint("machine-id|" + id). */
std::string machineFingerprint(const std::string& id);

/**
 * The host-name fingerprint, given the host name and Node's names for the
 * platform and architecture: hashFingerprint("<host>|<platform>|<arch>").
 */
std::string hostFingerprint(const std::string& host, const std::string& platform, const std::string& arch);

/** Node's name for this platform: "win32", "darwin", "linux", "freebsd", ... */
std::string platformName();

/** Node's name for the architecture this program was built for: "x64", "arm64", "ia32", "arm", ... */
std::string archName();

/** This machine's host name, as Node's os.hostname() reads it; std::nullopt when it cannot be read. */
std::optional<std::string> hostName();

/** What a host-name fingerprint hashes, as Node names them (hostFingerprint). */
struct HostNames {
  std::string host;
  std::string platform;
  std::string arch;
};

/** This machine's host names; std::nullopt when they cannot be read. */
std::optional<HostNames> hostNames();

/** The host-name fingerprint of these names; when they could not be read (std::nullopt), kUnknownHost's. */
std::string hostNamesFingerprint(const std::optional<HostNames>& names);

/**
 * The fingerprint before the machine id, and when a machine has none:
 * hashFingerprint("<hostname>|<platform>|<arch>"), or of "unknown-host".
 */
std::string hostnameFingerprint();

/** What reading the machine id gave. */
struct MachineIdReading {
  enum class Kind {
    /// The machine's id.
    Id,
    /// There is none to read, and there will not be on the next try either: fall back to the host name.
    Unavailable,
    /// The read did not finish: NO fingerprint this time, and the next call reads again.
    Transient,
  };
  Kind kind = Kind::Transient;
  std::string id;

  static MachineIdReading of(std::string id) { return {Kind::Id, std::move(id)}; }
  static MachineIdReading unavailable() { return {Kind::Unavailable, {}}; }
  static MachineIdReading transient() { return {Kind::Transient, {}}; }
  bool operator==(const MachineIdReading& other) const { return kind == other.kind && id == other.id; }
};

/** What RegGetValueW gave for MachineGuid: its status (a Win32 error code) and the REG_SZ data. */
struct RegistryRead {
  long status = 0;
  std::u16string data;
};

/** What IOKit gave for the platform expert's IOPlatformUUID. */
struct IoKitRead {
  enum class Status { Ok, NoService, NoProperty, NotAString, Failed };
  Status status = Status::Failed;
  std::string value;
};

/**
 * Where the machine id is read from: the real system, or a stand-in in tests.
 * Each read may throw; the caller classifies what it threw.
 */
class MachineIdSource {
 public:
  virtual ~MachineIdSource() = default;
  /// Node's name for the platform.
  virtual std::string platform() = 0;
  /// Windows: MachineGuid under HKLM\SOFTWARE\Microsoft\Cryptography, in the 64-bit view.
  virtual RegistryRead readMachineGuid() = 0;
  /// macOS: IOPlatformUUID of the IOPlatformExpertDevice service.
  virtual IoKitRead readPlatformUuid() = 0;
  /// A file's text; throws IoError ("ENOENT", "EACCES", ...) when it cannot be read.
  virtual std::string readFile(const std::string& path) = 0;
  /// When a file's content was last written (ms); std::nullopt: its age is not judged. Throws IoError.
  virtual std::optional<std::int64_t> mtimeMs(const std::string& path) = 0;
};

/** The running system. */
std::shared_ptr<MachineIdSource> systemMachineIdSource();

/**
 * This machine's id, or why there is none. Never throws, and never takes much
 * longer than `timeout`: a read that outlives it is abandoned as transient.
 *  - Windows: MachineGuid from the 64-bit registry view.
 *  - macOS: IOPlatformUUID of the platform expert device (IOKit, no subprocess).
 *  - Linux and others: /etc/machine-id, else /var/lib/dbus/machine-id; an id on
 *    a filesystem held in memory (tmpfs, ramfs), or written at this boot, is
 *    unavailable.
 * The source is held for as long as an abandoned read still runs.
 */
MachineIdReading readMachineId(const std::shared_ptr<MachineIdSource>& source,
                               std::chrono::milliseconds timeout = kMachineIdTimeout);

/**
 * Who this device is, as the default fingerprinter answers (identify), from a
 * reading of its machine id (readMachineId) and its host names (std::nullopt
 * when they cannot be read):
 *  - an id: the machine id's fingerprint (machineFingerprint), kind Machine,
 *    with the host name's (hostFingerprint) as an alternate, none when the two
 *    are the same;
 *  - unavailable (no id to read): the host name's, kind Hostname, no alternate;
 *  - transient (the read did not finish): no fingerprint this time, and the
 *    host name's as an alternate.
 * The host name's is kUnknownHost's when the names cannot be read.
 */
DeviceIdentity identityOf(const MachineIdReading& reading, const std::optional<HostNames>& host);

/**
 * The default fingerprinter: the operating system's machine id, hashed so the
 * raw id never leaves the machine (kind Machine); the host name's
 * (hostnameFingerprint, kind Hostname) when the machine has no id to read. A
 * read that did not finish gives NO fingerprint and is not kept, so the next
 * call reads again. A settled fingerprint is kept for the process (for the
 * fingerprinter, when given its own source). The host-name fingerprint is
 * offered as an alternate beside a machine id, so a seat activated under it
 * before is still this device.
 */
class SystemFingerprinter final : public Fingerprinter {
 public:
  /// The running system, settled once per process.
  SystemFingerprinter();
  /// A stand-in system, settled per fingerprinter.
  explicit SystemFingerprinter(std::shared_ptr<MachineIdSource> source);

  /// Waits as long as an activation does; throws when the id still could not be read.
  std::string get() override;
  std::optional<DeviceIdentity> identify(std::chrono::milliseconds timeout) override;

 private:
  struct Settled;
  /// The default fingerprinter's settled fingerprint for this process.
  static std::shared_ptr<Settled> processWide();

  std::shared_ptr<MachineIdSource> source_;
  std::shared_ptr<Settled> own_;
};

}  // namespace keygrant
