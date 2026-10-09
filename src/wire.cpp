#include "wire.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "utf.hpp"

namespace keygrant::detail {

std::string checkedProduct(const std::string& product) {
  if (product.empty() || product.size() > 64) throw std::invalid_argument("product: 1 to 64 characters");
  if (product == "." || product == "..") throw std::invalid_argument("product: not a slug");
  for (const char c : product) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte <= 0x20 || byte == 0x7F || c == '/' || c == '\\' || c == '?' || c == '#' || c == '%') {
      throw std::invalid_argument("product: not a slug (no spaces, slashes, '?', '#' or '%')");
    }
  }
  if (!isValidUtf8(product)) throw std::invalid_argument("product: not UTF-8");
  return product;
}

std::optional<std::string> errorWord(const json& body) {
  if (!body.is_object()) return std::nullopt;
  const auto it = body.find("error");
  if (it == body.end() || !it->is_string()) return std::nullopt;
  return it->get<std::string>();
}

ActivateError mapServerError(const std::optional<std::string>& error) {
  if (!error) return ActivateError::Network;
  if (*error == "invalid-key") return ActivateError::InvalidKey;
  if (*error == "limit") return ActivateError::Limit;
  if (*error == "revoked" || *error == "expired") return ActivateError::Revoked;
  if (*error == "upgrade-required") return ActivateError::Upgrade;
  if (*error == "update-required") return ActivateError::Outdated;
  return ActivateError::Network;
}

std::optional<Refusal> refusalOf(const std::optional<std::string>& error) {
  if (!error) return std::nullopt;
  if (*error == "upgrade-required") return Refusal::Upgrade;
  if (*error == "update-required") return Refusal::Outdated;
  if (*error == "revoked" || *error == "expired" || *error == "deactivated") return Refusal::Revoked;
  // The server's own check of this device's fingerprints against the seat.
  if (*error == "device") return Refusal::Device;
  return std::nullopt;
}

std::optional<std::string> enteredKey(const std::string& key) {
  std::string entered = jsUpper(jsTrim(key));
  if (entered.empty() || utf16Length(entered) > kMaxKey) return std::nullopt;
  return entered;
}

std::optional<std::string> trimToLength(const std::optional<std::string>& value, std::size_t max) {
  if (!value) return std::nullopt;
  const std::string text = sanitizeUtf8(*value);
  std::size_t units = 0;
  for (const CodePoint& cp : decodeUtf8(text)) {
    const std::size_t width = cp.value >= 0x10000 ? 2 : 1;
    // A character that does not fit whole is dropped whole: JavaScript's slice
    // would keep its high surrogate, which trimToLength then drops.
    if (units + width > max) return text.substr(0, cp.offset);
    units += width;
  }
  return text;
}

LicenseStatus licensedStatus(const std::optional<std::string>& key, bool test,
                             std::map<std::string, Entitlement> entitlements) {
  LicenseStatus status;
  status.state = LicenseState::Licensed;
  status.key = key;
  status.test = test;
  status.entitlements = std::move(entitlements);
  return status;
}

LicenseStatus invalidStatus(LicenseReason reason, const std::optional<std::string>& key) {
  LicenseStatus status;
  status.state = LicenseState::Invalid;
  status.reason = reason;
  status.key = key;
  return status;
}

LicenseReason reasonOf(Refusal refusal) {
  switch (refusal) {
    case Refusal::Revoked: return LicenseReason::Revoked;
    case Refusal::Upgrade: return LicenseReason::Upgrade;
    case Refusal::Outdated: return LicenseReason::Outdated;
    case Refusal::Device: return LicenseReason::Device;
  }
  return LicenseReason::Revoked;
}

LeaseBody parseLeaseBody(const json& body) {
  LeaseBody out;
  if (!body.is_object()) return out;
  if (const auto it = body.find("lease"); it != body.end() && it->is_string()) out.lease = it->get<std::string>();
  if (const auto it = body.find("activationId"); it != body.end() && it->is_string()) {
    out.activationId = it->get<std::string>();
  }
  return out;
}

std::optional<TrialBody> parseTrialBody(const json& body) {
  if (!body.is_object()) return std::nullopt;
  const auto start = body.find("trialStart");
  const auto ends = body.find("trialEndsAt");
  if (start == body.end() || ends == body.end()) return std::nullopt;
  const std::optional<std::int64_t> trialStart = anyNumber(*start);
  const std::optional<std::int64_t> trialEndsAt = anyNumber(*ends);
  if (!trialStart || !trialEndsAt) return std::nullopt;
  TrialBody out{*trialStart, *trialEndsAt, std::nullopt};
  if (const auto it = body.find("lease"); it != body.end() && it->is_string()) out.lease = it->get<std::string>();
  return out;
}

ClaimBody parseClaimBody(const json& body) {
  ClaimBody out;
  if (!body.is_object()) return out;
  const auto text = [&body](const char* name) -> std::optional<std::string> {
    const auto it = body.find(name);
    if (it == body.end() || !it->is_string()) return std::nullopt;
    return it->get<std::string>();
  };
  out.key = text("key");
  out.activationId = text("activationId");
  out.lease = text("lease");
  const auto pending = body.find("pending");
  out.pending = pending != body.end() && pending->is_boolean() && pending->get<bool>();
  if (std::optional<std::string> error = text("error"); error && !error->empty()) out.error = std::move(error);
  return out;
}

std::optional<std::int64_t> earliest(std::initializer_list<std::optional<std::int64_t>> values) {
  std::optional<std::int64_t> out;
  for (const auto& value : values) {
    if (value && (!out || *value < *out)) out = value;
  }
  return out;
}

}  // namespace keygrant::detail
