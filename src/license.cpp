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

ActivateResult Engine::activate(const std::string& key, const ActivateOptions& options) {
  ActivateResult result;
  // Behind a call that has not settled, nothing may be written: answered as
  // not reaching the server, to retry.
  serial([&](bool readOnly) {
    result = readOnly ? ActivateResult::failure(ActivateError::Network) : activateNow(key, options);
  });
  return result;
}

LicenseStatus Engine::status(bool forceOnline) {
  LicenseStatus result;
  bool stalled = false;
  try {
    serial([&](bool readOnly) {
      stalled = readOnly;
      if (!readOnly) {
        saveUnsavedTrial();
        saveUnsavedJournal();
      }
      const std::optional<View> view = loadForStatus(readOnly);
      result = view ? statusNow(*view, forceOnline, readOnly) : unreadable();
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
    const std::optional<View> view = loadForStatus(true);
    return view && grants(statusNow(*view, false, true).entitlements, code);
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
      const std::optional<View> view = loadForStatus(readOnly);
      if (!view) {
        result = unreadable();
      } else if (!readOnly) {
        result = startTrialNow(view->state);
      } else {
        result = trialStatus(view->state, effectiveNow(view->state));
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

ActivateResult Engine::activateNow(const std::string& key, const ActivateOptions& options) {
  const std::optional<std::string> entered = enteredKey(key);
  if (!entered) return ActivateResult::failure(ActivateError::InvalidKey);
  // What a person typed for a user-locked key: one that can be no address, or no code, is refused here, before
  // anything is asked of the server or its wrong tries are spent.
  const Typed typed = typedFor(options.email, options.code);
  if (typed.refused) return ActivateResult::failure(*typed.refused);
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
  if (typed.email) body["email"] = *typed.email;
  if (typed.code) body["code"] = *typed.code;
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
  // Activating another seat drops the journal of the old one, unreported; the same seat keeps it.
  saveWith(patch, std::nullopt, basis, {JournalOp::activatedOf(activationId)});
  return ActivateResult::success();
}

DeactivateResult Engine::deactivateNow() {
  // What this process could not save lands first, so the release reports it: the journal goes with the seat.
  saveUnsavedJournal();
  const View view = this->view(false);
  const StoredState& state = view.state;
  // Nothing held is nothing to release: the seat is as free as it gets. Nor is
  // a seat another device holds this one's to release (seatElsewhere).
  DeactivateResult result{true};
  if (truthy(state.key) && truthy(state.activationId)) {
    const Device here = device(timings_.machineId, state.deviceKind);
    if (!seatElsewhere(state, here)) result.released = release(view, here);
  }
  // Stamped, so that an answer to an older ask is not saved over it. The journal goes with the rest of the licence: what
  // the release carried is all the server will hear of it.
  StatePatch patch = unkeyedPatch();
  patch.verdictAt = Change<std::int64_t>::set(effectiveNow(state));
  saveWith(patch, std::nullopt, {}, {JournalOp::deactivated()});
  return result;
}

/**
 * The release request, carrying the journal of unsynced spends and, with it, the device's fingerprints as a validate
 * sends them (so the server holds the reports to the seat's device; none when it cannot tell). Only a 200 is KeyGrant
 * saying the seat is free: a 5xx, a 429 or a proxy's page is an answer, but not that one; no answer at all is none. The
 * local licence is cleared whatever the server says.
 */
bool Engine::release(const View& view, const Device& here) {
  const StoredState& state = view.state;
  ordered_json body = ordered_json::object();
  body["key"] = *state.key;
  body["activationId"] = *state.activationId;
  if (const std::optional<ordered_json> reports = spendsReported(reportable(view))) {
    body["spends"] = *reports;
    if (here.canTell) body["fingerprints"] = here.fingerprints;
  }
  const std::optional<Reply> reply = post("deactivate", body);
  return reply && reply->status == 200;
}

/**
 * Whether the activation stored is another device's: one copied or restored
 * from another PC (a roaming profile, a backup, a cloned disk) carries that
 * PC's, and releasing it would lock that PC out at its next check-in. Either a
 * device refusal that stands for this device, or a lease whose signature holds
 * (expired or not) held to a machine this run can tell is not this one
 * (heldElsewhere).
 */
bool Engine::seatElsewhere(const StoredState& state, const Device& here) {
  if (standingRefusal(state, major_, DeviceView{here.ownClaim, here.canTell}) == Refusal::Device) return true;
  return truthy(state.lease) && heldElsewhere(verify(*state.lease, effectiveNow(state)), here.claims, here.canTell);
}

LicenseStatus Engine::statusNow(const View& view, bool forceOnline, bool readOnly) {
  const StoredState& state = view.state;
  // A stored offline licence file that licenses this build wins, offline, with
  // no ask and no write; one that does not answers only when no key path and no
  // running trial does.
  const std::optional<LicenseStatus> file = fileStatus(state);
  if (file && file->state == LicenseState::Licensed) return *file;
  const std::int64_t now = effectiveNow(state);
  // A device refusal stands only for the device it was given for: who this is.
  std::optional<DeviceView> deviceView;
  if (state.refused == Refusal::Device) {
    const Device here = device(timings_.machineId, state.deviceKind);
    deviceView = DeviceView{here.ownClaim, here.canTell};
  }
  const std::optional<Refusal> refused = truthy(state.key) ? standingRefusal(state, major_, deviceView) : std::nullopt;
  if (refused && readOnly) return invalidStatus(reasonOf(*refused), state.key);
  if (refused) return refusedStatus(view, *refused, forceOnline);
  if (truthy(state.key) && truthy(state.lease)) return licenseStatus(view, *state.lease, now, forceOnline, readOnly);
  return trialOrFile(state, now, file, StatusAsk{forceOnline, readOnly});
}

/**
 * With no key to answer: a trial this device started while it runs, else a
 * stored file that does not license this build (why is the news), else the
 * trial that has ended, else unlicensed.
 */
LicenseStatus Engine::trialOrFile(const StoredState& state, std::int64_t now, const std::optional<LicenseStatus>& file,
                                  const StatusAsk& ask) {
  std::optional<LicenseStatus> trial;
  if (state.trialStart) trial = trialStatus(state, now, ask);
  if (trial && trial->state == LicenseState::Trial) return *trial;
  if (file) return *file;
  return trial ? *trial : unlicensed();
}

/**
 * A refusal that stands for this build: answered as stored, and asked again
 * once the wait is over, or when forced, so a renewed or reinstated key heals.
 */
LicenseStatus Engine::refusedStatus(const View& view, Refusal refused, bool force) {
  const LicenseStatus stored = invalidStatus(reasonOf(refused), view.state.key);
  if (!askDue(view.state, clockNow(), force)) return stored;
  return tryRenew(view).value_or(stored);
}

LicenseStatus Engine::licenseStatus(const View& view, const std::string& lease, std::int64_t now, bool forceOnline,
                                    bool readOnly) {
  const StoredState& state = view.state;
  const LeaseCheck evaluated = evaluate(lease, now, state.key, state.activationId, state.deviceKind);
  // What the lease says offline, entitlements and meters included (the meters net of the journal): the answer whenever
  // the server gives no verdict (leaseStatus).
  const LicenseStatus offline = evaluated.ok ? meteredStatus(leaseStatus(evaluated, state.key), *evaluated.claims, state)
                                             : leaseStatus(evaluated, state.key);
  if (evaluated.ok) {
    if (readOnly) return offline;
    const View held = noteKept(view, *evaluated.claims, now);
    if (forceOnline) return tryRenew(held).value_or(offline);
    // Asked, bounded in time, when the server should hear from this device
    // before the lease runs out (a newer major on an open lease, or a lease
    // nearing its end), or when it holds spends KeyGrant has not heard of and
    // the journal's own wait is not running, and the wait allows
    // (asksOnValidLease). The journal that asks is the one a request would
    // report (reportable): spends held in memory that no request carries do not
    // make status() ask at every call. The valid lease is the answer when the
    // ask gets no verdict.
    StoredState asking = held.state;
    asking.spends = reportable(held);
    if (!asksOnValidLease(*evaluated.claims, asking, major_, now, clockNow())) return offline;
    return tryRenew(held, timings_.statusCheck).value_or(offline);
  }
  // Locally invalid (expired, tampered, upgrade...): give the server a chance
  // to heal it before locking the user out, once the wait is over or when
  // forced; only when it is out of reach (or agrees) is the local reason the
  // answer.
  std::optional<LicenseStatus> renewed;
  if (!readOnly && askDue(state, clockNow(), forceOnline)) renewed = tryRenew(view);
  return renewed.value_or(offline);
}

/** The view with this run noted in keptMajor, saved when that changed. */
View Engine::noteKept(const View& view, const LeaseClaims& claims, std::int64_t now) {
  const StoredState& state = view.state;
  const std::optional<std::int64_t> kept = keptWith(state.keptMajor, claims, now, major_);
  if (kept == state.keptMajor) return view;
  StatePatch patch;
  patch.keptMajor = Change<std::int64_t>::of(kept);
  // About this activation: not saved onto another one stored meanwhile.
  Basis basis;
  basis.about = About{state.activationId};
  saveBestEffort(patch, std::nullopt, basis);
  StoredState held = state;
  held.keptMajor = kept;
  return view.with(std::move(held));
}

}  // namespace detail

// --- License -------------------------------------------------------------------

License::License(LicenseConfig config) : License(std::move(config), detail::Timings{}) {}

License::License(LicenseConfig config, const detail::Timings& timings)
    : engine_(std::make_shared<detail::Engine>(std::move(config), timings)) {}

License::~License() = default;

// Each call holds the engine for as long as it runs, so a License destroyed
// while a call is stuck in an adapter leaves that call to finish safely.

UseResult License::use(const std::string& meter, std::int64_t n) {
  const std::shared_ptr<detail::Engine> engine = engine_;
  return engine->use(meter, n);
}

ActivateResult License::activate(const std::string& key) { return activate(key, ActivateOptions{}); }

ActivateResult License::activate(const std::string& key, const ActivateOptions& options) {
  const std::shared_ptr<detail::Engine> engine = engine_;
  return engine->activate(key, options);
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

PickUpSentResult License::pickUp(const std::string& email) {
  const std::shared_ptr<detail::Engine> engine = engine_;
  return engine->pickUp(email);
}

PickUpKeysResult License::pickUp(const std::string& email, const std::string& code) {
  const std::shared_ptr<detail::Engine> engine = engine_;
  return engine->confirmPickup(email, code);
}

RestoreResult License::restoreByEmail(const std::string& email, const std::string& code) {
  const std::shared_ptr<detail::Engine> engine = engine_;
  return engine->restoreByEmail(email, code);
}

ImportLicenseFileResult License::importLicenseFile(std::string_view content) {
  return importLicenseFile(content, ActivateOptions{});
}

ImportLicenseFileResult License::importLicenseFile(const std::vector<std::uint8_t>& content) {
  return importLicenseFile(content, ActivateOptions{});
}

ImportLicenseFileResult License::importLicenseFile(std::string_view content, const ActivateOptions& options) {
  const std::shared_ptr<detail::Engine> engine = engine_;
  return engine->importLicenseFile(content, options);
}

ImportLicenseFileResult License::importLicenseFile(const std::vector<std::uint8_t>& content,
                                                   const ActivateOptions& options) {
  const std::shared_ptr<detail::Engine> engine = engine_;
  return engine->importLicenseFile(std::string_view(reinterpret_cast<const char*>(content.data()), content.size()),
                                   options);
}

void License::removeLicenseFile() {
  const std::shared_ptr<detail::Engine> engine = engine_;
  engine->removeLicenseFile();
}

std::optional<std::string> License::upgradeUrl(const std::string& paymentLink) {
  const std::shared_ptr<detail::Engine> engine = engine_;
  return engine->upgradeUrl(paymentLink);
}

}  // namespace keygrant
