// Key pickup under License (pickup.cpp holds the pure pieces): a code mailed to the address a key was sold to, that
// address's keys of the product for the code, and a restore that confirms and activates the key that fits this build.
// The two pickUp calls store nothing and run beside any call; a restore runs in the one-at-a-time order as activate does,
// and what it activates is stored as any activation is. Nothing here takes a licence away.
#include <utility>

#include "engine.hpp"
#include "pickup.hpp"

namespace keygrant::detail {

PickUpSentResult Engine::pickUp(const std::string& email) {
  const PickupAsk ask = pickupAsk(email, std::nullopt);
  if (ask.refused) return PickUpSentResult::failure(*ask.refused);
  ordered_json body = ordered_json::object();
  body["email"] = ask.email;
  const std::optional<Reply> reply = post("pickup", body);
  if (!reply) return PickUpSentResult::failure(PickupError::Network);
  if (reply->status == 200) return PickUpSentResult::sent();
  const PickupRefusal refusal = pickupRefusal(reply->body);
  return PickUpSentResult::failure(refusal.error, refusal.retryAfter);
}

PickUpKeysResult Engine::confirmPickup(const std::string& email, const std::string& code) {
  const PickupAsk ask = pickupAsk(email, code);
  if (ask.refused) return PickUpKeysResult::failure(*ask.refused);
  if (std::optional<std::vector<PickedKey>> kept = keptKeys(ask.email, *ask.code)) {
    return PickUpKeysResult::success(std::move(*kept));
  }
  // Read-only: this runs outside the one-at-a-time order, beside any call.
  StoredState stored;
  try {
    stored = load(true);
  } catch (...) {
    // A state that cannot be read: the device is read all the same.
  }
  const Device here = device(timings_.machineId, installKind(stored.deviceKind));
  ordered_json body = ordered_json::object();
  body["email"] = ask.email;
  body["code"] = *ask.code;
  if (here.canTell) body["fingerprints"] = here.fingerprints;
  const std::optional<Reply> reply = post("pickup/confirm", body);
  if (!reply) return PickUpKeysResult::failure(PickupError::Network);
  if (reply->status != 200) {
    const PickupRefusal refusal = pickupRefusal(reply->body);
    return PickUpKeysResult::failure(refusal.error, refusal.retryAfter);
  }
  std::optional<std::vector<PickedKey>> keys = pickedKeysOf(reply->body);
  if (!keys) return PickUpKeysResult::failure(PickupError::Network);
  {
    std::lock_guard<std::mutex> lock(keptMutex_);
    kept_ = KeptKeys{ask.email, *ask.code, *keys, clockNow()};
  }
  return PickUpKeysResult::success(std::move(*keys));
}

std::optional<std::vector<PickedKey>> Engine::keptKeys(const std::string& email, const std::string& code) {
  const std::int64_t now = clockNow();
  std::lock_guard<std::mutex> lock(keptMutex_);
  if (!kept_) return std::nullopt;
  const std::int64_t age = now - kept_->at;
  if (kept_->email != email || kept_->code != code || age < 0 || age >= kPickupKeysKeepMs) return std::nullopt;
  return kept_->keys;
}

RestoreResult Engine::restoreByEmail(const std::string& email, const std::string& code) {
  RestoreResult result;
  // Behind a call that has not settled, nothing may be written: answered as not reaching the server, to retry.
  serial([&](bool readOnly) {
    result = readOnly ? RestoreResult::activationFailure(ActivateError::Network) : restoreNow(email, code);
  });
  return result;
}

RestoreResult Engine::restoreNow(const std::string& email, const std::string& code) {
  const PickUpKeysResult picked = confirmPickup(email, code);
  if (!picked.ok) return RestoreResult::pickupFailure(*picked.error, picked.retryAfter);
  const Choice choice = chosenKey(picked.keys, major_, clockNow());
  if (choice.refused) {
    switch (*choice.refused) {
      case Unchosen::NoKey: return RestoreResult::noKeyFound();
      case Unchosen::Upgrade: return RestoreResult::activationFailure(ActivateError::Upgrade);
      case Unchosen::Outdated: return RestoreResult::activationFailure(ActivateError::Outdated);
    }
  }
  const PickedKey& key = picked.keys[*choice.index];
  // The address as the pickup sent it (the confirm just accepted it), and a lock's code when the pickup handed one back.
  const PickupAsk sent = pickupAsk(email, std::nullopt);
  const ActivateResult activated =
      activateNow(key.key, ActivateOptions{sent.refused ? email : sent.email, key.activationCode});
  if (!activated.ok) return RestoreResult::activationFailure(*activated.error);
  // Activated: the keys (and a lock's single-use code among them) are done with.
  {
    std::lock_guard<std::mutex> lock(keptMutex_);
    kept_.reset();
  }
  return RestoreResult::success();
}

}  // namespace keygrant::detail
