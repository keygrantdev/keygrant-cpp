#include "lease.hpp"

#include <algorithm>

#include "utf.hpp"

namespace keygrant::detail {

namespace {

/** Where a field stands: absent (undefined), or present with its value (which may be null). */
const json* field(const json& object, const char* name) {
  const auto it = object.find(name);
  return it == object.end() ? nullptr : &*it;
}

/** A string of `min` to `max` UTF-16 units. */
bool string(const json* value, std::size_t min, std::size_t max, std::string& out) {
  if (value == nullptr || !value->is_string()) return false;
  out = sanitizeUtf8(value->get_ref<const std::string&>());
  const std::size_t length = utf16Length(out);
  return length >= min && length <= max;
}

/** A whole number with a lower bound: `> 0` when `positive`, else `>= 0`. */
bool integer(const json* value, bool positive, std::int64_t& out) {
  if (value == nullptr) return false;
  const std::optional<std::int64_t> n = wholeNumber(*value);
  if (!n) return false;
  if (positive ? *n <= 0 : *n < 0) return false;
  out = *n;
  return true;
}

constexpr std::size_t kUnbounded = static_cast<std::size_t>(-1);

}  // namespace

std::map<std::string, Entitlement> entitlementsOf(const json& raw) {
  std::map<std::string, Entitlement> out;
  if (!raw.is_object()) return out;
  for (const auto& [name, value] : raw.items()) {
    if (value.is_boolean()) {
      out.emplace(sanitizeUtf8(name), Entitlement(value.get<bool>()));
    } else if (value.is_number()) {
      out.emplace(sanitizeUtf8(name), Entitlement(value.get<double>()));
    } else if (value.is_string()) {
      out.emplace(sanitizeUtf8(name), Entitlement(sanitizeUtf8(value.get_ref<const std::string&>())));
    }
  }
  return out;
}

json entitlementsJson(const std::map<std::string, Entitlement>& entitlements) {
  json out = json::object();
  for (const auto& [name, value] : entitlements) {
    switch (value.kind()) {
      case Entitlement::Kind::Flag: out[name] = *value.flag(); break;
      case Entitlement::Kind::Number: out[name] = *value.number(); break;
      case Entitlement::Kind::Text: out[name] = *value.text(); break;
    }
  }
  return out;
}

const char* to_string(VerifyFailure failure) noexcept {
  switch (failure) {
    case VerifyFailure::Malformed: return "malformed";
    case VerifyFailure::UnknownKey: return "unknown-key";
    case VerifyFailure::BadSignature: return "bad-signature";
    case VerifyFailure::BadClaims: return "bad-claims";
    case VerifyFailure::Expired: return "expired";
  }
  return "malformed";
}

std::optional<LeaseClaims> parseClaims(const json& payload) {
  if (!payload.is_object()) return std::nullopt;
  LeaseClaims c;
  std::string iss;
  if (!string(field(payload, "iss"), 0, kUnbounded, iss) || iss != "keygrant") return std::nullopt;
  if (!string(field(payload, "sub"), 1, kUnbounded, c.sub)) return std::nullopt;
  if (!string(field(payload, "product"), 1, kUnbounded, c.product)) return std::nullopt;
  if (!string(field(payload, "aid"), 1, kUnbounded, c.aid)) return std::nullopt;
  if (const json* fp = field(payload, "fp")) {
    std::string value;
    if (!string(fp, 1, 128, value)) return std::nullopt;
    c.fp = std::move(value);
  }
  if (const json* fpk = field(payload, "fpk")) {
    std::string value;
    if (!string(fpk, 0, kUnbounded, value)) return std::nullopt;
    c.fpk = deviceKindFromString(value);
    if (!c.fpk) return std::nullopt;
  }
  if (!string(field(payload, "model"), 0, kUnbounded, c.model)) return std::nullopt;
  if (c.model != "perpetual" && c.model != "subscription" && c.model != "trial") return std::nullopt;
  if (!integer(field(payload, "lim"), true, c.lim)) return std::nullopt;
  if (const json* major = field(payload, "major")) {
    if (!integer(major, false, c.major)) return std::nullopt;
  }
  if (const json* minMajor = field(payload, "minMajor")) {
    std::int64_t value = 0;
    if (!integer(minMajor, true, value)) return std::nullopt;
    c.minMajor = value;
  }
  if (const json* upgradeUntil = field(payload, "upgradeUntil")) {
    std::int64_t value = 0;
    if (!integer(upgradeUntil, false, value)) return std::nullopt;
    c.upgradeUntil = value;
  }
  // Read leniently, never refused: absent or malformed is none (entitlementsOf).
  if (const json* ent = field(payload, "ent")) c.ent = entitlementsOf(*ent);
  if (!integer(field(payload, "iat"), false, c.iat)) return std::nullopt;
  if (!integer(field(payload, "exp"), false, c.exp)) return std::nullopt;
  if (const json* grace = field(payload, "grace")) {
    if (!grace->is_boolean()) return std::nullopt;
    c.grace = grace->get<bool>();
  }
  if (const json* test = field(payload, "test")) {
    if (!test->is_boolean()) return std::nullopt;
    c.test = test->get<bool>();
  }
  return c;
}

json claimsJson(const LeaseClaims& c) {
  json out = json::object();
  out["iss"] = "keygrant";
  out["sub"] = c.sub;
  out["product"] = c.product;
  out["aid"] = c.aid;
  if (c.fp) out["fp"] = *c.fp;
  if (c.fpk) out["fpk"] = to_string(*c.fpk);
  out["model"] = c.model;
  out["lim"] = c.lim;
  out["major"] = c.major;
  if (c.minMajor) out["minMajor"] = *c.minMajor;
  if (c.upgradeUntil) out["upgradeUntil"] = *c.upgradeUntil;
  out["ent"] = entitlementsJson(c.ent);
  out["iat"] = c.iat;
  out["exp"] = c.exp;
  if (c.grace) out["grace"] = *c.grace;
  if (c.test) out["test"] = *c.test;
  return out;
}

std::optional<std::string> keyIdOf(std::string_view header) {
  return namedKeyId(header, R"({"alg":"EdDSA","typ":"JWT","kid":")");
}

std::optional<std::string> namedKeyId(std::string_view header, std::string_view start) {
  static constexpr std::string_view kEnd = R"("})";
  const std::optional<Bytes> bytes = base64UrlDecode(header);
  if (!bytes) return std::nullopt;
  const std::string text(bytes->begin(), bytes->end());
  // Long enough for both ends apart: `..."kid":"}` ends with `"}` only through the start's own quote.
  if (text.size() < start.size() + kEnd.size()) return std::nullopt;
  if (text.compare(0, start.size(), start) != 0 || text.compare(text.size() - kEnd.size(), kEnd.size(), kEnd) != 0) {
    return std::nullopt;
  }
  const std::string kid = text.substr(start.size(), text.size() - start.size() - kEnd.size());
  const auto base64url = [](unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
  };
  if (kid.empty() || kid.size() > 64 || !std::all_of(kid.begin(), kid.end(), base64url)) return std::nullopt;
  return kid;
}

