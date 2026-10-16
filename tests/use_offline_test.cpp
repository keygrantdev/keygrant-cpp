// use(meter, n) while the server gives no answer: the offline allowance, the journal it fills, the waits that keep a
// device out of reach from asking at every call, and everything the journal does to the stored state.
#include <algorithm>

#include "meter_support.hpp"

namespace kgtest {
namespace {

using keygrant::HttpResponse;
using keygrant::detail::kAskAgainMs;
using keygrant::detail::kMaxPendingSpends;
using keygrant::detail::kRetryUnansweredMs;
using keygrant::kMaxMeterUses;

TEST(UseOffline, SpendsFromTheAllowanceJournalsTheSpendUnderTheIdItSentAndWaitsAMinute) {
  MeterHarness h;
  h.http->fail = true;
  EXPECT_EQ(h.use("exports"), UseResult::spent(89, true));
  const std::string sent = h.useBody()["id"].get<std::string>();
  EXPECT_EQ(h.journal(), std::vector<SpendEntry>({SpendEntry{sent, "exports", 1, NOW, SpendKind::Unanswered}}));
  EXPECT_EQ(h.stored()->backoffSince, NOW);
  EXPECT_EQ(h.stored()->backoffMs, kRetryUnansweredMs);
  EXPECT_EQ(h.stored()->deviceKind, std::nullopt);
  // Inside the wait it asks nothing: the spend is made with no request, and grows one entry.
  EXPECT_EQ(h.use("exports", 2), UseResult::spent(87, true));
  EXPECT_EQ(h.http->count("use"), 1);
  EXPECT_EQ(h.shape(), "unanswered:1 offline:2");
  EXPECT_EQ(h.use("exports"), UseResult::spent(86, true));
  EXPECT_EQ(h.shape(), "unanswered:1 offline:3");
}

TEST(UseOffline, RefusesASpendPastTheAllowanceAndPastWhatIsLeft) {
  MeterHarness h(exportsOnly(90, 100, 3));
  h.http->fail = true;
  EXPECT_EQ(h.use("exports", 2), UseResult::spent(88, true));
  EXPECT_EQ(h.use("exports", 2), UseResult::refused(UseError::OfflineLimit, 88));
  EXPECT_EQ(h.use("exports", 1), UseResult::spent(87, true));
  EXPECT_EQ(h.use("exports", 1), UseResult::refused(UseError::OfflineLimit, 87));

  MeterHarness low(exportsOnly(2, 100, 50));
  low.http->fail = true;
  EXPECT_EQ(low.use("exports", 3), UseResult::refused(UseError::OfflineLimit, 2));
  EXPECT_EQ(low.use("exports", 2), UseResult::spent(0, true));
  EXPECT_EQ(low.use("exports"), UseResult::refused(UseError::OfflineLimit, 0));
}

TEST(UseOffline, RefusesEverySpendWithNoAnswerOnAMeterWhoseAllowanceIs0) {
  MeterHarness h;
  h.http->fail = true;
  EXPECT_EQ(h.use("renders"), UseResult::refused(UseError::OfflineLimit, 5));
  // Online it spends as any other.
  h.http->fail = false;
  h.setClock(NOW + 2 * MINUTE);
  h.answerUse(200, {{"spent", true},
                    {"meters", {{"renders", {{"left", 4}, {"total", 5}, {"offline", 0}, {"resets", true}}}}},
                    {"at", NOW + 2 * MINUTE}});
  EXPECT_EQ(h.use("renders"), UseResult::spent(4));
}

TEST(UseOffline, JournalsARefusalOfASpendThatWasSentAsAVoidForTheServerToTakeBackIfItRecordedIt) {
  MeterHarness h(exportsOnly(90, 100, 1));
  h.http->fail = true;
  h.use("exports");
  h.setClock(NOW + 2 * MINUTE);
  EXPECT_EQ(h.use("exports"), UseResult::refused(UseError::OfflineLimit, 89));
  const std::vector<SpendEntry> journal = h.journal();
  ASSERT_EQ(journal.size(), 2u);
  EXPECT_EQ(journal[0].kind, SpendKind::Unanswered);
  EXPECT_EQ(journal[0].n, 1);
  EXPECT_EQ(journal[1], (SpendEntry{h.useBody(1)["id"].get<std::string>(), "exports", 1, NOW + 2 * MINUTE, SpendKind::Void}));
}

TEST(UseOffline, AsksAgainForASpendTheAllowanceRefusesThoughTheMinuteHasNotPassed) {
  MeterHarness h(exportsOnly(90, 100, 1));
  h.http->fail = true;
  h.use("exports");
  // Inside the wait, but the allowance cannot cover it: asked all the same, so an online customer is not refused.
  EXPECT_EQ(h.use("exports"), UseResult::refused(UseError::OfflineLimit, 89));
  EXPECT_EQ(h.http->count("use"), 2);
  EXPECT_EQ(h.shape(), "unanswered:1 void:1");
}

TEST(UseOffline, ServesACustomerWhoIsBackOnlineAtOnceThoughTheLastOutageLeftAWait) {
  MeterHarness h;
  h.http->fail = true;
  // The outage: a spend the allowance covers is made with no answer, and the minute's wait begins.
  EXPECT_TRUE(h.use("exports").offline);
  h.http->fail = false;
  h.answerUse(200, {{"spent", true},
                    {"meters", {{"renders", {{"left", 4}, {"total", 5}, {"offline", 0}, {"resets", true}}}}},
                    {"at", NOW}});
  EXPECT_EQ(h.use("renders"), UseResult::spent(4));
  h.answerSpent(70);
  // The server's 70 less the one spend not yet reported.
  EXPECT_EQ(h.use("exports", 50), UseResult::spent(69));
  h.answerUse(200, {{"spent", true}, {"meters", {{"credits", {{"left", 9}, {"total", 10}, {"offline", 0}}}}}, {"at", NOW}});
  EXPECT_EQ(h.use("credits"), UseResult::spent(9));
  // A spend the allowance covers still waits out the minute.
  const int asked = h.http->count("use");
  EXPECT_TRUE(h.use("exports").offline);
  EXPECT_EQ(h.http->count("use"), asked);
}

TEST(UseOffline, CountsA5xxAndA429AsNoAnswerSavesNothingOfThemAndJournalsTheSpendInDoubt) {
  struct Case {
    int status;
    json body;
  };
  for (const Case& c : {Case{500, {{"error", "boom"}}}, Case{503, nullptr}, Case{429, {{"error", "rate-limited"}}}}) {
    SCOPED_TRACE(c.status);
    MeterHarness h;
    h.answerUse(c.status, c.body);
    EXPECT_EQ(h.use("exports"), UseResult::spent(89, true));
    EXPECT_EQ(h.journal().at(0).kind, SpendKind::Unanswered);
    EXPECT_EQ(h.journal().at(0).n, 1);
    EXPECT_EQ(h.stored()->backoffSince, std::nullopt);
  }
}

TEST(UseOffline, CountsAnyAnswerItCannotUseAsNoAnswerTooAndSavesNothingOfIt) {
  struct Case {
    int status;
    json body;
  };
  const std::vector<Case> cases = {{200, {{"spent", false}}}, {200, "ok"},  {200, nullptr},
                                   {400, {{"error", "invalid-request"}}},   {404, {{"error", "invalid-key"}}},
                                   {403, {{"error", "something-new"}}},     {401, "denied"},
                                   {200, json::array({1, 2})},              {200, {{"spent", "true"}}}};
  for (const Case& c : cases) {
    SCOPED_TRACE(std::to_string(c.status) + " " + c.body.dump());
    MeterHarness h;
    h.answerUse(c.status, c.body);
    EXPECT_EQ(h.use("exports"), UseResult::spent(89, true));
    EXPECT_EQ(h.journal().at(0).kind, SpendKind::Unanswered);
    EXPECT_EQ(h.stored()->backoffSince, std::nullopt);
    EXPECT_EQ(h.stored()->refused, std::nullopt);
    EXPECT_EQ(h.stored()->checkedMajor, 1);  // as it was stored: nothing of the answer is
  }
}

TEST(UseOffline, DoesNotTakeTheConfirmationsOfAnAnswerItCannotUse) {
  MeterHarness h(defaultMeters(), {entry(SpendKind::Unanswered, "E", 1, NOW)});
  h.answerUse(500, {{"spends", json::array({{{"id", spendId("E")}, {"n", 1}}})}});
  h.setClock(NOW + 2 * MINUTE);
  h.use("exports");
  EXPECT_EQ(h.http->count("use"), 1);
  const std::vector<SpendEntry> journal = h.journal();
  EXPECT_TRUE(std::any_of(journal.begin(), journal.end(), [](const SpendEntry& e) { return e.id == spendId("E"); }));
}

TEST(UseOffline, AsksAgainOnceTheMinuteIsOverAndKeepsTheJournalsOwnWaitDoublingToAnHour) {
  MeterHarness h(exportsOnly(1'000'000, 1'000'000, 1'000'000));
  h.http->fail = true;
  std::int64_t at = NOW;
  const std::int64_t waits[] = {1, 2, 4, 8, 16, 32, 60, 60};
  int ask = 0;
  for (const std::int64_t wait : waits) {
    ++ask;
    h.setClock(at);
    h.use("exports");
    EXPECT_EQ(h.http->count("use"), ask) << "ask " << ask;
    // One millisecond short of the wait: not asked.
    h.setClock(at + wait * MINUTE - 1);
    h.use("exports");
    EXPECT_EQ(h.http->count("use"), ask) << "short of wait " << ask;
    at += wait * MINUTE;
  }
}

TEST(UseOffline, KeepsTheJournalsWaitWhenTheDeviceClockIsSetBackAsTheGuardedClockDoesNotGoBack) {
  MeterHarness h;
  h.http->fail = true;
  h.setClock(NOW + 10 * MINUTE);
  h.use("exports");
  h.setClock(NOW);
  h.use("exports");
  EXPECT_EQ(h.http->count("use"), 1);
}

TEST(UseOffline, AsksAtOnceWhenAStoredNoAnswerWaitBeginsAfterTheDeviceClock) {
  MeterHarness fresh;
  fresh.store(withBackoff(*fresh.stored(), NOW + 10 * MINUTE));
  fresh.answerSpent(89);
  EXPECT_EQ(fresh.use("exports"), UseResult::spent(89));
}

TEST(UseOffline, AsksWhenTheNewestUnansweredSpendWasOpenedAfterTheGuardedClockNotDaysLater) {
  MeterHarness ahead(defaultMeters(), {entry(SpendKind::Unanswered, "F", 1, NOW + 5 * DAY)});
  ahead.answerSpent(89);
  EXPECT_EQ(ahead.use("exports"), UseResult::spent(88));
  EXPECT_EQ(ahead.http->count("use"), 1);
  MeterHarness now(defaultMeters(), {entry(SpendKind::Unanswered, "G", 1, NOW)});
  now.answerSpent(89);
  now.use("exports");
  EXPECT_EQ(now.http->count("use"), 0);
}

TEST(UseOffline, DoesNotWaitOnALongerWaitAValidateSetTheUseEndpointIsAskedAllTheSame) {
  MeterHarness h;
  h.store(withBackoff(*h.stored(), NOW, kAskAgainMs));
  h.answerSpent(89);
  EXPECT_EQ(h.use("exports"), UseResult::spent(89));
}

TEST(UseOffline, RefusesAMeterTheKeyHasNoFigureForAsNoMeterAndJournalsItAsAVoidWhenItWasInDoubt) {
  MeterHarness h;
  h.http->fail = true;
  EXPECT_EQ(h.use("credits"), UseResult::refused(UseError::NoMeter));
  ASSERT_EQ(h.journal().size(), 1u);
  EXPECT_EQ(h.journal()[0].kind, SpendKind::Void);
  EXPECT_EQ(h.journal()[0].meter, "credits");
  EXPECT_EQ(h.journal()[0].n, 1);

  // A wait running does not stop the ask: the server may know a meter the lease does not name.
  MeterHarness w;
  w.store(withBackoff(*w.stored()));
  w.http->fail = true;
  EXPECT_EQ(w.use("credits"), UseResult::refused(UseError::NoMeter));
  EXPECT_EQ(w.http->count("use"), 1);
  EXPECT_EQ(w.journal().at(0).kind, SpendKind::Void);
}

TEST(UseOffline, FillsTheJournalTo64AndRefusesThe65thSpendAsPastTheAllowance) {
  std::vector<SpendEntry> full;
  for (std::size_t i = 0; i < kMaxPendingSpends; ++i) {
    full.push_back(SpendEntry{spendId("x" + std::to_string(i)), "m" + std::to_string(i), 1, NOW - 1, SpendKind::Unanswered});
  }
  MeterHarness h(exportsOnly(90, 100, 10), full);
  h.store(withBackoff(*h.stored()));
  // A new entry has no room: the spend is refused, though the allowance had it.
  EXPECT_EQ(h.use("exports"), UseResult::refused(UseError::OfflineLimit, 90));
  EXPECT_EQ(h.journal().size(), kMaxPendingSpends);
  // And nothing was sent for it: the covered spend is made offline, never asked, so it has no void to journal.
  EXPECT_EQ(h.http->count("use"), 0);
}

TEST(UseOffline, LetsAnOfflineEntryThatIsAlreadyOpenGrowWhenTheJournalIsFull) {
  std::vector<SpendEntry> full = {entry(SpendKind::Offline, "x", 2, NOW)};
  for (std::size_t i = 0; i + 1 < kMaxPendingSpends; ++i) {
    full.push_back(SpendEntry{spendId("y" + std::to_string(i)), "m" + std::to_string(i), 1, NOW - 1, SpendKind::Unanswered});
  }
  MeterHarness h(exportsOnly(90, 100, 10), full);
  h.store(withBackoff(*h.stored()));
  EXPECT_EQ(h.use("exports"), UseResult::spent(87, true));
  EXPECT_EQ(h.journal().size(), kMaxPendingSpends);
  EXPECT_EQ(h.journal()[0].n, 3);
}

TEST(UseOffline, WaitsTheHourAfterAJournalOf64UnansweredEntriesWithNothingOverflowing) {
  std::vector<SpendEntry> unanswered;
  for (std::size_t i = 0; i < kMaxPendingSpends; ++i) {
    unanswered.push_back(SpendEntry{spendId("u" + std::to_string(i)), "exports", 1, NOW - static_cast<std::int64_t>(i), SpendKind::Unanswered});
  }
  MeterHarness h(exportsOnly(1'000, 1'000, 1'000), unanswered);
  h.setClock(NOW + HOUR - 1);
  h.use("exports");
  EXPECT_EQ(h.http->count("use"), 0);
  h.setClock(NOW + HOUR);
  h.answerSpent(900);
  h.use("exports");
  EXPECT_EQ(h.http->count("use"), 1);
}

// --- the stored state ---------------------------------------------------------------------------------------

TEST(UseOffline, SurvivesASaveThatFailsItCountsTowardTheAllowanceAndTheStatusAndLandsWithTheNextSaveThatCan) {
  MeterHarness h(exportsOnly(90, 100, 2));
  h.http->fail = true;
  h.storage->failWrites = true;
  EXPECT_EQ(h.use("exports"), UseResult::spent(89, true));
  EXPECT_EQ(h.use("exports"), UseResult::spent(88, true));
  EXPECT_TRUE(h.journal().empty());
  // It counts: the allowance is spent, and the status shows it.
  EXPECT_EQ(h.use("exports"), UseResult::refused(UseError::OfflineLimit, 88));
  EXPECT_EQ(h.meters().at("exports"), (MeterStatus{88, 100, 2, 2, false}));
  h.storage->failWrites = false;
  h.license().status();
  // The third call was asked, and in doubt: the server is told it is taken back.
  std::int64_t unsynced = 0;
  int voids = 0;
  for (const SpendEntry& e : h.journal()) {
    if (e.kind == SpendKind::Void) {
      ++voids;
    } else {
      unsynced += e.n;
    }
  }
  EXPECT_EQ(unsynced, 2);
  EXPECT_EQ(voids, 1);
  // Landed once: a failing disk again changes nothing the view shows.
  h.storage->failWrites = true;
  EXPECT_EQ(h.license().status().meters.at("exports").unsynced, 2);
}

TEST(UseOffline, LandsWithTheNextUseThatCanSaveInOrder) {
  MeterHarness h;
  h.http->fail = true;
  h.storage->failWrites = true;
  h.use("exports");
  h.storage->failWrites = false;
  h.http->fail = false;
  // The server records the report the request carries, and says so.
  h.http->on("use", [](const json& body) {
    json spends = json::array();
    for (const json& s : body["spends"]) spends.push_back({{"id", s["id"]}, {"n", s["n"]}});
    json answer = spentBody(88);
    answer["spends"] = spends;
    return HttpResponse{200, answer.dump()};
  });
  h.setClock(NOW + 2 * MINUTE);
  EXPECT_EQ(h.use("exports"), UseResult::spent(88));
  // The earlier spend rode this request as a report, and its confirmation dropped it.
  EXPECT_EQ(h.useBody(1)["spends"].size(), 1u);
  EXPECT_TRUE(h.journal().empty());
}

TEST(UseOffline, LandsOnTheStateAsStoredNowSoASecondProcesssSpendIsNeverLost) {
  std::shared_ptr<MergeStorage> merge;
  MeterHarness h(exportsOnly(90, 100, 10), {}, mergingOptions(merge));
  h.http->fail = true;
  // Another process saves an offline entry between this one's read and its save.
  merge->before(1, [](MemoryStorage& disk) {
    StoredState latest = *disk.get();
    latest.spends = {SpendEntry{spendId("O"), "exports", 4, NOW - 5, SpendKind::Offline}};
    latest.rev = 7;
    disk.set(latest);
  });
  // The first no-answer spend is journaled beside theirs; the second (in its wait) grows the offline entry that is there.
  h.use("exports");
  h.use("exports", 2);
  const std::vector<SpendEntry> journal = h.journal();
  ASSERT_EQ(journal.size(), 2u);
  EXPECT_EQ(journal[0].id, spendId("O"));
  EXPECT_EQ(journal[0].n, 6);
  EXPECT_EQ(journal[1].kind, SpendKind::Unanswered);
}

TEST(UseOffline, LandsOnTheFileAsItIsAtTheSaveWhenTheStorageHasNoMergeOfItsOwn) {
  MeterHarness h;
  h.http->fail = true;
  h.use("exports");
  // Another process added an entry; this one's next save reads the file again before it writes.
  StoredState latest = *h.stored();
  latest.spends.push_back(SpendEntry{spendId("T"), "exports", 3, NOW - 1, SpendKind::Offline});
  h.store(latest);
  h.use("exports", 2);
  std::int64_t offline = 0;
  for (const SpendEntry& e : h.journal()) offline += e.kind == SpendKind::Offline ? e.n : 0;
  EXPECT_EQ(offline, 5);
  EXPECT_EQ(h.journal().size(), 2u);
}

TEST(UseOffline, KeepsAnEntryInTheJournalOfTheActivationItIsAboutAndNothingOfItOnAnotherSeat) {
  MeterHarness h(defaultMeters(), {entry(SpendKind::Offline, "A", 2, NOW)});
  StoredState state = *h.stored();
  state.meterFigures["exports"] = MeterFigure{50, 100, 10, false, NOW};
  h.store(state);
  // The same seat activated again keeps it.
  h.http->reply("activate", 200, {{"lease", meterLease(h)}, {"activationId", "act_1"}});
  EXPECT_EQ(h.license().activate(KEY), ok());
  EXPECT_EQ(h.journal().size(), 1u);
  EXPECT_FALSE(h.stored()->meterFigures.empty());
  // Another seat of the same key starts afresh.
  LeaseSpec spec;
  spec.model = "subscription";
  spec.major = 3;
  spec.aid = "act_9";
  spec.extra["met"] = metClaim(defaultMeters());
  h.http->reply("activate", 200, {{"lease", h.sign(spec)}, {"activationId", "act_9"}});
  EXPECT_EQ(h.license().activate(KEY), ok());
  EXPECT_EQ(h.stored()->activationId, "act_9");
  EXPECT_TRUE(h.stored()->spends.empty());
  EXPECT_TRUE(h.stored()->meterFigures.empty());
}

TEST(UseOffline, DropsTheJournalOfAnOldActivationWhenAnotherIsActivatedUnreported) {
  MeterHarness h(defaultMeters(), {entry(SpendKind::Offline, "A", 2, NOW)});
  StoredState state = *h.stored();
  state.meterFigures["exports"] = MeterFigure{50, 100, 10, false, NOW};
  h.store(state);
  const std::string another = "SLUICE-OTHER-AAAA-BBBB-CCCC";
  LeaseSpec spec;
  spec.sub = another;
  spec.aid = "act_2";
  spec.model = "subscription";
  spec.major = 3;
  spec.extra["met"] = metClaim(defaultMeters());
  h.http->reply("activate", 200, {{"lease", h.sign(spec)}, {"activationId", "act_2"}});
  EXPECT_EQ(h.license().activate(another), ok());
  // Nothing of the journal was reported: an activation sends no report.
  EXPECT_EQ(h.http->count("validate"), 0);
  EXPECT_EQ(h.http->bodyOf("activate").contains("spends"), false);
  EXPECT_EQ(h.stored()->key, another);
  EXPECT_TRUE(h.stored()->spends.empty());
  EXPECT_TRUE(h.stored()->meterFigures.empty());
}

TEST(UseOffline, KeepsAnEntryOpenedExactly180DaysAgoAndDropsOneAMillisecondOlder) {
  MeterHarness h(defaultMeters(), {entry(SpendKind::Unanswered, "e", 1, NOW - 180 * DAY),
                                   entry(SpendKind::Unanswered, "d", 1, NOW - 180 * DAY - 1)});
  h.http->fail = true;
  h.use("exports");
  std::vector<std::string> ids;
  for (const SpendEntry& e : h.journal()) ids.push_back(e.id);
  EXPECT_EQ(std::count(ids.begin(), ids.end(), spendId("e")), 1);
  EXPECT_EQ(std::count(ids.begin(), ids.end(), spendId("d")), 0);
}

TEST(UseOffline, NeverTrapsOnAStateWithTimesAndCountsAtTheEndsOfWhatItHolds) {
  const std::int64_t top = 9'007'199'254'740'991;  // 2^53 - 1, the largest a stored figure or time is
  MeterHarness h(exportsOnly(top, top, top), {SpendEntry{spendId("T"), "exports", top, top, SpendKind::Offline},
                                              SpendEntry{spendId("U"), "exports", top, 0, SpendKind::Unanswered},
                                              SpendEntry{spendId("V"), "exports", 1, top, SpendKind::Void}});
  h.http->fail = true;
  const UseResult result = h.use("exports", kMaxMeterUses);
  // The unsynced spends are saturated, never wrapped round: the allowance is spent, and nothing is left.
  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.error, UseError::OfflineLimit);
  EXPECT_EQ(result.left, 0);
  EXPECT_EQ(h.meters().at("exports").left, 0);
  h.setClock(top);
  EXPECT_FALSE(h.use("exports").ok);
}

TEST(UseOffline, KeysTheInstallByTheMachineIdItReadWhenTheRequestGetsNoAnswer) {
  auto machine = std::make_shared<FakeFingerprinter>("fp-machine");
  machine->identifyAs(DeviceKind::Machine, {});
  HarnessOptions options;
  options.fingerprint = machine;
  MeterHarness h(defaultMeters(), {}, options);
  h.http->fail = true;
  h.use("exports");
  EXPECT_EQ(h.stored()->deviceKind, DeviceKind::Machine);
  EXPECT_EQ(h.stored()->backoffMs, kRetryUnansweredMs);
}

TEST(UseOffline, ALicenceStaysLicensedWhenAMeterIsUsedUpAbsentOrPastItsOfflineLimit) {
  MeterHarness h(exportsOnly(0, 100, 0));
  h.grantEdition("pro", exportsOnly(0, 100, 0));
  h.http->fail = true;
  EXPECT_EQ(h.use("exports"), UseResult::refused(UseError::OfflineLimit, 0));
  EXPECT_EQ(h.use("credits"), UseResult::refused(UseError::NoMeter));
  const LicenseStatus status = h.license().status();
  EXPECT_EQ(status.state, LicenseState::Licensed);
  EXPECT_EQ(status.meters.at("exports").left, 0);
  EXPECT_EQ(status.entitlements.at("edition"), keygrant::Entitlement("pro"));
  EXPECT_TRUE(h.license().has("edition"));
  EXPECT_NO_THROW(h.license().require("edition"));
  EXPECT_EQ(h.license().refresh().state, LicenseState::Licensed);
}

}  // namespace
}  // namespace kgtest
