#include "keygrant/meter.hpp"

namespace keygrant {

const char* to_string(UseError value) noexcept {
  switch (value) {
    case UseError::InvalidAmount: return "invalid-amount";
    case UseError::NoMeter: return "no-meter";
    case UseError::UsedUp: return "used-up";
    case UseError::OfflineLimit: return "offline-limit";
    case UseError::NotLicensed: return "not-licensed";
    case UseError::Storage: return "storage";
    case UseError::Revoked: return "revoked";
    case UseError::Upgrade: return "upgrade";
    case UseError::Outdated: return "outdated";
    case UseError::Device: return "device";
  }
  return "storage";
}

const char* to_string(SpendKind value) noexcept {
  switch (value) {
    case SpendKind::Offline: return "offline";
    case SpendKind::Unanswered: return "unanswered";
    case SpendKind::Void: return "void";
  }
  return "offline";
}

std::optional<SpendKind> spendKindFromString(std::string_view name) noexcept {
  if (name == "offline") return SpendKind::Offline;
  if (name == "unanswered") return SpendKind::Unanswered;
  if (name == "void") return SpendKind::Void;
  return std::nullopt;
}

std::ostream& operator<<(std::ostream& out, UseError value) { return out << to_string(value); }
std::ostream& operator<<(std::ostream& out, SpendKind value) { return out << to_string(value); }

std::ostream& operator<<(std::ostream& out, const UseResult& result) {
  out << "{ ok: " << (result.ok ? "true" : "false");
  if (result.error) out << ", error: " << *result.error;
  if (result.left) out << ", left: " << *result.left;
  if (result.offline) out << ", offline: true";
  return out << " }";
}

std::ostream& operator<<(std::ostream& out, const SpendEntry& entry) {
  return out << "{ id: " << entry.id << ", meter: " << entry.meter << ", n: " << entry.n << ", at: " << entry.at
             << ", kind: " << entry.kind << " }";
}

std::ostream& operator<<(std::ostream& out, const MeterFigure& figure) {
  return out << "{ left: " << figure.left << ", total: " << figure.total << ", offline: " << figure.offline
             << ", resets: " << (figure.resets ? "true" : "false") << ", at: " << figure.at << " }";
}

}  // namespace keygrant
