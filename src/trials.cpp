// The trial machinery under License: asking the server for this device's trial
// and saving its answer (askTrial), even when the disk refuses it for now
// (unsaved_); a trial's status from what the device holds (trialStatus); asking
// again for a trial whose lease does not verify while its stored end is still
// ahead (healTrial); and a running trial's launch, reported at most once a day
// on a thread of its own (reportLaunch). license.cpp decides when.
#include <thread>

#include "engine.hpp"
#include "wire.hpp"

namespace keygrant::detail {

/** startTrial()'s trial ask, under the device startTrial() read for both its asks (`known`). */
LicenseStatus Engine::startTrialNow(const StoredState& state, const Device& known) {
  const TrialAsk ask = askTrial(state, timings_.activateMachineId, std::nullopt, &known);
  // The server's trial even when it could not be saved: kept for this process
  // (unsaved_) until a later call saves it.
  return trialStatus(ask.latest, effectiveNow(state));
}

/**
 * Ask the server for this device's trial (`trial`, with the earliest start the
 * device holds as `clientStart`) and save its answer: `reached` when the
 * server answered at all, `latest` the state with its trial merged in. Under
 * the device already read (`known`), else read now for `machineIdTimeout`.
 */
Engine::TrialAsk Engine::askTrial(const StoredState& state, Milliseconds machineIdTimeout,
                                  std::optional<Milliseconds> timeout, const Device* known) {
  const Device here = known ? *known : device(machineIdTimeout, state.deviceKind);
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
 * A trial's status from what the device holds. With `ask` (a status()), a
 * trial whose lease does not verify while its stored end is still ahead is
 * asked for again first (healTrial), and a running one reports its launch
 * when due (reportLaunch); neither read-only.
 */
LicenseStatus Engine::trialStatus(const StoredState& state, std::int64_t now, const std::optional<StatusAsk>& ask) {
  // Only a valid trial lease runs a trial, to the end it was signed to, with what it grants.
  const TrialLease lease = truthy(state.trialLease) ? trialLease(*state.trialLease, now, state.deviceKind) : TrialLease{};
  const LicenseStatus status = trialStatusOf(state, now, lease.valid, lease.endsAt, lease.entitlements);
  if (!ask || ask->readOnly) return status;
  if (status.state == LicenseState::Trial) {
    reportLaunch(state, now);
    return status;
  }
  const bool storedEndAhead = state.trialEndsAt && now < *state.trialEndsAt;
  if (lease.valid || !storedEndAhead) return status;
  return healTrial(state, ask->forceOnline).value_or(status);
}

/**
 * A trial whose lease does not verify here while its stored end is still ahead:
 * a lease signed with the product's key before it was rotated (on a build that
 * ships the new one), one signed to end short of the trial's end, or one never
 * saved whole. The stored end is unsigned, so it runs no trial on its own
 * (trialStatusOf): the server is asked for the trial again instead, under the
 * usual waits (askDue) and bounded as a status check is, and its answer stands.
 * An ask that brings back no trial this build can verify starts the wait
 * (kRetryUnansweredMs when nothing answered, kAskAgainMs when the server did),
 * so a device that is offline, or that carries a key the product no longer
 * signs with, is not asked on every status(). None when it did not ask, or
 * nothing answered.
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

/**
 * A running trial's launch, reported so the server sees the trial come back
 * (its funnel counts a trial seen a day after it started): with no key held,
 * at most once a day (trialReportDue). The stamp (`trialReportedAt`) is saved
 * first, and nothing is sent when it cannot be, so "once a day" holds; then
 * `trial` is POSTed on a thread of its own, never awaited, so a launch never
 * waits on it and never fails for it offline (sendLaunch). Its answer is not
 * read: a change to the trial reaches the device at startTrial().
 *
 * The thread holds everything the report touches (LaunchReport) and nothing
 * of the engine, so a License destroyed first leaves nothing it uses
 * dangling, and the engine (its key among it) is never torn down on that thread.
 */
void Engine::reportLaunch(const StoredState& state, std::int64_t now) {
  if (truthy(state.key) || !trialReportDue(state, now)) return;
  StatePatch stamp;
  stamp.trialReportedAt = Change<std::int64_t>::set(now);
  if (!saveBestEffort(stamp)) return;
  const LaunchReport report{reader_,
                            a_.http,
                            endpoint("trial"),
                            DeviceBounds{timings_.machineId, timings_.identifyExtra},
                            state.deviceKind,
                            deviceHold_,
                            timings_.statusCheck,
                            state.trialStart};
  const std::shared_ptr<LaunchReports> reports = reports_;
  {
    std::lock_guard<std::mutex> lock(reports->mutex);
    ++reports->inFlight;
  }
  const auto settle = [reports] {
    {
      std::lock_guard<std::mutex> lock(reports->mutex);
      --reports->inFlight;
    }
    reports->settled.notify_all();
  };
  try {
    std::thread([report, settle] {
      try {
        sendLaunch(report);
      } catch (...) {
        // Every failure, offline included, is ignored.
      }
      settle();
    }).detach();
  } catch (...) {
    // No thread to be had: no report this time.
    settle();
  }
}

/**
 * The report itself, on its own thread: the device read for a status's time,
 * then `trial` with the body startTrial() sends (the stored start as
 * `clientStart`, no stash read), bounded as a status check is. A device that
 * cannot be told sends nothing; the answer is not read.
 */
void Engine::sendLaunch(const LaunchReport& report) {
  const Device here = report.reader->read(report.device, report.install, report.deviceHold);
  if (!here.own) return;
  ordered_json body = ordered_json::object();
  body["fingerprint"] = *here.own;
  if (here.kind) body["fingerprintKind"] = to_string(*here.kind);
  if (report.trialStart) body["clientStart"] = *report.trialStart;
  postTo(report.http, report.url, body, report.bound);
}

bool Engine::launchReportSettled(Milliseconds timeout) {
  const std::shared_ptr<LaunchReports> reports = reports_;
  std::unique_lock<std::mutex> lock(reports->mutex);
  return reports->settled.wait_for(lock, timeout, [&reports] { return reports->inFlight == 0; });
}

}  // namespace keygrant::detail
