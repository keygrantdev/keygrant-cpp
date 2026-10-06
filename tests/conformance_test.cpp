// conformance/vectors.json, replayed group by group as the reference answers
// it from the JSON (sdk/conformance/replay.ts): every answer must match.
#include <cstring>
#include <iomanip>
#include <regex>
#include <set>
#include <sstream>
#include <thread>

#include "clock.hpp"
#include "crypto.hpp"
#include "device.hpp"
#include "lease.hpp"
#include "machine_id.hpp"
#include "offline.hpp"
#include "renewal.hpp"
#include "state_json.hpp"
#include "support.hpp"
#include "wire.hpp"

namespace kgtest {
namespace {

using namespace keygrant::detail;
using keygrant::Refusal;

const VerifyingKey& vectorsKey() {
  static const VerifyingKey key = VerifyingKey::fromJwk(vectors()["publicJwk"].dump());
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
  return out;
}

template <class T, class Read>
void take(const json& patch, const char* name, Change<T>& change, Read read) {
  if (!patch.contains(name)) return;
  const json& value = patch[name];
  change = value.is_null() ? Change<T>::clear() : Change<T>::set(read(value));
}

/** A patch from JSON: null clears the field; an absent field is left alone. */
StatePatch patchFrom(const json& j) {
  static const std::set<std::string> known = {
      "key",          "activationId", "lease",        "trialStart", "trialEndsAt", "trialLease",
      "trialAt",      "lastSeen",     "checkedMajor", "keptMajor",  "backoffSince", "backoffMs",
      "refused",      "refusedMajor", "refusedFor",   "deviceKind", "verdictAt",   "rev"};
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
  return basis;
}

/** replay.ts BASE_CLAIMS: a valid licence lease's claims, which the pure decisions read one or two of. */
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

/** replay.ts `{ ...BASE_CLAIMS, ...given }`: the claims a vector names, over BASE_CLAIMS. */
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
                                     LeaseVerdict::Tampered, LeaseVerdict::Device}) {
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

TEST(Conformance, EveryGroupInTheFileIsReplayedHere) {
  // A group added to the file must be added here too, or this fails.
  const std::set<std::string> replayed = {
      "leases",       "trials",          "fingerprintClaims", "fingerprints",   "machineFingerprints",
      "hostFingerprints", "kept",        "renew",             "asks",           "standing",
      "trialStatuses", "refusalPatches", "leasePatches",      "keyedBy",        "merges",
      "guardedNow",   "clockCorrections", "mapServerError",   "refusalOf",      "enteredKey",
      "trimDeviceName", "noAnswer",      "parseLeaseBody",    "parseTrialBody", "mountinfo",
      "machineIds",   "renewals",        "validLeaseAsks",    "devices",        "identities",
      "savedLastSeen", "leaseStatuses",  "trialLeaseStatuses", "entitlementNumbers"};
  std::set<std::string> inFile;
  for (const auto& [name, value] : vectors().items()) {
    if (value.is_array() && !value.empty() && value.front().is_object()) inFile.insert(name);
  }
  EXPECT_EQ(inFile, replayed);
}

TEST(Conformance, TheConstantsAreTheReferences) {
  const json& c = vectors()["constants"];
  // A constant added to the file must be checked here too, or this fails.
  std::set<std::string> inFile;
  for (const auto& [name, value] : c.items()) {
    (void)value;
    inFile.insert(name);
  }
  EXPECT_EQ(inFile, (std::set<std::string>{"OPEN_MAJOR", "ASK_AGAIN_MS", "RETRY_UNANSWERED_MS", "RENEW_WITHIN_MS",
                                            "CLOCK_TOLERANCE_MS", "STATUS_CHECK_TIMEOUT_MS", "DEFAULT_HTTP_TIMEOUT_MS",
                                            "LOCK_GRACE_MS", "MACHINE_ID_TIMEOUT_MS", "ACTIVATE_MACHINE_ID_TIMEOUT_MS",
                                            "BOOT_SLACK_MS", "MAX_DEVICE_NAME", "MAX_KEY", "KEEP_CORRUPT",
                                            "STALE_TEMP_MS", "UPDATE_ATTEMPTS", "UNKNOWN_HOST"}));
  const keygrant::detail::Timings timings;
  EXPECT_EQ(c["OPEN_MAJOR"], keygrant::kOpenMajor);
  EXPECT_EQ(c["ASK_AGAIN_MS"], kAskAgainMs);
  EXPECT_EQ(c["RETRY_UNANSWERED_MS"], kRetryUnansweredMs);
  EXPECT_EQ(c["RENEW_WITHIN_MS"], kRenewWithinMs);
  EXPECT_EQ(c["CLOCK_TOLERANCE_MS"], kClockToleranceMs);
  EXPECT_EQ(c["STATUS_CHECK_TIMEOUT_MS"], timings.statusCheck.count());
  EXPECT_EQ(c["DEFAULT_HTTP_TIMEOUT_MS"], keygrant::kDefaultHttpTimeout.count());
  EXPECT_EQ(c["LOCK_GRACE_MS"], timings.lockGrace.count());
  EXPECT_EQ(c["MACHINE_ID_TIMEOUT_MS"], keygrant::kMachineIdTimeout.count());
  EXPECT_EQ(c["MACHINE_ID_TIMEOUT_MS"], timings.machineId.count());
  EXPECT_EQ(c["ACTIVATE_MACHINE_ID_TIMEOUT_MS"], keygrant::kActivateMachineIdTimeout.count());
  EXPECT_EQ(c["ACTIVATE_MACHINE_ID_TIMEOUT_MS"], timings.activateMachineId.count());
  EXPECT_EQ(c["BOOT_SLACK_MS"], keygrant::kBootSlackMs);
  EXPECT_EQ(c["MAX_DEVICE_NAME"], kMaxDeviceName);
  EXPECT_EQ(c["MAX_KEY"], kMaxKey);
  EXPECT_EQ(c["KEEP_CORRUPT"], keygrant::kKeepCorrupt);
  EXPECT_EQ(c["STALE_TEMP_MS"], keygrant::kStaleTempMs);
  EXPECT_EQ(c["UPDATE_ATTEMPTS"], keygrant::kUpdateAttempts);
  EXPECT_EQ(c["UNKNOWN_HOST"], keygrant::kUnknownHost);
}

TEST(Conformance, Leases) {
  std::set<std::string> verdicts;
  for (const json& v : group("leases")) {
    SCOPED_TRACE(nameOf(v));
    const VerifyResult r = verifyLease(v["token"].get<std::string>(), vectorsKey(), v["now"].get<std::int64_t>());
    json verified = {{"valid", r.valid}};
    if (!r.valid) verified["reason"] = to_string(*r.reason);
    if (r.claims) verified["claims"] = claimsJson(*r.claims);
    EXPECT_EQ(verified, v["verify"]);

    const json& h = v["holder"];
    const LeaseHolder holder{h["key"].get<std::string>(), h["activationId"].get<std::string>(),
                             h["claims"].get<std::vector<std::string>>(), h["canTell"].get<bool>()};
    const LeaseCheck check = checkLease(r, v["product"].get<std::string>(), v["major"].get<std::int64_t>(), holder);
    const json expect = check.ok ? json{{"ok", true}, {"test", check.test}} : json{{"ok", false}, {"reason", to_string(check.reason)}};
    EXPECT_EQ(expect, v["expect"]);
    verdicts.insert(check.ok ? "ok" : to_string(check.reason));
  }
  // Every kind of verdict the offline check can give is covered.
  EXPECT_EQ(verdicts, (std::set<std::string>{"device", "expired", "ok", "outdated", "tampered", "upgrade"}));
}

TEST(Conformance, TrialLeases) {
  for (const json& v : group("trials")) {
    SCOPED_TRACE(nameOf(v));
    const VerifyResult r = verifyLease(v["token"].get<std::string>(), vectorsKey(), v["now"].get<std::int64_t>());
    const json& device = v["device"];
    EXPECT_EQ(trialLeaseValid(r, v["product"].get<std::string>(), device["claims"].get<std::vector<std::string>>(),
                              device["canTell"].get<bool>()),
              v["expect"].get<bool>());
  }
}

TEST(Conformance, Fingerprints) {
  for (const json& v : group("fingerprintClaims")) EXPECT_EQ(fingerprintClaim(v["input"].get<std::string>()), v["claim"]);
  for (const json& v : group("fingerprints")) EXPECT_EQ(keygrant::hashFingerprint(v["raw"].get<std::string>()), v["fingerprint"]);
  for (const json& v : group("machineFingerprints")) {
    EXPECT_EQ(keygrant::machineFingerprint(v["id"].get<std::string>()), v["fingerprint"]);
  }
  for (const json& v : group("hostFingerprints")) {
    EXPECT_EQ(keygrant::hostFingerprint(v["host"].get<std::string>(), v["platform"].get<std::string>(),
                                        v["arch"].get<std::string>()),
              v["fingerprint"]);
  }
}

TEST(Conformance, KeptMajors) {
  for (const json& v : group("kept")) {
    SCOPED_TRACE(nameOf(v));
    LeaseClaims claims = baseClaims();
    claims.upgradeUntil = optInt(v["upgradeUntil"]);
    const auto kept = keptWith(optInt(v["kept"]), claims, v["now"].get<std::int64_t>(), v["major"].get<std::int64_t>());
    EXPECT_EQ(nullOr(kept), v["expect"]);
  }
}

TEST(Conformance, EarlyRenewal) {
  for (const json& v : group("renew")) {
    SCOPED_TRACE(nameOf(v));
    LeaseClaims claims = baseClaims();
    claims.iat = v["iat"].get<std::int64_t>();
    claims.exp = v["exp"].get<std::int64_t>();
    EXPECT_EQ(renewsEarly(claims, v["now"].get<std::int64_t>()), v["expect"].get<bool>());
  }
}

TEST(Conformance, Asks) {
  for (const json& v : group("asks")) {
    SCOPED_TRACE(nameOf(v));
    EXPECT_EQ(askDue(stateFrom(v["state"]), v["clockNow"].get<std::int64_t>(), v["force"].get<bool>()),
              v["expect"].get<bool>());
  }
}

TEST(Conformance, StandingRefusals) {
  for (const json& v : group("standing")) {
    SCOPED_TRACE(nameOf(v));
    std::optional<DeviceView> device;
    if (!v["device"].is_null()) {
      const json& d = v["device"];
      device = DeviceView{d.contains("ownClaim") ? optString(d["ownClaim"]) : std::nullopt,
                          d.contains("canTell") ? d["canTell"].get<bool>() : true};
    }
    const auto refused = standingRefusal(stateFrom(v["state"]), v["major"].get<std::int64_t>(), device);
    EXPECT_EQ(refused ? json(keygrant::to_string(*refused)) : json(nullptr), v["expect"]);
  }
}

TEST(Conformance, TrialStatuses) {
  for (const json& v : group("trialStatuses")) {
    SCOPED_TRACE(nameOf(v));
    EXPECT_EQ(statusJson(trialStatusOf(stateFrom(v["state"]), v["now"].get<std::int64_t>(), v["leaseValid"].get<bool>(),
                                       optInt(v["leaseEndsAt"]))),
              v["expect"]);
  }
}

TEST(Conformance, Patches) {
  for (const json& v : group("refusalPatches")) {
    SCOPED_TRACE(nameOf(v));
    const json& d = v["device"];
    const auto refused = *keygrant::refusalFromString(v["refused"].get<std::string>());
    const auto patch = refusalPatch(refused, v["major"].get<std::int64_t>(), d.contains("ownClaim") ? optString(d["ownClaim"]) : std::nullopt);
    EXPECT_EQ(patchJson(patch), v["expect"]);
  }
  for (const json& v : group("leasePatches")) {
    SCOPED_TRACE(nameOf(v));
    EXPECT_EQ(patchJson(v["patch"] == "LEASED" ? leasedPatch() : unkeyedPatch()), v["expect"]);
  }
  for (const json& v : group("keyedBy")) {
    SCOPED_TRACE(nameOf(v));
    const std::optional<DeviceKind> kind =
        v["kind"].is_null() ? std::nullopt : keygrant::deviceKindFromString(v["kind"].get<std::string>());
    EXPECT_EQ(patchJson(keyedBy(kind)), v["expect"]);
  }
}

TEST(Conformance, Merges) {
  for (const json& v : group("merges")) {
    SCOPED_TRACE(nameOf(v));
    const StoredState result = merged(stateFrom(v["latest"]), patchFrom(v["patch"]), basisFrom(v["basis"]));
    EXPECT_EQ(stateJson(result), v["expect"]);
  }
}

TEST(Conformance, TheClockGuard) {
  for (const json& v : group("guardedNow")) {
    SCOPED_TRACE(nameOf(v));
    EXPECT_EQ(guardedNow(v["clockNow"].get<std::int64_t>(), optInt(v["lastSeen"])), v["expect"].get<std::int64_t>());
  }
  for (const json& v : group("clockCorrections")) {
    SCOPED_TRACE(nameOf(v));
    std::optional<SignedAt> claims;
    if (!v["claims"].is_null()) {
      const json& c = v["claims"];
      claims = SignedAt{c["sub"].get<std::string>(), c["aid"].get<std::string>(), c["iat"].get<std::int64_t>()};
    }
    const auto corrected = clockCorrection(v["clockNow"].get<std::int64_t>(), optInt(v["lastSeen"]), claims,
                                           optInt(v["heldIat"]), v["key"].get<std::string>(),
                                           v["activationId"].get<std::string>());
    EXPECT_EQ(nullOr(corrected), v["expect"]);
  }
}

TEST(Conformance, TheWire) {
  for (const json& v : group("mapServerError")) {
    SCOPED_TRACE(v["error"].dump());
    EXPECT_EQ(keygrant::to_string(mapServerError(optString(v["error"]))), v["expect"].get<std::string>());
  }
  for (const json& v : group("refusalOf")) {
    SCOPED_TRACE(v["error"].dump());
    const auto refused = refusalOf(optString(v["error"]));
    EXPECT_EQ(refused ? json(keygrant::to_string(*refused)) : json(nullptr), v["expect"]);
  }
  for (const json& v : group("enteredKey")) {
    SCOPED_TRACE(v["input"].dump());
    EXPECT_EQ(nullOr(enteredKey(v["input"].get<std::string>())), v["expect"]);
  }
  for (const json& v : group("trimDeviceName")) {
    SCOPED_TRACE(v["input"].dump());
    EXPECT_EQ(nullOr(trimToLength(v["input"].get<std::string>(), v["max"].get<std::size_t>())), v["expect"]);
  }
  for (const json& v : group("noAnswer")) {
    EXPECT_EQ(noAnswer(v["status"].get<int>()), v["expect"].get<bool>()) << v["status"];
  }
  for (const json& v : group("parseLeaseBody")) {
    SCOPED_TRACE(v["body"].dump());
    const LeaseBody body = parseLeaseBody(v["body"]);
    json out = json::object();
    if (body.lease) out["lease"] = *body.lease;
    if (body.activationId) out["activationId"] = *body.activationId;
    EXPECT_EQ(out, v["expect"]);
  }
  for (const json& v : group("parseTrialBody")) {
    SCOPED_TRACE(v["body"].dump());
    const auto body = parseTrialBody(v["body"]);
    json out = nullptr;
    if (body) {
      out = {{"trialStart", body->trialStart}, {"trialEndsAt", body->trialEndsAt}};
      if (body->lease) out["lease"] = *body->lease;
    }
    EXPECT_EQ(out, v["expect"]);
  }
}

TEST(Conformance, Mountinfo) {
  for (const json& v : group("mountinfo")) {
    SCOPED_TRACE(nameOf(v));
    EXPECT_EQ(nullOr(machineIdFilesystem(v["mountinfo"].get<std::string>())), v["expect"]);
  }
}

/**
 * A vector's scripted system as this SDK reads it. Files are read as the
 * reference reads them. The reference's commands map to the API reads this
 * SDK makes in their place (reg.exe to RegGetValueW, ioreg to IOKit): the value
 * the command printed is the value the API returns; a command that ran and
 * said no, or is not there, is a value that is not there; one killed at its
 * time box is a read that does not finish (held here until its bound passes);
 * an error nobody expected is one the API reports too.
 */
class ScriptedSource final : public keygrant::MachineIdSource {
 public:
  explicit ScriptedSource(json source) : source_(std::move(source)) {}

  /// Lets a held read go, and waits for it to finish: it touches nothing but this object.
  void release() {
    gate_.open();
    for (int i = 0; i < 500 && inFlight_.load() > 0; ++i) std::this_thread::sleep_for(Millis{10});
  }

  std::string platform() override { return source_["platform"].get<std::string>(); }

  keygrant::RegistryRead readMachineGuid() override {
    const json* answer = command({"C:\\Windows\\System32\\reg.exe", root() + "\\System32\\reg.exe"});
    if (answer == nullptr) return {kWinFileNotFound, {}};
    if (answer->contains("error")) return {failedRead(answer->at("error")), {}};
    static const std::regex guid(R"(MachineGuid\s+REG_SZ\s+(\S+))");
    std::smatch match;
    const std::string output = answer->value("output", "");
    if (!std::regex_search(output, match, guid)) return {kWinFileNotFound, {}};
    const std::string value = match[1].str();
    return {kWinSuccess, std::u16string(value.begin(), value.end()) + u'\0'};
  }

  keygrant::IoKitRead readPlatformUuid() override {
    using Status = keygrant::IoKitRead::Status;
    const json* answer = command({"/usr/sbin/ioreg"});
    if (answer == nullptr) return {Status::NoService, {}};
    if (answer->contains("error")) {
      const long status = failedRead(answer->at("error"));
      return {status == kWinFileNotFound ? Status::NoService : Status::Failed, {}};
    }
    static const std::regex uuid(R"re("IOPlatformUUID"\s*=\s*"([^"]+)")re");
    std::smatch match;
    const std::string output = answer->value("output", "");
    if (!std::regex_search(output, match, uuid)) return {Status::NoProperty, {}};
    return {Status::Ok, match[1].str()};
  }

  std::string readFile(const std::string& path) override {
    const json* file = entry(path);
    if (file == nullptr) throw keygrant::IoError("ENOENT", path);
    if (file->contains("error")) throw keygrant::IoError(file->at("error").value("code", "EIO"), path);
    return file->value("content", "");
  }

  std::optional<std::int64_t> mtimeMs(const std::string& path) override {
    const json* file = entry(path);
    if (file == nullptr || !file->contains("mtimeMs")) throw keygrant::IoError("ENOENT", path);
    return file->at("mtimeMs").get<std::int64_t>();
  }

 private:
  std::string root() { return source_.value("systemRoot", "C:\\Windows"); }

  const json* entry(const std::string& path) {
    if (!source_.contains("files") || !source_["files"].contains(path)) return nullptr;
    return &source_["files"][path];
  }

  const json* command(std::initializer_list<std::string> paths) {
    if (!source_.contains("commands")) return nullptr;
    for (const std::string& path : paths) {
      if (source_["commands"].contains(path)) return &source_["commands"][path];
    }
    return nullptr;
  }

  /** What a failed command is as an API read. Killed: a read that does not finish (held). */
  long failedRead(const json& error) {
    if (error.value("killed", false) || error.contains("signal")) {
      ++inFlight_;
      gate_.wait();
      --inFlight_;
    }
    if (error.contains("code") && error["code"].is_number()) return kWinFileNotFound;
    if (error.contains("code") && error["code"] == "ENOENT") return kWinFileNotFound;
    return 31;  // ERROR_GEN_FAILURE: an error nobody expected
  }

  json source_;
  Gate gate_;
  std::atomic<int> inFlight_{0};
};

TEST(Conformance, MachineIds) {
  for (const json& v : group("machineIds")) {
    SCOPED_TRACE(nameOf(v));
    const auto source = std::make_shared<ScriptedSource>(v["source"]);
    // Through the bounded read, as the fingerprinter reads it.
    const keygrant::MachineIdReading reading = keygrant::readMachineId(source, Millis{50});
    source->release();
    EXPECT_EQ(readingJson(reading), v["expect"]);
  }
}

TEST(Conformance, Renewals) {
  std::set<std::string> answers;
  for (const json& v : group("renewals")) {
    SCOPED_TRACE(nameOf(v));
    RenewalInput input;
    input.response = responseFrom(v["response"]);
    input.check = checkFrom(v["check"]);
    input.askedAt = v["askedAt"].get<std::int64_t>();
    input.now = v["now"].get<std::int64_t>();
    input.major = v["major"].get<std::int64_t>();
    input.key = optString(v["key"]);
    input.keptMajor = optInt(v["keptMajor"]);
    input.kind = kindFrom(v["device"]["kind"]);
    input.ownClaim = optString(v["device"]["ownClaim"]);
    const RenewalOutcome outcome = renewalOutcome(input);
    EXPECT_EQ(renewalJson(outcome), v["expect"]);
    answers.insert(!outcome.answered ? "none" : outcome.status ? answerOf(*outcome.status) : "no verdict");
  }
  // Every kind of answer an ask can get is covered.
  EXPECT_EQ(answers, (std::set<std::string>{"invalid", "licensed", "licensed with entitlements", "no verdict", "none"}));
}

TEST(Conformance, ValidLeaseAsks) {
  for (const json& v : group("validLeaseAsks")) {
    SCOPED_TRACE(nameOf(v));
    EXPECT_EQ(asksOnValidLease(claimsWith(v["claims"]), stateFrom(v["state"]), v["major"].get<std::int64_t>(),
                               v["now"].get<std::int64_t>(), v["clockNow"].get<std::int64_t>()),
              v["expect"].get<bool>());
  }
}

TEST(Conformance, Devices) {
  for (const json& v : group("devices")) {
    SCOPED_TRACE(nameOf(v));
    const json& given = v["identity"];
    keygrant::DeviceIdentity identity;
    identity.fingerprint = optString(given["fingerprint"]);
    identity.kind = kindFrom(given["kind"]);
    identity.alternates = given["alternates"].get<std::vector<std::string>>();
    EXPECT_EQ(deviceJson(deviceFrom(identity, kindFrom(v["installKind"]), v["deviceHold"].get<bool>())), v["expect"]);
  }
}

TEST(Conformance, Identities) {
  for (const json& v : group("identities")) {
    SCOPED_TRACE(nameOf(v));
    const json& r = v["reading"];
    const std::string kind = r["kind"].get<std::string>();
    keygrant::MachineIdReading reading = keygrant::MachineIdReading::transient();
    if (kind == "id") {
      reading = keygrant::MachineIdReading::of(r["id"].get<std::string>());
    } else if (kind == "unavailable") {
      reading = keygrant::MachineIdReading::unavailable();
    } else {
      EXPECT_EQ(kind, "transient");
    }
    std::optional<keygrant::HostNames> host;
    if (!v["host"].is_null()) {
      const json& h = v["host"];
      host = keygrant::HostNames{h["host"].get<std::string>(), h["platform"].get<std::string>(),
                                 h["arch"].get<std::string>()};
    }
    EXPECT_EQ(identityJson(keygrant::identityOf(reading, host)), v["expect"]);
  }
}

TEST(Conformance, SavedLastSeen) {
  for (const json& v : group("savedLastSeen")) {
    SCOPED_TRACE(nameOf(v));
    EXPECT_EQ(savedLastSeen(optInt(v["stored"]), v["clockNow"].get<std::int64_t>(), optInt(v["guard"])),
              v["expect"].get<std::int64_t>());
  }
}

TEST(Conformance, LeaseStatuses) {
  std::set<std::string> answers;
  for (const json& v : group("leaseStatuses")) {
    SCOPED_TRACE(nameOf(v));
    // Judged as `leases` judges a lease, and the status it gives with no verdict from the server.
    const VerifyResult r = verifyLease(v["token"].get<std::string>(), vectorsKey(), v["now"].get<std::int64_t>());
    const json& h = v["holder"];
    const LeaseHolder holder{h["key"].get<std::string>(), h["activationId"].get<std::string>(),
                             h["claims"].get<std::vector<std::string>>(), h["canTell"].get<bool>()};
    const LeaseCheck check = checkLease(r, v["product"].get<std::string>(), v["major"].get<std::int64_t>(), holder);
    const LicenseStatus status = leaseStatus(check, holder.key);
    EXPECT_EQ(statusJson(status), v["expect"]);
    answers.insert(answerOf(status));
  }
  // A lease that licenses, with entitlements and without; one that does not, with none.
  EXPECT_EQ(answers, (std::set<std::string>{"expired", "invalid", "licensed", "licensed with entitlements"}));
}

TEST(Conformance, TrialLeaseStatuses) {
  std::set<std::string> answers;
  for (const json& v : group("trialLeaseStatuses")) {
    SCOPED_TRACE(nameOf(v));
    const VerifyResult r = verifyLease(v["token"].get<std::string>(), vectorsKey(), v["now"].get<std::int64_t>());
    const json& device = v["device"];
    const TrialLease lease = trialLeaseOf(r, v["product"].get<std::string>(),
                                          device["claims"].get<std::vector<std::string>>(), device["canTell"].get<bool>());
    // The stored trial is its start and its lease: its end and its entitlements are the lease's.
    StoredState state;
    state.trialStart = v["trialStart"].get<std::int64_t>();
    state.trialLease = v["token"].get<std::string>();
    const LicenseStatus status =
        trialStatusOf(state, v["now"].get<std::int64_t>(), lease.valid, lease.endsAt, lease.entitlements);
    EXPECT_EQ(statusJson(status), v["expect"]);
    answers.insert(answerOf(status));
  }
  EXPECT_EQ(answers, (std::set<std::string>{"expired", "trial", "trial with entitlements"}));
}

/** A double's IEEE-754 bits, sign first, as 16 lowercase hex digits: what no JSON reader can round. */
std::string doubleBits(double value) {
  std::uint64_t bits = 0;
  static_assert(sizeof bits == sizeof value, "a double is 64 bits");
  std::memcpy(&bits, &value, sizeof bits);
  std::ostringstream out;
  out << std::hex << std::setw(16) << std::setfill('0') << bits;
  return out.str();
}

TEST(Conformance, EntitlementNumbers) {
  for (const json& v : group("entitlementNumbers")) {
    SCOPED_TRACE(nameOf(v));
    // Read as a lease's `ent` is read: the payload's JSON reader, then the
    // entitlement reader. The file's own number is a string here, so the
    // reader's rounding cannot agree with itself.
    const auto read = entitlementsOf(parseJsonText(R"({"n":)" + v["json"].get<std::string>() + "}"));
    const auto n = read.find("n");
    ASSERT_TRUE(n != read.end() && n->second.isNumber()) << v["json"];
    EXPECT_EQ(doubleBits(*n->second.number()), v["expect"].get<std::string>());
  }
}

}  // namespace
}  // namespace kgtest
