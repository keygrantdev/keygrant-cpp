// What the engine decides from what the device holds, without asking the
// server (sdk/src/offline.ts): whether a lease licenses this build, what a
// trial shows, which refusal still stands, when to ask again, and how a save
// lands on a state another process saved meanwhile.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "keygrant/types.hpp"
#include "lease.hpp"

namespace keygrant::detail {

/// How long after an answer without a lease before status() asks again on its own.
inline constexpr std::int64_t kAskAgainMs = 60 * 60'000;
/// How long after an ask that got no answer at all.
inline constexpr std::int64_t kRetryUnansweredMs = 60'000;
/// A valid lease is renewed early once less than this (or half its life) is left.
inline constexpr std::int64_t kRenewWithinMs = 7LL * 24 * 60 * 60'000;
inline constexpr std::int64_t kDayMs = 86'400'000;

/** What a patch does to one field: nothing (not in it), clear it (undefined), or set it. */
template <class T>
struct Change {
  enum class Op : unsigned char { Keep, Clear, Set };
  Op op = Op::Keep;
  T value{};

  static Change set(T v) { return Change{Op::Set, std::move(v)}; }
  static Change clear() { return Change{Op::Clear, T{}}; }
  /// From an optional: set when it has a value, cleared when not (a JS `field: undefined`).
  static Change of(const std::optional<T>& v) { return v ? set(*v) : clear(); }
  /// Whether the field is in the patch at all (`field in patch`).
  bool present() const { return op != Op::Keep; }
  /// The value as `patch.field` reads it: none unless set.
  std::optional<T> get() const { return op == Op::Set ? std::optional<T>(value) : std::nullopt; }
  bool operator==(const Change& other) const { return op == other.op && (op != Op::Set || value == other.value); }
};

/** A partial StoredState: what a save changes. */
struct StatePatch {
  Change<std::string> key;
  Change<std::string> activationId;
  Change<std::string> lease;
  Change<std::int64_t> trialStart;
  Change<std::int64_t> trialEndsAt;
  Change<std::string> trialLease;
  Change<std::int64_t> trialAt;
  Change<std::int64_t> lastSeen;
  Change<std::int64_t> checkedMajor;
  Change<std::int64_t> keptMajor;
  Change<std::int64_t> backoffSince;
  Change<std::int64_t> backoffMs;
  Change<Refusal> refused;
  Change<std::int64_t> refusedMajor;
  Change<std::vector<std::string>> refusedFor;
  Change<DeviceKind> deviceKind;
  Change<std::int64_t> verdictAt;
  Change<std::int64_t> rev;

