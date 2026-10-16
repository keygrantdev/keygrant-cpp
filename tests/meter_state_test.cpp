// The lease's `met` and the stored journal, read leniently as the contract says; a number out of a double's range, which
// JavaScript reads as infinity and fails nothing for; and the words and values a caller sees.
#include <sstream>

#include "meter_support.hpp"

namespace kgtest {
namespace {

using namespace keygrant::detail;
using keygrant::HttpResponse;

Meters read(const std::string& text) { return metersOf(parseJsonText(text)); }

std::string meterOf(const std::string& left, const std::string& total = "10", const std::string& offline = "1") {
  return R"({"exports":{"left":)" + left + R"(,"total":)" + total + R"(,"offline":)" + offline + "}}";
}

// --- the lease's met ----------------------------------------------------------------------------------------------------

TEST(MeterState, ReadsAMetThatIsNotAnObjectAsNoMeters) {
  for (const char* text : {"null", "[]", R"([{"left":1,"total":1,"offline":1}])", R"("exports")", "7", "true", "{}"}) {
    EXPECT_TRUE(read(text).empty()) << text;
  }
}

TEST(MeterState, DropsAMemberWhoseFiguresAreNotNonNegativeWholeNumbersOfAtMost2To53Minus1) {
  for (const char* left : {"-1", "1.5", R"("4")", "null", "true", "[4]", "9007199254740992", "9007199254740993", "1e400", "-1e400"}) {
    EXPECT_TRUE(read(meterOf(left)).empty()) << left;
    EXPECT_TRUE(read(meterOf("4", left)).empty()) << left;
    EXPECT_TRUE(read(meterOf("4", "10", left)).empty()) << left;
  }
  for (const char* left : {"4", "4.0", "4e0", "0", "-0", "0.0", "9007199254740991", "9.007199254740991e15"}) {
    EXPECT_EQ(read(meterOf(left)).size(), 1u) << left;
  }
  EXPECT_EQ(read(meterOf("4.0")).at("exports").left, 4);
  EXPECT_EQ(read(meterOf("-0")).at("exports").left, 0);
  EXPECT_EQ(read(meterOf("9007199254740991")).at("exports").left, 9007199254740991LL);
  // A member that lacks a figure, or is not an object.
  for (const char* text : {R"({"exports":{"total":10,"offline":1}})", R"({"exports":{"left":4,"offline":1}})",
                           R"({"exports":{"left":4,"total":10}})", R"({"exports":5})", R"({"exports":"x"})", R"({"exports":null})",
                           R"({"exports":[4,10,1]})"}) {
    EXPECT_TRUE(read(text).empty()) << text;
  }
}

TEST(MeterState, ReadsResetsAsTrueOnlyForJsonTrue) {
  const auto resets = [](const std::string& value) {
    return read(R"({"exports":{"left":4,"total":10,"offline":1,"resets":)" + value + "}}").at("exports").resets;
  };
  EXPECT_TRUE(resets("true"));
  for (const char* other : {"false", R"("true")", "1", "null", "[true]", "{}"}) EXPECT_FALSE(resets(other)) << other;
  EXPECT_FALSE(read(meterOf("4")).at("exports").resets);
}

TEST(MeterState, KeepsNamesAsTheyAreTheServersCapsUnenforcedAndAMetersOtherMembersIgnored) {
  const Meters odd = read(R"({"__proto__":{"left":1,"total":2,"offline":0},"constructor":{"left":3,"total":3,"offline":1},)"
                          R"("":{"left":1,"total":1,"offline":1},"é":{"left":1,"total":1,"offline":1}})");
  EXPECT_EQ(odd.size(), 4u);
  EXPECT_EQ(odd.count("__proto__"), 1u);
  EXPECT_EQ(odd.count(""), 1u);
  EXPECT_EQ(odd.count("\xC3\xA9"), 1u);
  // More than eight meters, left above total, a figure past the server's bound: all kept.
  std::string many = "{";
  for (char code = 'a'; code <= 'j'; ++code) many += std::string(many.size() > 1 ? "," : "") + "\"" + code + R"(":{"left":20,"total":10,"offline":1})";
  EXPECT_EQ(read(many + "}").size(), 10u);
  EXPECT_EQ(read(meterOf("1000000001", "2000000000")).at("exports").left, 1000000001);
  EXPECT_EQ(read(R"({"exports":{"left":4,"total":10,"offline":1,"overage":3,"extra":{"a":1}}})").at("exports"),
            (BaseFigure{4, 10, 1, false}));
  // A malformed meter beside good ones: only it is dropped.
  const Meters mixed = read(R"({"a":{"left":4,"total":10,"offline":1},"b":{"left":-1,"total":10,"offline":1},"c":{"left":4,"total":10,"offline":1}})");
  EXPECT_EQ(mixed.size(), 2u);
  EXPECT_EQ(mixed.count("b"), 0u);
}

TEST(MeterState, AFigureOutOfADoublesRangeDropsItsMemberAndNeverTheLease) {
  Harness h;
  const std::string lease = rawMetLease(h, R"({"good":{"left":4,"total":10,"offline":1},"huge":{"left":1e400,"total":10,"offline":1}})");
  const VerifyResult verified = verifyLease(lease, KeySet::fromJwk(h.key.publicJwk()), NOW);
  ASSERT_TRUE(verified.valid);
  ASSERT_TRUE(verified.claims->met.has_value());
  EXPECT_EQ(verified.claims->met->size(), 1u);
  EXPECT_EQ(verified.claims->met->count("good"), 1u);
  // As a key's lease the harness stores, it licenses all the same, with the one meter.
  StoredState state = held(lease);
  h.store(state);
  const LicenseStatus status = h.license().status();
  EXPECT_EQ(status.state, LicenseState::Licensed);
  ASSERT_EQ(status.meters.size(), 1u);
  EXPECT_EQ(status.meters.at("good").left, 4);
}

TEST(MeterState, AMetClaimIsAbsentNullOrAnyJsonAndALeaseWithNoneIsUnchanged) {
  Harness h;
  const KeySet key = KeySet::fromJwk(h.key.publicJwk());
  const auto claimsOfLease = [&](const std::string& metText) { return *verifyLease(rawMetLease(h, metText), key, NOW).claims; };
  EXPECT_FALSE(verifyLease(h.sign(KEY, "perpetual", 3600), key, NOW).claims->met.has_value());
  for (const char* text : {"null", "[]", R"("x")", "7", "true"}) {
    const LeaseClaims claims = claimsOfLease(text);
    ASSERT_TRUE(claims.met.has_value()) << text;
    EXPECT_TRUE(claims.met->empty()) << text;
  }
  EXPECT_EQ(claimsOfLease(meterOf("4")).met->size(), 1u);
  // A lease with no claim is the claims JSON without it; one with an empty claim has an empty object.
  LeaseClaims none = claimsOfLease("null");
  none.met.reset();
  EXPECT_FALSE(claimsJson(none).contains("met"));
  EXPECT_EQ(claimsJson(claimsOfLease("[]"))["met"], json::object());
}

TEST(MeterState, ReadsAnOutOfRangeNumberAsNoNumberNeverAsNoJson) {
  EXPECT_TRUE(parseJsonText(R"({"x":1e400,"y":-1e400,"z":[1e999]})").is_object());
  EXPECT_TRUE(parseJsonText(R"({"x":1e400})")["x"].is_null());
  EXPECT_EQ(parseJsonText(R"({"x":1e300,"s":"1e400"})")["s"], "1e400");
  EXPECT_EQ(parseJsonText(R"({"x":1e300})")["x"], 1e300);
  // Inside a string it is text; a malformed number is still no JSON.
  EXPECT_TRUE(parseJsonText(R"({"x":"1e400"})").is_object());
  EXPECT_TRUE(parseJsonText(R"({"x":1e4e00})").is_discarded());
  EXPECT_TRUE(parseJsonText(R"({"x":1e})").is_discarded());
  EXPECT_TRUE(parseJsonText("1e400 junk").is_discarded());
  // A number of more than 308 digits.
  EXPECT_TRUE(parseJsonText("[" + std::string(400, '9') + "]").is_array());
  // A string with a backslash and a quote before the number.
  const json escaped = parseJsonText("{\"a\\\"b\":1e400}");
  EXPECT_TRUE(escaped["a\"b"].is_null());
}

TEST(MeterState, AUseAnswerWithANumberOutOfADoublesRangeIsStillAnAnswer) {
  MeterHarness h;
  const std::string spent =
      R"({"spent":true,"meters":{"exports":{"left":1e400,"total":100,"offline":10}},"at":1e400,"spends":[{"id":"x","n":1e400}]})";
  h.http->on("use", [spent](const json&) { return HttpResponse{200, spent}; });
  // A spend, answered by the server: not the offline path, and the figures it gave out of range are none.
  EXPECT_EQ(h.use("exports"), UseResult::spent(90));
  EXPECT_EQ(h.http->count("use"), 1);
  EXPECT_TRUE(h.journal().empty());
  EXPECT_TRUE(h.stored()->meterFigures.empty());
  EXPECT_EQ(h.stored()->backoffSince, std::nullopt);
  h.http->on("use", [](const json&) { return HttpResponse{409, R"({"error":"used-up","left":1e400})"}; });
  EXPECT_EQ(h.use("exports"), UseResult::refused(UseError::UsedUp));
}

// --- the stored state ------------------------------------------------------------------------------------------------------------

TEST(MeterState, TheStoredStateReadsTheJournalLenientlyAndWritesItBackWithoutWhatItDropped) {
  const std::string text = R"({"key":"K","activationId":"act_1","spends":[
      {"id":"a","meter":"exports","n":2,"at":5,"kind":"offline","extra":"dropped"},
      {"id":"b","meter":"exports","n":4.0,"at":6.0,"kind":"unanswered"},
      {"id":"c","meter":"exports","n":0,"at":7,"kind":"offline"},
      {"id":"d","meter":"exports","n":1,"at":-1,"kind":"offline"},
      {"id":"e","meter":"exports","n":1,"at":7,"kind":"sideways"},
      {"id":3,"meter":"exports","n":1,"at":7,"kind":"offline"},
      {"id":"f","meter":4,"n":1,"at":7,"kind":"offline"},
      {"id":"g","meter":"exports","n":1.5,"at":7,"kind":"offline"},
      {"id":"h","meter":"exports","n":9007199254740993,"at":7,"kind":"offline"},
      {"id":"","meter":"","n":1,"at":0,"kind":"void"},
      null, 5, "x", [1]],
      "meterFigures":{"exports":{"left":3,"total":10,"offline":1,"resets":true,"at":9,"more":1},
                      "bad":{"left":-1,"total":10,"offline":1,"at":9},"worse":{"left":1,"total":1,"offline":1},"ugly":5}})";
  const std::optional<StoredState> state = keygrant::parseState(text);
  ASSERT_TRUE(state.has_value());
  EXPECT_EQ(state->spends, (std::vector<SpendEntry>{SpendEntry{"a", "exports", 2, 5, SpendKind::Offline},
                                                    SpendEntry{"b", "exports", 4, 6, SpendKind::Unanswered},
                                                    SpendEntry{"", "", 1, 0, SpendKind::Void}}));
  ASSERT_EQ(state->meterFigures.size(), 1u);
  EXPECT_EQ(state->meterFigures.at("exports"), (MeterFigure{3, 10, 1, true, 9}));
  EXPECT_TRUE(state->otherFields.empty());
  // Written back without what was dropped: the entry's unknown member, the figure's, and every entry that was not one.
  const json written = json::parse(keygrant::serializeState(*state));
  EXPECT_EQ(written["spends"].size(), 3u);
  EXPECT_FALSE(written["spends"][0].contains("extra"));
  EXPECT_EQ(written["meterFigures"].size(), 1u);
  EXPECT_FALSE(written["meterFigures"]["exports"].contains("more"));
  EXPECT_EQ(keygrant::parseState(keygrant::serializeState(*state)), state);
}

