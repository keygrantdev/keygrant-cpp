#include "state_json.hpp"

#include <array>
#include <stdexcept>

#include "utf.hpp"

namespace keygrant {

namespace detail {

namespace {

constexpr std::array<const char*, 22> kKnownFields = {
    "key",          "activationId", "lease",        "trialStart",   "trialEndsAt",     "trialLease", "trialAt",
    "lastSeen",     "checkedMajor", "keptMajor",    "backoffSince", "backoffMs",       "refused",    "refusedMajor",
    "refusedFor",   "deviceKind",   "verdictAt",    "rev",          "trialReportedAt", "licenseFile",
    "spends",       "meterFigures",
};

/**
 * What an earlier version stored to buy from the app with no key typed (its
 * purchase secret and the two stamps beside it). Retired: not read, not even
 * into otherFields, and not written from an app's own otherFields either, so
 * the next save clears them.
 */
constexpr std::array<const char*, 3> kRetiredFields = {"purchaseToken", "purchaseTokenAt", "claimAskedAt"};

/**
 * How deep a value this SDK cannot read may nest and still be kept to be
 * written back: far past anything an SDK writes. Writing JSON recurses as deep
 * as it nests, so a value nested deeper (a file damaged or made so) is dropped,
 * not kept to overflow the stack on the next save.
 */
constexpr std::size_t kMaxKeptDepth = 64;

bool isRetired(const std::string& name) {
  for (const char* field : kRetiredFields) {
    if (name == field) return true;
  }
  return false;
}

bool isKnown(const std::string& name) {
  for (const char* field : kKnownFields) {
    if (name == field) return true;
  }
  return false;
}

void put(ordered_json& out, const char* name, const std::optional<std::string>& value) {
  if (value) out[name] = *value;
}

void put(ordered_json& out, const char* name, const std::optional<std::int64_t>& value) {
  if (value) out[name] = *value;
}

std::optional<std::string> stringField(const json& object, const char* name) {
  const auto it = object.find(name);
  if (it == object.end() || !it->is_string()) return std::nullopt;
  return it->get<std::string>();
}

std::optional<std::int64_t> numberField(const json& object, const char* name) {
  const auto it = object.find(name);
  if (it == object.end()) return std::nullopt;
  return anyNumber(*it);
}

/** Whether the state holds a value of its own for this known field. */
bool holds(const StoredState& s, const std::string& name) {
  if (name == "key") return s.key.has_value();
  if (name == "activationId") return s.activationId.has_value();
  if (name == "lease") return s.lease.has_value();
  if (name == "trialStart") return s.trialStart.has_value();
  if (name == "trialEndsAt") return s.trialEndsAt.has_value();
  if (name == "trialLease") return s.trialLease.has_value();
  if (name == "trialAt") return s.trialAt.has_value();
  if (name == "lastSeen") return s.lastSeen.has_value();
  if (name == "checkedMajor") return s.checkedMajor.has_value();
  if (name == "keptMajor") return s.keptMajor.has_value();
  if (name == "backoffSince") return s.backoffSince.has_value();
  if (name == "backoffMs") return s.backoffMs.has_value();
  if (name == "refused") return s.refused.has_value();
  if (name == "refusedMajor") return s.refusedMajor.has_value();
  if (name == "refusedFor") return s.refusedFor.has_value();
  if (name == "deviceKind") return s.deviceKind.has_value();
  if (name == "verdictAt") return s.verdictAt.has_value();
  if (name == "rev") return s.rev.has_value();
  if (name == "trialReportedAt") return s.trialReportedAt.has_value();
  if (name == "licenseFile") return s.licenseFile.has_value();
  // The journal and its figures are the device's own: a value this SDK cannot read is dropped, never kept to be
  // written back, so they are always its own.
  if (name == "spends" || name == "meterFigures") return true;
  return false;
}

/** A stored journal entry, or none when it is not one: an unknown member of a valid entry is not kept. */
std::optional<SpendEntry> spendFrom(const json& value) {
  if (!value.is_object()) return std::nullopt;
  const auto id = stringField(value, "id");
  const auto meter = stringField(value, "meter");
  const auto kind = stringField(value, "kind");
  const auto n = value.find("n");
  const auto at = value.find("at");
  if (!id || !meter || !kind || n == value.end() || at == value.end()) return std::nullopt;
  const std::optional<std::int64_t> count = countOf(*n);
  const std::optional<std::int64_t> opened = figureOf(*at);
  const std::optional<SpendKind> spendKind = spendKindFromString(*kind);
  if (!count || !opened || !spendKind) return std::nullopt;
  return SpendEntry{*id, *meter, *count, *opened, *spendKind};
}

/** A stored figure, or none when it is not one. */
std::optional<MeterFigure> figureFrom(const json& value) {
  const std::optional<std::int64_t> left = figureMember(value, "left");
  const std::optional<std::int64_t> total = figureMember(value, "total");
  const std::optional<std::int64_t> offline = figureMember(value, "offline");
  const std::optional<std::int64_t> at = figureMember(value, "at");
  if (!left || !total || !offline || !at) return std::nullopt;
  return MeterFigure{*left, *total, *offline, trueMember(value, "resets"), *at};
}

}  // namespace

ordered_json stateToJson(const StoredState& s) {
  ordered_json out = ordered_json::object();
  put(out, "key", s.key);
  put(out, "activationId", s.activationId);
  put(out, "lease", s.lease);
  put(out, "trialStart", s.trialStart);
  put(out, "trialEndsAt", s.trialEndsAt);
  put(out, "trialLease", s.trialLease);
  put(out, "trialAt", s.trialAt);
  put(out, "lastSeen", s.lastSeen);
  put(out, "checkedMajor", s.checkedMajor);
  put(out, "keptMajor", s.keptMajor);
  put(out, "backoffSince", s.backoffSince);
  put(out, "backoffMs", s.backoffMs);
  if (s.refused) out["refused"] = to_string(*s.refused);
  put(out, "refusedMajor", s.refusedMajor);
  if (s.refusedFor) out["refusedFor"] = *s.refusedFor;
  if (s.deviceKind) out["deviceKind"] = to_string(*s.deviceKind);
  put(out, "verdictAt", s.verdictAt);
  put(out, "rev", s.rev);
  put(out, "trialReportedAt", s.trialReportedAt);
  put(out, "licenseFile", s.licenseFile);
  if (!s.spends.empty()) {
    ordered_json journal = ordered_json::array();
    for (const SpendEntry& e : s.spends) {
      journal.push_back({{"id", e.id}, {"meter", e.meter}, {"n", e.n}, {"at", e.at}, {"kind", to_string(e.kind)}});
    }
    out["spends"] = std::move(journal);
  }
  if (!s.meterFigures.empty()) {
    ordered_json figures = ordered_json::object();
    for (const auto& [code, f] : s.meterFigures) {
      figures[code] = {{"left", f.left}, {"total", f.total}, {"offline", f.offline}, {"resets", f.resets}, {"at", f.at}};
    }
    out["meterFigures"] = std::move(figures);
  }
  // Kept as written: fields this SDK does not know, and values of known ones
  // it could not read (a newer SDK's), unless it now holds a value of its own.
  // A retired name is never written.
  for (const auto& [name, raw] : s.otherFields) {
    if (isRetired(name) || (isKnown(name) && holds(s, name))) continue;
    ordered_json value = ordered_json::parse(raw, nullptr, false);
    // Nested past what is kept (an app's own otherFields, say): not written, rather than overflow the stack.
    if (!value.is_discarded() && !nestsDeeperThan(value, kMaxKeptDepth)) out[name] = std::move(value);
  }
  return out;
}

StoredState stateFromJson(const json& object) {
  StoredState s;
  if (!object.is_object()) return s;
  s.key = stringField(object, "key");
  s.activationId = stringField(object, "activationId");
  s.lease = stringField(object, "lease");
  s.trialStart = numberField(object, "trialStart");
  s.trialEndsAt = numberField(object, "trialEndsAt");
  s.trialLease = stringField(object, "trialLease");
  s.trialAt = numberField(object, "trialAt");
  s.lastSeen = numberField(object, "lastSeen");
  s.checkedMajor = numberField(object, "checkedMajor");
  s.keptMajor = numberField(object, "keptMajor");
  s.backoffSince = numberField(object, "backoffSince");
  s.backoffMs = numberField(object, "backoffMs");
  if (const auto refused = stringField(object, "refused")) s.refused = refusalFromString(*refused);
  s.refusedMajor = numberField(object, "refusedMajor");
  if (const auto it = object.find("refusedFor"); it != object.end() && it->is_array()) {
    std::vector<std::string> claims;
    for (const json& claim : *it) {
      if (claim.is_string()) claims.push_back(claim.get<std::string>());
    }
    s.refusedFor = std::move(claims);
  }
  if (const auto kind = stringField(object, "deviceKind")) s.deviceKind = deviceKindFromString(*kind);
  s.verdictAt = numberField(object, "verdictAt");
  s.rev = numberField(object, "rev");
  s.trialReportedAt = numberField(object, "trialReportedAt");
  s.licenseFile = stringField(object, "licenseFile");
  // Read leniently: an entry or a figure of another shape is left out, and an empty journal is the field absent.
  if (const auto it = object.find("spends"); it != object.end() && it->is_array()) {
    for (const json& entry : *it) {
      if (std::optional<SpendEntry> spend = spendFrom(entry)) s.spends.push_back(std::move(*spend));
    }
  }
  if (const auto it = object.find("meterFigures"); it != object.end() && it->is_object()) {
    for (const auto& [code, member] : it->items()) {
      if (std::optional<MeterFigure> figure = figureFrom(member)) s.meterFigures.insert_or_assign(code, *figure);
    }
  }
  // A field it does not know, or a known one whose value it cannot read (a
  // newer SDK's refusal or device kind, say), is kept to be written back.
  for (const auto& [name, value] : object.items()) {
    if (isKnown(name) && holds(s, name)) continue;
    if (isRetired(name)) continue;
    if (nestsDeeperThan(value, kMaxKeptDepth)) continue;
    s.otherFields[name] = dumpJson(value);
  }
  return s;
}

}  // namespace detail

std::string serializeState(const StoredState& state) { return detail::dumpJson(detail::stateToJson(state)); }

std::optional<StoredState> parseState(std::string_view text) {
  const detail::json parsed = detail::parseJsonText(text);
  if (parsed.is_discarded()) throw std::invalid_argument("the state is not JSON");
  if (!parsed.is_object()) return std::nullopt;
  return detail::stateFromJson(parsed);
}

}  // namespace keygrant
