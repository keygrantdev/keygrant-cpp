// Client-side licensing for desktop apps.
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "keygrant/adapters.hpp"
#include "keygrant/types.hpp"

namespace keygrant {

/// How long a request may take when the call names no shorter bound of its own.
inline constexpr std::chrono::milliseconds kDefaultHttpTimeout{15'000};

/** How a License is set up. */
struct LicenseConfig {
  /// Product slug, matching the KeyGrant product.
  std::string product;
  /// KeyGrant API base, e.g. "https://api.keygrant.dev".
  std::string apiBaseUrl;
  /**
   * The product's public keys, embedded at build time for offline
   * verification: its key set, the `keys` of
   * GET https://api.keygrant.dev/v1/products/<product>/pubkey as
   * {"keys":[...]} (or that whole answer, or a JSON list of JWKs): the key
   * that signs its leases, and the next one once the product has one staged.
   * One key is a set of one: its JWK ({"kty":"OKP","crv":"Ed25519","x":"..."})
   * or its bare `x`. A lease naming its key (`kid`) is verified by that key;
   * one naming none by each in turn.
   */
  std::string publicJwk;
  /**
   * The running app's major version, at most kMaxMajor. Read as the server
   * reads a version: a 0.x build (0) runs as major 1, as does anything below 1.
   */
  std::int64_t major = 1;
  /// A human name for this device, shown in the customer's device list (cut to 120 UTF-16 units).
  std::optional<std::string> deviceName;
  /**
   * Whether a licence is held to the device it was activated on. False for a
   * product deployed on non-persistent, pooled VDI, where every session may be
   * another machine: no seat is keyed to the device, no lease is judged by it,
   * and the server is sent no fingerprints to check.
   */
  bool deviceHold = true;
  /// How long a request may take before the SDK abandons it (kept whatever the HTTP adapter does).
  std::chrono::milliseconds httpTimeout = kDefaultHttpTimeout;
  /// Adapter overrides; any left empty is the default.
  Adapters adapters;
};

namespace detail {
class Engine;
class LicenseAccess;

/**
 * The SDK's fixed timings. NOT a stable API: these are the reference's
 * constants, and exist as a struct only so tests can run them shorter.
 */
struct Timings {
  /// How long past its HTTP bound a call may hold the next one waiting.
  std::chrono::milliseconds lockGrace{15'000};
  /// How long a check status() makes on a valid lease may hold it up.
  std::chrono::milliseconds statusCheck{5'000};
  /// How long status() may spend reading who the device is.
  std::chrono::milliseconds machineId{2'000};
  /// How long activate() and startTrial() may.
  std::chrono::milliseconds activateMachineId{10'000};
  /// How much longer than its own bound a fingerprinter's identify() is given.
  std::chrono::milliseconds identifyExtra{1'000};
};
}  // namespace detail

/**
 * Client-side licensing for desktop apps. Verifies signed JWT leases OFFLINE
 * and only contacts the server to activate, to renew a lease nearing its end or
 * past it, to tell it of a newer major, to pick up a key bought through
 * purchaseUrl(), or to report a running trial's launch (at most once a day), so
 * a transient outage never locks out a paying customer while the lease is live.
 * A trial's start only moves earlier, and a monotonic clock guard stops a
 * rewound clock resurrecting an expired lease.
 *
 * Every call blocks, and is safe to call from several threads: calls on one
 * License run one at a time, and a call that waited past the previous call's
 * HTTP bound plus 15 s runs READ-ONLY (it answers from what is stored, with no
 * ask and no write, and says `stalled`). Call it off the UI thread: a call that
 * asks the server can take up to the HTTP bound.
 *
 * Destroying a License while a call is stuck in an adapter, or while a
 * running trial's launch report is in flight, is safe: either finishes on its
 * own.
 */
class License {
 public:
  /// Throws std::invalid_argument for a bad config (an unparseable public key, an empty product, ...).
  explicit License(LicenseConfig config);
  /// As above, with the SDK's timings shortened (tests only).
  License(LicenseConfig config, const detail::Timings& timings);
  ~License();

  License(const License&) = delete;
  License& operator=(const License&) = delete;
  License(License&&) = delete;
  License& operator=(License&&) = delete;

  /**
   * Activate a licence key on this device. The key is trimmed and upper-cased.
   * Throws when the stored state cannot be read or saved (the storage
   * adapter's exception, e.g. IoError): the customer asked, and can try again.
   */
  ActivateResult activate(const std::string& key);

  /**
   * The licence's status. Offline-first; asks the server only when forced or
   * needed. Never throws: a state that cannot be read just now is `Unknown`
   * (reason `Storage`), and a save that fails leaves the answer standing.
   *
   * With no key held and a purchase secret held (purchaseUrl), the key bought
   * with it is asked for, under this machine's own fingerprint only: for a
   * day after the link, once 5 minutes have passed since the last ask
   * (bounded by 5 s), and at once when forced, until the secret's 30 days are
   * over. A running trial reports its launch to the server at most once a
   * day, on a thread of its own and never awaited.
   */
  LicenseStatus status(bool forceOnline = false);

  /// status() asked online at once, whatever the waits say.
  LicenseStatus refresh();

  /**
   * Whether the licence this device holds grants the entitlement `code`: a
   * flag that is on, a number that is not zero, or a text that is not empty
   * (grants()). Offline, from the stored lease only: the entitlements of the
   * status status() gives from what is stored, with no ask and no write, so a
   * licence that does not license this build, and none at all, grant nothing.
   * A running trial grants what its trial lease does. Never throws: a state
   * that cannot be read grants nothing just now.
   */
  bool has(const std::string& code);

  /**
   * As has(), but throwing MissingEntitlement, which names `code`, when the
   * licence does not grant that entitlement: for work that may run only when
   * it does.
   */
  void require(const std::string& code);

  /**
   * Release this device's activation and drop the local licence, keeping only
   * the trial evidence and the clock guard. Answers whether the seat is now
   * free (`released`): true when KeyGrant answered the release with a 200, or
   * when no activation was held; false when it could not be asked or answered
   * anything else, and the seat may still be held. The local licence is
   * dropped either way. Throws StalledError behind a call that has not
   * finished (nothing released or dropped), and the storage adapter's
   * exception when the state cannot be read or saved.
   */
  DeactivateResult deactivate();

  /**
   * Begin (or re-sync) a trial for this device. Never throws: as status() for
   * storage, and the server's trial is kept for this process when it cannot
   * be saved, until a later call can. The device is read once for both its
   * asks. With no key held and a purchase secret held (purchaseUrl), the key
   * bought with it is asked for first, always, bounded by 5 s: bought, it is
   * the answer (`Licensed`) and no trial is asked.
   */
  LicenseStatus startTrial();

  /**
   * `paymentLink` with this device's ACTIVATION id as `client_reference_id`,
   * or std::nullopt when the device holds no activated licence. Never the
   * licence key. Runs beside any other call, reading only. Throws
   * std::invalid_argument for a link that is not an absolute URL, and the
   * storage adapter's exception when the state cannot be read.
   */
  std::optional<std::string> upgradeUrl(const std::string& paymentLink);

  /**
   * `paymentLink` with a PURCHASE REFERENCE as its `client_reference_id`, or
   * std::nullopt while the device holds a key (any key: use upgradeUrl(), or
   * the plain link). Send a customer with no key there to buy: the checkout
   * keeps the reference on the key it mints, and once they are back the SDK
   * asks for that key (at once on refresh() or startTrial(), and from
   * status() for a day after the link), activates this device on it and
   * answers `Licensed`, with nothing typed.
   *
   * The reference is a hash of a one-time secret (256 random bits, never the
   * fingerprint) that stays on the device and is sent only in that ask, so a
   * link that leaks claims nothing. The secret is made the first time, kept,
   * and its link handed out again by every later call; it is dropped 30 days
   * after the last one, or once a key bought with it is picked up. Runs one at
   * a time with the other calls; behind a call that has not finished, a held
   * secret's link is handed out as it is, and none held throws StalledError.
   * Throws std::invalid_argument for a link that is not an absolute URL
   * (before anything is read), the storage adapter's exception when the
   * state cannot be read or the secret cannot be saved (a link whose secret
   * is not kept could never be picked up), and std::runtime_error when the
   * CSPRNG cannot give a new secret.
   */
  std::optional<std::string> purchaseUrl(const std::string& paymentLink);

 private:
  friend class detail::LicenseAccess;
  std::shared_ptr<detail::Engine> engine_;
};

}  // namespace keygrant
