// Metered uses: what License::use() answers, what a status shows of a key's meters, and what the stored state keeps of them.
#pragma once

#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

namespace keygrant {

/// The most uses one spend may ask for, and the most a figure of a meter is when the server signs it.
inline constexpr std::int64_t kMaxMeterUses = 1'000'000'000;

/**
 * One meter of a key, as LicenseStatus::meters shows it. Every figure is a whole number; the server signs none past
 * 1,000,000,000.
 */
struct MeterStatus {
  /// What is left: the newest figure (the lease's, or the last answer to a spend), less what this device spent and has
  /// not reported. Never below 0.
  std::int64_t left = 0;
  /// The uses the key holds, for good or for this billing period.
  std::int64_t total = 0;
  /// How many uses this device may spend while KeyGrant cannot be reached.
  std::int64_t offline = 0;
  /// Uses this device spent that KeyGrant has not heard of yet.
  std::int64_t unsynced = 0;
  /// True for a meter that comes back to its full uses each billing period.
  bool resets = false;

  bool operator==(const MeterStatus& other) const {
    return left == other.left && total == other.total && offline == other.offline && unsynced == other.unsynced &&
           resets == other.resets;
  }
  bool operator!=(const MeterStatus& other) const { return !(*this == other); }
};

/**
 * Why License::use() did not spend. A refusal is an answer, never an exception.
 *
 * `InvalidAmount`: `n` is not a whole number from 1 to kMaxMeterUses (a bug in the app; nothing was asked). `NoMeter`:
 * the key has no meter of that code: the server's word; or, with no answer, no figure on this device names it; or a
 * code no meter can have (empty, or longer than 32 UTF-16 units). `UsedUp`: online, fewer left than asked
 * (UseResult::left says how many, when the server did). `OfflineLimit`: KeyGrant could not be reached and the spend is
 * past this device's offline allowance for the meter, or past what it knows is left (every spend, for an allowance of
 * 0). `NotLicensed`: no key's lease licenses this build on this device (a device trial and an offline licence file have
 * no meters); nothing was asked: read status(). `Storage`: the state could not be read just now, or an adapter threw
 * where there is no word for it: try again shortly. `Revoked`, `Upgrade`, `Outdated`, `Device`: the server's verdict on
 * the key, this build or this device, stored as a renewal's is and as status() says it.
 */
enum class UseError {
  InvalidAmount,
  NoMeter,
  UsedUp,
  OfflineLimit,
  NotLicensed,
  Storage,
  Revoked,
  Upgrade,
  Outdated,
  Device,
};

/// The Electron SDK's words: "invalid-amount", "used-up", "offline-limit", ...
const char* to_string(UseError value) noexcept;
std::ostream& operator<<(std::ostream& out, UseError value);

/**
 * What License::use() did: spent, or why not.
 *
 *   const keygrant::UseResult spent = license.use("exports", 3);
 *   if (spent.ok) { ...do the work the three uses count... }
 *   else if (*spent.error == keygrant::UseError::UsedUp) { ...offer a top-up; spent.left says how many are left... }
 */
struct UseResult {
  /// Whether the uses were spent: do the work they count.
  bool ok = false;
  /// Why not, when `ok` is false.
  std::optional<UseError> error;
  /// What is left of the meter now, net of this device's unsynced spends. Given when `ok`, and on a refusal for
  /// UsedUp (the server's word, when it gave one) and OfflineLimit; none otherwise.
  std::optional<std::int64_t> left;
  /// True when KeyGrant could not be asked and the spend came from this device's offline allowance (it is reported
  /// with the next request that can carry it).
  bool offline = false;

  /// The uses were spent, `left` are left; `offline` when from the allowance.
  static UseResult spent(std::int64_t left, bool offline = false) {
    UseResult result;
    result.ok = true;
    result.left = left;
    result.offline = offline;
    return result;
  }
  /// The uses were not spent, for `why`; `left` where the error gives one.
  static UseResult refused(UseError why, std::optional<std::int64_t> left = std::nullopt) {
    UseResult result;
    result.error = why;
    result.left = left;
    return result;
  }

  bool operator==(const UseResult& other) const {
    return ok == other.ok && error == other.error && left == other.left && offline == other.offline;
  }
  bool operator!=(const UseResult& other) const { return !(*this == other); }
};

std::ostream& operator<<(std::ostream& out, const UseResult& result);

/// Which kind of entry a SpendEntry is in the journal of unsynced spends.
enum class SpendKind {
  /// Spends made with no request sent: one entry for each meter, its count growing with each later such spend.
  Offline,
  /// One spend whose own request got no answer, made from the allowance. Its count never changes: the server may hold it.
  Unanswered,
  /// One spend whose own request got no answer and that was refused offline, reported so that the server takes it back
  /// if it recorded it.
  Void,
};

/// The word stored for it: "offline", "unanswered", "void".
const char* to_string(SpendKind value) noexcept;
std::optional<SpendKind> spendKindFromString(std::string_view name) noexcept;
std::ostream& operator<<(std::ostream& out, SpendKind value);

/**
 * A spend this device made that KeyGrant has not confirmed (StoredState::spends): kept in the stored state, reported
 * with the next request, and dropped only when the server says it recorded it.
 */
struct SpendEntry {
  /// The spend's id: 16 random bytes in base64url (22 characters), never reused.
  std::string id;
  /// The meter's code.
  std::string meter;
  /// How many uses.
  std::int64_t n = 0;
  /// When (the guarded device clock, ms since the epoch) the entry was opened.
  std::int64_t at = 0;
  SpendKind kind = SpendKind::Offline;

  bool operator==(const SpendEntry& other) const {
    return id == other.id && meter == other.meter && n == other.n && at == other.at && kind == other.kind;
  }
  bool operator!=(const SpendEntry& other) const { return !(*this == other); }
};

std::ostream& operator<<(std::ostream& out, const SpendEntry& entry);

/**
 * A meter's figures as the newest `use` answer gave them (StoredState::meterFigures): newer than the stored lease's own
 * until the lease is renewed.
 */
struct MeterFigure {
  /// What the key had left when the server answered, every device's reported spends counted.
  std::int64_t left = 0;
  /// The uses the key holds.
  std::int64_t total = 0;
  /// The allowance each of its devices may spend with no answer.
  std::int64_t offline = 0;
  /// Whether the meter comes back each billing period.
  bool resets = false;
  /// The server's time (ms since the epoch).
  std::int64_t at = 0;

  bool operator==(const MeterFigure& other) const {
    return left == other.left && total == other.total && offline == other.offline && resets == other.resets &&
           at == other.at;
  }
  bool operator!=(const MeterFigure& other) const { return !(*this == other); }
};

std::ostream& operator<<(std::ostream& out, const MeterFigure& figure);

}  // namespace keygrant