TEST(MeterState, AJournalThatIsNotAListAndFiguresThatAreNotAnObjectAreReadAsNoneAndNotKept) {
  for (const char* junk : {R"("junk")", "5", "null", R"({"not":"a list"})", "true"}) {
    const std::optional<StoredState> state = keygrant::parseState(std::string(R"({"spends":)") + junk + R"(,"meterFigures":)" + junk + "}");
    ASSERT_TRUE(state.has_value()) << junk;
    EXPECT_TRUE(state->spends.empty()) << junk;
    EXPECT_TRUE(state->meterFigures.empty()) << junk;
    EXPECT_TRUE(state->otherFields.empty()) << junk;
    const json written = json::parse(keygrant::serializeState(*state));
    EXPECT_FALSE(written.contains("spends")) << junk;
    EXPECT_FALSE(written.contains("meterFigures")) << junk;
  }
}

TEST(MeterState, AJournalAnAppPutInOtherFieldsIsNotWrittenBesideTheStatesOwn) {
  StoredState state;
  state.spends = {entry(SpendKind::Offline, "A", 2, NOW)};
  state.otherFields["spends"] = R"([{"id":"smuggled"}])";
  state.otherFields["meterFigures"] = R"({"x":1})";
  state.otherFields["future"] = "7";
  const json written = json::parse(keygrant::serializeState(state));
  EXPECT_EQ(written["spends"].size(), 1u);
  EXPECT_EQ(written["spends"][0]["id"], spendId("A"));
  EXPECT_FALSE(written.contains("meterFigures"));
  EXPECT_EQ(written["future"], 7);
}

