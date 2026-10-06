// The state's JSON: the reference's field names, absent fields omitted, so the
// file is portable between the SDKs.
#include <sstream>

#include "offline.hpp"
#include "support.hpp"
#include "utf.hpp"

namespace kgtest {
namespace {

using keygrant::parseState;
using keygrant::Refusal;
using keygrant::serializeState;

StoredState everything() {
  StoredState s;
  s.key = KEY;
  s.activationId = "act_1";
  s.lease = "h.p.s";
  s.trialStart = 1;
  s.trialEndsAt = 2;
  s.trialLease = "t.p.s";
  s.trialAt = 3;
  s.lastSeen = 4;
  s.checkedMajor = 5;
  s.keptMajor = 6;
  s.backoffSince = 7;
  s.backoffMs = 8;
  s.refused = Refusal::Device;
  s.refusedMajor = 9;
  s.refusedFor = std::vector<std::string>{"c1", "c2"};
  s.deviceKind = DeviceKind::Machine;
  s.verdictAt = 10;
  s.rev = 11;
  return s;
}

TEST(StateJson, UsesTheReferencesFieldNames) {
  const json written = json::parse(serializeState(everything()));
  const json expected = {
      {"key", KEY},          {"activationId", "act_1"}, {"lease", "h.p.s"},     {"trialStart", 1},
      {"trialEndsAt", 2},    {"trialLease", "t.p.s"},   {"trialAt", 3},         {"lastSeen", 4},
      {"checkedMajor", 5},   {"keptMajor", 6},          {"backoffSince", 7},    {"backoffMs", 8},
      {"refused", "device"}, {"refusedMajor", 9},       {"refusedFor", {"c1", "c2"}},
      {"deviceKind", "machine"}, {"verdictAt", 10},     {"rev", 11},
  };
  EXPECT_EQ(written, expected);
  EXPECT_EQ(parseState(serializeState(everything())), everything());
}

TEST(StateJson, OmitsAbsentFields) {
  StoredState s;
  s.key = KEY;
  EXPECT_EQ(serializeState(s), R"({"key":"SLUICE-AAAA-BBBB-CCCC-DDDD"})");
  EXPECT_EQ(serializeState(StoredState{}), "{}");
}

TEST(StateJson, KeepsFieldsItDoesNotKnowAndWritesThemBack) {
  const auto state = parseState(R"({"key":"K","futureField":{"a":[1,2]},"note":"x"})");
  ASSERT_TRUE(state);
  EXPECT_EQ(state->key, "K");
  EXPECT_EQ(json::parse(serializeState(*state)), json::parse(R"({"key":"K","futureField":{"a":[1,2]},"note":"x"})"));
}

TEST(StateJson, ReadsAKnownFieldOfTheWrongTypeAsAbsent) {
  const auto state = parseState(R"({"key":7,"lastSeen":"soon","refused":"weird","deviceKind":"vm","refusedFor":["a",3,null,"b"]})");
  ASSERT_TRUE(state);
  EXPECT_EQ(state->key, std::nullopt);
  EXPECT_EQ(state->lastSeen, std::nullopt);
  EXPECT_EQ(state->refused, std::nullopt);
  EXPECT_EQ(state->deviceKind, std::nullopt);
  EXPECT_EQ(state->refusedFor, (std::vector<std::string>{"a", "b"}));
}

TEST(StateJson, KeepsAValueItCannotReadOnlyWhileItIsShallowEnoughToWriteBack) {
  // Writing JSON recurses as deep as it nests: past 64 levels a value this SDK
  // cannot read is not kept, whether it is an unknown field or a known one.
  const auto state = parseState(R"({"key":"K","shallow":)" + nestedArrays(64) + R"(,"deep":)" + nestedArrays(65) +
                                R"(,"refused":)" + nestedArrays(65) + "}");
  ASSERT_TRUE(state);
  EXPECT_EQ(state->otherFields.count("shallow"), 1u);
  EXPECT_EQ(state->otherFields.count("deep"), 0u);
  EXPECT_EQ(state->otherFields.count("refused"), 0u);
  EXPECT_EQ(json::parse(serializeState(*state)), json::parse(R"({"key":"K","shallow":)" + nestedArrays(64) + "}"));
  // Whatever the depth, no crash reading or writing.
  for (const std::size_t depth : kDeepNesting) {
    SCOPED_TRACE(depth);
    const auto deep = parseState(R"({"key":"K","deep":)" + nestedArrays(depth) + "}");
    ASSERT_TRUE(deep);
    EXPECT_EQ(serializeState(*deep), R"({"key":"K"})");
  }
  // An app's own otherFields as deep: not written either.
  StoredState own;
  own.key = "K";
  own.otherFields["deep"] = nestedArrays(kDeepNesting[1]);
  EXPECT_EQ(serializeState(own), R"({"key":"K"})");
}

TEST(StateJson, KeepsAKnownFieldsValueItCannotReadUntilItWritesTheFieldItself) {
  // A newer SDK's refusal and device kind, and values of the wrong type: read
  // as absent, and written back as they were, as the reference keeps them.
  const std::string written =
      R"({"key":"K","refused":"suspended","deviceKind":"tpm","trialStart":"soon","lease":null,"rev":4})";
  const auto state = parseState(written);
  ASSERT_TRUE(state);
  EXPECT_EQ(state->refused, std::nullopt);
  EXPECT_EQ(state->deviceKind, std::nullopt);
  EXPECT_EQ(state->trialStart, std::nullopt);
  EXPECT_EQ(state->lease, std::nullopt);
  EXPECT_EQ(json::parse(serializeState(*state)), json::parse(written));

  // A save that leaves those fields alone keeps them as they were.
  keygrant::detail::StatePatch untouched;
  untouched.keptMajor = keygrant::detail::Change<std::int64_t>::set(2);
  StoredState kept = *state;
  keygrant::detail::apply(kept, untouched);
  EXPECT_EQ(json::parse(serializeState(kept)),
            json::parse(R"({"key":"K","refused":"suspended","deviceKind":"tpm","trialStart":"soon","lease":null,"rev":4,"keptMajor":2})"));

  // One that writes or clears a field makes it this SDK's: the kept value goes.
  keygrant::detail::StatePatch written2;
  written2.refused = keygrant::detail::Change<Refusal>::set(Refusal::Revoked);
  written2.deviceKind = keygrant::detail::Change<DeviceKind>::clear();
  written2.lease = keygrant::detail::Change<std::string>::clear();
  StoredState replaced = *state;
  keygrant::detail::apply(replaced, written2);
  EXPECT_EQ(json::parse(serializeState(replaced)), json::parse(R"({"key":"K","refused":"revoked","trialStart":"soon","rev":4})"));

  // A value this SDK sets directly (the storage's revision) is its own over the one kept.
  StoredState revised = *state;
  revised.trialStart = 5;
  EXPECT_EQ(json::parse(serializeState(revised))["trialStart"], 5);
}

TEST(StateJson, ReadsNumbersAsJavaScriptHoldsThem) {
  const auto state = parseState(R"({"lastSeen":1757000000000.0,"trialStart":1.9,"backoffMs":1e300,"verdictAt":-5})");
  ASSERT_TRUE(state);
  EXPECT_EQ(state->lastSeen, 1'757'000'000'000);
  EXPECT_EQ(state->trialStart, 1);
  EXPECT_EQ(state->backoffMs, 9'007'199'254'740'991);
  EXPECT_EQ(state->verdictAt, -5);
}

TEST(StateJson, IsNoStateForJsonThatIsNotAnObjectAndThrowsForWhatIsNotJson) {
  EXPECT_EQ(parseState("null"), std::nullopt);
  EXPECT_EQ(parseState("[1,2]"), std::nullopt);
  EXPECT_THROW(parseState("{\"key\":"), std::invalid_argument);
  EXPECT_THROW(parseState(""), std::invalid_argument);
}

TEST(StateJson, ReadsAByteOrderMarkAndIllFormedUtf8) {
  const auto state = parseState("\xEF\xBB\xBF{\"key\":\"K\xff\"}");
  ASSERT_TRUE(state);
  EXPECT_EQ(state->key, "K\xEF\xBF\xBD");
}

TEST(StateJson, WritesIllFormedUtf8AsTheWhatwgDecoderReadsItNeverThrowing) {
  // Each ill-formed sequence one U+FFFD (EF BF BD): a lone byte, a sequence
  // cut short, a surrogate's three bytes (three of them).
  const std::string fffd = "\xEF\xBF\xBD";
  const std::vector<std::pair<std::string, std::string>> cases = {
      {"K\xff", "K" + fffd},
      {"K\xE2\x82", "K" + fffd},
      {"K\xE2\x82Z", "K" + fffd + "Z"},
      {"K\xED\xA0\x80Z", "K" + fffd + fffd + fffd + "Z"},
  };
  for (const auto& [given, written] : cases) {
    StoredState s;
    s.key = given;
    std::string text;
    ASSERT_NO_THROW(text = serializeState(s));
    EXPECT_EQ(text, "{\"key\":\"" + written + "\"}");
    EXPECT_EQ(written, keygrant::detail::sanitizeUtf8(given));
    // Read back as written: the key every later run sees.
    EXPECT_EQ(parseState(text)->key, written);
  }
}

TEST(Statuses, PrintAsTheReferenceNamesThem) {
  std::ostringstream out;
  out << licensed(KEY, true) << ' ' << status(LicenseState::Invalid, LicenseReason::Tampered) << ' '
      << failed(ActivateError::InvalidLease);
  EXPECT_EQ(out.str(),
            "{ state: licensed, key: SLUICE-AAAA-BBBB-CCCC-DDDD, test: true } { state: invalid, reason: tampered } "
            "{ ok: false, error: invalid-lease }");
  EXPECT_STREQ(keygrant::to_string(LicenseState::Unknown), "unknown");
  EXPECT_STREQ(keygrant::to_string(LicenseReason::Storage), "storage");
  EXPECT_STREQ(keygrant::to_string(ActivateError::NotConfigured), "not-configured");
  EXPECT_EQ(keygrant::refusalFromString("upgrade"), Refusal::Upgrade);
  EXPECT_EQ(keygrant::deviceKindFromString("hostname"), DeviceKind::Hostname);
}

TEST(Statuses, AreEqualOnlyWhenAlikeInEveryMember) {
  // Each member alone tells two statuses apart: one left out of operator==
  // would let a status that says something else compare equal.
  LicenseStatus base = status(LicenseState::Trial, LicenseReason::Trial, KEY);
  base.trialDaysLeft = 3;
  base.trialEndsAt = NOW + 3 * DAY;
  base.entitlements = {{"edition", "pro"}};
  struct Case {
    const char* member;
    std::function<void(LicenseStatus&)> change;
  };
  const std::vector<Case> cases = {
      {"state", [](LicenseStatus& s) { s.state = LicenseState::Expired; }},
      {"reason", [](LicenseStatus& s) { s.reason = LicenseReason::Offline; }},
      {"key", [](LicenseStatus& s) { s.key = "SLUICE-ZZZZ-ZZZZ-ZZZZ-ZZZZ"; }},
      {"trialDaysLeft", [](LicenseStatus& s) { s.trialDaysLeft = 4; }},
      {"trialEndsAt", [](LicenseStatus& s) { s.trialEndsAt = NOW + 4 * DAY; }},
      {"test", [](LicenseStatus& s) { s.test = true; }},
      {"entitlements", [](LicenseStatus& s) { s.entitlements = {{"edition", "studio"}}; }},
      {"stalled", [](LicenseStatus& s) { s.stalled = true; }},
  };
  EXPECT_EQ(base, LicenseStatus(base));
  for (const Case& c : cases) {
    LicenseStatus other = base;
    c.change(other);
    EXPECT_NE(base, other) << c.member;
  }
}

}  // namespace
}  // namespace kgtest
