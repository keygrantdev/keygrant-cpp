// The online validate (a renewal), decided without I/O: when status() asks the server while it
// holds a lease that licenses this build (`asksOnValidLease`, license.cpp), and what an ask's
// answer saves and says (`renewalOutcome`, engine.cpp's renew). The engine makes the request,
// judges the lease a 200 carried, and saves the patch as the answer to its ask.
//
// Licensing fails in favour of the paying customer: only an explicit verdict
// (a lease, or a refusal refusalOf names) gives a status. No answer at all, a
// 5xx, a 429, any other error, and a 200 without a lease that can be used give
// none, and the caller's own answer (a valid lease, a stored refusal, the
// lease's own end) stands.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "keygrant/types.hpp"
#include "lease.hpp"
#include "offline.hpp"
#include "wire.hpp"

namespace keygrant::detail {

/** What an ask's answer is judged from. */
struct RenewalInput {
  /// The answer; none when there was none at all (the request threw or timed out).
  std::optional<Reply> response;
  /// The offline check (checkLease) of the lease the 200 carried, made as of `now` on this device; none when it carried none.
  std::optional<LeaseCheck> check;
  /// When the ask was made, by the device clock (ms): where the wait before asking again starts.
  std::int64_t askedAt = 0;
  /// The time the lease was judged at (ms): the guarded clock, reconciled to the server's (serverNow).
  std::int64_t now = 0;
  /// This build's major, sent with the ask.
  std::int64_t major = 1;
  /// The stored key, which a status names.
  std::optional<std::string> key;
  /// The stored kept major (keptWith).
  std::optional<std::int64_t> keptMajor;
  /// What this device's own fingerprint hashes, as the ask read it: a machine id keys the install (keyedBy).
  std::optional<DeviceKind> kind;
  /// The claim of this device's own fingerprint: what a device refusal is held to (refusalPatch).
  std::optional<std::string> ownClaim;
};

/** What an ask's answer does. */
struct RenewalOutcome {
  /// Whether the server answered at all: false for no answer, a 5xx or a 429.
  bool answered = false;
  /// The status the answer gives; none when it gives none, and the caller's own answer stands.
  std::optional<LicenseStatus> status;
  /// What is saved, as the answer to the ask (merged, basis `answer`).
  StatePatch patch;
};

/**
 * What a validate's answer saves and says. Every patch carries
 * `deviceKind: machine` when this run read a machine id (keyedBy): the seat is
 * re-keyed by it whatever the answer, so the install is keyed by it from now
 * on.
 *
 *  - no answer at all: wait kRetryUnansweredMs, no status;
 *  - a 5xx or a 429: wait kAskAgainMs, no status;
 *  - a 200 with a lease that licenses this build: the lease, the major heard
 *    (`checkedMajor`), the kept major (keptWith), any refusal and wait
 *    cleared (leasedPatch): licensed, with its `test` flag and entitlements
 *    (leaseStatus);
 *  - a 200 with a lease the server stands behind that does not license this
 *    build or this device (`upgrade`, `outdated`, `device`): kept as above,
 *    with no kept major, so every later status says the same offline: invalid
 *    with that reason, and no entitlements (leaseStatus);
 *  - any other 200 (no lease, or one expired or not this licence's): the major
 *    heard and a wait of kAskAgainMs, no status; never a lockout on a bad body;
 *  - a refusal (refusalOf of the body's error word, on any status but a 200,
 *    a 5xx or a 429): the major heard, the wait, and the refusal
 *    (refusalPatch): invalid with it;
 *  - any other answer (a proxy's 4xx, an unknown product, a body that is not
 *    an object): the major heard and the wait, no status.
 */
RenewalOutcome renewalOutcome(const RenewalInput& input);

/**
 * Whether status(), holding a lease that licenses this build, asks the server
 * now (unforced; refresh() always asks). When the server should hear from this
 * device before the lease runs out: a newer major than the server last heard,
 * on an open lease (`major >= kOpenMajor`), or a lease nearing its end
 * (renewsEarly, by the guarded clock `now`). And only once the wait after the
 * last ask is over (askDue, by the device clock `clockNow`).
 */
bool asksOnValidLease(const LeaseClaims& claims, const StoredState& state, std::int64_t major, std::int64_t now,
                      std::int64_t clockNow);

}  // namespace keygrant::detail