TEST(MeterState, ARoundTripKeepsEveryEntryAndFigureAndTheEmptyJournalIsTheFieldAbsent) {
  StoredState state;
  state.key = KEY;
  state.activationId = "act_1";
  state.spends = {entry(SpendKind::Offline, "A", 2, NOW), entry(SpendKind::Void, "B", 1, NOW + 1, "renders")};
  state.meterFigures = {{"exports", MeterFigure{3, 10, 1, true, NOW}}};
  const std::string text = keygrant::serializeState(state);
  EXPECT_EQ(keygrant::parseState(text), std::optional<StoredState>(state));
  const std::string bare = keygrant::serializeState(StoredState{});
  EXPECT_EQ(bare.find("spends"), std::string::npos);
  EXPECT_EQ(bare.find("meterFigures"), std::string::npos);
}

// --- the words and the values --------------------------------------------------------------------------------------------------------

TEST(MeterState, EveryErrorIsItsWordAndAResultSaysWhatItIs) {
  const std::vector<std::pair<UseError, const char*>> words = {
      {UseError::InvalidAmount, "invalid-amount"}, {UseError::NoMeter, "no-meter"},   {UseError::UsedUp, "used-up"},
      {UseError::OfflineLimit, "offline-limit"},   {UseError::NotLicensed, "not-licensed"}, {UseError::Storage, "storage"},
      {UseError::Revoked, "revoked"},              {UseError::Upgrade, "upgrade"},     {UseError::Outdated, "outdated"},
      {UseError::Device, "device"}};
  for (const auto& [error, word] : words) {
    EXPECT_STREQ(keygrant::to_string(error), word);
    std::ostringstream out;
    out << error;
    EXPECT_EQ(out.str(), word);
  }
  // Ten errors, ten words, none the same.
  std::set<std::string> distinct;
  for (const auto& [error, word] : words) distinct.insert(word);
  EXPECT_EQ(distinct.size(), 10u);
  const UseResult spent = UseResult::spent(5);
  EXPECT_TRUE(spent.ok);
  EXPECT_FALSE(spent.offline);
  EXPECT_EQ(spent.left, 5);
  EXPECT_EQ(spent.error, std::nullopt);
  const UseResult offline = UseResult::spent(4, true);
  EXPECT_TRUE(offline.offline);
  EXPECT_NE(spent, offline);
  const UseResult usedUp = UseResult::refused(UseError::UsedUp, 3);
  EXPECT_FALSE(usedUp.ok);
  EXPECT_EQ(usedUp.error, UseError::UsedUp);
  EXPECT_EQ(usedUp.left, 3);
  EXPECT_EQ(UseResult::refused(UseError::NoMeter).left, std::nullopt);
  EXPECT_NE(UseResult::refused(UseError::NoMeter), UseResult::refused(UseError::UsedUp));
  std::ostringstream shown;
  shown << offline << " " << usedUp;
  EXPECT_EQ(shown.str(), "{ ok: true, left: 4, offline: true } { ok: false, error: used-up, left: 3 }");
  for (const char* word : {"offline", "unanswered", "void"}) {
    EXPECT_STREQ(keygrant::to_string(*keygrant::spendKindFromString(word)), word);
  }
  EXPECT_EQ(keygrant::spendKindFromString("Offline"), std::nullopt);
  EXPECT_EQ(keygrant::spendKindFromString(""), std::nullopt);
}

