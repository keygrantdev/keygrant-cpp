// The KeyGrant licensing SDK for C++: the values a licence call answers with,
// and the state the SDK keeps on the device.
//
// A port of the Electron SDK, the reference: every name and every stored field
// means what it means there.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "keygrant/entitlement.hpp"

namespace keygrant {

/// A lease's `major` for a key with no upper bound.
inline constexpr std::int64_t kOpenMajor = 1'000'000;

/// The highest real major the server accepts in a request.
inline constexpr std::int64_t kMaxMajor = 99'999;

/**
 * What status() says of the licence. `Unknown` (reason `Storage`): the state
 * file could not be read just now (an antivirus scan holding it, say): nothing
 * is known either way, so keep running, and ask again shortly.
 */
enum class LicenseState { Licensed, Trial, Expired, Invalid, Unlicensed, Unknown };

/**
 * Why a licence does not license this build. `Upgrade`: the build is NEWER than
 * the key covers (buy an upgrade). `Outdated`: the build is OLDER than the key's
 * lowest version (update the app). `Device`: the licence was activated on
 * another device (this machine changed, or the state file was copied from
 * another one); activate again on this one. `Tampered`: the lease is not for
 * this licence at all, or the stored licence file is not one (the state file
 * was edited). `Storage`: the state file could not be read just now. On a
 * status from a licence file (`source` File), `Upgrade` and `Outdated` are the
 * file's range of versions, which no renewal changes.
 */
enum class LicenseReason {
  Revoked,
  Upgrade,
  Outdated,
  Offline,
  Tampered,
  Device,
  Trial,
  Storage,
  /// The lease is signed by a key this build does not carry (LicenseConfig::publicJwk). Asked
  /// online, the server signs one with the key it uses now, which a build carrying that key verifies.
  /// A stored licence file signed by such a key needs the app's update that carries it.
  UnknownKey,
  /// The stored offline licence file is one this build lists as revoked (LicenseConfig::revokedLicenseFiles).
  RevokedFile,
};

/** What a status rests on, when it is not a key's lease or a trial: a stored offline licence file. */
enum class LicenseSource { File };

/** Who a licence file is licensed to, as it signs it: a name, an email address, or both. */
struct LicensedTo {
  std::optional<std::string> name;
  std::optional<std::string> email;

