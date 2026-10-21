// Client-side licensing for desktop apps.
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "keygrant/adapters.hpp"
#include "keygrant/license_file.hpp"
#include "keygrant/pickup.hpp"
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
  /**
   * The ids of licence files this build refuses (a file's `fid`): how an app
   * update stops an offline licence file that leaked, since nothing else can.
   * Matched exactly. A stored file listed here reads as Invalid / RevokedFile;
   * importing one is refused (LicenseFileError::RevokedFile).
   */
  std::vector<std::string> revokedLicenseFiles;
  /// Adapter overrides; any left empty is the default.
  Adapters adapters;
};

/**
 * What a person typed for a user-locked key, for License::activate and License::importLicenseFile: the address
 * the key was sold to and, for a key that mails a code to it, the code that came. Each is optional, and
 * neither is ever stored.
 *
 * A licence model can let only the person a key was sold to activate a NEW device on it. The first activation
 * of such a key answers ActivateError::UserEmailRequired; with the address, ActivateError::UserCodeRequired (a
 * code was just mailed) or a lease; with the code, a lease. A key that is not locked ignores both once they
 * reach the server, but an address longer than any, or a code that is not six digits, is refused here for any
 * key (UserEmailMismatch, UserCodeInvalid) before anything is asked of the server.
 *
 *   license.activate(key, {std::string("mara@studio.example"), std::nullopt});
 */
struct ActivateOptions {
  /// The address the key was sold to, as the person typed it. The ends are trimmed and a blank is none; the
  /// server compares it without regard to case.
  std::optional<std::string> email;
  /// The code mailed to that address, as the person typed it. The ends are trimmed and the spaces inside
  /// dropped ("123 456" is "123456"); a blank is none. It must be exactly six ASCII digits.
  std::optional<std::string> code;
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
  /// How long a check status() makes on a valid lease, a trial heal or a use() request may hold it up.
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
 * past it, to tell it of a newer major, to spend a metered use, or to report a running trial's launch
 * (at most once a day), so a transient outage never locks out a paying customer while the lease is live.
 * A trial's start only moves earlier, and a monotonic clock guard stops a
 * rewound clock resurrecting an expired lease.
 *
 * Every call blocks, and is safe to call from several threads: calls on one
 * License run one at a time, and a call that waited past the previous call's
 * HTTP bound plus 15 s runs READ-ONLY (it answers from what is stored, with no
 * ask and no write, and says `stalled`; use() alone still asks, and saves nothing). Call it off the UI thread: a call that
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
   *
   * A user-locked key asks a new device who it is, and answers
   * ActivateError::UserEmailRequired here: the overload below carries the address and the code.
   */
  ActivateResult activate(const std::string& key);

  /**
   * As above, with what a person typed for a user-locked key (ActivateOptions): its address and, for a key
   * that mails a code to it, the code. Nothing is stored. The key is judged first, then the address, then the
   * code: an address longer than any answers ActivateError::UserEmailMismatch, and a code that is not six
   * digits ActivateError::UserCodeInvalid, at once and for any key, asking nothing (so costing the key none
   * of its wrong tries). Only an activation takes them: refresh(), deactivate() and startTrial() never ask a
   * device who it is.
   */
  ActivateResult activate(const std::string& key, const ActivateOptions& options);

  /**
   * The licence's status. Offline-first; asks the server only when forced or
   * needed, and never while a stored offline licence file licenses this build,
   * which answers first. Never throws: a state that cannot be read just now is `Unknown`
   * (reason `Storage`), and a save that fails leaves the answer standing. A
   * running trial reports its launch to the server at most once a day, on a
   * thread of its own and never awaited.
   */
  LicenseStatus status(bool forceOnline = false);

  /// status() asked online at once, whatever the waits say; a stored offline licence file that licenses this
  /// build still answers first, asking nothing.
  LicenseStatus refresh();

  /**
   * Whether the licence this device holds grants the entitlement `code`: a
   * flag that is on, a number that is not zero, or a text that is not empty
   * (grants()). Offline, from the stored lease or stored offline licence file
   * only: the entitlements of the status status() gives from what is stored,
   * with no ask and no write, so a licence that does not license this build,
   * and none at all, grant nothing.
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
   * Spend `n` uses (default 1) of the key's meter `meter`, before the work they count; do the work only when the result
   * is `ok`.
   *
   * Online it is exact: the server takes `n` only while the meter has room for all of it (UseError::UsedUp, with `left`,
   * otherwise). With no answer (no network, a timeout, a 5xx, a 429 or an answer the SDK cannot use) it spends from this
   * device's offline allowance for the meter, never past what the device last heard is left (UseError::OfflineLimit),
   * answers `offline`, and reports the spend with its next request (a check-in, a use or a deactivation). A spend the
   * allowance would refuse asks the server first, whatever the last outage left behind, so a customer who is back online
   * is not refused for it (once a minute while KeyGrant stays out of reach); one the allowance covers is not asked again
   * for a minute after an outage, growing to an hour.
   *
   * Needs a key's lease that licenses this build here (UseError::NotLicensed otherwise, asking nothing); a device trial
   * and an offline licence file have no meters. `n` from 1 to kMaxMeterUses (UseError::InvalidAmount otherwise) and a
   * meter code of 1 to 32 UTF-16 units, matched exactly (UseError::NoMeter otherwise), both without asking anything. Two
   * calls are two spends: the spend's id covers the SDK's own retries and reports, never an app that calls twice.
   *
   * Never throws: every outcome is in the result, behind a call that has not finished too (nothing is then saved: an
   * offline spend is kept in memory and saved by the next call that can), and a state that cannot be read just now is
   * UseError::Storage (a random source the platform cannot give, for the spend's id, is Storage too). The call blocks and runs to its bounded answer: a `use` request waits at most 5 seconds, or
   * LicenseConfig::httpTimeout when that is shorter. Calls on one License run one at a time, this one among them. Call it
   * off the UI thread.
   */
  UseResult use(const std::string& meter, std::int64_t n = 1);

