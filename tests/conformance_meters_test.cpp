// The metered-uses groups of the conformance vectors, which come before the key-pickup groups that close conformance/vectors.json: what every KeyGrant SDK
// must answer for the same inputs, each answered from its own JSON through the functions the engine decides with.
#include <optional>
#include <set>

#include "conformance.hpp"
#include "journal.hpp"
#include "meter_support.hpp"
#include "meters.hpp"

namespace kgtest {
namespace {

using namespace keygrant::detail;

/// Every group this file answers, in the file's order.
const std::vector<std::string>& kGroups = meteredGroups();

/// How many vectors the groups hold, and how many of them no call here can hold: a fraction and a text are
/// JavaScript's only (every other amount, a negative one included, fits a std::int64_t).
constexpr std::size_t kVectors = 412;
constexpr std::size_t kUnholdable = 2;

const json& groupOf(const json& root, const std::string& name) {
  static const json none = json::array();
  const json& list = root.contains(name) ? root[name] : none;
  EXPECT_TRUE(list.is_array() && !list.empty()) << name;
  return list;
}

TEST(ConformanceMeters, TheCapturedRequestBodiesAreFoundWhereTheyAreMade) {
  // The captured request bodies sit beside the reference's generator in the repository that makes them: a missing file
  // is a failure there, never a skip.
  if (!inVectorsRepository()) GTEST_SKIP() << "not the repository that makes the vectors";
  EXPECT_TRUE(aboveTests("conformance/wire-bodies.json").has_value()) << "the captured request bodies";
}

TEST(ConformanceMeters, TheHarnessAnswersEveryGroupTheVectorsHold) {
  const json* root = &vectors();
  for (const std::string& group : kGroups) EXPECT_TRUE(root->contains(group) && (*root)[group].is_array()) << group << ": no such group";
  std::size_t total = 0;
  for (const std::string& group : kGroups) total += root->contains(group) ? (*root)[group].size() : 0;
  EXPECT_EQ(total, kVectors) << "the vectors the harness answers";
}

TEST(ConformanceMeters, TheConstantsAreTheReferences) {
  const json* root = &vectors();
  const json& c = root->at("constants");
  EXPECT_EQ(c["MAX_METER_USES"], keygrant::kMaxMeterUses);
  EXPECT_EQ(c["MAX_PENDING_SPENDS"], kMaxPendingSpends);
  EXPECT_EQ(c["MAX_PENDING_VOIDS"], kMaxPendingVoids);
  EXPECT_EQ(c["PENDING_MAX_MS"], kPendingMaxMs);
  // The bound of a use request is the status check's (the HTTP bound when that is shorter), which the engine reads from its timings.
  EXPECT_EQ(c["USE_TIMEOUT_MS"], keygrant::detail::Timings{}.statusCheck.count());
}

// --- the lease's met ------------------------------------------------------------------------------

TEST(ConformanceMeters, MetersAreReadFromALeasesClaimAsTheReferenceReadsThem) {
  const json* root = &vectors();
  for (const json& v : groupOf(*root, "meterClaims")) {
    SCOPED_TRACE(nameOf(v));
    // The claim as the JSON text a lease carries it in, read as a lease's payload is; null is a lease with none.
    const Meters read = v["json"].is_string() ? metersOf(parseJsonText(v["json"].get<std::string>())) : Meters{};
    EXPECT_EQ(metersJson(read), v["expect"]);
  }
}

TEST(ConformanceMeters, LeasesThatCarryMetersVerifyAndCheckAsTheReference) {
  const json* root = &vectors();
  const KeySet key = KeySet::fromJwk((*root)["publicJwk"].dump());
  for (const json& v : groupOf(*root, "meterLeases")) {
    SCOPED_TRACE(nameOf(v));
    const VerifyResult r = verifyLease(v["token"].get<std::string>(), key, v["now"].get<std::int64_t>());
    json verified = {{"valid", r.valid}};
    if (!r.valid) verified["reason"] = to_string(*r.reason);
    if (r.claims) verified["claims"] = claimsJson(*r.claims);
    EXPECT_EQ(verified, v["verify"]);

    const json& h = v["holder"];
    const LeaseHolder holder{h["key"].get<std::string>(), h["activationId"].get<std::string>(),
                             h["claims"].get<std::vector<std::string>>(), h["canTell"].get<bool>()};
    const LeaseCheck check = checkLease(r, v["product"].get<std::string>(), v["major"].get<std::int64_t>(), holder);
    const json expect = check.ok ? json{{"ok", true}, {"test", check.test}}
                                 : json{{"ok", false}, {"reason", to_string(check.reason)}};
    EXPECT_EQ(expect, v["expect"]);
  }
}

// --- what a call asks for, and the figures ----------------------------------------------------------

TEST(ConformanceMeters, WhatACallAsksForIsWhatTheReferenceReads) {
  const json* root = &vectors();
  std::size_t skipped = 0;
  for (const json& v : groupOf(*root, "useAmounts")) {
    SCOPED_TRACE(nameOf(v));
    if (v.value("jsOnly", false)) {
      // A fraction or a text: no call here holds one.
      EXPECT_FALSE(v["n"].is_number_integer()) << "marked jsOnly but whole";
      ++skipped;
      continue;
    }
    // A call that gave no amount is a spend of 1.
    const std::int64_t n = v.contains("n") ? v["n"].get<std::int64_t>() : 1;
    const UseAsked asked = askedFor(v["meter"].get<std::string>(), n);
    const json answer = asked.refused ? json{{"refused", keygrant::to_string(*asked.refused)}}
                                      : json{{"meter", asked.meter}, {"n", asked.n}};
    EXPECT_EQ(answer, v["expect"]);
  }
  EXPECT_EQ(skipped, kUnholdable);
}

TEST(ConformanceMeters, BaseFiguresAreTheReferences) {
  const json* root = &vectors();
  for (const json& v : groupOf(*root, "meterFigures")) {
    SCOPED_TRACE(nameOf(v));
    const StoredState stored = stateFrom({{"meterFigures", v["figures"]}});
    const Meters figures = figuresOf(metersOf(v["met"]), v["iat"].get<std::int64_t>(), stored.meterFigures);
    EXPECT_EQ(metersJson(figures), v["expect"]);
  }
}

TEST(ConformanceMeters, StatusesShowTheMetersAsTheReferenceDoes) {
  const json* root = &vectors();
  for (const json& v : groupOf(*root, "meterStatuses")) {
    SCOPED_TRACE(nameOf(v));
    const StoredState stored = stateFrom({{"spends", v["spends"]}});
    EXPECT_EQ(meterStatusJson(meterStatusOf(metersOf(v["figures"]), stored.spends)), v["expect"]);
  }
}

TEST(ConformanceMeters, ASpendWithNoAnswerIsAllowedAsTheReferenceAllowsIt) {
  const json* root = &vectors();
  for (const json& v : groupOf(*root, "offlineSpends")) {
    SCOPED_TRACE(nameOf(v));
    const StoredState stored = stateFrom({{"spends", v["spends"]}});
    const OfflineVerdict verdict = offlineSpend(metersOf(v["figures"]), stored.spends, v["meter"].get<std::string>(),
                                                v["n"].get<std::int64_t>());
    json answer;
    switch (verdict.kind) {
      case OfflineVerdict::Kind::Allowed: answer = {{"ok", true}, {"left", verdict.left}, {"limit", verdict.limit}}; break;
      case OfflineVerdict::Kind::NoMeter: answer = {{"ok", false}, {"error", "no-meter"}}; break;
      case OfflineVerdict::Kind::OfflineLimit:
        answer = {{"ok", false}, {"error", "offline-limit"}, {"left", verdict.left}};
        break;
    }
    EXPECT_EQ(answer, v["expect"]);
  }
}

// --- when to ask --------------------------------------------------------------------------------------

TEST(ConformanceMeters, ACallAsksTheServerWhenTheReferenceDoes) {
  const json* root = &vectors();
  for (const json& v : groupOf(*root, "useAsks")) {
    SCOPED_TRACE(nameOf(v));
    EXPECT_EQ(useAsksNow(stateFrom(v["state"]), v["clockNow"].get<std::int64_t>(), v["now"].get<std::int64_t>(),
                         v["covered"].get<bool>()),
              v["expect"].get<bool>());
  }
}

TEST(ConformanceMeters, AValidLeaseAsksToReportAJournalWhenTheReferenceDoes) {
  const json* root = &vectors();
  for (const json& v : groupOf(*root, "syncAsks")) {
    SCOPED_TRACE(nameOf(v));
    const LeaseClaims claims = claimsWith(v["claims"]);
    EXPECT_EQ(asksOnValidLease(claims, stateFrom(v["state"]), v["major"].get<std::int64_t>(),
                               v["now"].get<std::int64_t>(), v["clockNow"].get<std::int64_t>()),
              v["expect"].get<bool>());
  }
}

// --- what an answer does ------------------------------------------------------------------------------

json confirmedJson(const std::vector<Confirmed>& entries) {
  json out = json::array();
  for (const Confirmed& c : entries) {
    out.push_back(c.voided ? json{{"id", c.id}, {"void", true}} : json{{"id", c.id}, {"n", c.n}});
  }
  return out;
}

TEST(ConformanceMeters, AUseAnswerDoesWhatTheReferenceDoes) {
  const json* root = &vectors();
  for (const json& v : groupOf(*root, "useOutcomes")) {
    SCOPED_TRACE(nameOf(v));
    const std::optional<Reply> response = responseFrom(v["response"]);
    const json& device = v["device"];
    UseInput input;
    input.response = response ? &*response : nullptr;
    input.askedAt = v["askedAt"].get<std::int64_t>();
    input.major = v["major"].get<std::int64_t>();
    input.kind = kindFrom(device["kind"]);
    input.ownClaim = optString(device["ownClaim"]);
    const UseOutcome outcome = useOutcome(input);
    json answer = {{"outcome", to_string(outcome.kind)}};
    if (outcome.refusal) answer["refusal"] = keygrant::to_string(*outcome.refusal);
    if (outcome.left) answer["left"] = *outcome.left;
    answer["patch"] = patchJson(outcome.patch);
    answer["figures"] = figuresJson(outcome.figures);
    answer["confirmed"] = confirmedJson(outcome.confirmed);
    EXPECT_EQ(answer, v["expect"]);
  }
}

TEST(ConformanceMeters, EveryKindOfAnswerHasAVector) {
  const json* root = &vectors();
  std::set<std::string> seen;
  for (const json& v : groupOf(*root, "useOutcomes")) seen.insert(v["expect"]["outcome"].get<std::string>());
  std::set<std::string> all;
  for (const UseOutcomeKind kind : {UseOutcomeKind::Spent, UseOutcomeKind::UsedUp, UseOutcomeKind::NoMeter,
                                    UseOutcomeKind::Refused, UseOutcomeKind::Unanswered}) {
    all.insert(to_string(kind));
  }
  EXPECT_EQ(seen, all);
}

// --- the journal ----------------------------------------------------------------------------------------

JournalOp opOf(const json& op) {
  const std::string kind = op["kind"].get<std::string>();
  const std::string about = op.contains("about") ? op["about"].get<std::string>() : std::string();
  if (kind == "offline" || kind == "unanswered" || kind == "void") {
    std::optional<std::int64_t> limit;
    if (op.contains("limit")) limit = op["limit"].get<std::int64_t>();
    return JournalOp::spendOf(*keygrant::spendKindFromString(kind), about, op["id"].get<std::string>(),
                              op["meter"].get<std::string>(), op["n"].get<std::int64_t>(), op["at"].get<std::int64_t>(),
                              limit, op.value("told", false));
  }
  if (kind == "confirmed") {
    std::vector<Confirmed> entries;
    for (const json& e : op["entries"]) {
      const std::string id = e["id"].get<std::string>();
      entries.push_back(e.value("void", false) ? Confirmed::taken(id) : Confirmed::count(id, e["n"].get<std::int64_t>()));
    }
    return JournalOp::confirmedOf(about, std::move(entries));
  }
  if (kind == "figures") return JournalOp::figuresOf(about, stateFrom({{"meterFigures", op["figures"]}}).meterFigures);
  if (kind == "activated") return JournalOp::activatedOf(about);
  if (kind == "deactivated") return JournalOp::deactivated();
  ADD_FAILURE() << "an op this harness does not know: " << kind;
  return JournalOp::deactivated();
}

TEST(ConformanceMeters, TheJournalLandsAsTheReferenceLandsIt) {
  const json* root = &vectors();
  for (const json& v : groupOf(*root, "journals")) {
    SCOPED_TRACE(nameOf(v));
    const Journaled landed = journaled(stateFrom(v["latest"]), opOf(v["op"]), v["now"].get<std::int64_t>());
    const json answer = {{"spends", spendsJson(landed.spends)},
                         {"meterFigures", figuresJson(landed.meterFigures)},
                         {"refused", landed.refused}};
    EXPECT_EQ(answer, v["expect"]);
  }
}

TEST(ConformanceMeters, AReportCarriesTheJournalAsTheReferenceReportsIt) {
  const json* root = &vectors();
  for (const json& v : groupOf(*root, "spendReports")) {
    SCOPED_TRACE(nameOf(v));
    const std::optional<ordered_json> report = spendsReported(stateFrom({{"spends", v["spends"]}}).spends);
    EXPECT_EQ(report ? json::parse(report->dump()) : json(nullptr), v["expect"]);
  }
}

TEST(ConformanceMeters, AnAnswerConfirmsTheReportsTheReferenceSaysItDoes) {
  const json* root = &vectors();
  for (const json& v : groupOf(*root, "spendConfirmations")) {
    SCOPED_TRACE(nameOf(v));
    const std::optional<Reply> response = responseFrom(v["response"]);
    EXPECT_EQ(confirmedJson(spendsConfirmed(response ? &*response : nullptr)), v["expect"]);
  }
}

}  // namespace
}  // namespace kgtest
