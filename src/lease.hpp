// A KeyGrant lease: a compact JWS (EdDSA over Ed25519) whose payload carries these claims, verified
// offline.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "crypto.hpp"
#include "json_util.hpp"
#include "keygrant/types.hpp"

namespace keygrant::detail {

/**
 * A meter's figures as a lease signs them or an answer gives them, without the time of the answer: the base of what a
 * status shows.
 */
struct BaseFigure {
  /// What the key had left when it was signed.
  std::int64_t left = 0;
  /// The uses the key holds.
  std::int64_t total = 0;
  /// The allowance each of its devices may spend with no answer.
  std::int64_t offline = 0;
  /// Whether the meter comes back each billing period.
  bool resets = false;

  bool operator==(const BaseFigure& other) const {
    return left == other.left && total == other.total && offline == other.offline && resets == other.resets;
  }
  bool operator!=(const BaseFigure& other) const { return !(*this == other); }
};

/// A key's meters by code, as a lease signs them.
using Meters = std::map<std::string, BaseFigure>;

/** The claims, as the reference's schema reads them (times in seconds). */
struct LeaseClaims {
  std::string sub;
  std::string product;
  std::string aid;
  std::optional<std::string> fp;
  std::optional<DeviceKind> fpk;
  std::string model;
  std::int64_t lim = 0;
  /// Highest major covered; 0 when absent; kOpenMajor when open-ended.
  std::int64_t major = 0;
  std::optional<std::int64_t> minMajor;
  std::optional<std::int64_t> upgradeUntil;
  /// Entitlements, read leniently (entitlementsOf); none when absent or malformed.
  std::map<std::string, Entitlement> ent;
  /// The key's meters (`met`), read leniently (metersOf); none when the lease has no such claim, and an empty map for
  /// a claim that is not an object.
  std::optional<Meters> met;
  std::int64_t iat = 0;
  std::int64_t exp = 0;
  std::optional<bool> grace;
  std::optional<bool> test;
};

enum class VerifyFailure { Malformed, UnknownKey, BadSignature, BadClaims, Expired };

const char* to_string(VerifyFailure failure) noexcept;

/** What verifying a lease gives: valid, or why not; the claims when valid or only expired. */
struct VerifyResult {
  bool valid = false;
  std::optional<VerifyFailure> reason;
  std::optional<LeaseClaims> claims;
};

/**
 * `ent` as a verifier reads it: an object's flag, number and text members, kept as they are (a
 * number as a double); every other member (an object, a list, null) dropped; and nothing at all
 * from anything but an object (null, a list, a text, a number, a flag). Never a refusal: a lease
 * that verifies is the server's word, and one malformed entitlement must not take away the licence
 * it rides on, or the entitlements beside it. Names are kept as they are, `__proto__` and any
 * other, and no cap the server keeps (how many, how long) is enforced: the signature already
 * vouches for them.
 */
std::map<std::string, Entitlement> entitlementsOf(const json& raw);

/// Entitlements as JSON writes them: an object of flags, numbers (doubles) and texts.
json entitlementsJson(const std::map<std::string, Entitlement>& entitlements);

/**
 * `met`, or the `meters` of a `use` answer, as a verifier reads it: the members that are objects whose `left`, `total`
 * and `offline` are each a figure (figureOf), `resets` true only for JSON true; every other member dropped, never the
 * lease, the answer, or the meters beside it; and no meters at all from anything but an object. Names are kept as they
 * are, `__proto__` included, and a meter's other members are ignored. The server's caps (how many meters, how large a
 * figure, `left` at most `total`) are not enforced: the signature is the server's word.
 */
Meters metersOf(const json& raw);

/// Meters as the vectors write them: `resets` always there.
json metersJson(const Meters& meters);

/**
 * The claims of a payload, checked as the server's claims schema checks them: none when any is
 * missing or wrong. Unknown claims are ignored, and `ent` and `met` are read leniently (entitlementsOf, metersOf).
 */
std::optional<LeaseClaims> parseClaims(const json& payload);

/// The claims as JSON, defaults applied (major 0, ent {}), as the reference's schema outputs them.
json claimsJson(const LeaseClaims& claims);

/**
 * Verify a lease offline against a key set, in the reference's order: three
 * parts (else malformed); the signature, canonical base64url (else malformed);
 * the key the header names by its id (`kid`), unknown-key when the set holds
 * none of that id, or each key in turn for a lease that names none; the
 * signature over `header.payload` as received (else bad-signature); the claims
 * (else bad-claims), then `exp * 1000 < now` is expired (the claims still
 * given). A lease is still valid AT its `exp` second. The header is read for
 * `kid` only: the algorithm is the verifier's, never the token's.
 */
VerifyResult verifyLease(std::string_view token, const KeySet& keys, std::int64_t now);

/**
 * The key a lease's header names (`kid`), or none. A header names a key only
 * when it is, byte for byte, the header the server writes for one: canonical
 * base64url of {"alg":"EdDSA","typ":"JWT","kid":"<kid>"}, <kid> 1 to 64
 * characters of the base64url alphabet. Any other header names no key. It is
 * matched as bytes, never parsed.
 */
std::optional<std::string> keyIdOf(std::string_view header);

/**
 * The key id a header names when it is, byte for byte, canonical base64url of
 * <start><kid>"}, <kid> 1 to 64 characters of the base64url alphabet; none for
 * any other header. A lease's header starts {"alg":"EdDSA","typ":"JWT","kid":",
 * a licence file's {"alg":"EdDSA","typ":"kglicense+jwt","kid":".
 */
std::optional<std::string> namedKeyId(std::string_view header, std::string_view start);

}  // namespace keygrant::detail
