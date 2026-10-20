// Key pickup: restoring a licence by the address its key was sold to.
//
// A build that may not take a typed key (an App Store build) restores a web buyer's licence this way:
// License::pickUp(email) asks for a code to be mailed to the address, License::pickUp(email, code) trades the
// code for the address's keys of the product, and License::restoreByEmail(email, code) activates the one that
// fits. A pickup never takes a licence away and stores nothing.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include "keygrant/entitlement.hpp"
#include "keygrant/types.hpp"

namespace keygrant {

/**
 * Why a pickup call did not do what it asked.
 *
 * `InvalidEmail`: the address is blank or longer than any address (nothing was asked). `WrongCode`: the code is not
 * six digits (nothing was asked, so the address lost none of its wrong tries), or it is not the address's live code.
 * `CodeExpired`: the address has no live code (it expired after 10 minutes, was used, was burnt by its fifth wrong
 * try, or was never asked for): ask for a new one with pickUp(email). `TooManyAttempts`: thirty wrong codes for the
 * address this hour, or too many asks or confirms from this network: wait `retryAfter` seconds. `PickupDisabled`: the
 * product's Key pickup is off. `TryAgain`: a count could not be made just now. `Network`: no answer, or an answer that
 * is none of a pickup's (a proxy's page, an unknown product, a 200 that holds no keys).
 */
enum class PickupError { InvalidEmail, WrongCode, CodeExpired, TooManyAttempts, PickupDisabled, TryAgain, Network };

/// The reference's wire names: "invalid-email", "wrong-code", "code-expired", ...
const char* to_string(PickupError value) noexcept;
std::ostream& operator<<(std::ostream& out, PickupError value);

/**
 * What License::pickUp(email) did: a code is on its way when the address holds a key, and the answer is the same when
 * it does not.
 */
struct PickUpSentResult {
  bool ok = false;
  /// Why not, when `ok` is false.
  std::optional<PickupError> error;
  /// The seconds to wait before asking again, when the server said so (TooManyAttempts, TryAgain), read from its body.
  std::optional<std::int64_t> retryAfter;

  static PickUpSentResult sent() { return PickUpSentResult{true, std::nullopt, std::nullopt}; }
  static PickUpSentResult failure(PickupError why, std::optional<std::int64_t> wait = std::nullopt) {
    return PickUpSentResult{false, why, wait};
  }

  bool operator==(const PickUpSentResult& other) const {
    return ok == other.ok && error == other.error && retryAfter == other.retryAfter;
  }
  bool operator!=(const PickUpSentResult& other) const { return !(*this == other); }
};

std::ostream& operator<<(std::ostream& out, const PickUpSentResult& result);

/** The versions a picked key covers: from `minMajor` up to `maxMajor` (none: no top), and every major from `minMajor` up while `upgradeUntil` is ahead. */
struct PickedVersions {
  /// The lowest major the key covers.
  std::int64_t minMajor = 0;
  /// The highest major it covers; none for no top.
  std::optional<std::int64_t> maxMajor;
  /// When (ms since the epoch) its upgrade window ends; none for no window.
  std::optional<std::int64_t> upgradeUntil;

  bool operator==(const PickedVersions& other) const {
    return minMajor == other.minMajor && maxMajor == other.maxMajor && upgradeUntil == other.upgradeUntil;
  }
  bool operator!=(const PickedVersions& other) const { return !(*this == other); }
};

/** The sale a picked key came with. */
struct PickedSale {
  std::string id;
  /// This key's place in the sale, from 1.
  std::int64_t index = 0;
  /// How many keys the sale made, from 1.
  std::int64_t size = 0;