  bool operator==(const LicensedTo& other) const { return name == other.name && email == other.email; }
  bool operator!=(const LicensedTo& other) const { return !(*this == other); }
};

/** Why activate() did not activate. */
enum class ActivateError {
  InvalidKey,
  Limit,
  Revoked,
  Upgrade,
  /// The key is for a later version than this build: update the app.
  Outdated,
  Network,
  /// Named for parity with the Electron SDK; never returned.
  NotConfigured,
  /// The server returned a lease that failed offline verification (an integrity or config error).
  InvalidLease,
  /// A user-locked key asks a NEW device who it is: ask the person for the address the key was sold to and
  /// activate again with it (ActivateOptions::email).
  UserEmailRequired,
  /// Not the key's address (or longer than any); nothing is said of the real one: ask again.
  UserEmailMismatch,
  /// A code was just mailed to that address: ask for it and activate again with the address and the code.
  UserCodeRequired,
  /// The code is wrong, expired, used or not six digits: ask again, or activate with the address alone for a new one.
  UserCodeInvalid,
  /// Three codes were sent for the key this hour: wait, or use the code sent.
  TooManyCodes,
  /// Too many wrong addresses or codes for the key this hour: try again later.
  TooManyAttempts,
  /// The code could not be sent just now and nothing was activated: try again.
  TryAgain,
};

/** A definitive refusal from the server, as StoredState::refused keeps it. */
enum class Refusal { Revoked, Upgrade, Outdated, Device };

/** What a fingerprint hashes: the operating system's machine id, or the host name. */
enum class DeviceKind { Machine, Hostname };

/// The reference's wire names: "licensed", "invalid-key", "machine", ...
const char* to_string(LicenseState value) noexcept;
const char* to_string(LicenseReason value) noexcept;
const char* to_string(ActivateError value) noexcept;
const char* to_string(Refusal value) noexcept;
const char* to_string(DeviceKind value) noexcept;
const char* to_string(LicenseSource value) noexcept;

std::optional<Refusal> refusalFromString(std::string_view name) noexcept;
std::optional<DeviceKind> deviceKindFromString(std::string_view name) noexcept;

std::ostream& operator<<(std::ostream& out, LicenseState value);
std::ostream& operator<<(std::ostream& out, LicenseReason value);
std::ostream& operator<<(std::ostream& out, ActivateError value);
std::ostream& operator<<(std::ostream& out, Refusal value);
std::ostream& operator<<(std::ostream& out, DeviceKind value);
std::ostream& operator<<(std::ostream& out, LicenseSource value);
/// "{ name: Ada Lovelace, email: ada@example.com }", each only when there is one.
std::ostream& operator<<(std::ostream& out, const LicensedTo& licensedTo);

/** What status(), refresh() and startTrial() answer. */
struct LicenseStatus {
  LicenseState state = LicenseState::Unlicensed;
  std::optional<LicenseReason> reason;
  /// The licence key; on a status from a licence file that is Licensed, or Invalid with RevokedFile,
  /// Upgrade or Outdated, the key the file records; none on any other status from a file.
  std::optional<std::string> key;
  std::optional<std::int64_t> trialDaysLeft;
  /// When the trial ends (ms since the epoch).
  std::optional<std::int64_t> trialEndsAt;
  /**
   * A TEST licence: one the developer made to exercise their app, which lapses
   * on its own within 30 days (an offline licence file made for one never does:
   * it is a file). It licenses the app like any other; check this
   * to refuse test licences in a release build.
   */
  bool test = false;
  /**
   * What the licence grants beyond the base product, by name, as its lease
   * signs them (the `ent` claim):
   *
   *   const auto edition = status.entitlements.find("edition");
   *   const bool pro = edition != status.entitlements.end() && edition->second == "pro";
   *
   * On a `Licensed` status, from the licence's lease, or the stored offline
   * licence file's `ent`; on a `Trial` status, from the trial's. Empty on
   * every other status (a stored refusal included, whatever the lease held
   * grants), and when the lease grants none. A member
   * of the claim that is not a flag, a number or a text is left out, and never
   * costs the licence. The status's own copy: changing it changes nothing the
   * next status says.
   */
  std::map<std::string, Entitlement> entitlements;
  /**
   * An earlier call on this License has not finished (an adapter stuck on
   * storage, say), so this answer comes from what is stored, with no ask and
   * no write. A restart clears it.
   */
  bool stalled = false;
  /**
   * `File` when the status rests on a stored offline licence file
   * (License::importLicenseFile), licensed or not; none for a key's lease, a
   * trial and no licence. A file holds no seat: offer to remove it
   * (License::removeLicenseFile) rather than deactivating.
   */
  std::optional<LicenseSource> source;
  /// Who the stored licence file is licensed to, on a file status that is Licensed, or Invalid with
  /// RevokedFile, Upgrade or Outdated; none otherwise.
  std::optional<LicensedTo> licensedTo;

  /// Every member alike, the entitlements by content (each name and value; numbers as numbers).
  bool operator==(const LicenseStatus& other) const;
  bool operator!=(const LicenseStatus& other) const { return !(*this == other); }
};

std::ostream& operator<<(std::ostream& out, const LicenseStatus& status);

/** What activate() answers: ok, or why not. */
struct ActivateResult {
  bool ok = false;
  std::optional<ActivateError> error;

  static ActivateResult success() { return ActivateResult{true, std::nullopt}; }
  static ActivateResult failure(ActivateError why) { return ActivateResult{false, why}; }

  bool operator==(const ActivateResult& other) const { return ok == other.ok && error == other.error; }
  bool operator!=(const ActivateResult& other) const { return !(*this == other); }
};

std::ostream& operator<<(std::ostream& out, const ActivateResult& result);

/** What deactivate() answers: whether the seat is now free. */
struct DeactivateResult {
  /**
   * True when KeyGrant confirmed the release (a 200), or when this device held
   * no activation to release. False when it could not be asked, or answered
   * anything else (a 5xx, a 429, a proxy's page): the seat may still be held,
   * and the app should say so (the developer's dashboard, or support, can
   * release it), not tell the customer it is free. The local licence is
   * dropped either way.
   */
  bool released = false;

