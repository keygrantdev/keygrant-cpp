// The lease's cryptography (shared/src/crypto.ts): base64url as the reference
// decodes it, SHA-256, and Ed25519 signature checks over OpenSSL 3.
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
 * CANONICAL base64url, as the reference's b64urlDecode reads a token's parts:
 * the URL-safe alphabet only, no padding, and no stray bits (encoding the
 * result gives the input back). None for anything else.
 */
std::optional<Bytes> base64UrlDecode(std::string_view input);

/**
 * Forgiving base64 (WHATWG atob, after `-`/`_` are mapped to `+`/`/`): white
 * space dropped, padding and stray bits allowed. For a configured key's `x`,
 * never for a token.
 */
std::optional<Bytes> base64Forgiving(std::string_view input);

/// base64url without padding.
std::string base64UrlEncode(const unsigned char* data, std::size_t size);
std::string base64UrlEncode(std::string_view bytes);

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

 private:
  std::shared_ptr<evp_pkey_st> key_;
};

}  // namespace keygrant::detail