  /// `{...this, ...other}`: every field `other` names overrides.
  StatePatch& operator+=(const StatePatch& other);
  bool operator==(const StatePatch& other) const;
};

StatePatch operator+(StatePatch a, const StatePatch& b);

/// `{...state, ...patch}`.
void apply(StoredState& state, const StatePatch& patch);

/// The names of the fields a patch sets, and of those it clears (sorted).
std::vector<std::string> setFields(const StatePatch& patch);
std::vector<std::string> clearedFields(const StatePatch& patch);

/** What a lease says about this build, offline. */
enum class LeaseVerdict { Expired, Upgrade, Outdated, Tampered, Device };

const char* to_string(LeaseVerdict verdict) noexcept;

struct LeaseCheck {
  bool ok = false;
  LeaseVerdict reason = LeaseVerdict::Tampered;
  bool test = false;
  std::optional<LeaseClaims> claims;
};

/** Whose a licence lease must be: the key and activation stored, and this device. */
struct LeaseHolder {
  std::string key;
  std::string activationId;
  /// Every `fp` claim this device answers to.
  std::vector<std::string> claims;
  /// Whether this run could tell who the device is.
  bool canTell = false;
};

/** Whether two licence keys are the same key: the server matches them without regard to case. */
bool sameKey(const std::string& a, const std::string& b);

/**
 * A verified licence lease held to this product, this holder and this build's
 * major, in order: not valid (expired, else tampered); another product
 * (tampered); another key or activation (tampered); held to its device
 * (`fpk` machine) and this run can tell and `fp` is none of its claims
 * (device); below minMajor (outdated); above major (upgrade); else licensed.
 */
LeaseCheck checkLease(const VerifyResult& v, const std::string& product, std::int64_t major, const LeaseHolder& holder);

/**
 * The status a stored licence lease gives, judged offline (checkLease), when
 * the server gives no verdict on it: licensed, with the check's `test` flag
 * (its one source) and the lease's entitlements (licensedStatus); `Expired` /
 * `Offline` once it has run out; `Invalid` with the reason, and the key, when
 * it is not for this build or this device; `Invalid` / `Tampered`, with no
 * key, when it is not this licence's at all. Only a lease that licenses
 * carries entitlements.
 */
LicenseStatus leaseStatus(const LeaseCheck& check, const std::optional<std::string>& key);

/**
 * Whether a licence lease is held to a machine that is not this device: its
 * seat keyed by a machine id (`fp`, `fpk` machine) that this run can tell is
 * not one of its own (`claims`). Read from the claims of a lease whose
 * signature holds, expired or not: a seat does not change hands when its lease
 * runs out. A seat that is another device's is not this device's to release
 * (deactivate()).
 */
bool heldElsewhere(const VerifyResult& v, const std::vector<std::string>& claims, bool canTell);

/** `kept` after running `major` at `now`: raised to it while the lease's upgrade window is open. */
std::optional<std::int64_t> keptWith(std::optional<std::int64_t> kept, const LeaseClaims& claims, std::int64_t now,
                                     std::int64_t major);

/**
 * A trial lease: verified, a trial's (`model` trial), for this product, and for
 * this device when it was started under a machine id and this run can tell.
 */
bool trialLeaseValid(const VerifyResult& v, const std::string& product, const std::vector<std::string>& claims,
                     bool canTell);

/** What a trial's status reads of its trial lease (trialLeaseOf). */
struct TrialLease {
  /// Whether it is valid here (trialLeaseValid).
  bool valid = false;
  /// The end it was signed to (its `exp`, in ms), when it is valid.
  std::optional<std::int64_t> endsAt;
  /// What it grants (its `ent`) when it is valid; none when it is not.
  std::map<std::string, Entitlement> entitlements;
};

/**
 * A trial lease, verified, as a trial's status reads it: whether it is valid
 * here (trialLeaseValid), and, when it is, the end it was signed to and its
 * entitlements. A lease that is not valid here says nothing of either.
 */
TrialLease trialLeaseOf(const VerifyResult& v, const std::string& product, const std::vector<std::string>& claims,
                        bool canTell);

/** What a check-in with a device of this kind writes of the install's key: a machine id keys it. */
StatePatch keyedBy(const std::optional<DeviceKind>& kind);

/**
 * A trial's status from what the device holds: `leaseValid` is its trial lease,
 * checked (trialLeaseValid), `leaseEndsAt` the end that lease was signed to
 * (its `exp`, in ms), when known, and `leaseEntitlements` what it grants (its
 * `ent`), which a running trial carries and no other answer does.
 *
 * A trial runs only while its trial lease is valid. The `trialEndsAt` stored
 * beside it is the server's word too, but UNSIGNED: trusted on its own,
 * editing it in the state file was a trial that never ended. KeyGrant signs a
 * trial lease to the trial's own end, so an honest trial holds a valid one for
 * every day it runs, and the signed end is the one shown; the stored end only
 * when the lease's is not known.
 */
LicenseStatus trialStatusOf(const StoredState& state, std::int64_t now, bool leaseValid,
                            std::optional<std::int64_t> leaseEndsAt = std::nullopt,
                            const std::map<std::string, Entitlement>& leaseEntitlements = {});

/** What storing the server's refusal changes. */
StatePatch refusalPatch(Refusal refused, std::int64_t major, const std::optional<std::string>& ownClaim);

/** When something was asked of the server (guarded clock, ms), and the stored stamp of that kind the asker saw. */
struct Stamp {
  std::int64_t at = 0;
  std::optional<std::int64_t> seen;
};

/** An ask about an activation, as a save of its answer sees it. */
struct Asked {
  std::int64_t at = 0;
  std::optional<std::string> activationId;
  std::optional<std::int64_t> seen;
};

/** A save about an activation that is not an answer. */
struct About {
  std::optional<std::string> activationId;
};

/** What a save is based on. */
struct Basis {
  std::optional<Asked> answer;
  std::optional<Asked> activation;
  std::optional<About> about;
  std::optional<Stamp> trial;
};

enum class OutOfDate { No, Activation, Answer };

/** Whether a stored stamp is a NEWER answer than `stamp`'s: not the one it saw, and later. */
bool outdated(const std::optional<std::int64_t>& stored, std::int64_t at, const std::optional<std::int64_t>& seen);

/** Whether an ask's answer is out of date by the time it is saved. */
OutOfDate superseded(const StoredState& latest, const Asked& asked);

/** A patch without the verdict fields. */
StatePatch withoutVerdict(StatePatch patch);

/** Only the trial evidence of a patch. */
StatePatch trialOnly(const StatePatch& patch);

/** A patch merged onto the state as stored now, by what it is based on. */
StoredState merged(const StoredState& latest, const StatePatch& patch, const Basis& basis = {});

/** What a new lease from the server clears: any refusal, and the wait. */
StatePatch leasedPatch();

/** What a deactivated device keeps of its licence: nothing but its trial evidence and clock guard. */
StatePatch unkeyedPatch();

/** The stored refusal that still stands for a build running `major`, or none. */
struct DeviceView {
  std::optional<std::string> ownClaim;
  bool canTell = false;
};
std::optional<Refusal> standingRefusal(const StoredState& state, std::int64_t major,
                                       const std::optional<DeviceView>& device);

/** Whether to ask the server now. `clockNow` is the device clock as it is, not the guarded one. */
bool askDue(const StoredState& state, std::int64_t clockNow, bool force);

/** Whether a valid lease is near enough its end to renew it early. */
bool renewsEarly(const LeaseClaims& claims, std::int64_t now);

}  // namespace keygrant::detail
