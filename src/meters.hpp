// Metered uses, decided without I/O: what use() may ask for, whether it asks the server now, what an answer does, what a
// device may spend while it gets none, the figures a status shows, and what a request reports of the journal and what
// the server's answers confirm of it. How the journal lands on the stored state is journal.hpp; the engine that makes
// the request is spend.cpp.
//
// A meter never takes a licence away: nothing here is read to decide whether the app is licensed.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "json_util.hpp"
#include "keygrant/types.hpp"
#include "lease.hpp"
#include "offline.hpp"
#include "wire.hpp"

namespace keygrant::detail {

/// The most entries the journal holds, and a report carries (the server reads no more).
inline constexpr std::size_t kMaxPendingSpends = 64;

/**
 * The most `Void` entries the journal holds: a void is the take-back of a spend that was refused after it got no answer,
 * and a device out of reach that keeps trying a spend its allowance cannot cover would otherwise fill the journal,
 * leaving no room for the spends it can make. A new void past them takes the place of the oldest.
 */
inline constexpr std::size_t kMaxPendingVoids = 16;

/// How long an entry is kept unreported (ms): twice the longest lease. The tenant loses a report, never the customer a use.
inline constexpr std::int64_t kPendingMaxMs = 180LL * 86'400'000;

/// The longest meter code a call may name, in UTF-16 units: an entitlement's name.
inline constexpr std::size_t kMaxMeterCode = 32;

/// `a + b` for the non-negative whole numbers of a stored state, held to the safe integers' range.
inline std::int64_t saturatingSum(std::int64_t a, std::int64_t b) {
  return a > kMaxSafeInteger - b ? kMaxSafeInteger : a + b;
}

// --- what was asked ------------------------------------------------------------------------

/** What use(meter, n) asks for, or why it asks nothing: the amount is judged before the code. */
struct UseAsked {
  /// Set when nothing is to be asked: `InvalidAmount` or `NoMeter`.
  std::optional<UseError> refused;
  std::string meter;
  std::int64_t n = 0;
};

UseAsked askedFor(const std::string& meter, std::int64_t n);

// --- the figures ---------------------------------------------------------------------------

/// A meter's figure without the time of the answer that gave it.
BaseFigure baseOf(const MeterFigure& figure);

/// Uses this device spent that KeyGrant has not heard of: the `n` of a meter's `Offline` and `Unanswered` entries, never a `Void`.
std::int64_t unsyncedOf(const std::vector<SpendEntry>& spends, const std::string& meter);

/**
 * The base figures of each meter: its stored figure (a `use` answer's) when that figure is at or after the lease's own
 * second (`iat`, seconds), else the lease's `met` member, else none, and the meter is unknown on this device. A meter the
 * newest lease no longer names, and no newer answer names, is gone. A stored figure replaces the lease's whole member:
 * its total, its allowance and its `resets` too.
 */
Meters figuresOf(const std::optional<Meters>& met, std::int64_t iat, const std::map<std::string, MeterFigure>& stored);

/// The base figures of the lease that licenses this device and the answers stored beside it.
Meters figuresFor(const LeaseClaims& claims, const StoredState& state);

/// What is left of `meter` by `state`: its base figure less the unsynced spends, never below 0; 0 for a meter with no figure.
std::int64_t leftOf(const LeaseClaims& claims, const StoredState& state, const std::string& meter);

/// status().meters: each meter with base figures, its `left` net of this device's unsynced spends and never below 0.
std::map<std::string, MeterStatus> meterStatusOf(const Meters& figures, const std::vector<SpendEntry>& spends);

/// A Licensed status from a key's lease with its meters (meterStatusOf); any other status, and a lease with no meter, unchanged.
LicenseStatus meteredStatus(LicenseStatus status, const LeaseClaims& claims, const StoredState& state);

/** What a spend with no answer comes to (offlineSpend). */
struct OfflineVerdict {
  enum class Kind { Allowed, NoMeter, OfflineLimit };
  Kind kind = Kind::NoMeter;
  /// Allowed: what is left after the spend. OfflineLimit: what is left now, never below 0.
  std::int64_t left = 0;
  /// Allowed: the most the meter's unsynced spends may come to with it (the smaller of the allowance and what the
  /// figure says is left), which the journal judges again where the spend lands.
  std::int64_t limit = 0;

