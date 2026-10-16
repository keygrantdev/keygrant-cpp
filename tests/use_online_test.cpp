// use(meter, n) while the server answers: exact, sending the key, the seat, the meter, the amount and a fresh id; what each
// word of its answer does; what is asked of it and what is not; and every licence that has no meter to spend.
#include <chrono>
#include <regex>
#include <set>

#include "license_file_support.hpp"
#include "meter_support.hpp"

namespace kgtest {
namespace {

using keygrant::HttpResponse;
using keygrant::Refusal;
using keygrant::kMaxMeterUses;

const std::regex kIdPattern("^[A-Za-z0-9_-]{22}$");

TEST(UseOnline, SpendsExactlySendingTheKeyTheSeatTheMeterTheAmountAndAFreshId) {
  MeterHarness h;
  h.answerSpent(89, NOW + 1);
  EXPECT_EQ(h.use("exports"), UseResult::spent(89));
  const json body = h.useBody();
  EXPECT_EQ(body["key"], KEY);
  EXPECT_EQ(body["activationId"], "act_1");
  EXPECT_EQ(body["meter"], "exports");
  EXPECT_EQ(body["n"], 1);
  EXPECT_EQ(body["major"], 1);
  EXPECT_TRUE(std::regex_match(body["id"].get<std::string>(), kIdPattern));
  EXPECT_EQ(body["kids"], h.key.kids());
  EXPECT_EQ(body["fingerprints"], json::array({"fp-test"}));
  EXPECT_FALSE(body.contains("spends"));
  EXPECT_FALSE(body.contains("keptMajor"));
  // The figures it was given are stored, stamped with the server's time; nothing is journaled.
  ASSERT_TRUE(h.stored().has_value());
  EXPECT_EQ(figuresJson(h.stored()->meterFigures), json({{"exports", {{"left", 89}, {"total", 100}, {"offline", 10},
                                                                      {"resets", false}, {"at", NOW + 1}}}}));
  EXPECT_TRUE(h.journal().empty());
}

TEST(UseOnline, PostsToTheProductsUseEndpoint) {
  MeterHarness h;
  h.answerSpent(89);
  h.use("exports");
  EXPECT_EQ(h.http->urlOf("use"), std::string("https://api.test/v1/products/") + PRODUCT + "/use");
}

TEST(UseOnline, SpendsABatchWholeInOneRequest) {
  MeterHarness h;
  h.answerSpent(50);
  EXPECT_EQ(h.use("exports", 40), UseResult::spent(50));
  EXPECT_EQ(h.useBody()["n"], 40);
  EXPECT_EQ(h.http->count("use"), 1);
}

TEST(UseOnline, MakesIdsOf22Base64UrlCharactersThatAreNeverTheSame) {
  MeterHarness h;
  h.answerSpent(89);
  std::set<std::string> ids;
  for (int i = 0; i < 200; ++i) {
    h.use("exports");
    ids.insert(h.useBody(static_cast<std::size_t>(i))["id"].get<std::string>());
  }
  EXPECT_EQ(ids.size(), 200u);
  for (const std::string& id : ids) EXPECT_TRUE(std::regex_match(id, kIdPattern)) << id;
}

TEST(UseOnline, SpendsOneUseWhenNoAmountIsGiven) {
  MeterHarness h;
  h.answerSpent(89);
  EXPECT_EQ(h.license().use("exports"), UseResult::spent(89));
  EXPECT_EQ(h.useBody()["n"], 1);
}

TEST(UseOnline, AnswersFromTheFigureTheServerGaveNetOfWhatThisDeviceHasNotReported) {
  MeterHarness h(defaultMeters(), {entry(SpendKind::Offline, "A", 3, NOW)});
  h.answerSpent(80);
  // The server's 80 does not count the 3 unreported, so 77 is what the customer has.
  EXPECT_EQ(h.use("exports"), UseResult::spent(77));
}

TEST(UseOnline, AnswersFromTheBaseFiguresAsTheyWereWhenTheServerGaveNoneForTheMeter) {
  MeterHarness h;
  h.answerUse(200, {{"spent", true}, {"meters", json::object()}, {"at", NOW}});
  EXPECT_EQ(h.use("exports"), UseResult::spent(90));
  h.answerUse(200, {{"spent", true}});
  EXPECT_EQ(h.use("exports"), UseResult::spent(90));
}

TEST(UseOnline, AnswersNothingLeftForAMeterNeitherTheLeaseNorTheAnswerNames) {
  MeterHarness h;
  h.answerUse(200, {{"spent", true}});
  EXPECT_EQ(h.use("credits"), UseResult::spent(0));
}

TEST(UseOnline, KeepsAFigureTheLeaseIsNewerThanOnlyUntilTheLeaseIsRenewed) {
  MeterHarness h;
  h.answerSpent(70, NOW + 5'000);
  h.use("exports");
  EXPECT_EQ(h.meters().at("exports").left, 70);
  // A renewal brings a lease signed after that figure, whose own figures win.
  const std::string renewed = meterLease(h, {{"exports", MeterSpec{60, 100, 10, false}}}, NOW + 10'000);
  h.http->reply("validate", 200, {{"lease", renewed}});
  h.setClock(NOW + 10'000);
  EXPECT_EQ(h.license().refresh().meters.at("exports").left, 60);
  EXPECT_EQ(h.license().status().meters.at("exports").left, 60);
}

TEST(UseOnline, AnswersUsedUpWithWhatTheServerSaysIsLeftSavesTheFiguresAndTheReportsItConfirmsAndSpendsNothingOfflineAfter) {
  MeterHarness h(defaultMeters(), {entry(SpendKind::Unanswered, "B", 2, NOW - 2 * HOUR)});
  h.answerUse(409, {{"error", "used-up"},
                    {"left", 1},
                    {"meters", {{"exports", {{"left", 1}, {"total", 100}, {"offline", 10}}}}},
                    {"spends", json::array({{{"id", spendId("B")}, {"n", 2}}})},
                    {"at", NOW}});
  EXPECT_EQ(h.use("exports", 5), UseResult::refused(UseError::UsedUp, 1));
  EXPECT_TRUE(h.journal().empty());
  EXPECT_EQ(h.stored()->meterFigures.at("exports").left, 1);
  EXPECT_EQ(h.stored()->meterFigures.at("exports").at, NOW);
  // The server's word stands offline too: one is left, and a spend of two is past it.
  h.http->fail = true;
  h.setClock(NOW + 2 * MINUTE);
  EXPECT_EQ(h.use("exports", 2), UseResult::refused(UseError::OfflineLimit, 1));
}

TEST(UseOnline, AnswersUsedUpWithNoLeftWhenTheAnswerGaveNone) {
  MeterHarness h;
  h.answerUse(409, {{"error", "used-up"}});
  EXPECT_EQ(h.use("exports"), UseResult::refused(UseError::UsedUp));
  // A left that is no figure is none as well: a fraction, a negative, a text.
  for (const json& left : {json(1.5), json(-1), json("3"), json(nullptr), json(9007199254740993.0)}) {
    h.answerUse(409, {{"error", "used-up"}, {"left", left}});
    EXPECT_EQ(h.use("exports"), UseResult::refused(UseError::UsedUp)) << left;
  }
}

TEST(UseOnline, ReadsTheServersWordsOnAnyStatusButA200A5xxAnd429) {
  MeterHarness h;
  h.answerUse(400, {{"error", "used-up"}, {"left", 0}});
  EXPECT_EQ(h.use("exports"), UseResult::refused(UseError::UsedUp, 0));
  // A 200 is a spend only with spent: true; anything else is no answer.
  h.answerUse(200, {{"error", "used-up"}, {"left", 0}});
  const UseResult none = h.use("exports");
  EXPECT_TRUE(none.ok);
  EXPECT_TRUE(none.offline);
}

TEST(UseOnline, AnswersNoMeterForAMeterTheServerDoesNotKnowAndConfirmsTheReportsItCarried) {
  MeterHarness h(defaultMeters(), {entry(SpendKind::Unanswered, "C", 1, NOW - 2 * HOUR)});
  h.answerUse(404, {{"error", "unknown-meter"}, {"spends", json::array({{{"id", spendId("C")}, {"n", 1}}})}});
  EXPECT_EQ(h.use("exports"), UseResult::refused(UseError::NoMeter));
  EXPECT_TRUE(h.journal().empty());
}

TEST(UseOnline, StoresAVerdictAsAValidatesAndAnswersItAsStatusWillSayIt) {
  struct Case {
    const char* word;
    Refusal refusal;
    UseError error;
    keygrant::LicenseReason reason;
  };
  const Case cases[] = {
      {"revoked", Refusal::Revoked, UseError::Revoked, keygrant::LicenseReason::Revoked},
      {"expired", Refusal::Revoked, UseError::Revoked, keygrant::LicenseReason::Revoked},
      {"deactivated", Refusal::Revoked, UseError::Revoked, keygrant::LicenseReason::Revoked},
      {"upgrade-required", Refusal::Upgrade, UseError::Upgrade, keygrant::LicenseReason::Upgrade},
      {"update-required", Refusal::Outdated, UseError::Outdated, keygrant::LicenseReason::Outdated},
      {"device", Refusal::Device, UseError::Device, keygrant::LicenseReason::Device},
  };
  for (const Case& c : cases) {
    SCOPED_TRACE(c.word);
    MeterHarness h;
    h.answerUse(403, {{"error", c.word}});
    EXPECT_EQ(h.use("exports"), UseResult::refused(c.error));
    EXPECT_EQ(h.stored()->refused, c.refusal);
    EXPECT_EQ(h.license().status(), status(LicenseState::Invalid, c.reason, KEY));
    // The refusal stands: the next spend is not licensed, and asks nothing.
    EXPECT_EQ(h.use("exports"), UseResult::refused(UseError::NotLicensed));
    EXPECT_EQ(h.http->count("use"), 1);
  }
}

TEST(UseOnline, StoresAVerdictsWaitAndTheMajorAsAValidateDoes) {
  MeterHarness h;
  h.answerUse(403, {{"error", "revoked"}});
  h.use("exports");
  const StoredState state = *h.stored();
  EXPECT_EQ(state.checkedMajor, 1);
  EXPECT_EQ(state.backoffSince, NOW);
  EXPECT_EQ(state.backoffMs, keygrant::detail::kAskAgainMs);
  EXPECT_EQ(state.verdictAt, NOW);
  EXPECT_FALSE(state.lease.has_value());
}

TEST(UseOnline, RecordsTheReportsAVerdictConfirms) {
  MeterHarness h(defaultMeters(), {entry(SpendKind::Void, "D", 1, NOW - 2 * HOUR)});
  h.answerUse(403, {{"error", "revoked"}, {"spends", json::array({{{"id", spendId("D")}, {"void", true}}})}});
  h.use("exports");
  EXPECT_TRUE(h.journal().empty());
}

TEST(UseOnline, AUsedUpMeterRefusesSpendsNeverTheLicence) {
  MeterHarness h;
  h.grantEdition();
  h.answerUse(409, {{"error", "used-up"}, {"left", 0},
                    {"meters", {{"exports", {{"left", 0}, {"total", 100}, {"offline", 10}}}}}, {"at", NOW}});
  EXPECT_EQ(h.use("exports"), UseResult::refused(UseError::UsedUp, 0));
  // Every way the app reads the licence says it is licensed, the meter used up.
  const LicenseStatus status = h.license().status();
  EXPECT_EQ(status.state, LicenseState::Licensed);
  EXPECT_EQ(status.meters.at("exports").left, 0);
  EXPECT_EQ(h.license().refresh().state, LicenseState::Licensed);
  // What the lease grants is granted still.
  EXPECT_EQ(status.entitlements.at("edition"), keygrant::Entitlement("pro"));
  EXPECT_TRUE(h.license().has("edition"));
  EXPECT_NO_THROW(h.license().require("edition"));
  EXPECT_EQ(h.license().status().state, LicenseState::Licensed);
}

// --- the bound of a use request ------------------------------------------------------------------

TEST(UseOnline, AsksTheAdapterToWaitAtMostFiveSecondsAndTheHttpBoundWhenThatIsShorter) {
  MeterHarness h;
  h.answerSpent(89);
  h.use("exports");
  EXPECT_EQ(h.http->timeouts().back(), Millis{5'000});

  HarnessOptions quick;
  quick.httpTimeout = Millis{1'000};
  MeterHarness shorter(defaultMeters(), {}, quick);
  shorter.answerSpent(89);
  shorter.use("exports");
  EXPECT_EQ(shorter.http->timeouts().back(), Millis{1'000});

  HarnessOptions slow;
  slow.httpTimeout = Millis{60'000};
  MeterHarness longer(defaultMeters(), {}, slow);
  longer.answerSpent(89);
  longer.use("exports");
  EXPECT_EQ(longer.http->timeouts().back(), Millis{5'000});
}

TEST(UseOnline, WaitsForTheUseBoundForTheServerThenSpendsFromTheAllowanceWhateverTheAdapterDoes) {
  HarnessOptions options;
  options.timings = shortTimings();
  MeterHarness h(defaultMeters(), {}, options);
  h.http->hang = true;
  UseResult result;
  const Millis took = timed([&] { result = h.use("exports"); });
  EXPECT_EQ(result, UseResult::spent(89, true));
  // The status check's bound (150 ms here), kept by the SDK itself whatever the adapter does with its timeout.
  EXPECT_GE(took, Millis{140});
  EXPECT_LT(took, Millis{4'000});
}

TEST(UseOnline, WaitsForTheHttpBoundInsteadWhenThatIsShorter) {
  HarnessOptions options;
  options.httpTimeout = Millis{120};
  MeterHarness h(defaultMeters(), {}, options);
  h.http->hang = true;
  UseResult result;
  const Millis took = timed([&] { result = h.use("exports"); });
  EXPECT_TRUE(result.ok);
  EXPECT_TRUE(result.offline);
  EXPECT_GE(took, Millis{110});
  EXPECT_LT(took, Millis{4'000});
}

// --- what is asked ----------------------------------------------------------------------------------------

TEST(UseOnline, RefusesAnAmountThatIsNotAWholeNumberFrom1ToTheBoundAskingNothing) {
  MeterHarness h;
  h.answerSpent(89);
  for (const std::int64_t n : {std::int64_t{0}, std::int64_t{-1}, kMaxMeterUses + 1, std::int64_t{INT64_MIN}, std::int64_t{INT64_MAX}}) {
    EXPECT_EQ(h.use("exports", n), UseResult::refused(UseError::InvalidAmount)) << n;
  }
  EXPECT_EQ(h.http->count("use"), 0);
  EXPECT_EQ(h.storage->writes(), 0);
}

TEST(UseOnline, TakesTheBoundItselfAndOneWhenNoAmountIsGiven) {
  MeterHarness h({{"exports", MeterSpec{kMaxMeterUses, kMaxMeterUses, 10, false}}});
  h.answerSpent(5);
  EXPECT_TRUE(h.use("exports", kMaxMeterUses).ok);
  EXPECT_EQ(h.useBody()["n"], kMaxMeterUses);
  h.license().use("exports");
  EXPECT_EQ(h.useBody(1)["n"], 1);
}

TEST(UseOnline, JudgesTheAmountBeforeTheMeterAndRefusesACodeOfNoMeterAskingNothing) {
  MeterHarness h;
  EXPECT_EQ(h.use("", 0), UseResult::refused(UseError::InvalidAmount));
  for (const std::string& code : {std::string(), std::string(33, 'x'), std::string(31, 'x') + "\xF0\x9F\x98\x80"}) {
    EXPECT_EQ(h.use(code), UseResult::refused(UseError::NoMeter)) << code;
  }
  EXPECT_EQ(h.http->count("use"), 0);
}

TEST(UseOnline, TakesTheLongestCodeOfThreeByteCharactersAndRefusesOneTooLongToBeOneBeforeReadingIt) {
  std::string euros;
  for (int i = 0; i < 32; ++i) euros += "\xE2\x82\xAC";  // 32 UTF-16 units in 96 bytes
  MeterHarness h({{euros, MeterSpec{5, 5, 1, false}}});
  h.http->fail = true;
  EXPECT_EQ(h.use(euros), UseResult::spent(4, true));
  EXPECT_EQ(h.http->count("use"), 1);
  // One unit more, and a code of megabytes of bytes that are no text: refused with nothing asked.
  EXPECT_EQ(h.use(euros + "\xE2\x82\xAC"), UseResult::refused(UseError::NoMeter));
  EXPECT_EQ(h.use(std::string(3 * 1024 * 1024, static_cast<char>(0xE2))), UseResult::refused(UseError::NoMeter));
  EXPECT_EQ(h.http->count("use"), 1);
}

TEST(UseOnline, SendsACodeWithAByteThatIsNoTextAsTheReplacementCharacterAndAnswersNoMeterForIt) {
  MeterHarness h;
  h.http->fail = true;
  // One unit for the bad byte, as every bad sequence is one U+FFFD to the server, the hashes and the file alike.
  EXPECT_EQ(h.use("caf\xE9"), UseResult::refused(UseError::NoMeter));
  EXPECT_EQ(h.useBody()["meter"], "caf\xEF\xBF\xBD");
}

TEST(UseOnline, TakesACodeOf32Utf16UnitsASurrogatePairAtTheEdgeIncludedAndMatchesItExactly) {
  const std::string edge = std::string(30, 'x') + "\xF0\x9F\x98\x80";
  MeterHarness h({{edge, MeterSpec{5, 5, 1, false}}, {std::string(32, 'x'), MeterSpec{5, 5, 1, false}}});
  h.http->fail = true;
  EXPECT_EQ(h.use(edge), UseResult::spent(4, true));
  EXPECT_EQ(h.use(std::string(32, 'x')), UseResult::spent(4, true));
  EXPECT_EQ(h.use(std::string(32, 'X')), UseResult::refused(UseError::NoMeter));
}

TEST(UseOnline, SpendsAMeterNamedLikeAnObjectsMemberOnlyWhenTheKeyHasIt) {
  MeterHarness h;
  h.http->fail = true;
  for (const char* code : {"constructor", "__proto__"}) {
    EXPECT_EQ(h.use(code), UseResult::refused(UseError::NoMeter)) << code;
  }
  MeterHarness own({{"constructor", MeterSpec{3, 3, 1, false}}});
  own.http->fail = true;
  EXPECT_EQ(own.use("constructor"), UseResult::spent(2, true));
}

// --- when it is not licensed ---------------------------------------------------------------------------------

TEST(UseOnline, IsNotLicensedWithNoLicenceATrialAnExpiredOrTamperedLeaseAskingNothing) {
  Harness none;
  EXPECT_EQ(none.license().use("exports"), UseResult::refused(UseError::NotLicensed));

  Harness trial;
  trial.http->reply("trial", 200,
                    {{"trialStart", NOW}, {"trialEndsAt", NOW + DAY}, {"lease", trial.sign("trial:fp-test", "trial", 86'400)}});
  EXPECT_EQ(trial.license().startTrial().state, LicenseState::Trial);
  EXPECT_EQ(trial.license().use("exports"), UseResult::refused(UseError::NotLicensed));

  MeterHarness expired;
  expired.setClock(NOW + 40 * DAY);
  EXPECT_EQ(expired.use("exports"), UseResult::refused(UseError::NotLicensed));

  MeterHarness other;
  StoredState swapped = *other.stored();
  swapped.key = "SLUICE-ZZZZ-ZZZZ-ZZZZ-ZZZZ";
  other.store(swapped);
  EXPECT_EQ(other.use("exports"), UseResult::refused(UseError::NotLicensed));

  for (Harness* h : {&none, &trial, static_cast<Harness*>(&expired), static_cast<Harness*>(&other)}) {
    EXPECT_EQ(h->http->count("use"), 0);
  }
}

TEST(UseOnline, IsNotLicensedWithAStoredRefusalThatStandsAndForABuildTheLeaseDoesNotCover) {
  MeterHarness refused;
  StoredState revoked = *refused.stored();
  revoked.refused = Refusal::Revoked;
  refused.store(revoked);
  EXPECT_EQ(refused.use("exports"), UseResult::refused(UseError::NotLicensed));

  MeterHarness upgraded;
  StoredState upgrade = *upgraded.stored();
  upgrade.refused = Refusal::Upgrade;
  upgrade.refusedMajor = 1;
  upgraded.store(upgrade);
  EXPECT_EQ(upgraded.use("exports"), UseResult::refused(UseError::NotLicensed));

  // A build newer than the lease covers.
  HarnessOptions newer;
  newer.major = 4;
  MeterHarness build(defaultMeters(), {}, newer);
  EXPECT_EQ(build.use("exports"), UseResult::refused(UseError::NotLicensed));
  EXPECT_EQ(refused.http->count("use") + upgraded.http->count("use") + build.http->count("use"), 0);
}

TEST(UseOnline, IsNotLicensedWithAnOfflineLicenceFileWhichHasNoMeters) {
  Harness h;
  EXPECT_TRUE(h.license().importLicenseFile(fileText(offlineFile(h.key))).ok);
  EXPECT_EQ(h.license().status().source, keygrant::LicenseSource::File);
  EXPECT_EQ(h.license().use("exports"), UseResult::refused(UseError::NotLicensed));
  EXPECT_EQ(h.http->count("use"), 0);
}

TEST(UseOnline, AnswersStorageWhenTheStateCannotBeReadJustNowAskingNothing) {
  MeterHarness h;
  h.storage->failNextRead = true;
  EXPECT_EQ(h.use("exports"), UseResult::refused(UseError::Storage));
  EXPECT_EQ(h.http->count("use"), 0);
  h.answerSpent(89);
  EXPECT_EQ(h.use("exports"), UseResult::spent(89));
}

TEST(UseOnline, NeverThrowsWhenAnAdapterDoesAClockAFingerprintOrARequest) {
  MeterHarness clock;
  clock.clock->failing = true;
  EXPECT_EQ(clock.use("exports"), UseResult::refused(UseError::Storage));
  // A request that throws, even before it is sent, is no answer: the allowance serves.
  MeterHarness post;
  post.http->fail = true;
  EXPECT_EQ(post.use("exports"), UseResult::spent(89, true));
  // A device that cannot be told is asked about all the same, and sends no fingerprints.
  auto failing = std::make_shared<FakeFingerprinter>();
  failing->failing = true;
  HarnessOptions options;
  options.fingerprint = failing;
  MeterHarness fingerprint(defaultMeters(), {}, options);
  fingerprint.answerSpent(89);
  EXPECT_EQ(fingerprint.use("exports"), UseResult::spent(89));
}

// --- the device --------------------------------------------------------------------------------------------------

TEST(UseOnline, NamesTheDeviceAsAValidateDoesItsFingerprintsAndWhatTheyHash) {
  auto machine = std::make_shared<FakeFingerprinter>("fp-machine");
  machine->identifyAs(DeviceKind::Machine, {"fp-host"});
  HarnessOptions options;
  options.fingerprint = machine;
  MeterHarness h(defaultMeters(), {}, options);
  h.answerSpent(89);
  h.use("exports");
  const json body = h.useBody();
  EXPECT_EQ(body["fingerprintKind"], "machine");
  // The very ones a validate of the same device sends.
  h.http->reply("validate", 200, {{"lease", meterLease(h)}});
  h.license().refresh();
  EXPECT_EQ(body["fingerprints"].size(), 2u);
  EXPECT_EQ(body["fingerprints"], h.http->bodyOf("validate")["fingerprints"]);
  EXPECT_EQ(body["fingerprintKind"], h.http->bodyOf("validate")["fingerprintKind"]);
}

TEST(UseOnline, SendsNoFingerprintsWhenDevicesAreNotHeldOrTheDeviceCannotBeTold) {
  HarnessOptions pooled;
  pooled.deviceHold = false;
  MeterHarness a(defaultMeters(), {}, pooled);
  a.answerSpent(89);
  a.use("exports");
  EXPECT_FALSE(a.useBody().contains("fingerprints"));
  EXPECT_FALSE(a.useBody().contains("fingerprintKind"));

  auto unknown = std::make_shared<FakeFingerprinter>();
  unknown->failing = true;
  HarnessOptions blind;
  blind.fingerprint = unknown;
  MeterHarness b(defaultMeters(), {}, blind);
  b.answerSpent(89);
  b.use("exports");
  ASSERT_EQ(b.http->count("use"), 1);
  EXPECT_FALSE(b.useBody().contains("fingerprints"));
}

}  // namespace
}  // namespace kgtest