  /**
   * Release this device's activation and drop the local licence, keeping only
   * the trial evidence, the clock guard and a stored offline licence file
   * (removeLicenseFile() drops that). Answers whether the seat is now
   * free (`released`): true when KeyGrant answered the release with a 200, or
   * when no activation was held; false when it could not be asked or answered
   * anything else, and the seat may still be held. The local licence is
   * dropped either way. The journal of unsynced metered spends goes with the
   * release (and the device's fingerprints, so the server holds it to the seat's
   * device) and is dropped with the licence, reported or not. Throws StalledError
   * behind a call that has not finished (nothing released or dropped), and the
   * storage adapter's exception when the state cannot be read or saved.
   */
  DeactivateResult deactivate();

  /**
   * Ask KeyGrant to mail a code to `email`, the address a key was sold to. For a build that may not take a typed key (an
   * App Store build): the customer types the code, then pickUp(email, code) hands back that address's keys, or
   * restoreByEmail activates the one that fits. `ok` means the code is on its way when the address holds a key: the
   * answer is the same when it holds none.
   *
   * The product's Key pickup must be on (PickupError::PickupDisabled otherwise). A blank address, or one longer than
   * any, is PickupError::InvalidEmail, asking nothing. Stores nothing, runs beside any call, and never throws: no answer
   * at all is PickupError::Network.
   */
  PickUpSentResult pickUp(const std::string& email);

  /**
   * The keys of the product that `email` was sold, for the `code` mailed to it: each with what the app may show (its
   * `modelName`) and `held` when this device already holds a seat on it (the fingerprints a validate sends go with the
   * code, when the device can tell who it is). An address with no key answers an empty list. Activate one with
   * `activate(picked.key, {address, picked.activationCode})`, or let restoreByEmail choose.
   *
   * The address is judged first (PickupError::InvalidEmail), then the code: the ends trimmed, the spaces dropped, exactly
   * six ASCII digits or PickupError::WrongCode, at once and asking nothing, so the address loses none of its wrong tries.
   * The first confirm spends the code, so the keys it answered are kept in memory for 10 minutes by the address and code
   * as sent: the same pair answers from them and asks nothing. Stores nothing (not the address, the code, the keys or a
   * refusal), runs beside any call, never throws, and a 200 that holds no list of keys is PickupError::Network.
   */
  PickUpKeysResult pickUp(const std::string& email, const std::string& code);

  /**
   * Confirm a pickup's code and activate, on this device, the key of the address that fits this build: the one this
   * device already holds a seat on, else one with a free seat, else the first. It activates as activate(key, options)
   * would, with the address as the pickup sent it and, for a code-locked key, the code the pickup handed back, so
   * nothing more is asked of the customer. The key is stored as an activation's is and never handed to the app; nothing
   * else is stored. The keys the confirm answered are kept in memory for 10 minutes (see pickUp(email, code)), so a
   * restore whose activation got no answer can be retried with the same code; a restore that activates drops them.
   *
   * Answers the confirm's errors (`pickupError`); then `noKey` for an address with no key, and ActivateError::Upgrade or
   * ActivateError::Outdated (`error`) when none of its keys covers this build (Upgrade: they are for an older major, buy
   * an upgrade; Outdated: update the app), activating nothing; then the activation's own errors, as they come. Behind a
   * call that has not finished it answers ActivateError::Network, to retry, as activate() does. Throws as activate() does,
   * when the stored state cannot be read or saved.
   */
  RestoreResult restoreByEmail(const std::string& email, const std::string& code);

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
   * storage adapter's exception when the state cannot be read. Selling
   * through Paddle, the link is the tenant's own checkout page, which gives
   * `client_reference_id` to Paddle.js as custom data under the same name.
   */
  std::optional<std::string> upgradeUrl(const std::string& paymentLink);

  /**
   * Take a licence file the customer was sent (.kglicense): its content, as a
   * drop handler or an open dialog reads it, never its path. Verified offline
   * against the build's key set, for this product and, for an offline file,
   * this build's major; refused when revokedLicenseFiles lists it (`error`,
   * nothing stored). An OFFLINE file is then stored, replacing any stored
   * before, and licenses the app with no network, ever: status() answers it
   * with source File. A KEY file's key is activated online exactly as
   * activate() would, and what that answers is `activateError`. Throws
   * StalledError behind a call that has not finished, and the storage
   * adapter's exception when the state cannot be read or saved.
   */
  ImportLicenseFileResult importLicenseFile(std::string_view content);
  /// As above, for the file's bytes.
  ImportLicenseFileResult importLicenseFile(const std::vector<std::uint8_t>& content);
  /// As above, with what a person typed for a user-locked key, passed on to a key file's activation exactly as
  /// activate() takes it. An offline file ignores it, and a file that is no licence file is refused before any
  /// key is asked for.
  ImportLicenseFileResult importLicenseFile(std::string_view content, const ActivateOptions& options);
  /// As above, for the file's bytes.
  ImportLicenseFileResult importLicenseFile(const std::vector<std::uint8_t>& content, const ActivateOptions& options);

  /**
   * Drop the stored offline licence file, if there is one; a key this device
   * holds keeps working. deactivate() leaves a stored file alone. Throws as
   * importLicenseFile() does.
   */
  void removeLicenseFile();

 private:
  friend class detail::LicenseAccess;
  std::shared_ptr<detail::Engine> engine_;
};

}  // namespace keygrant
