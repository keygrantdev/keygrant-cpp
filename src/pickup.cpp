#include "pickup.hpp"

#include <openssl/rand.h>

#include <stdexcept>

#include "crypto.hpp"
#include "url.hpp"
#include "utf.hpp"

namespace keygrant::detail {

bool csprngBytes(unsigned char* out, std::size_t size) { return RAND_bytes(out, static_cast<int>(size)) == 1; }

RandomSource& purchaseSecretSource() {
  static RandomSource source = &csprngBytes;
  return source;
}

std::string newPurchaseSecret() {
  unsigned char bytes[32];
  if (!purchaseSecretSource()(bytes, sizeof bytes)) {
    throw std::runtime_error("the CSPRNG could not give a purchase secret");
  }
  return base64UrlEncode(bytes, sizeof bytes);
}

std::string purchaseReference(const std::string& secret) {
  // The secret's bytes as JavaScript would hash its string (UTF-8; ill-formed bytes as U+FFFD).
  const Bytes digest = sha256(sanitizeUtf8(secret));
  return std::string(kPurchaseReferencePrefix) + base64UrlEncode(digest.data(), digest.size());
}

std::string purchaseLink(const std::string& link, const std::string& secret) {
  return referencedLink(link, purchaseReference(secret));
}

std::optional<ClaimRequest> claimRequest(const std::string& secret, const Device& device,
                                         const std::optional<std::string>& name, std::int64_t major, bool start) {
  if (!start && !device.own) return std::nullopt;
  const std::optional<ActivationIdentity> identity = activationIdentity(device, std::nullopt);
  if (!identity) return std::nullopt;
  ClaimRequest request{secret, identity->fingerprint, identity->kind, {}, name, major};
  for (const std::string& fingerprint : device.fingerprints) {
    if (request.fingerprints.size() == kMaxClaimFingerprints) break;
    const std::size_t units = utf16Length(fingerprint);
    if (units >= 1 && units <= kMaxClaimFingerprintLength) request.fingerprints.push_back(fingerprint);
  }
  return request;
}

ordered_json claimJson(const ClaimRequest& request) {
  ordered_json body = ordered_json::object();
  body["token"] = request.token;
  body["fingerprint"] = request.fingerprint;
  if (request.fingerprintKind) body["fingerprintKind"] = to_string(*request.fingerprintKind);
  if (!request.fingerprints.empty()) body["fingerprints"] = request.fingerprints;
  if (request.name) body["name"] = *request.name;
  body["major"] = request.major;
  return body;
}

bool purchaseTokenKept(const StoredState& state, std::int64_t now) {
  if (!state.purchaseToken || !state.purchaseTokenAt) return false;
  return now - *state.purchaseTokenAt < kPurchaseTokenMs;
}

bool claimDue(const StoredState& state, std::int64_t clockNow, std::int64_t now) {
  // Fresh, by the guarded clock.
  if (!state.purchaseTokenAt || now - *state.purchaseTokenAt >= kClaimEagerMs) return false;
  // The wait over, by the device clock.
  const std::optional<std::int64_t>& asked = state.claimAskedAt;
  return !asked || clockNow < *asked || clockNow - *asked >= kClaimWaitMs;
}

StatePatch activationPatch(const Activation& a) {
  StatePatch patch;
  patch.key = Change<std::string>::set(a.key);
  patch.activationId = Change<std::string>::set(a.activationId);
  patch.lease = Change<std::string>::set(a.lease);
  patch.checkedMajor = Change<std::int64_t>::set(a.major);
  patch.keptMajor = Change<std::int64_t>::of(keptWith(std::nullopt, a.claims, a.now, a.major));
  patch += leasedPatch();
  // What this activation keyed the install by; none for a fingerprint of no kind.
  patch.deviceKind = Change<DeviceKind>::of(a.kind);
  return patch + noPurchasePatch();
}

const char* to_string(ClaimResult result) noexcept {
  switch (result) {
    case ClaimResult::Bought: return "bought";
    case ClaimResult::Kept: return "kept";
    case ClaimResult::Dropped: return "dropped";
  }
  return "kept";
}

ClaimOutcome claimOutcome(const ClaimInput& input) {
  StatePatch wait;
  wait.claimAskedAt = Change<std::int64_t>::set(input.clockNow);
  const ClaimOutcome kept{ClaimResult::Kept, std::nullopt, wait};
  const ClaimOutcome dropped{ClaimResult::Dropped, std::nullopt, noPurchasePatch()};
  if (!input.response || noAnswer(input.response->status)) return kept;
  const int status = input.response->status;
  const ClaimBody body = parseClaimBody(input.response->body);
  if (status == 200 && body.key && body.activationId && body.lease) {
    // Judged by its lease, whatever else the answer says (`pending`, an error word).
    const std::optional<LeaseCheck>& check = input.check;
    if (!check || !check->ok || !check->claims) return dropped;
    const Activation activation{*body.key, *body.activationId, *body.lease, *check->claims,
                                input.now, input.major,        input.kind};
    return ClaimOutcome{ClaimResult::Bought, leaseStatus(*check, body.key), activationPatch(activation)};
  }
  if (status == 200 && body.pending) return kept;
  return body.error ? dropped : kept;
}

}  // namespace keygrant::detail
