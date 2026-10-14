// Licence files, as the SDK decides them offline: the token a file's content
// holds, the token verified against a key set, what it says about this app, and
// the status a stored one gives. A licence file is a compact JWS signed with the
// product's Ed25519 key, as a lease is, but never taken for one, either way: its
// header is exactly {"alg":"EdDSA","typ":"kglicense+jwt","kid":"<kid>"}, and its
// claims carry none of the `aid`, `lim`, `exp` and `model` every lease reader
// requires.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "crypto.hpp"
#include "json_util.hpp"
#include "keygrant/license_file.hpp"
#include "keygrant/types.hpp"

namespace keygrant::detail {

/**
 * A licence file's claims, as the reference's schema reads them (seconds for
 * times): unknown claims ignored, an optional claim absent or of its type (null
 * is not absent, but for `ent`), an integer any whole JSON number (4.0 is 4),
 * held to the safe range (wholeNumber). A key file's versions and entitlements
 * are not read even when present.
 */
struct LicenseFileClaims {
  LicenseFileKind kind = LicenseFileKind::Key;
  std::string fid;
  std::string sub;
  std::string product;
  std::optional<std::string> name;
  std::optional<std::string> email;
  std::int64_t iat = 0;
  std::optional<bool> test;
  /// An offline file's highest major (kOpenMajor for no upper bound); 0 on a key file.
  std::int64_t major = 0;
  /// An offline file's lowest major, when it names one.
  std::optional<std::int64_t> minMajor;
  /// An offline file's entitlements, read leniently as a lease's are (entitlementsOf).
  std::map<std::string, Entitlement> ent;
};

enum class FileVerifyFailure { Malformed, NotALicenseFile, UnknownKey, BadSignature, BadClaims };

const char* to_string(FileVerifyFailure failure) noexcept;

/** A licence file's token verified: valid with its claims, or why not. */
struct FileVerifyResult {
  std::optional<FileVerifyFailure> failure;
  std::optional<LicenseFileClaims> claims;
};

/**
 * The token a licence file's content holds, or none when it is no licence
 * file's container: more than kMaxLicenseFileBytes; not UTF-8 (read strictly);
 * or not exactly one line once each line (split on LF) is trimmed of spaces,
 * tabs and carriage returns at both ends, and empty lines and lines starting
 * with `#` are left out. One leading byte-order mark is dropped; a lone CR
 * breaks no line.
 */
std::optional<std::string> licenseFileToken(std::string_view content);

/// The claims of a payload, or none when they are not a licence file's.
std::optional<LicenseFileClaims> parseFileClaims(const json& payload);

/// The claims as JSON, as the reference's schema gives them back.
json fileClaimsJson(const LicenseFileClaims& claims);

/**
 * A licence file's token verified against a key set, in this order: three
 * parts (malformed); a licence file's header, matched as bytes
 * (not-a-license-file); the signature canonical base64url (malformed); the key
 * the header names in the set (unknown-key); a signature of 64 bytes verified by
 * THAT key alone (bad-signature); the payload canonical base64url of JSON
 * matching the claims (bad-claims). It never expires.
 */
FileVerifyResult verifyLicenseFile(std::string_view token, const KeySet& keys);

/** What a licence file says about this app: taken, or why not (with its claims once the signature held). */
struct LicenseFileCheck {
  std::optional<LicenseFileError> refused;
  std::optional<LicenseFileClaims> claims;

  bool ok() const noexcept { return !refused.has_value(); }
};

/**
 * A verified licence file held to this product, this build's major and the
 * app's denylist, in this order: not valid (unknown-key, bad-signature and
 * not-a-license-file as they are; bad-claims and malformed as malformed);
 * another product (wrong-product); its id in `revoked`, matched exactly
 * (revoked-file); for an offline file, the major below minMajor (absent: 1,
 * outdated) or above major (upgrade). A key file's versions are never judged.
 */
LicenseFileCheck checkLicenseFile(const FileVerifyResult& verified, const std::string& product, std::int64_t major,
                                  const std::vector<std::string>& revoked);

/// Who a file is licensed to, as it signs it.
LicensedTo licensedToOf(const LicenseFileClaims& claims);

/**
 * The status a STORED offline licence file gives, every one with source File:
 * Licensed, with its key, licensee, `test` and entitlements; Invalid with
 * RevokedFile, Upgrade or Outdated, its key and licensee, no entitlements;
 * Invalid / UnknownKey when the build does not carry its key; and Invalid /
 * Tampered for anything else, a key file included (one is never stored).
 */
LicenseStatus licenseFileStatus(const LicenseFileCheck& check);

}  // namespace keygrant::detail
