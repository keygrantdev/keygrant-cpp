#include "keygrant/license.hpp"

#include "engine.hpp"
#include "url.hpp"
#include "wire.hpp"

namespace keygrant {

namespace detail {

namespace {

/** What status() answers when the stored state cannot be read just now. */
LicenseStatus unreadable() {
  LicenseStatus status;
  status.state = LicenseState::Unknown;
  status.reason = LicenseReason::Storage;
  return status;
}

LicenseStatus unlicensed() { return LicenseStatus{}; }

/** The fingerprint an activation is made under, and what it hashes. */
struct ActivationIdentity {
  std::string fingerprint;
  std::optional<DeviceKind> kind;
};

/**
 * The device's own fingerprint; when even an activation's time cannot tell it:
 * the host name, except on an install already keyed by a machine id (a seat
 * under the host name would read as another device to every normal run after
 * it), or when there is no host-name alternate (a custom get()). None is
 * answered `network`, for the app to retry.
 */
std::optional<ActivationIdentity> activationIdentity(const Device& here, std::optional<DeviceKind> install) {
  if (here.own) return ActivationIdentity{*here.own, here.kind};
  if (install == DeviceKind::Machine || !here.hostname) return std::nullopt;
  return ActivationIdentity{*here.hostname, DeviceKind::Hostname};
}

}  // namespace

// --- the public calls ------------------------------------------------------

ActivateResult Engine::activate(const std::string& key) {
  ActivateResult result;
  // Behind a call that has not settled, nothing may be written: answered as
  // not reaching the server, to retry.
  serial([&](bool readOnly) { result = readOnly ? ActivateResult::failure(ActivateError::Network) : activateNow(key); });
  return result;
}

LicenseStatus Engine::status(bool forceOnline) {
  LicenseStatus result;
  bool stalled = false;
  try {
    serial([&](bool readOnly) {
      stalled = readOnly;
      if (!readOnly) saveUnsavedTrial();
      const std::optional<StoredState> state = loadForStatus(readOnly);
      result = state ? statusNow(*state, forceOnline, readOnly) : unreadable();
    });
  } catch (...) {
    // Never thrown out of status(): an app awaits it at launch. Nothing is
    // known either way, so the app keeps running and asks again.
    result = unreadable();
  }
  result.stalled = stalled;
  return result;
}

DeactivateResult Engine::deactivate() {
  DeactivateResult result;
  serial([&](bool readOnly) {
    // The local licence cannot be dropped safely until the call before has settled.
    if (readOnly) throw StalledError();
    result = deactivateNow();
  });
  return result;
}

LicenseStatus Engine::startTrial() {
  LicenseStatus result;
  bool stalled = false;
  try {
    serial([&](bool readOnly) {
      stalled = readOnly;
      const std::optional<StoredState> state = loadForStatus(readOnly);
      if (!state) {
        result = unreadable();
      } else if (!readOnly) {
        result = startTrialNow(*state);
      } else {
        result = trialStatus(*state, effectiveNow(*state));
      }
    });
  } catch (...) {
    result = unreadable();
  }
  result.stalled = stalled;
  return result;
}

std::optional<std::string> Engine::upgradeUrl(const std::string& paymentLink) {
  // Read-only: this runs outside the one-at-a-time order, beside any call.
  const StoredState state = load(true);
  if (!truthy(state.key) || !truthy(state.activationId)) return std::nullopt;
  // The activation id, never the key: a URL lands in browser history and logs.
  return withQueryParam(paymentLink, "client_reference_id", *state.activationId);
}

// --- what each does ----------------------------------------------------------

ActivateResult Engine::activateNow(const std::string& key) {
  const std::optional<std::string> entered = enteredKey(key);
  if (!entered) return ActivateResult::failure(ActivateError::InvalidKey);
  // Install memory protects the seat this install holds: re-activating that
  // key keeps to it; a first activation, or another key's, starts afresh.
  const StoredState stored = load();
  const bool sameSeat = stored.key && sameKey(*stored.key, *entered);
  const std::optional<DeviceKind> install = sameSeat ? installKind(stored.deviceKind) : std::nullopt;
  const std::optional<ActivationIdentity> identity =
      activationIdentity(device(timings_.activateMachineId, install), install);
  if (!identity) return ActivateResult::failure(ActivateError::Network);
  // Stamped with when it was asked for: an older ask's answer is not saved over it.
  const std::int64_t askedAt = effectiveNow(stored);
  ordered_json body = ordered_json::object();
  body["key"] = *entered;
  body["fingerprint"] = identity->fingerprint;
  if (identity->kind) body["fingerprintKind"] = to_string(*identity->kind);
  if (deviceName_) body["name"] = *deviceName_;
  body["major"] = major_;
  const std::optional<Reply> reply = post("activate", body);
  if (!reply) return ActivateResult::failure(ActivateError::Network);
  if (reply->status != 200) return ActivateResult::failure(mapServerError(errorWord(reply->body)));
  const LeaseBody answer = parseLeaseBody(reply->body);
  if (!truthy(answer.lease) || !truthy(answer.activationId)) return ActivateResult::failure(ActivateError::Network);
  const std::string& lease = *answer.lease;
  const std::string& activationId = *answer.activationId;
  const std::int64_t now = serverNow(lease, *entered, activationId,
                                     [this](const StatePatch& patch, std::int64_t guard) { save(patch, guard); });
  const LeaseCheck evaluated = evaluate(lease, now, entered, activationId, identity->kind);
  if (!evaluated.ok) return ActivateResult::failure(ActivateError::InvalidLease);
  StatePatch patch;
  patch.key = Change<std::string>::set(*entered);
  patch.activationId = Change<std::string>::set(activationId);
  patch.lease = Change<std::string>::set(lease);
  patch.checkedMajor = Change<std::int64_t>::set(major_);
  patch.keptMajor = Change<std::int64_t>::of(keptWith(std::nullopt, *evaluated.claims, now, major_));
  patch += leasedPatch();
  // What this activation keyed the install by; none for a fingerprint of unknown kind.
  patch.deviceKind = Change<DeviceKind>::of(identity->kind);
  Basis basis;
  basis.activation = Asked{askedAt, activationId, stored.verdictAt};
  save(patch, std::nullopt, basis);
  return ActivateResult::success();
}

DeactivateResult Engine::deactivateNow() {
  const StoredState state = load();
  // Nothing held is nothing to release: the seat is as free as it gets. Nor is
  // a seat another device holds this one's to release (seatElsewhere).
  DeactivateResult result{true};
  if (truthy(state.key) && truthy(state.activationId) && !seatElsewhere(state)) {
    ordered_json body = ordered_json::object();
    body["key"] = *state.key;
    body["activationId"] = *state.activationId;
    // Only a 200 is KeyGrant saying the seat is free. A 5xx, a 429 or a
    // proxy's page is an answer, but not that one; no answer at all is none.
    // The local licence is cleared whatever the server says.
    const std::optional<Reply> reply = post("deactivate", body);
    result.released = reply && reply->status == 200;
  }
  // Stamped, so that an answer to an older ask is not saved over it.
  StatePatch patch = unkeyedPatch();
  patch.verdictAt = Change<std::int64_t>::set(effectiveNow(state));
  save(patch);
  return result;
}

/**
 * Whether the activation stored is another device's: one copied or restored
 * from another PC (a roaming profile, a backup, a cloned disk) carries that
 * PC's, and releasing it would lock that PC out at its next check-in. Either a
 * device refusal that stands for this device, or a lease whose signature holds
 * (expired or not) held to a machine this run can tell is not this one
 * (heldElsewhere).
 */
bool Engine::seatElsewhere(const StoredState& state) {
  const Device here = device(timings_.machineId, state.deviceKind);
  if (standingRefusal(state, major_, DeviceView{here.ownClaim, here.canTell}) == Refusal::Device) return true;
  return truthy(state.lease) && heldElsewhere(verify(*state.lease, effectiveNow(state)), here.claims, here.canTell);
}

LicenseStatus Engine::statusNow(const StoredState& state, bool forceOnline, bool readOnly) {
  const std::int64_t now = effectiveNow(state);
  // A device refusal stands only for the device it was given for: who this is.
  std::optional<DeviceView> view;
  if (state.refused == Refusal::Device) {
    const Device here = device(timings_.machineId, state.deviceKind);
    view = DeviceView{here.ownClaim, here.canTell};
  }
  const std::optional<Refusal> refused = truthy(state.key) ? standingRefusal(state, major_, view) : std::nullopt;
  if (refused && readOnly) return invalidStatus(reasonOf(*refused), state.key);
  if (refused) return refusedStatus(state, *refused, forceOnline);
  if (truthy(state.key) && truthy(state.lease)) return licenseStatus(state, *state.lease, now, forceOnline, readOnly);
  if (state.trialStart) return trialStatus(state, now, StatusAsk{forceOnline, readOnly});
  return unlicensed();
}

LicenseStatus Engine::startTrialNow(const StoredState& state) {
  const TrialAsk ask = askTrial(state, timings_.activateMachineId);
  // The server's trial even when it could not be saved: kept for this process
  // (unsaved_) until a later call saves it.
  return trialStatus(ask.latest, effectiveNow(state));
}

/**
 * Ask the server for this device's trial (`trial`, with the earliest start the
 * device holds as `clientStart`) and save its answer: `reached` when the
 * server answered at all, `latest` the state with its trial merged in.
 */
Engine::TrialAsk Engine::askTrial(const StoredState& state, Milliseconds machineIdTimeout,
                                  std::optional<Milliseconds> timeout) {
  const Device here = device(machineIdTimeout, state.deviceKind);
  // The stash is the app's own adapter (the Windows registry, say), and it can
  // cost nothing: one that cannot be read is no evidence, and startTrial() still answers.
  std::optional<std::int64_t> stashed;
  try {
    stashed = a_.registry->readTrialStart();
  } catch (...) {
    stashed.reset();
  }
  const std::optional<std::int64_t> clientStart = earliest({state.trialStart, stashed});
  TrialAsk ask{false, state};
  try {
    // A device that cannot be told this time starts no trial: as offline.
    if (!here.own) return ask;
    ordered_json body = ordered_json::object();
    body["fingerprint"] = *here.own;
    if (here.kind) body["fingerprintKind"] = to_string(*here.kind);
    if (clientStart) body["clientStart"] = *clientStart;
    // Stamped with when it was asked for: an older answer is not saved over it.
    const Stamp asked{effectiveNow(state), state.trialAt};
    const std::optional<Reply> reply = post("trial", body, timeout);
    if (!reply) return ask;
    ask.reached = true;
    const std::optional<TrialBody> trial = reply->status == 200 ? parseTrialBody(reply->body) : std::nullopt;
    if (!trial) return ask;
    // The earliest start across all evidence: never moved forward.
    const std::int64_t trialStart =
        earliest({trial->trialStart, state.trialStart, stashed}).value_or(trial->trialStart);
    // Mirrored to the stash before the save; a stash that cannot be written
    // does not throw the server's trial away with it.
    try {
      a_.registry->writeTrialStart(trialStart);
    } catch (...) {
      // The stash is best-effort.
    }
    StatePatch patch;
    patch.trialStart = Change<std::int64_t>::set(trialStart);
    patch.trialEndsAt = Change<std::int64_t>::set(trial->trialEndsAt);
    // A trial lease this build cannot verify (signed with a key the product
    // rotated to ahead of this build, say) never replaces one it can: a trial
    // runs only on a valid lease (trialStatusOf), so taking it would end a
    // running trial. As a renewal keeps the lease it has when the answer's will
    // not verify. An answer with no lease (the trial is over) is taken as it is.
    const bool keepStored = keepsItsTrialLease(state, trial->lease);
    patch.trialLease = Change<std::string>::of(keepStored ? state.trialLease : trial->lease);
    Basis basis;
    basis.trial = asked;
    ask.latest = merged(state, patch, basis);
    // The server's trial even when it could not be saved: kept for this process until a later call saves it.
    const bool saved = saveBestEffort(patch, std::nullopt, basis);
    std::lock_guard<std::mutex> lock(unsavedMutex_);
    if (saved) {
      unsaved_.reset();
    } else {
      unsaved_ = UnsavedTrial{patch, basis};
    }
  } catch (...) {
    // Offline: whatever local trial evidence there is.
  }
  return ask;
}

/** Whether a trial answer's lease fails here while the stored one verifies (askTrial). */
bool Engine::keepsItsTrialLease(const StoredState& state, const std::optional<std::string>& answered) {
  if (!answered || !state.trialLease) return false;
  const std::int64_t now = effectiveNow(state);
  if (trialLease(*answered, now, state.deviceKind).valid) return false;
  return trialLease(*state.trialLease, now, state.deviceKind).valid;
}

/**
 * A refusal that stands for this build: answered as stored, and asked again
 * once the wait is over, or when forced, so a renewed or reinstated key heals.
 */
LicenseStatus Engine::refusedStatus(const StoredState& state, Refusal refused, bool force) {
  const LicenseStatus stored = invalidStatus(reasonOf(refused), state.key);
  if (!askDue(state, clockNow(), force)) return stored;
  return tryRenew(state).value_or(stored);
}

LicenseStatus Engine::licenseStatus(const StoredState& state, const std::string& lease, std::int64_t now,
                                    bool forceOnline, bool readOnly) {
  const LeaseCheck evaluated = evaluate(lease, now, state.key, state.activationId, state.deviceKind);
  // What the lease says offline, entitlements included: the answer whenever
  // the server gives no verdict (leaseStatus).
  const LicenseStatus offline = leaseStatus(evaluated, state.key);
  if (evaluated.ok) {
    if (readOnly) return offline;
    const StoredState held = noteKept(state, *evaluated.claims, now);
    if (forceOnline) return tryRenew(held).value_or(offline);
    // Asked, bounded in time, when the server should hear from this device
    // before the lease runs out (a newer major on an open lease, or a lease
    // nearing its end) and the wait allows (asksOnValidLease). The valid lease
    // is the answer when the ask gets no verdict.
    if (!asksOnValidLease(*evaluated.claims, held, major_, now, clockNow())) return offline;
    return tryRenew(held, timings_.statusCheck).value_or(offline);
  }
  // Locally invalid (expired, tampered, upgrade...): give the server a chance
  // to heal it before locking the user out, once the wait is over or when
  // forced; only when it is out of reach (or agrees) is the local reason the
  // answer.
  std::optional<LicenseStatus> renewed;
  if (!readOnly && askDue(state, clockNow(), forceOnline)) renewed = tryRenew(state);
  return renewed.value_or(offline);
}

/** The state with this run noted in keptMajor, saved when that changed. */
StoredState Engine::noteKept(const StoredState& state, const LeaseClaims& claims, std::int64_t now) {
  const std::optional<std::int64_t> kept = keptWith(state.keptMajor, claims, now, major_);
  if (kept == state.keptMajor) return state;
  StatePatch patch;
  patch.keptMajor = Change<std::int64_t>::of(kept);
  // About this activation: not saved onto another one stored meanwhile.
  Basis basis;
  basis.about = About{state.activationId};
  saveBestEffort(patch, std::nullopt, basis);
  StoredState held = state;
  held.keptMajor = kept;
  return held;
}

/**
 * A trial's status from what the device holds. With `ask` (a status()), a
 * trial whose lease does not verify while its stored end is still ahead is
 * asked for again first (healTrial).
 */
LicenseStatus Engine::trialStatus(const StoredState& state, std::int64_t now, const std::optional<StatusAsk>& ask) {
  // Only a valid trial lease runs a trial, to the end it was signed to, with what it grants.
  const TrialLease lease = truthy(state.trialLease) ? trialLease(*state.trialLease, now, state.deviceKind) : TrialLease{};
  const LicenseStatus status = trialStatusOf(state, now, lease.valid, lease.endsAt, lease.entitlements);
  const bool storedEndAhead = state.trialEndsAt && now < *state.trialEndsAt;
  if (!ask || ask->readOnly || lease.valid || !storedEndAhead) return status;
  return healTrial(state, ask->forceOnline).value_or(status);
}

/**
 * A trial whose lease does not verify here while its stored end is still
 * ahead: a lease signed with the product's key before it was rotated (on a
 * build that ships the new one), one from before KeyGrant signed trial leases
 * to the trial's end, or one never saved whole. The stored end is unsigned, so
 * it runs no trial on its own (trialStatusOf): the server is asked for the
 * trial again instead, under the usual waits (askDue) and bounded as a status
 * check is, and its answer stands. An ask that brings back no trial this build
 * can verify starts the wait (kRetryUnansweredMs when nothing answered,
 * kAskAgainMs when the server did), so a device that is offline, or that
 * carries a key the product no longer signs with, is not asked on every
 * status(). None when it did not ask, or nothing answered.
 */
std::optional<LicenseStatus> Engine::healTrial(const StoredState& state, bool forceOnline) {
  // The wait runs on the device clock, as askDue reads it.
  const std::int64_t askedAt = clockNow();
  if (!askDue(state, askedAt, forceOnline)) return std::nullopt;
  const TrialAsk ask = askTrial(state, timings_.machineId, timings_.statusCheck);
  std::optional<LicenseStatus> after;
  if (ask.reached) after = trialStatus(ask.latest, effectiveNow(ask.latest));
  if (after && after->state == LicenseState::Trial) return after;
  StatePatch wait;
  wait.backoffSince = Change<std::int64_t>::set(askedAt);
  wait.backoffMs = Change<std::int64_t>::set(ask.reached ? kAskAgainMs : kRetryUnansweredMs);
  saveBestEffort(wait);
  return after;
}

/** The stored state for a status, with a trial this process could not save; none when it cannot be read. */
std::optional<StoredState> Engine::loadForStatus(bool readOnly) {
  std::optional<StoredState> state;
  try {
    state = load(readOnly);
  } catch (...) {
    return std::nullopt;
  }
  std::optional<UnsavedTrial> unsaved;
  {
    std::lock_guard<std::mutex> lock(unsavedMutex_);
    unsaved = unsaved_;
  }
  // As a save would merge it: a newer trial answer stored since wins.
  if (unsaved) return merged(*state, unsaved->patch, unsaved->basis);
  return state;
}

/** Save the trial this process could not, and forget it once saved. Best-effort. */
void Engine::saveUnsavedTrial() {
  std::optional<UnsavedTrial> unsaved;
  {
    std::lock_guard<std::mutex> lock(unsavedMutex_);
    unsaved = unsaved_;
  }
  if (!unsaved || !saveBestEffort(unsaved->patch, std::nullopt, unsaved->basis)) return;
  std::lock_guard<std::mutex> lock(unsavedMutex_);
  unsaved_.reset();
}

}  // namespace detail

// --- License -------------------------------------------------------------------

License::License(LicenseConfig config) : License(std::move(config), detail::Timings{}) {}

License::License(LicenseConfig config, const detail::Timings& timings)
    : engine_(std::make_shared<detail::Engine>(std::move(config), timings)) {}

License::~License() = default;

// Each call holds the engine for as long as it runs, so a License destroyed
// while a call is stuck in an adapter leaves that call to finish safely.

ActivateResult License::activate(const std::string& key) {
  const std::shared_ptr<detail::Engine> engine = engine_;
  return engine->activate(key);
}

LicenseStatus License::status(bool forceOnline) {
  const std::shared_ptr<detail::Engine> engine = engine_;
  return engine->status(forceOnline);
}

LicenseStatus License::refresh() { return status(true); }

DeactivateResult License::deactivate() {
  const std::shared_ptr<detail::Engine> engine = engine_;
  return engine->deactivate();
}

LicenseStatus License::startTrial() {
  const std::shared_ptr<detail::Engine> engine = engine_;
  return engine->startTrial();
}

std::optional<std::string> License::upgradeUrl(const std::string& paymentLink) {
  const std::shared_ptr<detail::Engine> engine = engine_;
  return engine->upgradeUrl(paymentLink);
}

}  // namespace keygrant
