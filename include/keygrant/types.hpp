// The KeyGrant licensing SDK for C++: the values a licence call answers with,
// and the state the SDK keeps on the device.
//
// A port of the Electron SDK (sdk/src/types.ts), the reference: every name and
// every stored field means what it means there.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace keygrant {

/// A lease's `major` for a key with no upper bound (shared/src/lease.ts OPEN_MAJOR).
inline constexpr std::int64_t kOpenMajor = 1'000'000;

/// The highest real major the server accepts in a request (shared/src/lease.ts MAX_MAJOR).
inline constexpr std::int64_t kMaxMajor = 99'999;

/**
 * What status() says of the licence. `Unknown` (reason `Storage`): the licence
 * file could not be read just now (an antivirus scan holding it, say): nothing
 * is known either way, so keep running, and ask again shortly.
 */
enum class LicenseState { Licensed, Trial, Expired, Invalid, Unlicensed, Unknown };

/**
 * Why a licence does not license this build. `Upgrade`: the build is NEWER than
 * the key covers (buy an upgrade). `Outdated`: the build is OLDER than the key's
 * lowest version (update the app). `Device`: the licence was activated on
 * another device (this machine changed, or the licence file was copied from
 * another one); activate again on this one. `Tampered`: the lease is not for
 * this licence at all. `Storage`: the licence file could not be read just now.
 */
enum class LicenseReason { Revoked, Upgrade, Outdated, Offline, Tampered, Device, Trial, Storage };

/** Why activate() did not activate. */
enum class ActivateError {
  InvalidKey,
  Limit,
  Revoked,
  Upgrade,
  /// The key is for a later version than this build: update the app.
  Outdated,
  Network,
  /// Reserved: the reference names it, and neither SDK returns it.
  NotConfigured,
  /// The server returned a lease that failed offline verification (an integrity or config error).
  InvalidLease,
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

std::optional<Refusal> refusalFromString(std::string_view name) noexcept;
std::optional<DeviceKind> deviceKindFromString(std::string_view name) noexcept;

std::ostream& operator<<(std::ostream& out, LicenseState value);
std::ostream& operator<<(std::ostream& out, LicenseReason value);
std::ostream& operator<<(std::ostream& out, ActivateError value);
std::ostream& operator<<(std::ostream& out, Refusal value);
std::ostream& operator<<(std::ostream& out, DeviceKind value);

namespace detail {

/**
 * The character types (char, signed and unsigned char, wchar_t, char8_t where
 * the language has it, char16_t and char32_t): a character, never a number, so
 * never an entitlement's number either (Entitlement).
 */
template <class T>
struct IsCharacter : std::false_type {};
template <>
struct IsCharacter<char> : std::true_type {};
template <>
struct IsCharacter<signed char> : std::true_type {};
template <>
struct IsCharacter<unsigned char> : std::true_type {};
template <>
struct IsCharacter<wchar_t> : std::true_type {};
#if defined(__cpp_char8_t)
template <>
struct IsCharacter<char8_t> : std::true_type {};
#endif
template <>
struct IsCharacter<char16_t> : std::true_type {};
template <>
struct IsCharacter<char32_t> : std::true_type {};

/// A pointer to narrow text (`const char*`, `char*`): a C string, which makes a text.
template <class T>
inline constexpr bool kNarrowText = std::is_same_v<std::remove_cv_t<std::remove_pointer_t<T>>, char>;

/**
 * What would otherwise slip into an Entitlement as a flag, through the
 * conversion to bool every pointer, member pointer and unscoped enum has, or
 * as a number: a character; an enum; a null pointer; and a pointer that is not
 * narrow text, which takes in the wide (`L"pro"`, `TEXT("pro")` on a UNICODE
 * Win32 build), UTF-16, UTF-32 and C++20 `u8` string literals. None of them
 * is an entitlement, so none compiles as one.
 */
template <class T>
inline constexpr bool kNeverAnEntitlement = IsCharacter<T>::value || std::is_enum_v<T> || std::is_null_pointer_v<T> ||
                                            std::is_member_pointer_v<T> || (std::is_pointer_v<T> && !kNarrowText<T>);

}  // namespace detail

/**
 * One thing a licence grants beyond the base product, as its lease signs it
 * (a member of the `ent` claim): a flag (`true`), a number (`5`) or a text
 * (`"pro"`), exactly one of them. A number is a double, as JSON and the
 * reference read one, so 5 and 5.0 are the same entitlement.
 *
 * A class over std::variant<bool, double, std::string> rather than that bare
 * variant, whose converting constructor reads against the app: built from an
 * int it does not compile (bool and double are as good as each other), and
 * built from a string literal it holds a bool wherever the standard library
 * keeps C++17's rule (a pointer converts to bool before it converts to
 * std::string; P0608 changed that, and not every library applies the change
 * in C++17 mode). Here a narrow string literal or C string is a text, any
 * number is a double, and a flag is only a bool, so `{"edition", "pro"}` and
 * `{"seats", 5}` mean what they say on every compiler, and the typed accessors
 * read a value without std::get or std::visit. What would otherwise turn into
 * the flag true or a number does not compile at all, as an entitlement or
 * compared with one: a character ('p'), an enum, a null pointer, and a wide,
 * UTF-16, UTF-32 or C++20 u8 literal (`L"pro"`, `u"pro"`, `U"pro"`, `u8"pro"`;
 * an entitlement's text is UTF-8 in a std::string). Two entitlements are
 * equal when they are the same kind and the same value: a flag never equals a
 * number or a text.
 */
class Entitlement {
 public:
  /// Which of the three it is.
  enum class Kind { Flag, Number, Text };

