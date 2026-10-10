#include "engine.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "bounded.hpp"
#include "keygrant/fingerprint.hpp"
#include "keygrant/storage.hpp"
#include "utf.hpp"
#include "wire.hpp"

namespace keygrant::detail {

namespace {

/** The API base, without the one trailing slash the reference drops. */
std::string checkedBase(const std::string& apiBaseUrl) {
  const std::string lower = asciiLower(apiBaseUrl);
  const std::size_t scheme = lower.rfind("https://", 0) == 0 ? 8 : lower.rfind("http://", 0) == 0 ? 7 : 0;
  if (scheme == 0 || apiBaseUrl.size() <= scheme || apiBaseUrl[scheme] == '/') {
    throw std::invalid_argument("apiBaseUrl: an http(s) URL with a host, e.g. https://api.keygrant.dev");
  }
  std::string base = apiBaseUrl;
  if (base.back() == '/') base.pop_back();
  return base;
}

std::int64_t checkedMajor(std::int64_t configured) {
  // As the server reads a version (majorOf): a 0.x build runs as major 1, one
  // reading used wherever the major is.
  const std::int64_t major = majorOf(configured);
  // The server refuses a larger one in every body, so every call would fail.
  if (major > kMaxMajor) throw std::invalid_argument("major: at most 99999");
  return major;
}

std::chrono::milliseconds checkedTimeout(std::chrono::milliseconds timeout) {
  if (timeout.count() <= 0) throw std::invalid_argument("httpTimeout: must be positive");
  return std::min(timeout, kLongestBound);
}

Adapters withDefaults(Adapters a, const std::string& product) {
  if (!a.storage) a.storage = std::make_shared<FileStorage>(defaultStoragePath(product));
  if (!a.registry) a.registry = std::make_shared<NoopRegistryStash>();
  if (!a.clock) a.clock = std::make_shared<SystemClock>();
  if (!a.fingerprint) a.fingerprint = std::make_shared<SystemFingerprinter>();
  if (!a.http) a.http = std::make_shared<CurlHttp>();
  return a;
}

}  // namespace

Engine::Engine(LicenseConfig config, const Timings& timings)
    : product_(checkedProduct(config.product)),
      base_(checkedBase(config.apiBaseUrl)),
      major_(checkedMajor(config.major)),
      // Cut, because the integrating app supplies it and the endpoint bounds it:
      // a long name would turn a first activation into a 400.
      deviceName_(trimToLength(config.deviceName, kMaxDeviceName)),
      deviceHold_(config.deviceHold),
      httpTimeout_(checkedTimeout(config.httpTimeout)),
      timings_(timings),
      keys_(KeySet::fromJwk(config.publicJwk)),
      a_(withDefaults(std::move(config.adapters), product_)),
      reader_(std::make_shared<DeviceIdentityReader>(a_.fingerprint)) {}

/**
 * Run `work` once every call before it on this instance has finished: each
 * starts from what the last one left, and the second of two overlapping checks
 * finds it already answered. A call waits for the one before it to settle, but
 * not past the HTTP bound plus the lock grace from when that one STARTED
 * running. The storage, fingerprint and registry
 * adapters are not bounded (cutting a storage write short could leave it
 * half-written), so a call stuck
 * on one is left to finish, and the calls behind it go ahead READ-ONLY for as
 * long as any call before them is unsettled: answering from what is stored,
 * with no ask and no write, since a write of theirs could be reverted by the
 * stuck call's late one.
 */
void Engine::serial(const std::function<void(bool readOnly)>& work) {
  auto own = std::make_shared<Slot>();
  bool readOnly = false;
  {
    std::unique_lock<std::mutex> lock(serialMutex_);
    const std::shared_ptr<Slot> prev = std::exchange(last_, own);
    if (prev) {
      // The grace runs from when the call before started, however late this one was queued.
      serialChanged_.wait(lock, [&] { return prev->started; });
      serialChanged_.wait_until(lock, prev->graceOver, [&] { return prev->settled; });
    }
    readOnly = writing_ > 0;
    if (!readOnly) ++writing_;
    own->started = true;
    own->graceOver = std::chrono::steady_clock::now() + httpTimeout_ + std::min(timings_.lockGrace, kLongestBound);
  }
  serialChanged_.notify_all();
  struct Settle {
    Engine& engine;
    Slot& slot;
    bool readOnly;
    ~Settle() {
      {
        std::lock_guard<std::mutex> lock(engine.serialMutex_);
        if (!readOnly) --engine.writing_;
        slot.settled = true;
      }
      engine.serialChanged_.notify_all();
    }
  } settle{*this, *own, readOnly};
  work(readOnly);
}

/**
 * POST to the product's endpoint, ALWAYS bounded: by `timeout`, else by the
 * configured httpTimeout. The bound is passed to the adapter and kept by the
 * SDK itself. No answer (it threw, or ran out of time) is none.
 */
std::optional<Reply> Engine::post(const std::string& action, const ordered_json& body,
                                  std::optional<Milliseconds> timeout) {
  return postTo(a_.http, endpoint(action), body, timeout.value_or(httpTimeout_));
}

/** The product's endpoint for `action`. */
std::string Engine::endpoint(const std::string& action) const {
  return base_ + "/v1/products/" + product_ + "/" + action;
}

/** POST `body` to `url` through `http`, held to `bound` whatever the adapter does; none when there was no answer. */
std::optional<Reply> Engine::postTo(const std::shared_ptr<HttpAdapter>& http, const std::string& url,
                                    const ordered_json& body, Milliseconds bound) {
  const Milliseconds held = std::clamp(bound, Milliseconds{1}, kLongestBound);
  const std::string text = dumpJson(body);
  const auto response =
      runBounded<HttpResponse>([http, url, text, held] { return http->post(url, text, held); }, held);
  if (!response) return std::nullopt;
  return Reply{response->status, parseJsonText(response->body)};
}

VerifyResult Engine::verify(const std::string& lease, std::int64_t now) const { return verifyLease(lease, keys_, now); }

/**
 * The ids of the keys this build carries (`kid`, each key's thumbprint), in
 * the order configured: sent with each activation and validate, so the
 * product's dashboard can count the devices whose builds do not carry a key yet.
 */
ordered_json Engine::kidsJson() const { return keys_.kids(); }

/** A licence lease verified offline and held to this product and build, its key and activation, on this device. */
LeaseCheck Engine::evaluate(const std::string& lease, std::int64_t now, const std::optional<std::string>& key,
                            const std::optional<std::string>& activationId, std::optional<DeviceKind> deviceKind) {
  const Device here = device(timings_.machineId, deviceKind);
  const LeaseHolder holder{key.value_or(""), activationId.value_or(""), here.claims, here.canTell};
  return checkLease(verify(lease, now), product_, major_, holder);
}

/**
 * A trial lease verified offline, for this product and this device, with the
 * end it was signed to (ms) and its entitlements when it is valid
 * (trialLeaseOf).
 */
TrialLease Engine::trialLease(const std::string& lease, std::int64_t now, std::optional<DeviceKind> installKind) {
  const Device here = device(timings_.machineId, installKind);
  return trialLeaseOf(verify(lease, now), product_, here.claims, here.canTell);
}

/**
 * Who this device is, read for at most about `timeout`, on an install keyed as
 * `install`, held to its device as configured (deviceHold): deviceFrom.
 */
Device Engine::device(Milliseconds timeout, std::optional<DeviceKind> install) {
  return reader_->read(DeviceBounds{timeout, timings_.identifyExtra}, install, deviceHold_);
}

std::optional<DeviceKind> Engine::installKind(std::optional<DeviceKind> stored) const {
  return deviceHold_ ? stored : std::nullopt;
}

StoredState Engine::load(bool readOnly) { return a_.storage->read(readOnly).value_or(StoredState{}); }

/**
 * Persist a patch onto the state as it is stored NOW, by what it is based on,
 * advancing the clock guard unless it is being brought back on the server's
 * evidence (`guard`). Through the storage's update() when it has one.
 */
void Engine::save(const StatePatch& patch, std::optional<std::int64_t> guard, const Basis& basis) {
  const std::function<StoredState(const StoredState&)> merge = [&](const StoredState& latest) {
    StoredState next = merged(latest, patch, basis);
    next.lastSeen = savedLastSeen(latest.lastSeen, clockNow(), guard);
    return next;
  };
  if (a_.storage->update(merge)) return;
  a_.storage->write(merge(load()));
}

/** A save for status() and startTrial(), whose failure fails nothing. Whether it was saved. */
bool Engine::saveBestEffort(const StatePatch& patch, std::optional<std::int64_t> guard, const Basis& basis) {
  try {
    save(patch, guard, basis);
    return true;
  } catch (...) {
    return false;
  }
}

/** The device clock, held to JavaScript's safe integers so no time arithmetic can overflow. */
std::int64_t Engine::clockNow() { return std::clamp(a_.clock->now(), -kMaxSafeInteger, kMaxSafeInteger); }

/** Never earlier than the highest wall-clock ever seen (anti clock-rollback). */
std::int64_t Engine::effectiveNow(const StoredState& state) { return guardedNow(clockNow(), state.lastSeen); }

/**
 * `now` for judging a lease the server has just signed, with the clock guard
 * brought back to it when the guard is more than the tolerance past its
 * signing time (a clock that once jumped ahead and was put right). Only a
 * lease for THIS key and activation, no older than the one held, can move it.
 */
std::int64_t Engine::serverNow(const std::string& lease, const std::string& key, const std::string& activationId,
                               const GuardSave& saveGuard) {
  const StoredState stored = load();
  const std::int64_t now = effectiveNow(stored);
  const VerifyResult fresh = verify(lease, now);
  std::optional<SignedAt> signedAt;
  if (fresh.claims) signedAt = SignedAt{fresh.claims->sub, fresh.claims->aid, fresh.claims->iat};
  std::optional<std::int64_t> heldIat;
  if (truthy(stored.lease)) {
    const VerifyResult held = verify(*stored.lease, now);
    if (held.claims) heldIat = held.claims->iat;
  }
  const std::optional<std::int64_t> corrected =
      clockCorrection(clockNow(), stored.lastSeen, signedAt, heldIat, key, activationId);
  if (!corrected) return now;
  saveGuard(StatePatch{}, *corrected);
  return *corrected;
}

std::optional<LicenseStatus> Engine::tryRenew(const StoredState& state, std::optional<Milliseconds> timeout) {
  const Renewal renewal = renew(state, timeout);
  return renewal.answered ? renewal.status : std::nullopt;
}

/**
 * An online validate, and whether the server gave a verdict. What its answer
 * saves and says is renewalOutcome: any verdict (a lease, a refusal, a body it
 * could not use) records the major it was sent as heard (`checkedMajor`); an
 * ask that came back without a lease starts the wait before the SDK asks again
 * on its own (askDue): kRetryUnansweredMs when nothing answered at all,
 * kAskAgainMs when the server did.
 */
Renewal Engine::renew(const StoredState& state, std::optional<Milliseconds> timeout) {
  // The wait runs on the device clock, as askDue reads it; the answer is
  // stamped by the guarded one, which a clock put back does not move back, so
  // an answer asked for later still reads as later to another process.
  const std::int64_t askedAt = clockNow();
  Basis asked;
  asked.answer = Asked{effectiveNow(state), state.activationId, state.verdictAt};
  // A machine id read here keys this install from now on (deviceKind):
  // written with whatever this ask writes, as the seat is re-keyed by it.
  const Device here = device(timings_.machineId, state.deviceKind);
  std::optional<Reply> response = post("validate", validateBody(state, here), timeout);
  const Judged judged = judgeFresh(state, response);
  RenewalInput input;
  // Moved, never copied (see Reply).
  input.response = std::move(response);
  input.check = judged.check;
  input.askedAt = askedAt;
  input.now = judged.now;
  input.major = major_;
  input.key = state.key;
  input.keptMajor = state.keptMajor;
  input.kind = here.kind;
  input.ownClaim = here.ownClaim;
  const RenewalOutcome outcome = renewalOutcome(input);
  // Saved as the answer to its ask; a save that fails leaves the answer standing (saveBestEffort).
  saveBestEffort(outcome.patch, std::nullopt, asked);
  return outcome.answered ? Renewal{true, outcome.status} : Renewal{};
}

/**
 * The lease a validate's 200 carried, checked on this device (evaluate) as of
 * the server's time (serverNow, the clock guard brought back best-effort: the
 * server's time even when the state cannot be read or saved just now); no
 * check for any other answer. With the time it was judged at.
 */
Engine::Judged Engine::judgeFresh(const StoredState& state, const std::optional<Reply>& response) {
  const std::optional<std::string> lease =
      response && response->status == 200 ? parseLeaseBody(response->body).lease : std::nullopt;
  if (!truthy(lease)) return Judged{std::nullopt, effectiveNow(state)};
  return judged(*lease, state.key.value_or(""), state.activationId.value_or(""), state.deviceKind,
                [this, &state] { return effectiveNow(state); });
}

/**
 * A lease the server just answered, checked for `key` and `activationId` on
 * this device, keyed as `kind` (evaluate), as of the server's time (serverNow,
 * the clock guard brought back best-effort; `fallbackNow` when the state
 * cannot be read or saved just now). With the time it was judged at.
 */
Engine::Judged Engine::judged(const std::string& lease, const std::string& key, const std::string& activationId,
                              std::optional<DeviceKind> kind, const std::function<std::int64_t()>& fallbackNow) {
  std::int64_t now = 0;
  try {
    now = serverNow(lease, key, activationId,
                    [this](const StatePatch& patch, std::int64_t guard) { saveBestEffort(patch, guard); });
  } catch (...) {
    now = fallbackNow();
  }
  return Judged{evaluate(lease, now, key, activationId, kind), now};
}

/**
 * A validate's body: with every fingerprint this device answers to when it
 * can tell, so the server refuses a licence file copied from another machine;
 * with none when it cannot, so a slow read never costs a renewal.
 */
ordered_json Engine::validateBody(const StoredState& state, const Device& here) const {
  ordered_json body = ordered_json::object();
  if (state.key) body["key"] = *state.key;
  if (state.activationId) body["activationId"] = *state.activationId;
  body["major"] = major_;
  if (state.keptMajor) body["keptMajor"] = *state.keptMajor;
  if (here.canTell) body["fingerprints"] = here.fingerprints;
  if (here.canTell && here.kind) body["fingerprintKind"] = to_string(*here.kind);
  body["kids"] = kidsJson();
  return body;
}

}  // namespace keygrant::detail