  bool operator==(const DeactivateResult& other) const { return released == other.released; }
  bool operator!=(const DeactivateResult& other) const { return !(*this == other); }
};

std::ostream& operator<<(std::ostream& out, const DeactivateResult& result);

/**
 * The state the SDK keeps on the device, field for field the reference's
 * StoredState: times are milliseconds since the epoch, and the JSON field names
 * are the same, so the file is portable between SDKs.
 */
struct StoredState {
  std::optional<std::string> key;
  std::optional<std::string> activationId;
  /// Signed JWT lease for a paid licence.
  std::optional<std::string> lease;
  /// Earliest trial start ever seen. Only ever moves earlier.
  std::optional<std::int64_t> trialStart;
  /// Server-authoritative trial end.
  std::optional<std::int64_t> trialEndsAt;
  /// Signed JWT lease for a trial.
  std::optional<std::string> trialLease;
  /// When (guarded clock) the stored trial answer was asked for.
  std::optional<std::int64_t> trialAt;
  /// Monotonic high-water clock: guards against clock rollback.
  std::optional<std::int64_t> lastSeen;
  /// The app major sent with the last check-in the server answered.
  std::optional<std::int64_t> checkedMajor;
  /// The newest major run while holding a lease inside the key's upgrade window.
  std::optional<std::int64_t> keptMajor;
  /// When the last ask came back without a lease, and how long to wait after it.
  std::optional<std::int64_t> backoffSince;
  std::optional<std::int64_t> backoffMs;
  /// The server's definitive refusal of this key on this device.
  std::optional<Refusal> refused;
  /// The build major an `upgrade` or `outdated` refusal was given for.
  std::optional<std::int64_t> refusedMajor;
  /// The device a `device` refusal was given for (its `fp` claims, never fingerprints).
  std::optional<std::vector<std::string>> refusedFor;
  /// What this install is keyed by.
  std::optional<DeviceKind> deviceKind;
  /// When (guarded clock) the server's answer stored here was asked for.
  std::optional<std::int64_t> verdictAt;
  /// The default storage's revision of this state, one more on every save.
  std::optional<std::int64_t> rev;
  /// When (guarded clock) a running trial last reported its launch.
  std::optional<std::int64_t> trialReportedAt;
  /// The stored offline licence file's token, as importLicenseFile stored it; verified again on every status.
  std::optional<std::string> licenseFile;
  /**
   * What this SDK cannot read, as raw JSON text by name: fields it does not
   * know (a newer SDK's), and values of known fields it cannot read (a newer
   * SDK's refusal or device kind, say; the field itself is then absent above).
   * Kept, and written back unchanged, as the reference keeps them, until this
   * SDK writes or clears the field itself.
   */
  std::map<std::string, std::string> otherFields;

  bool operator==(const StoredState& other) const;
  bool operator!=(const StoredState& other) const { return !(*this == other); }
};

std::ostream& operator<<(std::ostream& out, const StoredState& state);

/** The state as the file holds it: compact JSON, absent fields omitted. Never throws for content. */
std::string serializeState(const StoredState& state);

/**
 * A state read back from its JSON. Throws std::invalid_argument when the text
 * is not JSON; std::nullopt for JSON that is not an object (`null`). A known
 * field of the wrong type, or with a value this SDK does not know, is read as
 * absent, and kept as written (otherFields).
 */
std::optional<StoredState> parseState(std::string_view json);

/**
 * A file or system operation that failed. `code()` is the errno-style name the
 * reference's storage uses ("ENOENT", "EBUSY", "EPERM", "EACCES", "EIO", and
 * "ECONFLICT" for a save that kept losing to another process).
 */
class IoError : public std::runtime_error {
 public:
  IoError(std::string code, const std::string& message);
  const std::string& code() const noexcept { return code_; }

 private:
  std::string code_;
};

/**
 * deactivate(), importLicenseFile() or removeLicenseFile() behind a call that
 * has not finished (nothing was released, dropped or written).
 */
class StalledError : public std::runtime_error {
 public:
  StalledError();
};

/** What License::require() throws when the licence does not grant an entitlement: names it. */
class MissingEntitlement : public std::runtime_error {
 public:
  explicit MissingEntitlement(std::string entitlement);
  /// The entitlement's code, as the app asked for it.
  const std::string& entitlement() const noexcept { return entitlement_; }

 private:
  std::string entitlement_;
};

/**
 * Whether entitlements grant `code`: a flag that is on, a number that is not
 * zero, or a text that is not empty. Absent, off, zero or empty is not
 * granted. The code is matched exactly, byte for byte.
 */
bool grants(const std::map<std::string, Entitlement>& entitlements, const std::string& code);

}  // namespace keygrant
