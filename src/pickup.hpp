// Key pickup, decided without I/O: what a pickup call sends for what a person typed, the error each refusal of the
// server is, the keys a confirm hands back (read leniently), and the key a restore activates. The calls themselves are
// engine_pickup.cpp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "json_util.hpp"
#include "keygrant/pickup.hpp"

namespace keygrant::detail {

/// How long the keys a confirm answered are kept in memory (never stored), for a confirm or a restore with the same
/// address and code: a code's own life. The first confirm spends the code.
inline constexpr std::int64_t kPickupKeysKeepMs = 10 * 60'000;

/** What a pickup call sends for what a person typed, or the error that refuses it before anything is sent. */
struct PickupAsk {
  /// The address, trimmed.
  std::string email;
  /// The code (spaces dropped, six ASCII digits); none for the ask for a code.
  std::optional<std::string> code;
  std::optional<PickupError> refused;
};

/**
 * What a pickup call sends for what a person typed. `code` none is the ask for a code (pickUp(email)); a code, even a
 * blank one, is a confirm. The address is entered as an activation's is: trimmed, and a blank one, or one longer than
 * any address, is InvalidEmail. The code is entered as an activation's is (ends trimmed, spaces dropped): a blank one,
 * or one that is not six ASCII digits, is WrongCode, refused here without costing the address one of its wrong tries.
 * The address is judged first.
 */
PickupAsk pickupAsk(const std::string& email, const std::optional<std::string>& code);

/** A refused pickup call: the error, and the seconds to wait when the server said. */
struct PickupRefusal {
  PickupError error = PickupError::Network;
  std::optional<std::int64_t> retryAfter;
};

/**
 * The error a refused pickup call is, from the server's body: each pickup word as itself; the per-network limit
 * (`rate-limited`) as TooManyAttempts; a count that could not be made (`unavailable`) as TryAgain; a body the server
 * refused to read (`invalid-request`) as InvalidEmail, since what a person typed is all a pickup sends; and anything
 * else, a proxy's page or an unknown product included, as Network. The status is never read. `retryAfter` is kept, on
 * the two errors that wait, when the body gives it as a whole number of seconds.
 */
PickupRefusal pickupRefusal(const json& body);

/**
 * The keys a confirm's 200 answers, read leniently, in the order given; none when the body is not a pickup's answer at
 * all (not an object, or `keys` not a list), which the SDK takes as no answer (Network). A member that is not an object
 * with a `key` (text, 1 to kMaxKey UTF-16 units) is left out; every other field falls back when it is not what it should
 * be.
 */
std::optional<std::vector<PickedKey>> pickedKeysOf(const json& body);

/**
 * Whether a key covers the build's `major` at `now` (ms): a key not yet bound always does; else from `minMajor` up to
 * `maxMajor` (none: no top), and every major from `minMajor` up while `upgradeUntil` is after `now`.
 */
bool covers(const std::optional<PickedVersions>& versions, std::int64_t major, std::int64_t now);

/** Why no picked key is activated. */
enum class Unchosen { NoKey, Upgrade, Outdated };

/// "no-key", "upgrade", "outdated".
const char* to_string(Unchosen value) noexcept;

/** The place in the keys of the one a restore activates, or why none is. */
struct Choice {
  std::optional<std::size_t> index;
  std::optional<Unchosen> refused;
};

/**
 * The key restoreByEmail activates, by its place in `keys` (the server's order: first sold first), or why none is. Of
 * the keys that cover the build's `major`: the first this device already holds a seat on (`held`, a reinstall takes that
 * seat back), else the first with a free seat, else the first (its activation answers Limit). No key at all is NoKey;
 * none that covers the build is Upgrade when one of them is for an older major than the build (buy an upgrade), else
 * Outdated (update the app).
 */
Choice chosenKey(const std::vector<PickedKey>& keys, std::int64_t major, std::int64_t now);

}  // namespace keygrant::detail