  bool operator==(const PickedSale& other) const { return id == other.id && index == other.index && size == other.size; }
  bool operator!=(const PickedSale& other) const { return !(*this == other); }
};

/**
 * One key a pickup handed back: enough for the app to let the customer pick one (by `modelName`), or to let
 * License::restoreByEmail choose and activate one. Never show `key`, and never ask the customer to type anything but the
 * address and the code. Activate one yourself with `activate(picked.key, {address, picked.activationCode})`.
 */
struct PickedKey {
  /// The licence key.
  std::string key;
  /// The licence model's kind ("perpetual", "subscription"); empty when the server did not say.
  std::string model;
  /// The licence model's name, to show.
  std::optional<std::string> modelName;
  /// The key's state ("active", ...); empty when the server did not say.
  std::string state;
  /// What the key grants, read as a lease's `ent` is.
  std::map<std::string, Entitlement> entitlements;
  /// When (ms since the epoch) the key ends; none for a key that does not.
  std::optional<std::int64_t> expiresAt;
  /// The versions the key covers. None is a key sold without a version and not yet bound: its first activation binds it
  /// to the build's major, so it covers any build.
  std::optional<PickedVersions> versions;
  /// How many devices the key allows.
  std::int64_t deviceLimit = 0;
  /// How many devices hold a seat now.
  std::int64_t activeDevices = 0;
  /// True when this device already holds a seat on the key (a reinstall takes it back).
  bool held = false;
  /// The sale the key came with, when it came with one.
  std::optional<PickedSale> sale;
  /// The code a user-locked key's activation takes, already proven by the pickup (the address's code is all a lock's code
  /// proves); none on any other key.
  std::optional<std::string> activationCode;

  bool operator==(const PickedKey& other) const;
  bool operator!=(const PickedKey& other) const { return !(*this == other); }
};

/** What License::pickUp(email, code) did: the address's keys of the product, first sold first, or why not. */
struct PickUpKeysResult {
  bool ok = false;
  /// The address's keys (there may be none) when `ok`; empty otherwise.
  std::vector<PickedKey> keys;
  /// Why not, when `ok` is false.
  std::optional<PickupError> error;
  /// The seconds to wait before asking again, when the server said so.
  std::optional<std::int64_t> retryAfter;

  static PickUpKeysResult success(std::vector<PickedKey> keys) {
    return PickUpKeysResult{true, std::move(keys), std::nullopt, std::nullopt};
  }
  static PickUpKeysResult failure(PickupError why, std::optional<std::int64_t> wait = std::nullopt) {
    return PickUpKeysResult{false, {}, why, wait};
  }

  bool operator==(const PickUpKeysResult& other) const {
    return ok == other.ok && keys == other.keys && error == other.error && retryAfter == other.retryAfter;
  }
  bool operator!=(const PickUpKeysResult& other) const { return !(*this == other); }
};

std::ostream& operator<<(std::ostream& out, const PickUpKeysResult& result);

/**
 * What License::restoreByEmail did: a key of the address activated on this device, or why not. A refusal is in one of
 * three members: `pickupError` (the code was not taken), `noKey` (the address holds no key of the product) or `error`,
 * an ActivateError: `Upgrade` (the keys are for an older major than this build) or `Outdated` (their lowest version is
 * newer), activating nothing; then activate()'s own errors as they come; and `Network` behind a call that has not
 * finished, as activate() answers.
 */
struct RestoreResult {
  bool ok = false;
  /// The pickup's own refusal (the confirm's), when the code was not taken.
  std::optional<PickupError> pickupError;
  /// True when the address holds no key of the product.
  bool noKey = false;
  /// Why the key was not activated.
  std::optional<ActivateError> error;
  /// The seconds to wait before asking again, when the server said so on a pickup refusal.
  std::optional<std::int64_t> retryAfter;

  static RestoreResult success() { return RestoreResult{true, std::nullopt, false, std::nullopt, std::nullopt}; }
  static RestoreResult noKeyFound() { return RestoreResult{false, std::nullopt, true, std::nullopt, std::nullopt}; }
  static RestoreResult pickupFailure(PickupError why, std::optional<std::int64_t> wait = std::nullopt) {
    return RestoreResult{false, why, false, std::nullopt, wait};
  }
  static RestoreResult activationFailure(ActivateError why) {
    return RestoreResult{false, std::nullopt, false, why, std::nullopt};
  }

  bool operator==(const RestoreResult& other) const {
    return ok == other.ok && pickupError == other.pickupError && noKey == other.noKey && error == other.error &&
           retryAfter == other.retryAfter;
  }
  bool operator!=(const RestoreResult& other) const { return !(*this == other); }
};

std::ostream& operator<<(std::ostream& out, const RestoreResult& result);

}  // namespace keygrant
