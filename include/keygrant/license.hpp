// Client-side licensing for desktop apps (sdk/src/license.ts, engine.ts).
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
   * The product's public key, embedded at build time for offline verification:
   * the JWK as JSON ({"kty":"OKP","crv":"Ed25519","x":"..."}), or its bare `x`.
   */
  std::string publicJwk;
  /// The running app's major version, 1 to kMaxMajor.
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
 * past it, or to tell it of a newer major, so a transient outage never locks out
 * a paying customer while the lease is live. A trial's start only moves
 * earlier, and a monotonic clock guard stops a rewound clock resurrecting an
 * expired lease.
 *
 * Every call blocks, and is safe to call from several threads: calls on one
 * License run one at a time, and a call that waited past the previous call's
 * HTTP bound plus 15 s runs READ-ONLY (it answers from what is stored, with no
 * ask and no write, and says `stalled`). Call it off the UI thread: a call that
 * asks the server can take up to the HTTP bound.
 *
 * Destroying a License while a call is stuck in an adapter is safe: that call
 * finishes on its own.
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
   */
  LicenseStatus status(bool forceOnline = false);

  /// status() asked online at once, whatever the waits say.
  LicenseStatus refresh();

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
   * be saved, until a later call can.
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

 private:
  std::shared_ptr<detail::Engine> engine_;
};

}  // namespace keygrant
