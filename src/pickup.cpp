#include "pickup.hpp"

#include <algorithm>
#include <utility>

#include "lease.hpp"
#include "utf.hpp"
#include "wire.hpp"

namespace keygrant {

const char* to_string(PickupError value) noexcept {
  switch (value) {
    case PickupError::InvalidEmail: return "invalid-email";
    case PickupError::WrongCode: return "wrong-code";
    case PickupError::CodeExpired: return "code-expired";
    case PickupError::TooManyAttempts: return "too-many-attempts";
    case PickupError::PickupDisabled: return "pickup-disabled";
    case PickupError::TryAgain: return "try-again";
    case PickupError::Network: return "network";
  }
  return "network";
}

std::ostream& operator<<(std::ostream& out, PickupError value) { return out << to_string(value); }

namespace {

/// "{ ok: false, error: wrong-code, retryAfter: 60 }", each member only when there is one.
void describe(std::ostream& out, bool ok, const std::optional<PickupError>& error,
              const std::optional<std::int64_t>& retryAfter) {
  out << "{ ok: " << (ok ? "true" : "false");
  if (error) out << ", error: " << *error;
  if (retryAfter) out << ", retryAfter: " << *retryAfter;
  out << " }";
}

}  // namespace

std::ostream& operator<<(std::ostream& out, const PickUpSentResult& result) {
  describe(out, result.ok, result.error, result.retryAfter);
  return out;
}

std::ostream& operator<<(std::ostream& out, const PickUpKeysResult& result) {
  describe(out, result.ok, result.error, result.retryAfter);
  return out << " (" << result.keys.size() << " keys)";
}

std::ostream& operator<<(std::ostream& out, const RestoreResult& result) {
  out << "{ ok: " << (result.ok ? "true" : "false");
  if (result.pickupError) out << ", pickupError: " << *result.pickupError;
  if (result.noKey) out << ", noKey: true";
  if (result.error) out << ", error: " << *result.error;
  if (result.retryAfter) out << ", retryAfter: " << *result.retryAfter;
  return out << " }";
}

bool PickedKey::operator==(const PickedKey& other) const {
  return key == other.key && model == other.model && modelName == other.modelName && state == other.state &&
         entitlements == other.entitlements && expiresAt == other.expiresAt && versions == other.versions &&
         deviceLimit == other.deviceLimit && activeDevices == other.activeDevices && held == other.held &&
         sale == other.sale && activationCode == other.activationCode;
}

namespace detail {

PickupAsk pickupAsk(const std::string& email, const std::optional<std::string>& code) {
  PickupAsk out;
  out.email = jsTrim(email);
  if (out.email.empty() || utf16Length(out.email) > kMaxEmail) {
    out.refused = PickupError::InvalidEmail;
    return out;
  }
  if (!code) return out;
  // The ends trimmed as a key's are and then the spaces (U+0020) dropped: a byte of 0x20 is only ever U+0020 in
  // UTF-8, so dropping bytes drops exactly those.
  std::string entered;
  for (const char c : jsTrim(*code)) {
    if (c != ' ') entered.push_back(c);
  }
  const bool sixDigits = entered.size() == kCodeDigits &&
                         std::all_of(entered.begin(), entered.end(), [](char c) { return c >= '0' && c <= '9'; });
  if (!sixDigits) {
    out.refused = PickupError::WrongCode;
    return out;
  }
  out.code = std::move(entered);
  return out;
}

PickupRefusal pickupRefusal(const json& body) {
  PickupRefusal out;
  const std::optional<std::string> word = errorWord(body);
  if (!word) {
    out.error = PickupError::Network;
  } else if (*word == "pickup-disabled") {
    out.error = PickupError::PickupDisabled;
  } else if (*word == "wrong-code") {
    out.error = PickupError::WrongCode;
  } else if (*word == "code-expired") {
    out.error = PickupError::CodeExpired;
  } else if (*word == "too-many-attempts" || *word == "rate-limited") {
    out.error = PickupError::TooManyAttempts;
  } else if (*word == "try-again" || *word == "unavailable") {
    out.error = PickupError::TryAgain;
  } else if (*word == "invalid-request") {
    out.error = PickupError::InvalidEmail;
  } else {
    out.error = PickupError::Network;
  }
  if (out.error == PickupError::TooManyAttempts || out.error == PickupError::TryAgain) {
    out.retryAfter = figureMember(body, "retryAfter");
  }
  return out;
}

namespace {

/// Exactly six ASCII digits.
bool isCode(const std::string& text) {
  return text.size() == kCodeDigits && std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
}

/// The member `name` of an object as text, or none when it is missing or no text.
std::optional<std::string> textMember(const json& object, const char* name) {
  const auto it = object.find(name);
  if (it == object.end() || !it->is_string()) return std::nullopt;
  return sanitizeUtf8(it->get_ref<const std::string&>());
}

/// A key's versions as read: none for a key not yet bound and for anything that is not an object with a whole
/// `minMajor`; else `minMajor`, and `maxMajor` and `upgradeUntil` each a whole number or none.
std::optional<PickedVersions> versionsOf(const json& object) {
  const auto it = object.find("versions");
  if (it == object.end()) return std::nullopt;
  const std::optional<std::int64_t> minMajor = figureMember(*it, "minMajor");
  if (!minMajor) return std::nullopt;
  return PickedVersions{*minMajor, figureMember(*it, "maxMajor"), figureMember(*it, "upgradeUntil")};
}

/// A key's sale, kept only with a text `id` and whole `index` and `size` of at least 1.
std::optional<PickedSale> saleOf(const json& object) {
  const auto it = object.find("sale");
  if (it == object.end() || !it->is_object()) return std::nullopt;
  const std::optional<std::string> id = textMember(*it, "id");
  const auto index = it->find("index");
  const auto size = it->find("size");
  if (!id || index == it->end() || size == it->end()) return std::nullopt;
  const std::optional<std::int64_t> place = countOf(*index);
  const std::optional<std::int64_t> count = countOf(*size);
  if (!place || !count) return std::nullopt;
  return PickedSale{*id, *place, *count};
}

/// One key of a confirm's answer, or none when it has no key.
std::optional<PickedKey> pickedKey(const json& raw) {
  if (!raw.is_object()) return std::nullopt;
  const std::optional<std::string> key = textMember(raw, "key");
  if (!key || key->empty() || utf16Length(*key) > kMaxKey) return std::nullopt;
  PickedKey out;
  out.key = *key;
  out.model = textMember(raw, "model").value_or("");
  out.modelName = textMember(raw, "modelName");
  out.state = textMember(raw, "state").value_or("");
  if (const auto it = raw.find("entitlements"); it != raw.end()) out.entitlements = entitlementsOf(*it);
  out.expiresAt = figureMember(raw, "expiresAt");
  out.versions = versionsOf(raw);
  out.deviceLimit = figureMember(raw, "deviceLimit").value_or(0);
  out.activeDevices = figureMember(raw, "activeDevices").value_or(0);
  out.held = trueMember(raw, "held");
  out.sale = saleOf(raw);
  if (const std::optional<std::string> code = textMember(raw, "activationCode"); code && isCode(*code)) {
    out.activationCode = *code;
  }
  return out;
}

}  // namespace

std::optional<std::vector<PickedKey>> pickedKeysOf(const json& body) {
  if (!body.is_object()) return std::nullopt;
  const auto it = body.find("keys");
  if (it == body.end() || !it->is_array()) return std::nullopt;
  std::vector<PickedKey> keys;
  for (const json& raw : *it) {
    if (std::optional<PickedKey> key = pickedKey(raw)) keys.push_back(std::move(*key));
  }
  return keys;
}

bool covers(const std::optional<PickedVersions>& versions, std::int64_t major, std::int64_t now) {
  if (!versions) return true;
  if (major < versions->minMajor) return false;
  if (!versions->maxMajor || major <= *versions->maxMajor) return true;
  return versions->upgradeUntil && *versions->upgradeUntil > now;
}

const char* to_string(Unchosen value) noexcept {
  switch (value) {
    case Unchosen::NoKey: return "no-key";
    case Unchosen::Upgrade: return "upgrade";
    case Unchosen::Outdated: return "outdated";
  }
  return "no-key";
}

Choice chosenKey(const std::vector<PickedKey>& keys, std::int64_t major, std::int64_t now) {
  if (keys.empty()) return Choice{std::nullopt, Unchosen::NoKey};
  std::vector<std::size_t> covering;
  for (std::size_t i = 0; i < keys.size(); ++i) {
    if (covers(keys[i].versions, major, now)) covering.push_back(i);
  }
  for (const std::size_t i : covering) {
    if (keys[i].held) return Choice{i, std::nullopt};
  }
  for (const std::size_t i : covering) {
    if (keys[i].activeDevices < keys[i].deviceLimit) return Choice{i, std::nullopt};
  }
  if (!covering.empty()) return Choice{covering.front(), std::nullopt};
  const bool older = std::any_of(keys.begin(), keys.end(), [&](const PickedKey& key) {
    return key.versions && key.versions->minMajor <= major;
  });
  return Choice{std::nullopt, older ? Unchosen::Upgrade : Unchosen::Outdated};
}

}  // namespace detail

}  // namespace keygrant