namespace {

/**
 * The keys a lease is verified by: the one its header names (none when the set
 * lacks it), else each key of the set in turn.
 */
std::vector<const VerifyingKey*> candidatesFor(std::string_view header, const KeySet& keys) {
  const std::optional<std::string> kid = keyIdOf(header);
  std::vector<const VerifyingKey*> candidates;
  for (const VerifyingKey& key : keys.keys()) {
    if (!kid || key.kid() == *kid) candidates.push_back(&key);
  }
  return candidates;
}

/// The payload's claims, or none when it is not canonical base64url of the claims' JSON.
std::optional<LeaseClaims> claimsOf(std::string_view payload) {
  const std::optional<Bytes> decoded = base64UrlDecode(payload);
  if (!decoded) return std::nullopt;
  return parseClaims(parseJsonText(std::string(decoded->begin(), decoded->end())));
}

VerifyResult failed(VerifyFailure reason) {
  VerifyResult result;
  result.reason = reason;
  return result;
}

}  // namespace

VerifyResult verifyLease(std::string_view token, const KeySet& keys, std::int64_t now) {
  const std::size_t first = token.find('.');
  const std::size_t second = first == std::string_view::npos ? first : token.find('.', first + 1);
  if (second == std::string_view::npos || token.find('.', second + 1) != std::string_view::npos) {
    return failed(VerifyFailure::Malformed);
  }
  const std::optional<Bytes> signature = base64UrlDecode(token.substr(second + 1));
  if (!signature) return failed(VerifyFailure::Malformed);
  // The key the header names, else each key in turn; one it names that the set lacks is told apart.
  const std::vector<const VerifyingKey*> candidates = candidatesFor(token.substr(0, first), keys);
  if (candidates.empty()) return failed(VerifyFailure::UnknownKey);
  // The signing input is the token's own text, as received (TextEncoder of the JS string).
  const std::string signedInput = sanitizeUtf8(token.substr(0, second));
  const bool holds = std::any_of(candidates.begin(), candidates.end(),
                                 [&](const VerifyingKey* key) { return key->verify(signedInput, *signature); });
  if (!holds) return failed(VerifyFailure::BadSignature);
  std::optional<LeaseClaims> claims = claimsOf(token.substr(first + 1, second - first - 1));
  if (!claims) return failed(VerifyFailure::BadClaims);
  VerifyResult result;
  // exp is at most 2^53 - 1 seconds: times 1000 it still fits.
  if (claims->exp * 1000 < now) {
    result.reason = VerifyFailure::Expired;
    result.claims = std::move(claims);
    return result;
  }
  result.valid = true;
  result.claims = std::move(claims);
  return result;
}

}  // namespace keygrant::detail