TEST(MeterState, EveryMemberOfAValueCountsInItsEquality) {
  // A comparison that left a member out would make every expectation on that member, in every test, hold for any value.
  const auto differs = [](const auto& base, const char* member, const auto& change) {
    auto other = base;
    change(other);
    EXPECT_NE(base, other) << member;
    EXPECT_EQ(base, base) << member;
  };
  const UseResult spent = UseResult::spent(5);
  differs(spent, "ok", [](UseResult& r) { r.ok = false; });
  differs(spent, "left", [](UseResult& r) { r.left = 6; });
  differs(spent, "no left", [](UseResult& r) { r.left = std::nullopt; });
  differs(spent, "offline", [](UseResult& r) { r.offline = true; });
  differs(spent, "error", [](UseResult& r) { r.error = UseError::Storage; });
  const MeterStatus status{1, 2, 3, 4, false};
  differs(status, "left", [](MeterStatus& m) { m.left = 9; });
  differs(status, "total", [](MeterStatus& m) { m.total = 9; });
  differs(status, "offline", [](MeterStatus& m) { m.offline = 9; });
  differs(status, "unsynced", [](MeterStatus& m) { m.unsynced = 9; });
  differs(status, "resets", [](MeterStatus& m) { m.resets = true; });
  const SpendEntry spend = entry(SpendKind::Offline, "A", 1, NOW);
  differs(spend, "id", [](SpendEntry& e) { e.id = spendId("B"); });
  differs(spend, "meter", [](SpendEntry& e) { e.meter = "renders"; });
  differs(spend, "n", [](SpendEntry& e) { e.n = 2; });
  differs(spend, "at", [](SpendEntry& e) { e.at = NOW + 1; });
  differs(spend, "kind", [](SpendEntry& e) { e.kind = SpendKind::Unanswered; });
  const MeterFigure figure{1, 2, 3, false, 4};
  differs(figure, "left", [](MeterFigure& f) { f.left = 9; });
  differs(figure, "total", [](MeterFigure& f) { f.total = 9; });
  differs(figure, "offline", [](MeterFigure& f) { f.offline = 9; });
  differs(figure, "resets", [](MeterFigure& f) { f.resets = true; });
  differs(figure, "at", [](MeterFigure& f) { f.at = 9; });
}

