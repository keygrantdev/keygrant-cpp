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
  // A user-locked key's new device: each word is its own error.
  if (*error == "user-email-required") return ActivateError::UserEmailRequired;
  if (*error == "user-email-mismatch") return ActivateError::UserEmailMismatch;
  if (*error == "user-code-required") return ActivateError::UserCodeRequired;
  if (*error == "user-code-invalid") return ActivateError::UserCodeInvalid;
  if (*error == "too-many-codes") return ActivateError::TooManyCodes;
  if (*error == "too-many-attempts") return ActivateError::TooManyAttempts;
  if (*error == "try-again") return ActivateError::TryAgain;
  return ActivateError::Network;
}

Typed typedFor(const std::optional<std::string>& email, const std::optional<std::string>& code) {
  Typed out;
  // The address, trimmed as a key is: a blank is none, and a longer one can match no key's address. The server
  // compares it without regard to case, so it is sent as typed.
  std::string enteredEmail = email ? jsTrim(*email) : std::string();
  if (utf16Length(enteredEmail) > kMaxEmail) {
    out.refused = ActivateError::UserEmailMismatch;
    return out;
  }
  // The code, its ends trimmed as a key's are and then its spaces (U+0020) dropped, since a code read off an
  // email is typed as 123 456 as often as 123456: exactly six ASCII digits, else refused. A byte of 0x20 is
  // only ever U+0020 in UTF-8, so dropping bytes drops exactly those.
  std::string enteredCode;
  if (code) {
    for (const char c : jsTrim(*code)) {
      if (c != ' ') enteredCode.push_back(c);
    }
  }
  const bool sixDigits = enteredCode.size() == kCodeDigits &&
                         std::all_of(enteredCode.begin(), enteredCode.end(), [](char c) { return c >= '0' && c <= '9'; });
  if (!enteredCode.empty() && !sixDigits) {
    out.refused = ActivateError::UserCodeInvalid;
    return out;
  }
  if (!enteredEmail.empty()) out.email = std::move(enteredEmail);
  if (!enteredCode.empty()) out.code = std::move(enteredCode);
  return out;
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

std::optional<std::int64_t> earliest(std::initializer_list<std::optional<std::int64_t>> values) {
  std::optional<std::int64_t> out;
  for (const auto& value : values) {
    if (value && (!out || *value < *out)) out = value;
  }
  return out;
}

}  // namespace keygrant::detail
