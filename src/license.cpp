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

bool Engine::has(const std::string& code) {
  try {
    // From what is stored only, as a status behind an unsettled call answers: no ask, no write.
    const std::optional<StoredState> state = loadForStatus(true);
    return state && grants(statusNow(*state, false, true).entitlements, code);
  } catch (...) {
    // A state that cannot be read grants nothing just now.
    return false;
  }
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
  return referencedLink(paymentLink, *state.activationId);
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
  body["kids"] = kidsJson();
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
  const StatePatch patch =
      activationPatch(Activation{*entered, activationId, lease, *evaluated.claims, now, major_, identity->kind});
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

bool License::has(const std::string& code) {
  const std::shared_ptr<detail::Engine> engine = engine_;
  return engine->has(code);
}

void License::require(const std::string& code) {
  if (!has(code)) throw MissingEntitlement(code);
}

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