  Entitlement(bool flag) noexcept : value_(std::in_place_type<bool>, flag) {}
  /// Any number (an int, a double, ...), held as a double. Not a character (below).
  template <class Number, std::enable_if_t<std::is_arithmetic_v<Number> && !std::is_same_v<Number, bool> &&
                                               !detail::IsCharacter<Number>::value,
                                           int> = 0>
  Entitlement(Number number) noexcept : value_(std::in_place_type<double>, static_cast<double>(number)) {}
  /**
   * Not from a character, an enum, a null pointer, or a pointer that is not
   * narrow text (detail::kNeverAnEntitlement): each would otherwise be read as
   * a number (a character's code, 112 for 'p') or, through bool, as the flag
   * true, so it does not compile; nor does comparing an entitlement with one.
   * std::int8_t and std::uint8_t are character types too: cast one to int.
   */
  template <class T, std::enable_if_t<detail::kNeverAnEntitlement<T>, int> = 0>
  Entitlement(T) = delete;
  Entitlement(std::string text) : value_(std::in_place_type<std::string>, std::move(text)) {}
  Entitlement(const char* text) : value_(std::in_place_type<std::string>, text) {}

  Kind kind() const noexcept { return static_cast<Kind>(value_.index()); }
  bool isFlag() const noexcept { return kind() == Kind::Flag; }
  bool isNumber() const noexcept { return kind() == Kind::Number; }
  bool isText() const noexcept { return kind() == Kind::Text; }

  /// The flag; none when it is a number or a text.
  std::optional<bool> flag() const noexcept;
  /// The number; none when it is a flag or a text.
  std::optional<double> number() const noexcept;
  /// The text; none when it is a flag or a number.
  std::optional<std::string> text() const;

  friend bool operator==(const Entitlement& a, const Entitlement& b) { return a.value_ == b.value_; }
  friend bool operator!=(const Entitlement& a, const Entitlement& b) { return !(a == b); }

 private:
  std::variant<bool, double, std::string> value_;
};

/// As JSON writes it: `true`, `5.0`, `"pro"`.
std::ostream& operator<<(std::ostream& out, const Entitlement& entitlement);

/** What status(), refresh() and startTrial() answer. */
struct LicenseStatus {
  LicenseState state = LicenseState::Unlicensed;
  std::optional<LicenseReason> reason;
  std::optional<std::string> key;
  std::optional<std::int64_t> trialDaysLeft;
  /// When the trial ends (ms since the epoch).
  std::optional<std::int64_t> trialEndsAt;
  /**
   * A TEST licence: one the developer made to exercise their app, which lapses
   * on its own within 30 days. It licenses the app like any other; check this
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
   * On a `Licensed` status, from the licence's lease; on a `Trial` status,
   * from the trial's. Empty on every other status (a stored refusal included,
   * whatever the lease held grants), and when the lease grants none, so a
   * lease with no entitlements leaves the status exactly as it was before
   * entitlements were read. A member of the claim that is not a flag, a number
   * or a text is left out (docs/specs/sdk-contract.md section 2.1), and never
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
 * (sdk/src/types.ts StoredState): times are milliseconds since the epoch, and
 * the JSON field names are the same, so the file is portable between SDKs.
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

/** deactivate() behind a call that has not finished: nothing was released or dropped. */
class StalledError : public std::runtime_error {
 public:
  StalledError();
};

}  // namespace keygrant
