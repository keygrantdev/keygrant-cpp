#include "lease.hpp"

#include "utf.hpp"

namespace keygrant::detail {

namespace {

/** Where a field stands: absent (undefined), or present with its value (which may be null). */
const json* field(const json& object, const char* name) {
  const auto it = object.find(name);
  return it == object.end() ? nullptr : &*it;
}

/** z.string().min(min).max(max), lengths in UTF-16 units. */
bool string(const json* value, std::size_t min, std::size_t max, std::string& out) {
  if (value == nullptr || !value->is_string()) return false;
  out = sanitizeUtf8(value->get_ref<const std::string&>());
  const std::size_t length = utf16Length(out);
  return length >= min && length <= max;
}

/** z.number().int() with a lower bound: `> 0` when `positive`, else `>= 0`. */
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

VerifyResult verifyLease(std::string_view token, const VerifyingKey& key, std::int64_t now) {
  VerifyResult result;
  const std::size_t first = token.find('.');
  const std::size_t second = first == std::string_view::npos ? first : token.find('.', first + 1);
  if (second == std::string_view::npos || token.find('.', second + 1) != std::string_view::npos) {
    result.reason = VerifyFailure::Malformed;
    return result;
  }
  const std::string_view signingInput = token.substr(0, second);
  const std::string_view payload = token.substr(first + 1, second - first - 1);
  const std::optional<Bytes> signature = base64UrlDecode(token.substr(second + 1));
  if (!signature) {
    result.reason = VerifyFailure::Malformed;
    return result;
  }
  // The signing input is the token's own text, as received (TextEncoder of the JS string).
  if (!key.verify(sanitizeUtf8(signingInput), *signature)) {
    result.reason = VerifyFailure::BadSignature;
    return result;
  }
  const std::optional<Bytes> decoded = base64UrlDecode(payload);
  std::optional<LeaseClaims> claims;
  if (decoded) {
    const std::string text(decoded->begin(), decoded->end());
    claims = parseClaims(parseJsonText(text));
  }
  if (!claims) {
    result.reason = VerifyFailure::BadClaims;
    return result;
  }
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
