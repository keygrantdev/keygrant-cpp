// Buying from the app with no key typed, decided without I/O: the purchase
// secret purchaseUrl() keeps (newPurchaseSecret), the reference to it a link
// carries (purchaseReference, purchaseLink, built as upgradeUrl's link is:
// referencedLink), how long a secret is held (purchaseTokenKept), when status()
// asks for the key bought with it (claimDue), what an activation saves
// (activationPatch, a typed key's and a bought key's alike), and what a claim's
// answer saves and says (claimOutcome). purchases.cpp makes the requests and
// the saves.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "device.hpp"
#include "json_util.hpp"
#include "keygrant/types.hpp"
#include "lease.hpp"
#include "offline.hpp"
#include "wire.hpp"

namespace keygrant::detail {

/// What starts every purchase reference, so the server tells one apart from an activation id or a key.
inline constexpr const char* kPurchaseReferencePrefix = "kgp_";

/** Fills `size` bytes at `out`, or answers false when it cannot. */
using RandomSource = bool (*)(unsigned char* out, std::size_t size);

/** OpenSSL's CSPRNG (RAND_bytes, seeded by the operating system's): the source a secret is drawn from. */
bool csprngBytes(unsigned char* out, std::size_t size);

/**
 * The source newPurchaseSecret() draws from: csprngBytes. Test support swaps
 * it (to see what is drawn, or a source that fails), not public API.
 */
RandomSource& purchaseSecretSource();

/**
 * A new purchase secret: the base64url (no padding) of 32 bytes from the
 * CSPRNG (purchaseSecretSource), 43 characters. It stays on the device
 * (`purchaseToken`) and is sent only in the claim: a link carries the
 * reference to it (purchaseReference), so a link that leaks (browser history,
 * the tenant's Stripe session) claims nothing. Random, and never the
 * fingerprint: a fingerprint is no secret. Throws std::runtime_error when the
 * CSPRNG cannot give the bytes.
 */
std::string newPurchaseSecret();

/**
 * The reference a payment link carries for `secret`: `kgp_` and the base64url
 * (no padding) of the SHA-256 of the secret's UTF-8 bytes, 47 characters. The
 * checkout keeps it on the key it mints; the server derives it from the secret
 * a claim sends, to find that key.
 */
std::string purchaseReference(const std::string& secret);

/** The link purchaseUrl() answers for a held `secret`: the link with its reference (referencedLink). */
std::string purchaseLink(const std::string& link, const std::string& secret);

/**
 * Whether the stored purchase secret is still held at `now` (the guarded
 * clock): there is one, stamped with when purchaseUrl last handed out its link
 * (`purchaseTokenAt`), and less than kPurchaseTokenMs has passed since. A
 * stamp ahead of `now` holds it too; a secret with no stamp is not held.
 */
bool purchaseTokenKept(const StoredState& state, std::int64_t now);

/**
 * Whether status() asks the claim now, unforced (refresh() and startTrial()
 * always ask). Only while the link is fresh: less than kClaimEagerMs since
 * purchaseUrl last handed it out (`purchaseTokenAt`, judged at `now`, the
 * guarded clock; a stamp ahead of `now` is fresh), so an abandoned checkout
 * does not put the network in front of every launch for a month. And only once
 * the wait is over (`claimAskedAt`, judged at `clockNow`, the device clock, as
 * askDue: the wait is courtesy, not protection): not asked since the link, the
 * device clock set back before the last ask, or kClaimWaitMs since it. The two
 * clocks are never compared with each other.
 */
bool claimDue(const StoredState& state, std::int64_t clockNow, std::int64_t now);

/// At most this many fingerprints in a claim's `fingerprints`: the server's bound.
inline constexpr std::size_t kMaxClaimFingerprints = 4;

/// At most this many characters (UTF-16 units) in each of a claim's `fingerprints`: the server's bound.
inline constexpr std::size_t kMaxClaimFingerprintLength = 200;

/** What a claim sends (claimRequest). */
struct ClaimRequest {
  /// The purchase secret.
  std::string token;
  std::string fingerprint;
  std::optional<DeviceKind> fingerprintKind;
  /// None is left out of the body.
  std::vector<std::string> fingerprints;
  std::optional<std::string> name;
  std::int64_t major = 1;
};

/**
 * The claim's body, or none when none is sent. The fingerprint is a first
 * activation's (activationIdentity: the device's own, else the host name), and
 * from status() and refresh() (`start` false) only the device's own: a claim
 * under the host-name fallback could take the seat under a fingerprint the
 * next ask does not send, so with no own fingerprint nothing is sent (and
 * nothing saved). `fingerprints` is every fingerprint the run read, its own
 * first, then the host name and any other alternates, whether or not the
 * device is held (they only match this claim to an earlier one, and judge no
 * seat), within the server's bounds: those of 1 to kMaxClaimFingerprintLength
 * UTF-16 units, the first kMaxClaimFingerprints of them, never one cut (a cut
 * fingerprint matches nothing); left out when none is. A body with too many,
 * or one too long, is refused with an error word, which would drop the secret.
 */
std::optional<ClaimRequest> claimRequest(const std::string& secret, const Device& device,
                                         const std::optional<std::string>& name, std::int64_t major, bool start);

/** A claim's body as JSON: `{token, fingerprint, fingerprintKind?, fingerprints?, name?, major}`. */
ordered_json claimJson(const ClaimRequest& request);

/** An activation the server answered with a lease that licenses this build (activationPatch). */
struct Activation {
  std::string key;
  std::string activationId;
  std::string lease;
  /// The lease's claims.
  LeaseClaims claims;
  /// The time the lease was judged at.
  std::int64_t now = 0;
  /// This build's major.
  std::int64_t major = 1;
  /// What the activation's fingerprint hashes: none for one of no kind.
  std::optional<DeviceKind> kind;
};

/**
 * What an activation saves, a typed key's (activate()) and a bought key's
 * claim (claimOutcome) alike, stamped as an activation: the key, the
 * activation, the lease, the major heard, the kept major (keptWith), any
 * refusal and wait cleared (leasedPatch), what the install is keyed by
 * (cleared for a fingerprint of no kind), and the purchase secret and its
 * stamps cleared (noPurchasePatch).
 */
StatePatch activationPatch(const Activation& activation);

/** What a claim's answer is judged from (claimOutcome). */
struct ClaimInput {
  /// The answer; none when there was none (it threw or timed out, or no fingerprint could be sent).
  std::optional<Reply> response;
  /**
   * The offline check (checkLease, as of `now`, for the key and activation it
   * came with, on this device) of the lease a 200 carried with a key and an
   * activation id; none otherwise.
   */
  std::optional<LeaseCheck> check;
  /// The device clock when the claim was asked: where the wait before asking again starts.
  std::int64_t clockNow = 0;
  /// The time the lease was judged at: the guarded clock, reconciled to the server's.
  std::int64_t now = 0;
  /// This build's major.
  std::int64_t major = 1;
  /// What the claim's fingerprint hashes.
  std::optional<DeviceKind> kind;
};

/** What a claim's answer does: `Bought` licenses, `Kept` waits, `Dropped` lets the secret go. */
enum class ClaimResult { Bought, Kept, Dropped };

const char* to_string(ClaimResult result) noexcept;

struct ClaimOutcome {
  ClaimResult result = ClaimResult::Kept;
  /// The licensed status of a bought key; none otherwise.
  std::optional<LicenseStatus> status;
  /// What is saved: as an activation when bought, else about the secret.
  StatePatch patch;
};

/**
 * What a claim's answer saves and says:
 *  - a 200 with a key, an activation id and a lease (strings) whose lease
 *    licenses this build here (`check`): saved exactly as activate(key) saves
 *    (activationPatch), and licensed (leaseStatus);
 *  - a 200 with all three whose lease does not license this build here: the
 *    secret dropped (the customer has the key by email, and typing it gives
 *    the proper error);
 *  - a 200 `{pending: true}` ("not bought yet"), no answer, a 5xx or a 429,
 *    and any other answer with no error word (a captive portal's page, an old
 *    server's 404 page): the secret kept, and the wait started (`claimAskedAt`);
 *  - any other answer with an error word (`spent`, `pickup-off`, a refusal):
 *    the secret dropped.
 * Only a bought key gives a status: otherwise the caller answers as it would
 * with no secret held.
 */
ClaimOutcome claimOutcome(const ClaimInput& input);

}  // namespace keygrant::detail
