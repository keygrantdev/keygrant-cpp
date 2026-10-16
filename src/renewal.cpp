#include "renewal.hpp"

#include "meters.hpp"

namespace keygrant::detail {

namespace {

/**
 * Whether a lease that does not license here is one the server stands behind:
 * not for this build (`upgrade`, `outdated`) or not for this device
 * (`device`). Not one expired, not this licence's, or signed by a key this
 * build does not carry.
 */
bool standsBehind(LeaseVerdict verdict) {
  switch (verdict) {
    case LeaseVerdict::Upgrade:
    case LeaseVerdict::Outdated:
    case LeaseVerdict::Device: return true;
    case LeaseVerdict::Expired:
    case LeaseVerdict::Tampered:
    case LeaseVerdict::UnknownKey: return false;
  }
  return false;
}

/** The major the server heard: what any answer it gave records. */
StatePatch heard(std::int64_t major) {
  StatePatch patch;
  patch.checkedMajor = Change<std::int64_t>::set(major);
  return patch;
}

/** A 200's answer (renewalOutcome): by the lease it carried, as checked here. */
RenewalOutcome leaseOutcome(const RenewalInput& input, const Reply& response, const StatePatch& backoff) {
  const std::optional<std::string> lease = parseLeaseBody(response.body).lease;
  if (truthy(lease) && input.check) {
    const LeaseCheck& check = *input.check;
    StatePatch leased = heard(input.major);
    leased.lease = Change<std::string>::set(*lease);
    // Licensed, with the lease's `test` flag and entitlements (leaseStatus).
    if (check.ok) {
      // A check without claims names no upgrade window: the kept major as it was.
      const std::optional<std::int64_t> kept =
          check.claims ? keptWith(input.keptMajor, *check.claims, input.now, input.major) : input.keptMajor;
      leased.keptMajor = Change<std::int64_t>::of(kept);
      return RenewalOutcome{true, leaseStatus(check, input.key), leased + leasedPatch() + keyedBy(input.kind)};
    }
    // A lease the server stands behind that does not license this build, or
    // this device: kept, so every later status() says the same offline.
    if (standsBehind(check.reason)) {
      return RenewalOutcome{true, leaseStatus(check, input.key), leased + leasedPatch() + keyedBy(input.kind)};
    }
  }
  // 200 but no usable lease: nothing to heal with; whatever the caller has is
  // kept (fail in favour of the customer) rather than locked out on a bad body.
  return RenewalOutcome{true, std::nullopt, heard(input.major) + backoff};
}

}  // namespace

RenewalOutcome renewalOutcome(const RenewalInput& input) {
  const StatePatch keyed = keyedBy(input.kind);
  StatePatch backoff;
  backoff.backoffSince = Change<std::int64_t>::set(input.askedAt);
  if (!input.response) {
    backoff.backoffMs = Change<std::int64_t>::set(kRetryUnansweredMs);
    return RenewalOutcome{false, std::nullopt, backoff + keyed};
  }
  backoff.backoffMs = Change<std::int64_t>::set(kAskAgainMs);
  backoff += keyed;
  const Reply& response = *input.response;
  if (noAnswer(response.status)) return RenewalOutcome{false, std::nullopt, backoff};
  if (response.status == 200) return leaseOutcome(input, response, backoff);
  // Lock out ONLY on an explicit definitive negative verdict. Everything else
  // (a proxy's or WAF's 4xx, an unknown-product 404, a malformed-body 400, any
  // answer not recognised) is treated like a network failure, so a still-valid
  // lease is never revoked by an ambiguous answer.
  const std::optional<Refusal> refused = refusalOf(errorWord(response.body));
  StatePatch patch = heard(input.major) + backoff;
  if (!refused) return RenewalOutcome{true, std::nullopt, patch};
  // On a verdict, the next status() must not license offline what the server
  // has just refused (refusalPatch: what is dropped, what is kept).
  patch += refusalPatch(*refused, input.major, input.ownClaim);
  return RenewalOutcome{true, invalidStatus(reasonOf(*refused), input.key), patch};
}

bool asksOnValidLease(const LeaseClaims& claims, const StoredState& state, std::int64_t major, std::int64_t now,
                      std::int64_t clockNow) {
  const bool newMajor = claims.major >= kOpenMajor && major > state.checkedMajor.value_or(0);
  const bool due = newMajor || renewsEarly(claims, now) || journalAsks(state.spends, now);
  return due && askDue(state, clockNow, false);
}

}  // namespace keygrant::detail