  bool allowed() const { return kind == Kind::Allowed; }
};

/**
 * Whether a spend with no answer is allowed, from the meter's base figures and the journal: its unsynced spends with this
 * one at most the figure's `offline` allowance and at most its `left`. A meter with no base figure is `NoMeter`; an
 * allowance of 0 refuses every spend.
 */
OfflineVerdict offlineSpend(const Meters& figures, const std::vector<SpendEntry>& spends, const std::string& meter,
                            std::int64_t n);

// --- when to ask ---------------------------------------------------------------------------

/**
 * Whether the journal's wait is running at `now` (the guarded clock): with k `Unanswered` and `Void` entries (k at least
 * 1), until `min(kAskAgainMs, kRetryUnansweredMs x 2^(k-1))` after the newest of them was opened: a minute after one, two
 * after two, four, eight, and an hour from the seventh on. An entry from after `now` (a clock that jumped ahead and was
 * corrected) is no wait that can be measured, and is left out of the count and of the newest.
 */
bool journalWaiting(const std::vector<SpendEntry>& spends, std::int64_t now);

/**
 * Whether the wait of an uncovered spend runs at `now` (the guarded clock): a `Void` opened less than a minute ago. A
 * void opened after `now`, or after the device clock (`clockNow`), is no wait that can be measured and is left out.
 */
bool voidWaiting(const std::vector<SpendEntry>& spends, std::int64_t now, std::int64_t clockNow);

/**
 * Whether use() asks the server. A spend the offline allowance would refuse (`covered` false) asks whatever the waits of
 * an outage, but for a wait of its own (voidWaiting); a spend the allowance covers asks every time, except while a
 * no-answer wait runs (`backoffSince` set, `backoffMs` a minute, and the device clock `clockNow` at or after it and
 * before its end) and while the journal's wait runs (journalWaiting, by the guarded clock `now`).
 */
bool useAsksNow(const StoredState& state, std::int64_t clockNow, std::int64_t now, bool covered);

/// Whether the journal makes status() ask a valid lease's server now: it holds an entry and its wait is not running.
bool journalAsks(const std::vector<SpendEntry>& spends, std::int64_t now);

// --- reports -------------------------------------------------------------------------------

/**
 * The report a body carries: the journal's entries, oldest first, at most kMaxPendingSpends, a void marked; none for an
 * empty journal (the member is absent, never an empty list).
 */
std::optional<ordered_json> spendsReported(const std::vector<SpendEntry>& spends);

/** A report the server names as recorded: how many it holds of a spend, or that it took a void. */
struct Confirmed {
  std::string id;
  bool voided = false;
  /// For a count: how many of it the server holds (at least 1).
  std::int64_t n = 0;

  static Confirmed count(std::string id, std::int64_t n) { return Confirmed{std::move(id), false, n}; }
  static Confirmed taken(std::string id) { return Confirmed{std::move(id), true, 0}; }
  bool operator==(const Confirmed& other) const { return id == other.id && voided == other.voided && n == other.n; }
};

/**
 * The reports an answer names as recorded (`spends`), read leniently: a member of another shape confirms nothing. A void
 * names itself (its `void` is JSON true); any other names how many the server holds (a whole number from 1 to 2^53 - 1).
 * No answer at all, a 5xx and a 429 confirm nothing, whatever their body says.
 */
std::vector<Confirmed> spendsConfirmed(const Reply* response);

// --- what an answer does -------------------------------------------------------------------

/** What a `use` answer comes to. */
enum class UseOutcomeKind { Spent, UsedUp, NoMeter, Refused, Unanswered };

const char* to_string(UseOutcomeKind kind) noexcept;

/** What a `use` answer is judged from. */
struct UseInput {
  /// The answer; none when there was none at all (the request threw or timed out).
  const Reply* response = nullptr;
  /// When the request was made, by the device clock (ms): where the wait before asking again starts.
  std::int64_t askedAt = 0;
  /// This build's major, sent with the request.
  std::int64_t major = 1;
  /// What the request read this device as: a machine id keys the install.
  std::optional<DeviceKind> kind;
  /// The claim of this device's own fingerprint, which a device refusal is held to.
  std::optional<std::string> ownClaim;
};

/** What a `use` answer does. */
struct UseOutcome {
  UseOutcomeKind kind = UseOutcomeKind::Unanswered;
  /// The refusal, when the server's verdict was one.
  std::optional<Refusal> refusal;
  /// What the answer says is left, for `UsedUp`.
  std::optional<std::int64_t> left;
  /// What is saved as the answer to the ask.
  StatePatch patch;
  /// The figures saved (the newer by `at` is kept).
  std::map<std::string, MeterFigure> figures;
  /// The reports the answer confirms.
  std::vector<Confirmed> confirmed;
};

/**
 * What a `use` answer saves and says. No answer at all: the wait (kRetryUnansweredMs) and the device's kind. A 5xx or a
 * 429, and any answer that is neither a spend nor a recognised word: nothing, the offline path. A 200 is a spend only with
 * `spent: true`. On any other status, `used-up`, `unknown-meter` and a validate's verdicts (refusalOf, stored as a
 * validate's are) are read; each saves the reports it confirms, and a verdict its refusal.
 */
UseOutcome useOutcome(const UseInput& input);

}  // namespace keygrant::detail
