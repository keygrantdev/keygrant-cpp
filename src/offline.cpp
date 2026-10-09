#include "offline.hpp"

#include <algorithm>

#include "utf.hpp"
#include "wire.hpp"

// Every StoredState field, for what is done to all of them alike.
#define KEYGRANT_STATE_FIELDS(X)                                                                          \
  X(key) X(activationId) X(lease) X(trialStart) X(trialEndsAt) X(trialLease) X(trialAt) X(lastSeen)     \
  X(checkedMajor) X(keptMajor) X(backoffSince) X(backoffMs) X(refused) X(refusedMajor) X(refusedFor) \
  X(deviceKind) X(verdictAt) X(rev) X(purchaseToken) X(purchaseTokenAt) X(claimAskedAt) X(trialReportedAt)

namespace keygrant::detail {

namespace {

template <class T>
void applyField(std::optional<T>& field, const Change<T>& change) {
  if (change.op == Change<T>::Op::Set) {
    field = change.value;
  } else if (change.op == Change<T>::Op::Clear) {
    field.reset();
  }
}

template <class T>
void keepOut(Change<T>& change) {
  change = Change<T>{};
}

bool contains(const std::vector<std::string>& list, const std::string& value) {
  return std::find(list.begin(), list.end(), value) != list.end();
}

LeaseCheck refused(LeaseVerdict reason) {
  LeaseCheck check;
  check.reason = reason;
  return check;
}

/** How out of date a save is by what it is based on (see superseded). */
OutOfDate outOfDate(const StoredState& latest, const Basis& basis) {
  if (basis.answer) return superseded(latest, *basis.answer);
  // An activation's answer on the same seat, activated again: an answer like
  // any, so a refusal another process asked for later is not undone by it.
  if (basis.activation) {
    const bool same = latest.activationId == basis.activation->activationId;
    return same && outdated(latest.verdictAt, basis.activation->at, basis.activation->seen) ? OutOfDate::Answer
                                                                                          : OutOfDate::No;
  }
  if (basis.about && latest.activationId != basis.about->activationId) return OutOfDate::Activation;
  return OutOfDate::No;
}

}  // namespace

std::int64_t majorOf(std::optional<std::int64_t> configured) {
  return std::max<std::int64_t>(1, configured.value_or(1));
}

StatePatch& StatePatch::operator+=(const StatePatch& other) {
#define KEYGRANT_OVERLAY(f) \
  if (other.f.present()) f = other.f;
  KEYGRANT_STATE_FIELDS(KEYGRANT_OVERLAY)
#undef KEYGRANT_OVERLAY
  return *this;
}

bool StatePatch::operator==(const StatePatch& other) const {
#define KEYGRANT_SAME(f) \
  if (!(f == other.f)) return false;
  KEYGRANT_STATE_FIELDS(KEYGRANT_SAME)
#undef KEYGRANT_SAME
  return true;
}

StatePatch operator+(StatePatch a, const StatePatch& b) {
  a += b;
  return a;
}

void apply(StoredState& state, const StatePatch& patch) {
  // A field the patch writes or clears is this SDK's from now on: a value of
  // it kept as another SDK wrote it (otherFields) goes.
#define KEYGRANT_APPLY(f)                                \
  applyField(state.f, patch.f);                          \
  if (patch.f.present()) state.otherFields.erase(#f);
  KEYGRANT_STATE_FIELDS(KEYGRANT_APPLY)
#undef KEYGRANT_APPLY
}

std::vector<std::string> setFields(const StatePatch& patch) {
  std::vector<std::string> out;
#define KEYGRANT_SET(f) \
  if (patch.f.get()) out.emplace_back(#f);
  KEYGRANT_STATE_FIELDS(KEYGRANT_SET)
#undef KEYGRANT_SET
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<std::string> clearedFields(const StatePatch& patch) {
  std::vector<std::string> out;
#define KEYGRANT_CLEARED(f) \
  if (patch.f.present() && !patch.f.get()) out.emplace_back(#f);
  KEYGRANT_STATE_FIELDS(KEYGRANT_CLEARED)
#undef KEYGRANT_CLEARED
  std::sort(out.begin(), out.end());
  return out;
}

const char* to_string(LeaseVerdict verdict) noexcept {
  switch (verdict) {
    case LeaseVerdict::Expired: return "expired";
    case LeaseVerdict::Upgrade: return "upgrade";
    case LeaseVerdict::Outdated: return "outdated";
    case LeaseVerdict::Tampered: return "tampered";
    case LeaseVerdict::Device: return "device";
  }
  return "tampered";
}

bool sameKey(const std::string& a, const std::string& b) { return jsUpper(a) == jsUpper(b); }

LeaseCheck checkLease(const VerifyResult& v, const std::string& product, std::int64_t major, const LeaseHolder& holder) {
  if (!v.valid || !v.claims) {
    return refused(v.reason == VerifyFailure::Expired ? LeaseVerdict::Expired : LeaseVerdict::Tampered);
  }
  const LeaseClaims& c = *v.claims;
  if (c.product != product) return refused(LeaseVerdict::Tampered);
  if (!sameKey(c.sub, holder.key) || c.aid != holder.activationId) return refused(LeaseVerdict::Tampered);
  // Held to its device only when its seat is keyed by a machine id (`fpk`): a
  // host name changes with a rename or an architecture switch. Not judged at
  // all when this run cannot tell who the device is.
  const bool held = c.fp.has_value() && c.fpk == DeviceKind::Machine;
  if (held && holder.canTell && !contains(holder.claims, *c.fp)) return refused(LeaseVerdict::Device);
  if (major < c.minMajor.value_or(1)) return refused(LeaseVerdict::Outdated);
  if (major > c.major) return refused(LeaseVerdict::Upgrade);
  LeaseCheck check;
  check.ok = true;
  check.test = c.test.value_or(false);
  check.claims = c;
  return check;
}

LicenseStatus leaseStatus(const LeaseCheck& check, const std::optional<std::string>& key) {
  if (check.ok) {
    // With the lease's own entitlements: a check that names no claims grants none.
    return licensedStatus(key, check.test, check.claims ? check.claims->ent : std::map<std::string, Entitlement>{});
  }
  switch (check.reason) {
    case LeaseVerdict::Expired: {
      LicenseStatus expired;
      expired.state = LicenseState::Expired;
      expired.reason = LicenseReason::Offline;
      expired.key = key;
      return expired;
    }
    case LeaseVerdict::Upgrade: return invalidStatus(LicenseReason::Upgrade, key);
    case LeaseVerdict::Outdated: return invalidStatus(LicenseReason::Outdated, key);
    case LeaseVerdict::Device: return invalidStatus(LicenseReason::Device, key);
    case LeaseVerdict::Tampered: break;
  }
  // Not this licence at all: no key to show.
  return invalidStatus(LicenseReason::Tampered, std::nullopt);
}

bool heldElsewhere(const VerifyResult& v, const std::vector<std::string>& claims, bool canTell) {
  // The signature must hold: valid, or merely expired.
  const bool signedHere = v.valid || v.reason == VerifyFailure::Expired;
  if (!signedHere || !v.claims) return false;
  const LeaseClaims& c = *v.claims;
  if (!c.fp || c.fpk != DeviceKind::Machine || !canTell) return false;
  return !contains(claims, *c.fp);
}

std::optional<std::int64_t> keptWith(std::optional<std::int64_t> kept, const LeaseClaims& claims, std::int64_t now,
                                     std::int64_t major) {
  if (!claims.upgradeUntil || now >= *claims.upgradeUntil * 1000) return kept;
  return std::max(kept.value_or(0), major);
}

bool trialLeaseValid(const VerifyResult& v, const std::string& product, const std::vector<std::string>& claims,
                     bool canTell) {
  // A licence's lease in the trial's place is not a trial: the server signs a trial's as `model: trial`.
  if (!v.valid || !v.claims || v.claims->model != "trial" || v.claims->product != product) return false;
  return v.claims->fpk != DeviceKind::Machine || !canTell || contains(claims, v.claims->aid);
}

TrialLease trialLeaseOf(const VerifyResult& v, const std::string& product, const std::vector<std::string>& claims,
                        bool canTell) {
  if (!trialLeaseValid(v, product, claims, canTell)) return TrialLease{};
  return TrialLease{true, v.claims->exp * 1000, v.claims->ent};
}

StatePatch keyedBy(const std::optional<DeviceKind>& kind) {
  StatePatch patch;
  if (kind == DeviceKind::Machine) patch.deviceKind = Change<DeviceKind>::set(DeviceKind::Machine);
  return patch;
}

LicenseStatus trialStatusOf(const StoredState& state, std::int64_t now, bool leaseValid,
                            std::optional<std::int64_t> leaseEndsAt,
                            const std::map<std::string, Entitlement>& leaseEntitlements) {
  LicenseStatus status;
  // No trial evidence at all: the trial was never started, not expired.
  if (!state.trialStart && !state.trialEndsAt && !state.trialLease) {
    status.state = LicenseState::Unlicensed;
    return status;
  }
  // Only a valid trial lease runs a trial: the stored end alone is unsigned.
  if (!leaseValid) {
    status.state = LicenseState::Expired;
    status.reason = LicenseReason::Trial;
    return status;
  }
  status.state = LicenseState::Trial;
  // The end the lease was signed to; the stored one only when the lease's is not known.
  status.trialEndsAt = state.trialEndsAt;
  if (leaseEndsAt) status.trialEndsAt = leaseEndsAt;
  if (status.trialEndsAt) {
    const std::int64_t left = *status.trialEndsAt - now;
    status.trialDaysLeft = left > 0 ? (left + kDayMs - 1) / kDayMs : 0;
  }
  // What the trial lease grants rides only on a running trial.
  status.entitlements = leaseEntitlements;
  return status;
}

StatePatch refusalPatch(Refusal refusal, std::int64_t major, const std::optional<std::string>& ownClaim) {
  StatePatch patch;
  patch.refused = Change<Refusal>::set(refusal);
  if (refusal == Refusal::Revoked) {
    patch.lease = Change<std::string>::clear();
    patch.refusedMajor = Change<std::int64_t>::clear();
    patch.refusedFor = Change<std::vector<std::string>>::clear();
  } else if (refusal == Refusal::Device) {
    patch.refusedMajor = Change<std::int64_t>::clear();
    patch.refusedFor = Change<std::vector<std::string>>::set(ownClaim ? std::vector<std::string>{*ownClaim}
                                                                      : std::vector<std::string>{});
  } else {
    patch.refusedMajor = Change<std::int64_t>::set(major);
    patch.refusedFor = Change<std::vector<std::string>>::clear();
  }
  return patch;
}

bool outdated(const std::optional<std::int64_t>& stored, std::int64_t at, const std::optional<std::int64_t>& seen) {
  return stored != seen && stored.has_value() && *stored > at;
}

OutOfDate superseded(const StoredState& latest, const Asked& asked) {
  if (latest.activationId != asked.activationId) return OutOfDate::Activation;
  return outdated(latest.verdictAt, asked.at, asked.seen) ? OutOfDate::Answer : OutOfDate::No;
}

StatePatch withoutVerdict(StatePatch patch) {
  keepOut(patch.lease);
  keepOut(patch.refused);
  keepOut(patch.refusedMajor);
  keepOut(patch.refusedFor);
  keepOut(patch.backoffSince);
  keepOut(patch.backoffMs);
  keepOut(patch.checkedMajor);
  keepOut(patch.verdictAt);
  return patch;
}

StatePatch trialOnly(const StatePatch& patch) {
  StatePatch kept;
  kept.trialStart = patch.trialStart;
  kept.trialEndsAt = patch.trialEndsAt;
  kept.trialLease = patch.trialLease;
  kept.trialAt = patch.trialAt;
  return kept;
}

StoredState merged(const StoredState& latest, const StatePatch& patch, const Basis& basis) {
  const std::optional<Asked>& stamp = basis.answer ? basis.answer : basis.activation;
  StatePatch applied = patch;
  if (stamp) applied.verdictAt = Change<std::int64_t>::set(stamp->at);
  if (basis.trial) applied.trialAt = Change<std::int64_t>::set(basis.trial->at);
  const OutOfDate state = outOfDate(latest, basis);
  if (state == OutOfDate::Activation) applied = trialOnly(applied);
  if (state == OutOfDate::Answer) applied = withoutVerdict(applied);
  if (basis.trial && outdated(latest.trialAt, basis.trial->at, basis.trial->seen)) {
    // A trial answer older than the stored one: its end and lease, taken together from the newest.
    keepOut(applied.trialEndsAt);
    keepOut(applied.trialLease);
    keepOut(applied.trialAt);
  }
  const bool aboutStored = basis.purchase && latest.purchaseToken == basis.purchase->token &&
                           (!basis.purchase->at || latest.purchaseTokenAt == basis.purchase->at);
  if (basis.purchase && !aboutStored) {
    // About a secret no longer stored (or, for a wait, one whose link was handed
    // out again since): another process's new one, or its cleared wait, stays.
    keepOut(applied.purchaseToken);
    keepOut(applied.purchaseTokenAt);
    keepOut(applied.claimAskedAt);
  }
  const std::optional<std::int64_t> trialStart = earliest({latest.trialStart, applied.trialStart.get()});
  StoredState result = latest;
  apply(result, applied);
  if (trialStart) result.trialStart = trialStart;
  // About the stored activation: its kept major only goes up, a renewal's none
  // included. Unbased (a deactivation) or a fresh activation: as patched.
  std::optional<std::optional<std::string>> about;
  if (stamp) {
    about.emplace(stamp->activationId);
  } else if (basis.about) {
    about.emplace(basis.about->activationId);
  }
  if (about && latest.activationId == *about) {
    std::optional<std::int64_t> kept = latest.keptMajor;
    const std::optional<std::int64_t> noted = applied.keptMajor.get();
    if (noted && (!kept || *noted > *kept)) kept = noted;
    if (kept) result.keptMajor = kept;
  }
  return result;
}

StatePatch leasedPatch() {
  StatePatch patch;
  patch.refused = Change<Refusal>::clear();
  patch.refusedMajor = Change<std::int64_t>::clear();
  patch.refusedFor = Change<std::vector<std::string>>::clear();
  patch.backoffSince = Change<std::int64_t>::clear();
  patch.backoffMs = Change<std::int64_t>::clear();
  return patch;
}

StatePatch unkeyedPatch() {
  StatePatch patch;
  patch.key = Change<std::string>::clear();
  patch.activationId = Change<std::string>::clear();
  patch.lease = Change<std::string>::clear();
  patch.checkedMajor = Change<std::int64_t>::clear();
  patch.keptMajor = Change<std::int64_t>::clear();
  // Install memory protects a current seat; with none, the next activation is free of it.
  patch.deviceKind = Change<DeviceKind>::clear();
  return patch + leasedPatch();
}

StatePatch noPurchasePatch() {
  StatePatch patch;
  patch.purchaseToken = Change<std::string>::clear();
  patch.purchaseTokenAt = Change<std::int64_t>::clear();
  patch.claimAskedAt = Change<std::int64_t>::clear();
  return patch;
}

std::optional<Refusal> standingRefusal(const StoredState& state, std::int64_t major,
                                       const std::optional<DeviceView>& device) {
  if (!state.refused) return std::nullopt;
  const Refusal refusal = *state.refused;
  // A device verdict stands for the device it was given for, by its OWN claim,
  // and only while this run can tell it is still that one.
  if (refusal == Refusal::Device && state.refusedFor) {
    const std::optional<std::string> own = device && device->canTell ? device->ownClaim : std::nullopt;
    return own && contains(*state.refusedFor, *own) ? state.refused : std::nullopt;
  }
  if (refusal == Refusal::Revoked || refusal == Refusal::Device || !state.refusedMajor) return state.refused;
  const bool stands = refusal == Refusal::Upgrade ? major >= *state.refusedMajor : major <= *state.refusedMajor;
  return stands ? state.refused : std::nullopt;
}

bool askDue(const StoredState& state, std::int64_t clockNow, bool force) {
  if (force || !state.backoffSince || clockNow < *state.backoffSince) return true;
  return clockNow - *state.backoffSince >= state.backoffMs.value_or(kAskAgainMs);
}

bool renewsEarly(const LeaseClaims& claims, std::int64_t now) {
  // In doubles, as the reference computes it (half a life may be a fraction).
  const double left = static_cast<double>(claims.exp) * 1000.0 - static_cast<double>(now);
  const double life = (static_cast<double>(claims.exp) - static_cast<double>(claims.iat)) * 1000.0;
  return left < std::min(static_cast<double>(kRenewWithinMs), life / 2.0);
}

bool trialReportDue(const StoredState& state, std::int64_t now) {
  const auto recent = [now](const std::optional<std::int64_t>& at) { return at && now - *at < kTrialReportMs; };
  return !recent(state.trialAt) && !recent(state.trialReportedAt);
}

}  // namespace keygrant::detail
