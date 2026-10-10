// What the conformance replays share (conformance.hpp): reading a vector's
// JSON inputs, and writing an answer as the file writes it.
#include "conformance.hpp"

#include <set>

namespace kgtest {

using namespace keygrant::detail;
using keygrant::Refusal;

namespace {

template <class T, class Read>
void take(const json& patch, const char* name, Change<T>& change, Read read) {
  if (!patch.contains(name)) return;
  const json& value = patch[name];
  change = value.is_null() ? Change<T>::clear() : Change<T>::set(read(value));
}

}  // namespace

const KeySet& vectorsKey() {
  static const KeySet key = KeySet::fromJwk(vectors()["publicJwk"].dump());
  return key;
}

const json& group(const char* name) {
  const json& list = vectors()[name];
  EXPECT_TRUE(list.is_array() && !list.empty()) << name;
  return list;
}

std::string nameOf(const json& v) { return v.contains("name") ? v["name"].get<std::string>() : v.dump(); }

std::optional<std::string> optString(const json& v) {
  return v.is_string() ? std::optional<std::string>(v.get<std::string>()) : std::nullopt;
}

std::optional<std::int64_t> optInt(const json& v) {
  return v.is_null() ? std::nullopt : std::optional<std::int64_t>(v.get<std::int64_t>());
}

json nullOr(const std::optional<std::string>& v) { return v ? json(*v) : json(nullptr); }
json nullOr(const std::optional<std::int64_t>& v) { return v ? json(*v) : json(nullptr); }

/** A state as JSON carries it (absent fields omitted). */
json stateJson(const StoredState& state) { return json::parse(keygrant::serializeState(state)); }

/** A patch as JSON carries it: a cleared field is null. */
json patchJson(const StatePatch& p) {
  json out = json::object();
  const auto put = [&](const char* name, const auto& change, auto encode) {
    if (!change.present()) return;
    const auto value = change.get();
    out[name] = value ? encode(*value) : json(nullptr);
  };
  const auto same = [](const auto& v) { return json(v); };
  const auto named = [](const auto& v) { return json(keygrant::to_string(v)); };
  put("key", p.key, same);
  put("activationId", p.activationId, same);
  put("lease", p.lease, same);
  put("trialStart", p.trialStart, same);
  put("trialEndsAt", p.trialEndsAt, same);
  put("trialLease", p.trialLease, same);
  put("trialAt", p.trialAt, same);
  put("lastSeen", p.lastSeen, same);
  put("checkedMajor", p.checkedMajor, same);
  put("keptMajor", p.keptMajor, same);
  put("backoffSince", p.backoffSince, same);
  put("backoffMs", p.backoffMs, same);
  put("refused", p.refused, named);
  put("refusedMajor", p.refusedMajor, same);
  put("refusedFor", p.refusedFor, same);
  put("deviceKind", p.deviceKind, named);
  put("verdictAt", p.verdictAt, same);
  put("rev", p.rev, same);
  put("trialReportedAt", p.trialReportedAt, same);
  return out;
}

/** A patch from JSON: null clears the field; an absent field is left alone. */
StatePatch patchFrom(const json& j) {
  static const std::set<std::string> known = {
      "key",          "activationId", "lease",        "trialStart", "trialEndsAt", "trialLease",    "trialAt",
      "lastSeen",     "checkedMajor", "keptMajor",    "backoffSince", "backoffMs",  "refused",       "refusedMajor",
      "refusedFor",   "deviceKind",   "verdictAt",    "rev",          "trialReportedAt"};
  for (const auto& [name, value] : j.items()) {
    (void)value;
    EXPECT_TRUE(known.count(name)) << "a patch field this harness does not know: " << name;
  }
  StatePatch p;
  const auto str = [](const json& v) { return v.get<std::string>(); };
  const auto num = [](const json& v) { return v.get<std::int64_t>(); };
  take(j, "key", p.key, str);
  take(j, "activationId", p.activationId, str);
  take(j, "lease", p.lease, str);
  take(j, "trialStart", p.trialStart, num);
  take(j, "trialEndsAt", p.trialEndsAt, num);
  take(j, "trialLease", p.trialLease, str);
  take(j, "trialAt", p.trialAt, num);
  take(j, "lastSeen", p.lastSeen, num);
  take(j, "checkedMajor", p.checkedMajor, num);
  take(j, "keptMajor", p.keptMajor, num);
  take(j, "backoffSince", p.backoffSince, num);
  take(j, "backoffMs", p.backoffMs, num);
  take(j, "refused", p.refused, [](const json& v) { return *keygrant::refusalFromString(v.get<std::string>()); });
  take(j, "refusedMajor", p.refusedMajor, num);
  take(j, "refusedFor", p.refusedFor, [](const json& v) { return v.get<std::vector<std::string>>(); });
  take(j, "deviceKind", p.deviceKind, [](const json& v) { return *keygrant::deviceKindFromString(v.get<std::string>()); });
  take(j, "verdictAt", p.verdictAt, num);
  take(j, "rev", p.rev, num);
  take(j, "trialReportedAt", p.trialReportedAt, num);
  return p;
}

Asked askedFrom(const json& j) {
  return Asked{j["at"].get<std::int64_t>(), j.contains("activationId") ? optString(j["activationId"]) : std::nullopt,
               j.contains("seen") ? optInt(j["seen"]) : std::nullopt};
}

Basis basisFrom(const json& j) {
  Basis basis;
  if (j.is_null()) return basis;
  if (j.contains("answer")) basis.answer = askedFrom(j["answer"]);
  if (j.contains("activation")) basis.activation = askedFrom(j["activation"]);
  if (j.contains("about")) {
    basis.about = About{j["about"].contains("activationId") ? optString(j["about"]["activationId"]) : std::nullopt};
  }
  if (j.contains("trial")) {
    const json& t = j["trial"];
    basis.trial = Stamp{t["at"].get<std::int64_t>(), t.contains("seen") ? optInt(t["seen"]) : std::nullopt};
  }
  for (const auto& [name, value] : j.items()) {
    (void)value;
    EXPECT_TRUE(name == "answer" || name == "activation" || name == "about" || name == "trial")
        << "a basis this harness does not know: " << name;
  }
  return basis;
}

/** The vectors' base claims: a valid licence lease's claims, which the pure decisions read one or two of. */
LeaseClaims baseClaims() {
  LeaseClaims c;
  c.sub = "SLUICE-AAAA-BBBB-CCCC-DDDD";
  c.product = "sluice";
  c.aid = "act_1";
  c.model = "perpetual";
  c.lim = 3;
  c.major = 3;
  c.iat = 1'789'913'600;
  c.exp = 1'792'505'600;
  return c;
}

/** A status as JSON says it: a member with no value left out, and entitlements only when there are any. */
json statusJson(const LicenseStatus& s) {
  json out = {{"state", keygrant::to_string(s.state)}};
  if (s.reason) out["reason"] = keygrant::to_string(*s.reason);
  if (s.key) out["key"] = *s.key;
  if (s.trialEndsAt) out["trialEndsAt"] = *s.trialEndsAt;
  if (s.trialDaysLeft) out["trialDaysLeft"] = *s.trialDaysLeft;
  if (s.test) out["test"] = true;
  // Doubles, compared with the file's numbers as numbers (5 and 5.0 alike).
  if (!s.entitlements.empty()) out["entitlements"] = entitlementsJson(s.entitlements);
  return out;
}

/** What a status answer covers: its state, and whether it carried entitlements. */
std::string answerOf(const LicenseStatus& s) {
  return std::string(keygrant::to_string(s.state)) + (s.entitlements.empty() ? "" : " with entitlements");
}

json readingJson(const keygrant::MachineIdReading& r) {
  switch (r.kind) {
    case keygrant::MachineIdReading::Kind::Id: return {{"kind", "id"}, {"id", r.id}};
    case keygrant::MachineIdReading::Kind::Unavailable: return {{"kind", "unavailable"}};
    case keygrant::MachineIdReading::Kind::Transient: return {{"kind", "transient"}};
  }
  return nullptr;
}

/** A device kind as JSON gives it: null is none. */
std::optional<DeviceKind> kindFrom(const json& v) {
  if (v.is_null()) return std::nullopt;
  const std::optional<DeviceKind> kind = keygrant::deviceKindFromString(v.get<std::string>());
  EXPECT_TRUE(kind.has_value()) << "a device kind this harness does not know: " << v.dump();
  return kind;
}

/** The claims a vector names, over the base claims. */
LeaseClaims claimsWith(const json& given) {
  LeaseClaims c = baseClaims();
  for (const auto& [name, value] : given.items()) {
    if (name == "major") {
      c.major = value.get<std::int64_t>();
    } else if (name == "iat") {
      c.iat = value.get<std::int64_t>();
    } else if (name == "exp") {
      c.exp = value.get<std::int64_t>();
    } else if (name == "upgradeUntil") {
      c.upgradeUntil = value.get<std::int64_t>();
    } else {
      ADD_FAILURE() << "a claim this harness does not know: " << name;
    }
  }
  return c;
}

LeaseVerdict verdictFrom(const std::string& word) {
  for (const LeaseVerdict verdict : {LeaseVerdict::Expired, LeaseVerdict::Upgrade, LeaseVerdict::Outdated,
                                     LeaseVerdict::Tampered, LeaseVerdict::UnknownKey, LeaseVerdict::Device}) {
    if (word == to_string(verdict)) return verdict;
  }
  ADD_FAILURE() << "a lease verdict this harness does not know: " << word;
  return LeaseVerdict::Tampered;
}

/** A lease's offline check as a vector gives it: {ok: true, test, claims}, {ok: false, reason}, or null for none. */
std::optional<LeaseCheck> checkFrom(const json& v) {
  if (v.is_null()) return std::nullopt;
  LeaseCheck check;
  check.ok = v["ok"].get<bool>();
  if (check.ok) {
    check.test = v["test"].get<bool>();
    check.claims = parseClaims(v["claims"]);
    EXPECT_TRUE(check.claims.has_value()) << "claims the schema refuses: " << v["claims"].dump();
  } else {
    check.reason = verdictFrom(v["reason"].get<std::string>());
  }
  return check;
}

/** A validate's answer as a vector gives it: {status, json} (json null: no body), or null for none at all. */
std::optional<Reply> responseFrom(const json& v) {
  if (v.is_null()) return std::nullopt;
  return Reply{v["status"].get<int>(), v["json"]};
}

/** A renewal's outcome as JSON says it: the status as plain JSON, the patch as a patch. */
json renewalJson(const RenewalOutcome& outcome) {
  return {{"answered", outcome.answered},
          {"status", outcome.status ? statusJson(*outcome.status) : json(nullptr)},
          {"patch", patchJson(outcome.patch)}};
}

/** A device as JSON says it: no own or host name is null; no claim of its own, or no kind, is left out. */
json deviceJson(const Device& d) {
  json out = {{"own", nullOr(d.own)},          {"canTell", d.canTell},
              {"fingerprints", d.fingerprints}, {"claims", d.claims},
              {"hostname", nullOr(d.hostname)}};
  if (d.ownClaim) out["ownClaim"] = *d.ownClaim;
  if (d.kind) out["kind"] = keygrant::to_string(*d.kind);
  return out;
}

/** A fingerprinter's answer as JSON says it: no fingerprint is null; no kind is left out. */
json identityJson(const keygrant::DeviceIdentity& identity) {
  json out = {{"fingerprint", nullOr(identity.fingerprint)}, {"alternates", identity.alternates}};
  if (identity.kind) out["kind"] = keygrant::to_string(*identity.kind);
  return out;
}

}  // namespace kgtest
