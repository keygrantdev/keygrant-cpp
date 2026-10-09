#include "keygrant/types.hpp"

#include "json_util.hpp"

namespace keygrant {

const char* to_string(LicenseState value) noexcept {
  switch (value) {
    case LicenseState::Licensed: return "licensed";
    case LicenseState::Trial: return "trial";
    case LicenseState::Expired: return "expired";
    case LicenseState::Invalid: return "invalid";
    case LicenseState::Unlicensed: return "unlicensed";
    case LicenseState::Unknown: return "unknown";
  }
  return "unknown";
}

const char* to_string(LicenseReason value) noexcept {
  switch (value) {
    case LicenseReason::Revoked: return "revoked";
    case LicenseReason::Upgrade: return "upgrade";
    case LicenseReason::Outdated: return "outdated";
    case LicenseReason::Offline: return "offline";
    case LicenseReason::Tampered: return "tampered";
    case LicenseReason::Device: return "device";
    case LicenseReason::Trial: return "trial";
    case LicenseReason::Storage: return "storage";
    case LicenseReason::UnknownKey: return "unknown-key";
  }
  return "storage";
}

const char* to_string(ActivateError value) noexcept {
  switch (value) {
    case ActivateError::InvalidKey: return "invalid-key";
    case ActivateError::Limit: return "limit";
    case ActivateError::Revoked: return "revoked";
    case ActivateError::Upgrade: return "upgrade";
    case ActivateError::Outdated: return "outdated";
    case ActivateError::Network: return "network";
    case ActivateError::NotConfigured: return "not-configured";
    case ActivateError::InvalidLease: return "invalid-lease";
  }
  return "network";
}

const char* to_string(Refusal value) noexcept {
  switch (value) {
    case Refusal::Revoked: return "revoked";
    case Refusal::Upgrade: return "upgrade";
    case Refusal::Outdated: return "outdated";
    case Refusal::Device: return "device";
  }
  return "revoked";
}

const char* to_string(DeviceKind value) noexcept {
  switch (value) {
    case DeviceKind::Machine: return "machine";
    case DeviceKind::Hostname: return "hostname";
  }
  return "hostname";
}

std::optional<Refusal> refusalFromString(std::string_view name) noexcept {
  if (name == "revoked") return Refusal::Revoked;
  if (name == "upgrade") return Refusal::Upgrade;
  if (name == "outdated") return Refusal::Outdated;
  if (name == "device") return Refusal::Device;
  return std::nullopt;
}

std::optional<DeviceKind> deviceKindFromString(std::string_view name) noexcept {
  if (name == "machine") return DeviceKind::Machine;
  if (name == "hostname") return DeviceKind::Hostname;
  return std::nullopt;
}

std::ostream& operator<<(std::ostream& out, LicenseState value) { return out << to_string(value); }
std::ostream& operator<<(std::ostream& out, LicenseReason value) { return out << to_string(value); }
std::ostream& operator<<(std::ostream& out, ActivateError value) { return out << to_string(value); }
std::ostream& operator<<(std::ostream& out, Refusal value) { return out << to_string(value); }
std::ostream& operator<<(std::ostream& out, DeviceKind value) { return out << to_string(value); }

std::optional<bool> Entitlement::flag() const noexcept {
  const bool* flag = std::get_if<bool>(&value_);
  return flag != nullptr ? std::optional<bool>(*flag) : std::nullopt;
}

std::optional<double> Entitlement::number() const noexcept {
  const double* number = std::get_if<double>(&value_);
  return number != nullptr ? std::optional<double>(*number) : std::nullopt;
}

std::optional<std::string> Entitlement::text() const {
  const std::string* text = std::get_if<std::string>(&value_);
  return text != nullptr ? std::optional<std::string>(*text) : std::nullopt;
}

std::ostream& operator<<(std::ostream& out, const Entitlement& entitlement) {
  switch (entitlement.kind()) {
    case Entitlement::Kind::Flag: return out << detail::dumpJson(detail::json(*entitlement.flag()));
    case Entitlement::Kind::Number: return out << detail::dumpJson(detail::json(*entitlement.number()));
    case Entitlement::Kind::Text: return out << detail::dumpJson(detail::json(*entitlement.text()));
  }
  return out;
}

bool LicenseStatus::operator==(const LicenseStatus& other) const {
  return state == other.state && reason == other.reason && key == other.key && trialDaysLeft == other.trialDaysLeft &&
         trialEndsAt == other.trialEndsAt && test == other.test && entitlements == other.entitlements &&
         stalled == other.stalled;
}

std::ostream& operator<<(std::ostream& out, const LicenseStatus& status) {
  out << "{ state: " << status.state;
  if (status.reason) out << ", reason: " << *status.reason;
  if (status.key) out << ", key: " << *status.key;
  if (status.trialEndsAt) out << ", trialEndsAt: " << *status.trialEndsAt;
  if (status.trialDaysLeft) out << ", trialDaysLeft: " << *status.trialDaysLeft;
  if (status.test) out << ", test: true";
  if (!status.entitlements.empty()) {
    out << ", entitlements: {";
    const char* separator = " ";
    for (const auto& [name, value] : status.entitlements) {
      out << separator << detail::dumpJson(detail::json(name)) << ": " << value;
      separator = ", ";
    }
    out << " }";
  }
  if (status.stalled) out << ", stalled: true";
  return out << " }";
}

std::ostream& operator<<(std::ostream& out, const ActivateResult& result) {
  if (result.ok) return out << "{ ok: true }";
  out << "{ ok: false";
  if (result.error) out << ", error: " << *result.error;
  return out << " }";
}

std::ostream& operator<<(std::ostream& out, const DeactivateResult& result) {
  return out << "{ released: " << (result.released ? "true" : "false") << " }";
}

bool StoredState::operator==(const StoredState& other) const {
  return key == other.key && activationId == other.activationId && lease == other.lease &&
         trialStart == other.trialStart && trialEndsAt == other.trialEndsAt && trialLease == other.trialLease &&
         trialAt == other.trialAt && lastSeen == other.lastSeen && checkedMajor == other.checkedMajor &&
         keptMajor == other.keptMajor && backoffSince == other.backoffSince && backoffMs == other.backoffMs &&
         refused == other.refused && refusedMajor == other.refusedMajor && refusedFor == other.refusedFor &&
         deviceKind == other.deviceKind && verdictAt == other.verdictAt && rev == other.rev &&
         purchaseToken == other.purchaseToken && purchaseTokenAt == other.purchaseTokenAt &&
         claimAskedAt == other.claimAskedAt && trialReportedAt == other.trialReportedAt &&
         otherFields == other.otherFields;
}

std::ostream& operator<<(std::ostream& out, const StoredState& state) { return out << serializeState(state); }

IoError::IoError(std::string code, const std::string& message)
    : std::runtime_error(code + ": " + message), code_(std::move(code)) {}

StalledError::StalledError() : std::runtime_error("a previous licence call has not finished; try again") {}

MissingEntitlement::MissingEntitlement(std::string entitlement)
    : std::runtime_error("the licence does not grant \"" + entitlement + "\""), entitlement_(std::move(entitlement)) {}

bool grants(const std::map<std::string, Entitlement>& entitlements, const std::string& code) {
  const auto found = entitlements.find(code);
  if (found == entitlements.end()) return false;
  const Entitlement& value = found->second;
  if (const std::optional<bool> flag = value.flag()) return *flag;
  if (const std::optional<double> number = value.number()) return *number != 0;
  const std::optional<std::string> text = value.text();
  return text.has_value() && !text->empty();
}

}  // namespace keygrant