TEST(MeterState, AStatusCountsItsMetersInItsEqualityAndShowsThem) {
  LicenseStatus a = licensed(KEY);
  LicenseStatus b = licensed(KEY);
  EXPECT_EQ(a, b);
  a.meters["exports"] = MeterStatus{1, 2, 3, 4, true};
  EXPECT_NE(a, b);
  b.meters["exports"] = MeterStatus{1, 2, 3, 4, true};
  EXPECT_EQ(a, b);
  b.meters["exports"].resets = false;
  EXPECT_NE(a, b);
  b.meters["exports"].resets = true;
  b.meters["renders"] = MeterStatus{};
  EXPECT_NE(a, b);
  std::ostringstream out;
  out << a;
  EXPECT_NE(out.str().find("meters: { \"exports\": { left: 1, total: 2, offline: 3, unsynced: 4, resets: true } }"), std::string::npos)
      << out.str();
  // The state compares its journal and its figures.
  StoredState s1;
  StoredState s2;
  EXPECT_EQ(s1, s2);
  s1.spends = {entry(SpendKind::Offline, "A", 1, NOW)};
  EXPECT_NE(s1, s2);
  s2.spends = s1.spends;
  s1.meterFigures["x"] = MeterFigure{1, 1, 1, false, 1};
  EXPECT_NE(s1, s2);
}

}  // namespace
}  // namespace kgtest
