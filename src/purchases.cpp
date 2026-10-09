// Buying from the app with no key typed, under License: the purchase link (purchaseUrl, which keeps
// this install's one-time secret and hands out the reference to it) and the pickup (pickUp): while
// no key is held and a secret is, asking the server for the key bought with it (`claim`) and saving
// it exactly as activate(key) saves one. What each answer does is pickup.cpp; license.cpp decides
// when to ask.
#include "engine.hpp"
#include "url.hpp"
#include "wire.hpp"

namespace keygrant::detail {

std::optional<std::string> Engine::purchaseUrl(const std::string& paymentLink) {
  // A link that is not an absolute URL is refused before anything is read.
  absoluteLink(paymentLink);
  std::optional<std::string> result;
  // One at a time, like the other calls that write.
  serial([&](bool readOnly) { result = purchaseUrlNow(paymentLink, readOnly); });
  return result;
}

/**
 * purchaseUrl's answer: none while a key is held; else the link carrying the
 * reference to the held secret, or to a new one, saved with when its link was
 * handed out (the claim's wait cleared, so the next status() asks). Read-only
 * (behind a call that has not settled), a held secret's link is answered as it
 * is and none held throws StalledError: a secret that is not saved could never
 * be picked up.
 */
std::optional<std::string> Engine::purchaseUrlNow(const std::string& link, bool readOnly) {
  const StoredState state = load(readOnly);
  // Any key (licensed, refused or expired): a secret on a keyed install is never picked up.
  if (truthy(state.key)) return std::nullopt;
  const std::int64_t now = effectiveNow(state);
  const std::optional<std::string> held = purchaseTokenKept(state, now) ? state.purchaseToken : std::nullopt;
  if (readOnly) {
    if (!held) throw StalledError();
    return purchaseLink(link, *held);
  }
  const std::string secret = held ? *held : newPurchaseSecret();
  StatePatch patch;
  patch.purchaseToken = Change<std::string>::set(secret);
  patch.purchaseTokenAt = Change<std::int64_t>::set(now);
  patch.claimAskedAt = Change<std::int64_t>::clear();
  // A save that fails throws: a link whose secret is not kept could never be picked up.
  save(patch);
  // The reference, never the secret: a link lands in browser history and the tenant's Stripe session.
  return purchaseLink(link, secret);
}

/**
 * The key bought through purchaseUrl, picked up: with no key held and a
 * secret held, the secret is dropped once past its bound (purchaseTokenKept),
 * and otherwise the claim is asked as `ask` says. Never asked behind a call
 * that has not settled: the caller skips it.
 */
Engine::Pickup Engine::pickUp(const StoredState& state, PickupAsk ask) {
  if (truthy(state.key) || !state.purchaseToken) return Pickup{std::nullopt, state};
  const std::string secret = *state.purchaseToken;
  const std::int64_t now = effectiveNow(state);
  if (!purchaseTokenKept(state, now)) return Pickup{std::nullopt, dropSecret(state, secret)};
  if (!ask.start && !ask.force && !claimDue(state, clockNow(), now)) return Pickup{std::nullopt, state};
  return claim(state, secret, ask);
}

/** The secret dropped (saved about it, best-effort): the state without it. */
StoredState Engine::dropSecret(const StoredState& state, const std::string& secret) {
  Basis basis;
  basis.purchase = AboutToken{secret, std::nullopt};
  const StatePatch drop = noPurchasePatch();
  saveBestEffort(drop, std::nullopt, basis);
  return merged(state, drop, basis);
}

/**
 * Ask for the key bought with `secret` (`claim`) and save what the answer does
 * (claimOutcome): a bought key as an activation, a wait or a drop about the
 * secret. The body is claimRequest's: from status(), a device whose own
 * fingerprint did not read is not asked at all, and nothing is saved, so the
 * next status() asks again; at startTrial(), a device with no fingerprint at
 * all is no answer. Saved best-effort: a bought key the disk refused is
 * answered all the same, and the next claim answers it again (the server
 * knows this device by any of the fingerprints it sends).
 */
Engine::Pickup Engine::claim(const StoredState& state, const std::string& secret, PickupAsk ask) {
  const std::int64_t askedClock = clockNow();
  const std::int64_t askedAt = effectiveNow(state);
  const Device here = ask.device ? *ask.device : device(timings_.machineId, state.deviceKind);
  // No key is held, so no install memory: the fingerprint a first activation uses.
  const std::optional<ClaimRequest> body = claimRequest(secret, here, deviceName_, major_, ask.start);
  if (!body && !ask.start) return Pickup{std::nullopt, state};
  // refresh(): the HTTP bound; status() and startTrial(): a status check's.
  const std::optional<Milliseconds> timeout =
      ask.force && !ask.start ? std::nullopt : std::optional<Milliseconds>(timings_.statusCheck);
  std::optional<Reply> response = body ? postClaim(*body, timeout) : std::nullopt;
  const std::optional<DeviceKind> kind = body ? body->fingerprintKind : std::nullopt;
  const Judged judged = judgeClaim(response, kind, askedAt);
  ClaimInput input;
  // Moved, never copied (see Reply).
  input.response = std::move(response);
  input.check = judged.check;
  input.clockNow = askedClock;
  input.now = judged.now;
  input.major = major_;
  input.kind = kind;
  const ClaimOutcome outcome = claimOutcome(input);
  Basis basis;
  if (outcome.result == ClaimResult::Bought) {
    basis.activation = Asked{askedAt, outcome.patch.activationId.get(), state.verdictAt};
    saveBestEffort(outcome.patch, std::nullopt, basis);
    return Pickup{outcome.status, state};
  }
  // A wait is about the secret as its link was handed out when asked; a drop about the secret alone (merged).
  const bool wait = outcome.result == ClaimResult::Kept;
  basis.purchase = AboutToken{secret, wait ? state.purchaseTokenAt : std::nullopt};
  saveBestEffort(outcome.patch, std::nullopt, basis);
  return Pickup{std::nullopt, merged(state, outcome.patch, basis)};
}

/** POST `claim` with its body (claimRequest); none when it threw or timed out. */
std::optional<Reply> Engine::postClaim(const ClaimRequest& body, std::optional<Milliseconds> timeout) {
  ordered_json sent = claimJson(body);
  sent["kids"] = kidsJson();
  return post("claim", sent, timeout);
}

/**
 * The lease a claim's 200 carried with a key and an activation id, checked
 * for them on this device as of the server's time (judged); no check for any
 * other answer. With the time it was judged at.
 */
Engine::Judged Engine::judgeClaim(const std::optional<Reply>& response, std::optional<DeviceKind> kind,
                                  std::int64_t askedAt) {
  if (!response || response->status != 200) return Judged{std::nullopt, askedAt};
  const ClaimBody body = parseClaimBody(response->body);
  if (!body.key || !body.activationId || !body.lease) return Judged{std::nullopt, askedAt};
  return judged(*body.lease, *body.key, *body.activationId, kind, [askedAt] { return askedAt; });
}

}  // namespace keygrant::detail
