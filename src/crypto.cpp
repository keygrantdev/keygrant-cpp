#include "crypto.hpp"

#include <openssl/err.h>
#include <openssl/evp.h>

#include <stdexcept>

#include "json_util.hpp"

namespace keygrant::detail {

namespace {

constexpr const char* kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int sextet(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

bool asciiWhitespace(char c) { return c == '\t' || c == '\n' || c == '\f' || c == '\r' || c == ' '; }

/** WHATWG forgiving-base64 decode, as atob runs it. */
std::optional<Bytes> forgivingBase64(std::string_view input) {
  std::string data;
  data.reserve(input.size());
  for (char c : input) {
    if (!asciiWhitespace(c)) data.push_back(c);
  }
  if (data.size() % 4 == 0) {
    if (data.size() >= 2 && data[data.size() - 1] == '=' && data[data.size() - 2] == '=') {
      data.resize(data.size() - 2);
    } else if (!data.empty() && data.back() == '=') {
      data.pop_back();
    }
  }
  if (data.size() % 4 == 1) return std::nullopt;
  Bytes out;
  out.reserve(data.size() * 3 / 4);
  unsigned buffer = 0;
  int bits = 0;
  for (char c : data) {
    const int value = sextet(c);
    if (value < 0) return std::nullopt;
    buffer = (buffer << 6) | static_cast<unsigned>(value);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<unsigned char>((buffer >> bits) & 0xFFu));
    }
  }
  return out;
}

struct PkeyDeleter {
  void operator()(EVP_PKEY* key) const { EVP_PKEY_free(key); }
};

struct MdCtxDeleter {
  void operator()(EVP_MD_CTX* ctx) const { EVP_MD_CTX_free(ctx); }
};

std::string requireString(const json& jwk, const char* name) {
  const auto it = jwk.find(name);
  if (it == jwk.end() || !it->is_string()) {
    throw std::invalid_argument(std::string("publicJwk: \"") + name + "\" is missing or not a string");
  }
  return it->get<std::string>();
}

/** The JWK's `x`, checked as WebCrypto checks an Ed25519 public key it imports for `verify`. */
std::string jwkX(std::string_view text) {
  const json jwk = json::parse(std::string(text), nullptr, false);
  if (jwk.is_discarded() || !jwk.is_object()) throw std::invalid_argument("publicJwk: not a JSON object");
  if (requireString(jwk, "kty") != "OKP") throw std::invalid_argument("publicJwk: kty is not \"OKP\"");
  if (requireString(jwk, "crv") != "Ed25519") throw std::invalid_argument("publicJwk: crv is not \"Ed25519\"");
  if (jwk.contains("d")) {
    throw std::invalid_argument("publicJwk: this is a PRIVATE key (it has \"d\"); embed the public JWK only");
  }
  if (jwk.contains("alg")) {
    const std::string alg = requireString(jwk, "alg");
    if (alg != "EdDSA" && alg != "Ed25519") throw std::invalid_argument("publicJwk: alg is not EdDSA");
  }
  if (jwk.contains("use") && requireString(jwk, "use") != "sig") throw std::invalid_argument("publicJwk: use is not \"sig\"");
  if (const auto ops = jwk.find("key_ops"); ops != jwk.end()) {
    bool verifies = false;
    if (ops->is_array()) {
      for (const json& op : *ops) verifies = verifies || (op.is_string() && op.get<std::string>() == "verify");
    }
    if (!verifies) throw std::invalid_argument("publicJwk: key_ops does not include \"verify\"");
  }
  return requireString(jwk, "x");
}

}  // namespace

std::optional<Bytes> base64UrlDecode(std::string_view input) {
  if (input.size() % 4 == 1) return std::nullopt;
  Bytes out;
  out.reserve(input.size() * 3 / 4);
  unsigned buffer = 0;
  int bits = 0;
  for (const char c : input) {
    int value = -1;
    if (c == '-') {
      value = 62;
    } else if (c == '_') {
      value = 63;
    } else if (c != '+' && c != '/') {
      value = sextet(c);
    }
    if (value < 0) return std::nullopt;
    buffer = (buffer << 6) | static_cast<unsigned>(value);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<unsigned char>((buffer >> bits) & 0xFFu));
    }
  }
  // Canonical: no stray bits in the last character, so it encodes back to itself.
  if (base64UrlEncode(out.data(), out.size()) != input) return std::nullopt;
  return out;
}

