// The lease's cryptography: base64url as the reference decodes it, SHA-256, and
// Ed25519 signature checks over OpenSSL 3.
#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct evp_pkey_st;

namespace keygrant::detail {

using Bytes = std::vector<unsigned char>;

/**
 * CANONICAL base64url, as the server's verifier reads a token's parts: the
 * URL-safe alphabet only, no padding, and no stray bits (encoding the result
 * gives the input back). None for anything else.
 */
std::optional<Bytes> base64UrlDecode(std::string_view input);

/// base64url without padding.
std::string base64UrlEncode(const unsigned char* data, std::size_t size);
std::string base64UrlEncode(std::string_view bytes);

/// The SHA-256 of the bytes (32 bytes).
Bytes sha256(std::string_view bytes);

/// Lowercase hex SHA-256 of the bytes.
std::string sha256Hex(std::string_view bytes);

/// A lease's `fp` claim for a fingerprint: hex SHA-256 of "keygrant-fp|" + fingerprint.
std::string fingerprintClaim(const std::string& fingerprint);

/** A product's Ed25519 public key. Immutable, and safe to use from several threads. */
class VerifyingKey {
 public:
  /**
   * The key from its JWK ({"kty":"OKP","crv":"Ed25519","x":"..."}) or its bare
   * `x`. Throws std::invalid_argument when it is neither, names a private key,
   * or is not 32 bytes.
   */
  static VerifyingKey fromJwk(std::string_view jwkOrX);

  /// Whether `signature` is this key's Ed25519 signature of `message`.
  bool verify(std::string_view message, const Bytes& signature) const;

  /**
   * The key's id, the `kid` a lease's header names it by: its RFC 7638
   * thumbprint, the base64url (no padding) of the SHA-256 of
   * {"crv":"Ed25519","kty":"OKP","x":"<x>"}, with `x` the key's 32 bytes
   * written canonically.
   */
  const std::string& kid() const noexcept { return kid_; }

 private:
  std::shared_ptr<evp_pkey_st> key_;
  std::string kid_;
};

/**
 * A product's key set: the key that signs its leases, and the next one once
 * the product has one staged, in the order given, a key given twice kept once.
 * Immutable, and safe to use from several threads.
 */
class KeySet {
 public:
  /**
   * The set from text: a JWK set ({"keys":[...]}, as /pubkey answers it), a
   * JSON list of JWKs, or one key as VerifyingKey::fromJwk reads it (a JWK, or
   * its bare `x`): a set of one. Throws std::invalid_argument for an empty set
   * or a key VerifyingKey::fromJwk refuses.
   */
  static KeySet fromJwk(std::string_view text);

  const std::vector<VerifyingKey>& keys() const noexcept { return keys_; }

  /// The ids of its keys, in its order: what a check-in reports as `kids`.
  std::vector<std::string> kids() const;

 private:
  std::vector<VerifyingKey> keys_;
};

}  // namespace keygrant::detail
