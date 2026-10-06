// A KeyGrant lease: a compact JWS (EdDSA over Ed25519) whose payload carries
// these claims (shared/src/lease.ts), verified offline (shared/src/crypto.ts).
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
  std::int64_t iat = 0;
  std::int64_t exp = 0;
  std::optional<bool> grace;
  std::optional<bool> test;
};

enum class VerifyFailure { Malformed, BadSignature, BadClaims, Expired };

const char* to_string(VerifyFailure failure) noexcept;

/** What verifying a lease gives: valid, or why not; the claims when valid or only expired. */
struct VerifyResult {
  bool valid = false;
  std::optional<VerifyFailure> reason;
  std::optional<LeaseClaims> claims;
};

/**
 * `ent` as a verifier reads it (docs/specs/sdk-contract.md section 2.1;
 * shared/src/lease.ts entitlementsOf): an object's flag, number and text
 * members, kept as they are (a number as a double); every other member (an
 * object, a list, null) dropped; and nothing at all from anything but an
 * object (null, a list, a text, a number, a flag). Never a refusal: a lease
 * that verifies is the server's word, and one malformed entitlement must not
 * take away the licence it rides on, or the entitlements beside it. Names are
 * kept as they are, `__proto__` and any other, and no cap the server keeps
 * (how many, how long) is enforced: the signature already vouches for them.
 */
std::map<std::string, Entitlement> entitlementsOf(const json& raw);

/// Entitlements as JSON writes them: an object of flags, numbers (doubles) and texts.
json entitlementsJson(const std::map<std::string, Entitlement>& entitlements);

/**
 * The claims of a payload, checked as the reference's zod schema checks them
 * (shared/src/lease.ts LeaseClaims): none when any is missing or wrong.
 * Unknown claims are ignored, and `ent` is read leniently (entitlementsOf).
 */
std::optional<LeaseClaims> parseClaims(const json& payload);

/// The claims as JSON, defaults applied (major 0, ent {}), as the reference's schema outputs them.
json claimsJson(const LeaseClaims& claims);

/**
 * Verify a lease offline, in the reference's order: three parts (else
 * malformed; the header is not read), the signature over `header.payload` as
 * received (else bad-signature; malformed when it cannot be decoded), the
 * claims (else bad-claims), then `exp * 1000 < now` is expired (the claims
 * still given). A lease is still valid AT its `exp` second.
 */
VerifyResult verifyLease(std::string_view token, const VerifyingKey& key, std::int64_t now);

}  // namespace keygrant::detail