std::optional<Bytes> base64Forgiving(std::string_view input) {
  std::string s;
  s.reserve(input.size());
  for (char c : input) {
    if (asciiWhitespace(c)) continue;
    if (c == '-') c = '+';
    if (c == '_') c = '/';
    s.push_back(c);
  }
  const std::size_t pad = s.size() % 4 == 0 ? 0 : 4 - s.size() % 4;
  s.append(pad, '=');
  return forgivingBase64(s);
}

std::string base64UrlEncode(const unsigned char* data, std::size_t size) {
  std::string out;
  out.reserve((size + 2) / 3 * 4);
  std::size_t i = 0;
  for (; i + 2 < size; i += 3) {
    const unsigned n = (unsigned{data[i]} << 16) | (unsigned{data[i + 1]} << 8) | unsigned{data[i + 2]};
    out.push_back(kAlphabet[(n >> 18) & 63]);
    out.push_back(kAlphabet[(n >> 12) & 63]);
    out.push_back(kAlphabet[(n >> 6) & 63]);
    out.push_back(kAlphabet[n & 63]);
  }
  if (i + 1 == size) {
    const unsigned n = unsigned{data[i]} << 16;
    out.push_back(kAlphabet[(n >> 18) & 63]);
    out.push_back(kAlphabet[(n >> 12) & 63]);
  } else if (i + 2 == size) {
    const unsigned n = (unsigned{data[i]} << 16) | (unsigned{data[i + 1]} << 8);
    out.push_back(kAlphabet[(n >> 18) & 63]);
    out.push_back(kAlphabet[(n >> 12) & 63]);
    out.push_back(kAlphabet[(n >> 6) & 63]);
  }
  for (char& c : out) {
    if (c == '+') c = '-';
    if (c == '/') c = '_';
  }
  return out;
}

std::string base64UrlEncode(std::string_view bytes) {
  return base64UrlEncode(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
}

Bytes sha256(std::string_view bytes) {
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int length = 0;
  if (EVP_Digest(bytes.data(), bytes.size(), digest, &length, EVP_sha256(), nullptr) != 1) {
    ERR_clear_error();
    throw std::runtime_error("SHA-256 failed");
  }
  return Bytes(digest, digest + length);
}

std::string sha256Hex(std::string_view bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  for (const unsigned char byte : sha256(bytes)) {
    out.push_back(kHex[byte >> 4]);
    out.push_back(kHex[byte & 0x0F]);
  }
  return out;
}

std::string fingerprintClaim(const std::string& fingerprint) { return sha256Hex("keygrant-fp|" + fingerprint); }

VerifyingKey VerifyingKey::fromJwk(std::string_view jwkOrX) {
  std::size_t from = 0;
  std::size_t to = jwkOrX.size();
  while (from < to && asciiWhitespace(jwkOrX[from])) ++from;
  while (to > from && asciiWhitespace(jwkOrX[to - 1])) --to;
  const std::string_view text = jwkOrX.substr(from, to - from);
  if (text.empty()) throw std::invalid_argument("publicJwk is empty");
  const std::string x = text.front() == '{' ? jwkX(text) : std::string(text);
  const std::optional<Bytes> raw = base64Forgiving(x);
  if (!raw || raw->size() != 32) throw std::invalid_argument("publicJwk: x is not a 32-byte base64url Ed25519 key");
  EVP_PKEY* key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, raw->data(), raw->size());
  if (key == nullptr) {
    ERR_clear_error();
    throw std::invalid_argument("publicJwk: OpenSSL refused the Ed25519 key");
  }
  VerifyingKey out;
  out.key_ = std::shared_ptr<EVP_PKEY>(key, PkeyDeleter{});
  return out;
}

bool VerifyingKey::verify(std::string_view message, const Bytes& signature) const {
  // An Ed25519 signature is 64 bytes; anything else is a bad signature, as WebCrypto answers it.
  if (!key_ || signature.size() != 64) return false;
  std::unique_ptr<EVP_MD_CTX, MdCtxDeleter> ctx(EVP_MD_CTX_new());
  if (!ctx || EVP_DigestVerifyInit(ctx.get(), nullptr, nullptr, nullptr, key_.get()) != 1) {
    ERR_clear_error();
    return false;
  }
  const int rc = EVP_DigestVerify(ctx.get(), signature.data(), signature.size(),
                                  reinterpret_cast<const unsigned char*>(message.data()), message.size());
  if (rc != 1) ERR_clear_error();
  return rc == 1;
}

}  // namespace keygrant::detail
