// The journal of unsynced spends is reported with the next validate, use and deactivate, dropped only when the server
// confirms it, and never costs a customer a renewal or a release; and a status shows a key's meters net of it.
#include <fstream>
#include <future>
#include <algorithm>
#include <set>
#include <tuple>
#include <sstream>

#include "license_file_support.hpp"
#include "meter_support.hpp"

namespace kgtest {
namespace {

using keygrant::HttpResponse;
using keygrant::Refusal;
using keygrant::detail::kAskAgainMs;
using keygrant::detail::kMaxPendingSpends;
using keygrant::detail::kRetryUnansweredMs;

constexpr std::int64_t LONG_AGO = NOW - 2 * HOUR;

json confirmedCount(const std::string& who, std::int64_t n) { return {{"id", spendId(who)}, {"n", n}}; }

// --- what a validate reports ---------------------------------------------------------------------------------------

TEST(MeterReports, ACheckInCarriesTheJournalOldestFirstAVoidMarkedAndNoSpendsAtAllWhenThereIsNone) {
  MeterHarness h(defaultMeters(), {entry(SpendKind::Offline, "A", 3, NOW - 3000), entry(SpendKind::Unanswered, "B", 1, NOW - 2000),
                                   entry(SpendKind::Void, "C", 2, NOW - 1000)});
  renewal(h);
  h.license().refresh();
  EXPECT_EQ(reported(h), json::array({{{"id", spendId("A")}, {"meter", "exports"}, {"n", 3}, {"at", NOW - 3000}},
                                      {{"id", spendId("B")}, {"meter", "exports"}, {"n", 1}, {"at", NOW - 2000}},
                                      {{"id", spendId("C")}, {"meter", "exports"}, {"n", 2}, {"at", NOW - 1000}, {"void", true}}}));
  MeterHarness bare;
  renewal(bare);
  bare.license().refresh();
  EXPECT_FALSE(bare.http->bodyOf("validate").contains("spends"));
}

TEST(MeterReports, RenewsWithAJournalThatIsFull) {
  std::vector<SpendEntry> full;
  for (std::size_t i = 0; i < kMaxPendingSpends; ++i) {
    full.push_back(SpendEntry{spendId("q" + std::to_string(i)), "m" + std::to_string(i), 1, LONG_AGO, SpendKind::Unanswered});
  }
  MeterHarness h(defaultMeters(), full);
  renewal(h);
  EXPECT_EQ(h.license().refresh().state, LicenseState::Licensed);
  EXPECT_EQ(reported(h).size(), kMaxPendingSpends);
}

TEST(MeterReports, ReportsTheFirst64OfALongerJournal) {
  std::vector<SpendEntry> longer;
  for (std::size_t i = 0; i < kMaxPendingSpends + 6; ++i) {
    longer.push_back(SpendEntry{spendId("p" + std::to_string(i)), "m" + std::to_string(i), 1, LONG_AGO + static_cast<std::int64_t>(i),
                                SpendKind::Unanswered});
  }
  MeterHarness h(defaultMeters(), longer);
  renewal(h);
  h.license().refresh();
  const json report = reported(h);
  ASSERT_EQ(report.size(), kMaxPendingSpends);
  EXPECT_EQ(report[0]["id"], spendId("p0"));
  EXPECT_EQ(report[63]["id"], spendId("p63"));
}

TEST(MeterReports, DropsExactlyTheEntriesTheAnswerNamesAsRecorded) {
  MeterHarness h(defaultMeters(), {entry(SpendKind::Unanswered, "A", 3, LONG_AGO), entry(SpendKind::Offline, "B", 5, LONG_AGO, "renders"),
                                   entry(SpendKind::Void, "C", 1, LONG_AGO), entry(SpendKind::Void, "D", 1, LONG_AGO, "renders")});
  renewal(h, json::array({confirmedCount("A", 3),
                          // More recorded than ever held: dropped. Less than held: kept.
                          confirmedCount("B", 4),
                          // A void is named void; a count for a void entry does not name it.
                          {{"id", spendId("C")}, {"void", true}},
                          confirmedCount("D", 1),
                          // Another shape confirms nothing, and an id the journal has not is nothing.
                          confirmedCount("E", 1),
                          {{"n", 2}},
                          "x",
                          nullptr}));
  h.license().refresh();
  EXPECT_EQ(ids(h.journal()), (std::vector<std::string>{spendId("B"), spendId("D")}));
}

TEST(MeterReports, KeepsAnOfflineEntryAnotherProcessGrewMeanwhileWithItsCount) {
  MeterHarness h(defaultMeters(), {entry(SpendKind::Offline, "A", 3, LONG_AGO)});
  h.http->on("validate", [&h](const json&) {
    // Another process spent two more while this request was out.
    StoredState grown = *h.stored();
    grown.spends = {entry(SpendKind::Offline, "A", 5, LONG_AGO)};
    h.store(grown);
    return HttpResponse{200, json{{"lease", meterLease(h)}, {"spends", json::array({confirmedCount("A", 3)})}}.dump()};
  });
  h.license().refresh();
  EXPECT_EQ(h.journal(), std::vector<SpendEntry>({entry(SpendKind::Offline, "A", 5, LONG_AGO)}));
}

TEST(MeterReports, DropsNothingWhenTheAnswerNamesNothingAndWaitsAnHourBeforeStatusOffersItAgain) {
  MeterHarness h(defaultMeters(), {entry(SpendKind::Offline, "A", 1, LONG_AGO)});
  renewal(h);
  h.license().refresh();
  EXPECT_EQ(h.journal().size(), 1u);
  EXPECT_EQ(h.stored()->backoffMs, kAskAgainMs);
  // A valid, far lease with a journal asks only once the wait is over.
  h.license().status();
  EXPECT_EQ(h.http->count("validate"), 1);
  h.setClock(NOW + HOUR);
  h.license().status();
  EXPECT_EQ(h.http->count("validate"), 2);
}

TEST(MeterReports, DropsNothingOnA5xxOrA429WhateverTheirBodySaysAndNothingOnNoAnswer) {
  for (const int status : {503, 429, 0}) {
    SCOPED_TRACE(status);
    MeterHarness h(defaultMeters(), {entry(SpendKind::Unanswered, "A", 3, LONG_AGO)});
    if (status == 0) {
      h.http->fail = true;
    } else {
      h.http->reply("validate", status, {{"spends", json::array({confirmedCount("A", 3)})}});
    }
    h.license().refresh();
    EXPECT_EQ(h.http->count("validate"), 1);
    EXPECT_EQ(h.journal().size(), 1u);
  }
}

TEST(MeterReports, DropsWhatARefusalConfirmsWhateverItsStatus) {
  MeterHarness h(defaultMeters(), {entry(SpendKind::Unanswered, "A", 3, LONG_AGO)});
  h.http->reply("validate", 403, {{"error", "revoked"}, {"spends", json::array({confirmedCount("A", 3)})}});
  EXPECT_EQ(h.license().refresh(), status(LicenseState::Invalid, LicenseReason::Revoked, KEY));
  EXPECT_TRUE(h.journal().empty());
  // A 200 without a lease is an answer too.
  MeterHarness g(defaultMeters(), {entry(SpendKind::Unanswered, "A", 3, LONG_AGO)});
  g.http->reply("validate", 200, {{"spends", json::array({confirmedCount("A", 3)})}});
  g.license().refresh();
  EXPECT_TRUE(g.journal().empty());
}

TEST(MeterReports, KeepsTheConfirmationInMemoryWhenItCannotBeSavedSoTheEntryIsNotReportedAgainAndSavesItNext) {
  MeterHarness h(defaultMeters(), {entry(SpendKind::Unanswered, "A", 3, LONG_AGO)});
  renewal(h, json::array({confirmedCount("A", 3)}));
  h.storage->failWrites = true;
  h.license().refresh();
  EXPECT_EQ(h.journal().size(), 1u);
  EXPECT_EQ(h.license().status().meters.at("exports").unsynced, 0);
  h.license().refresh();
  EXPECT_EQ(reported(h, 1), json(nullptr));
  h.storage->failWrites = false;
  h.license().status();
  EXPECT_TRUE(h.journal().empty());
}

TEST(MeterReports, ReportsToAValidLeaseAtTheNextStatusOnceItsOwnWaitIsNotRunningAndNotBefore) {
  MeterHarness h(defaultMeters(), {entry(SpendKind::Unanswered, "A", 1, NOW - 30'000)});
  renewal(h, json::array({confirmedCount("A", 1)}));
  // The journal's wait: a minute after one unanswered spend.
  h.license().status();
  EXPECT_EQ(h.http->count("validate"), 0);
  h.setClock(NOW - 30'000 + MINUTE);
  h.license().status();
  EXPECT_EQ(h.http->count("validate"), 1);
  EXPECT_TRUE(h.journal().empty());
  // Nothing left to report: a valid, far lease asks nothing.
  h.license().status();
  EXPECT_EQ(h.http->count("validate"), 1);
}

TEST(MeterReports, ReportsAnOfflineOnlyJournalAtOnceAndWaitsForTheAsksOwnWaitAfterARequestThatGotNoAnswer) {
  MeterHarness h(defaultMeters(), {entry(SpendKind::Offline, "A", 2, NOW)});
  h.http->fail = true;
  h.license().status();
  EXPECT_EQ(h.http->count("validate"), 1);
  EXPECT_EQ(h.stored()->backoffMs, kRetryUnansweredMs);
  h.license().status();
  EXPECT_EQ(h.http->count("validate"), 1);
  h.http->fail = false;
  renewal(h, json::array({confirmedCount("A", 2)}));
  h.setClock(NOW + MINUTE);
  h.license().status();
  EXPECT_TRUE(h.journal().empty());
}

TEST(MeterReports, AsksOfALeasePastItsEndToHealItWithTheJournal) {
  MeterHarness h(defaultMeters(), {entry(SpendKind::Offline, "A", 1, NOW - 40 * DAY)});
  h.setClock(NOW + 40 * DAY);
  renewal(h);
  h.license().status();
  EXPECT_EQ(reported(h).size(), 1u);
}

TEST(MeterReports, RidesAUseRequestTooAndIsDroppedByWhatThatRequestConfirms) {
  MeterHarness h(defaultMeters(), {entry(SpendKind::Unanswered, "A", 2, LONG_AGO)});
  json answer = spentBody(87);
  answer["spends"] = json::array({confirmedCount("A", 2)});
  h.answerUse(200, answer);
  EXPECT_EQ(h.use("exports"), UseResult::spent(87));
  EXPECT_EQ(h.useBody()["spends"], json::array({{{"id", spendId("A")}, {"meter", "exports"}, {"n", 2}, {"at", LONG_AGO}}}));
  EXPECT_TRUE(h.journal().empty());
}

TEST(MeterReports, CarriesTheIdAFailedUseSentIntoTheNextRequestsReport) {
  MeterHarness h;
  h.http->fail = true;
  h.use("exports");
  const std::string sent = h.useBody()["id"].get<std::string>();
  h.http->fail = false;
  renewal(h);
  h.setClock(NOW + 2 * MINUTE);
  h.license().refresh();
  const json report = reported(h);
  ASSERT_EQ(report.size(), 1u);
  EXPECT_EQ(report[0]["id"], sent);
  EXPECT_EQ(report[0]["n"], 1);
}

TEST(MeterReports, ReportsAnEntryTheServerDidNotConfirmAgainWithTheSameIdMeterCountAndTime) {
  struct Case {
    const char* name;
    std::function<void(MeterHarness&)> arrange;
  };
  const std::vector<Case> cases = {
      {"a lease naming nothing", [](MeterHarness& h) { renewal(h); }},
      {"a 503", [](MeterHarness& h) { h.http->reply("validate", 503, json::object()); }},
      {"a 429", [](MeterHarness& h) { h.http->reply("validate", 429, json::object()); }},
      {"no answer at all", [](MeterHarness& h) { h.http->fail = true; }},
      {"a count lower than the entry holds", [](MeterHarness& h) { renewal(h, json::array({confirmedCount("A", 1)})); }},
  };
  for (const Case& c : cases) {
    SCOPED_TRACE(c.name);
    MeterHarness h(defaultMeters(), {entry(SpendKind::Unanswered, "A", 3, LONG_AGO)});
    c.arrange(h);
    h.license().refresh();
    h.http->fail = false;
    renewal(h);
    h.setClock(NOW + 2 * HOUR);
    h.license().refresh();
    const json again = reported(h, 1);
    ASSERT_EQ(again.size(), 1u);
    EXPECT_EQ(again[0], (json{{"id", spendId("A")}, {"meter", "exports"}, {"n", 3}, {"at", LONG_AGO}}));
  }
}

TEST(MeterReports, SendsNoSpendsWithAnActivationOrATrialThoughTheJournalHoldsSome) {
  MeterHarness h(defaultMeters(), {entry(SpendKind::Offline, "A", 2, NOW)});
  h.http->reply("activate", 200, {{"lease", meterLease(h)}, {"activationId", "act_1"}});
  EXPECT_EQ(h.license().activate(KEY), ok());
  EXPECT_FALSE(h.http->bodyOf("activate").contains("spends"));

  MeterHarness trial(defaultMeters(), {});
  StoredState state;
  state.trialStart = NOW - DAY;
  state.trialEndsAt = NOW + DAY;
  state.trialLease = trial.sign("trial:fp-test", "trial", 2 * 86'400, NOW - DAY);
  state.lastSeen = NOW;
  state.spends = {entry(SpendKind::Offline, "A", 2, NOW)};
  trial.store(state);
  trial.http->reply("trial", 200, {{"trialStart", NOW - DAY}, {"trialEndsAt", NOW + DAY},
                                   {"lease", trial.sign("trial:fp-test", "trial", 2 * 86'400, NOW - DAY)}});
  trial.license().startTrial();
  ASSERT_GE(trial.http->count("trial"), 1);
  EXPECT_FALSE(trial.http->bodyOf("trial").contains("spends"));
}

TEST(MeterReports, ARefusalOfACoveredSpendBecauseTheJournalIsFullThoughTheRequestWasSentAppendsNoVoidItHasNoRoomFor) {
  std::vector<SpendEntry> full;
  for (std::size_t i = 0; i < kMaxPendingSpends; ++i) {
    full.push_back(SpendEntry{spendId("f" + std::to_string(i)), "m" + std::to_string(i), 1, LONG_AGO, SpendKind::Unanswered});
  }
  MeterHarness h(defaultMeters(), full);
  h.http->fail = true;
  EXPECT_EQ(h.use("exports"), UseResult::refused(UseError::OfflineLimit, 90));
  EXPECT_EQ(h.http->count("use"), 1);
  EXPECT_EQ(h.journal().size(), kMaxPendingSpends);
  const std::vector<SpendEntry> journal = h.journal();
  EXPECT_TRUE(std::none_of(journal.begin(), journal.end(), [](const SpendEntry& e) { return e.kind == SpendKind::Void; }));
}

// --- what a deactivate reports -------------------------------------------------------------------------------------------

TEST(MeterReports, ADeactivateCarriesTheJournalWithTheDevicesFingerprintsThenClearsItWithTheLicence) {
  MeterHarness h(defaultMeters(), {entry(SpendKind::Offline, "A", 2, NOW), entry(SpendKind::Void, "B", 1, NOW)});
  h.http->reply("deactivate", 200, {{"ok", true}});
  EXPECT_EQ(h.license().deactivate(), keygrant::DeactivateResult{true});
  const json body = h.http->bodyOf("deactivate");
  EXPECT_EQ(body["key"], KEY);
  EXPECT_EQ(body["activationId"], "act_1");
  EXPECT_EQ(body["spends"].size(), 2u);
  EXPECT_EQ(body["fingerprints"], json::array({"fp-test"}));
  const StoredState state = *h.stored();
  EXPECT_TRUE(state.spends.empty());
  EXPECT_TRUE(state.meterFigures.empty());
  EXPECT_EQ(state.key, std::nullopt);
  EXPECT_EQ(state.activationId, std::nullopt);
}

TEST(MeterReports, ADeactivateSendsExactlyTheKeyAndTheActivationWhenThereIsNothingToReport) {
  MeterHarness h;
  StoredState state = *h.stored();
  state.meterFigures["exports"] = MeterFigure{5, 100, 10, false, NOW};
  h.store(state);
  h.http->reply("deactivate", 200, json::object());
  h.license().deactivate();
  EXPECT_EQ(h.http->bodyOf("deactivate"), (json{{"key", KEY}, {"activationId", "act_1"}}));
  EXPECT_TRUE(h.stored()->meterFigures.empty());
}

TEST(MeterReports, ADeactivateNamesNoFingerprintsWhenItCannotTellWhoTheDeviceIs) {
  auto blind = std::make_shared<FakeFingerprinter>();
  blind->failing = true;
  HarnessOptions options;
  options.fingerprint = blind;
  MeterHarness h(defaultMeters(), {entry(SpendKind::Offline, "A", 1, NOW)}, options);
  h.http->reply("deactivate", 200, json::object());
  h.license().deactivate();
  const json body = h.http->bodyOf("deactivate");
  EXPECT_EQ(body["spends"].size(), 1u);
  EXPECT_FALSE(body.contains("fingerprints"));
}

TEST(MeterReports, ADeactivateClearsTheJournalLocallyAndSaysTheSeatMayBeHeldWhenTheReleaseGotNo200) {
  for (const bool fail : {true, false}) {
    SCOPED_TRACE(fail);
    MeterHarness h(defaultMeters(), {entry(SpendKind::Offline, "A", 1, NOW)});
    if (fail) {
      h.http->fail = true;
    } else {
      h.http->reply("deactivate", 503, json::object());
    }
    EXPECT_EQ(h.license().deactivate(), keygrant::DeactivateResult{false});
    EXPECT_TRUE(h.stored()->spends.empty());
  }
}

TEST(MeterReports, ADeactivateReportsWhatThisProcessCouldNotSaveAndForgetsItWithTheRest) {
  MeterHarness h;
  h.http->fail = true;
  h.storage->failWrites = true;
  EXPECT_TRUE(h.use("exports").ok);
  h.storage->failWrites = false;
  h.http->fail = false;
  h.http->reply("deactivate", 200, json::object());
  EXPECT_EQ(h.license().deactivate(), keygrant::DeactivateResult{true});
  EXPECT_EQ(h.http->bodyOf("deactivate")["spends"].size(), 1u);
  // Nothing of it comes back with the next licence.
  h.http->reply("activate", 200, {{"lease", meterLease(h)}, {"activationId", "act_1"}});
  EXPECT_EQ(h.license().activate(KEY), ok());
  EXPECT_EQ(h.license().status().meters.at("exports").unsynced, 0);
  EXPECT_TRUE(h.journal().empty());
  EXPECT_TRUE(keygrant::detail::LicenseAccess::heldChanges(h.license()).empty());
}

TEST(MeterReports, ADeactivateWhoseStateCannotBeSavedFailsKeepingTheKeyAndReportsTheJournalAgain) {
  MeterHarness h(defaultMeters(), {entry(SpendKind::Offline, "A", 2, NOW)});
  h.http->reply("deactivate", 200, json::object());
  h.storage->failWrites = true;
  EXPECT_THROW(h.license().deactivate(), keygrant::IoError);
  EXPECT_EQ(h.http->bodyOf("deactivate")["spends"].size(), 1u);
  // The key is kept, and so is the journal: a second call reports it again and the server counts it once.
  EXPECT_EQ(h.stored()->key, KEY);
  EXPECT_EQ(h.journal().size(), 1u);
  h.storage->failWrites = false;
  EXPECT_EQ(h.license().deactivate(), keygrant::DeactivateResult{true});
  ASSERT_EQ(h.http->bodiesOf("deactivate").size(), 2u);
  EXPECT_EQ(h.http->bodiesOf("deactivate")[1]["spends"], h.http->bodiesOf("deactivate")[0]["spends"]);
}

TEST(MeterReports, ADeactivateReportsTheFirst64OfALongerJournalAndClearsEveryEntryWithTheLicence) {
  std::vector<SpendEntry> longer;
  for (std::size_t i = 0; i < kMaxPendingSpends + 5; ++i) {
    longer.push_back(SpendEntry{spendId("d" + std::to_string(i)), "m" + std::to_string(i), 1, NOW - 100, SpendKind::Unanswered});
  }
  MeterHarness h(defaultMeters(), longer);
  h.http->reply("deactivate", 200, json::object());
  h.license().deactivate();
  EXPECT_EQ(h.http->bodyOf("deactivate")["spends"].size(), kMaxPendingSpends);
  EXPECT_TRUE(h.stored()->spends.empty());
}

TEST(MeterReports, ADeactivateThatCannotLandWhatItHoldsGoesWithWhatIsStoredAndTheCallThenFailsWithTheKeyKept) {
  // The stored entry is old enough for no wait to run: the covered spend asks (and gets no answer), and so does the uncovered one.
  MeterHarness h(defaultMeters(), {entry(SpendKind::Unanswered, "S", 1, LONG_AGO)});
  h.http->fail = true;
  h.storage->failWrites = true;
  // Held in memory: an unanswered spend and a void; the disk refuses.
  EXPECT_TRUE(h.use("exports", 2).ok);
  EXPECT_EQ(h.use("renders").error, UseError::OfflineLimit);
  h.http->fail = false;
  h.http->reply("deactivate", 200, json::object());
  EXPECT_THROW(h.license().deactivate(), keygrant::IoError);
  const json report = h.http->bodyOf("deactivate")["spends"];
  // The stored entry, and the held unanswered spend and void (an offline spend held would not be said).
  ASSERT_EQ(report.size(), 3u);
  std::vector<std::tuple<std::string, std::int64_t, bool>> entries;
  for (const json& e : report) entries.emplace_back(e["meter"].get<std::string>(), e["n"].get<std::int64_t>(), e.contains("void"));
  std::sort(entries.begin(), entries.end());
  EXPECT_EQ(entries, (std::vector<std::tuple<std::string, std::int64_t, bool>>{
                         {"exports", 1, false}, {"exports", 2, false}, {"renders", 1, true}}));
  EXPECT_EQ(h.stored()->key, KEY);
}

TEST(MeterReports, ReportsNothingForASeatThatIsAnotherDevicesTheReleaseIsNotItsToMake) {
  auto machine = std::make_shared<FakeFingerprinter>("fp-this");
  machine->identifyAs(DeviceKind::Machine, {});
  HarnessOptions options;
  options.fingerprint = machine;
  MeterHarness h(defaultMeters(), {entry(SpendKind::Offline, "A", 1, NOW)}, options);
  // Its lease is held to a machine this one is not: a state file copied from another PC.
  LeaseSpec spec;
  spec.model = "subscription";
  spec.major = 3;
  spec.extra["fp"] = claimOf("fp-other");
  spec.extra["fpk"] = "machine";
  spec.extra["met"] = metClaim(defaultMeters());
  StoredState state = *h.stored();
  state.lease = h.sign(spec);
  h.store(state);
  EXPECT_EQ(h.license().deactivate(), keygrant::DeactivateResult{true});
  EXPECT_EQ(h.http->count("deactivate"), 0);
  // The journal goes with the licence it was about, though nothing was reported.
  EXPECT_TRUE(h.stored()->spends.empty());
}

// --- status().meters ------------------------------------------------------------------------------------------------------

TEST(MeterReports, AStatusShowsEachMeterOfTheLeaseWithItsFiguresAndNothingWhenTheLeaseHasNone) {
  MeterHarness h;
  LicenseStatus expected = licensed(KEY);
  expected.meters["exports"] = MeterStatus{90, 100, 10, 0, false};
  expected.meters["renders"] = MeterStatus{5, 5, 0, 0, true};
  EXPECT_EQ(h.license().status(), expected);
  EXPECT_EQ(h.http->count("validate"), 0);

  Harness bare;
  bare.store(held(bare.sign(KEY, "subscription", 30 * 24 * 3600)));
  EXPECT_EQ(bare.license().status(), licensed(KEY));
  EXPECT_TRUE(bare.license().status().meters.empty());
}

TEST(MeterReports, AStatusIsNetOfTheUnsyncedSpendsOfEachKindNeverBelow0AndCountsNoVoid) {
  MeterHarness h(defaultMeters(), {entry(SpendKind::Offline, "A", 4, NOW), entry(SpendKind::Unanswered, "B", 3, NOW),
                                   entry(SpendKind::Void, "C", 50, NOW), entry(SpendKind::Offline, "D", 9, NOW, "renders")});
  StoredState state = withBackoff(*h.stored(), NOW, kRetryUnansweredMs);
  h.store(state);
  const auto meters = h.license().status().meters;
  EXPECT_EQ(meters.at("exports"), (MeterStatus{83, 100, 10, 7, false}));
  EXPECT_EQ(meters.at("renders"), (MeterStatus{0, 5, 0, 9, true}));
}

TEST(MeterReports, AStatusWorksBehindACallThatHasNotFinishedSayingSo) {
  HarnessOptions options;
  options.httpTimeout = Millis{100};
  options.timings = shortTimings();
  MeterHarness h(defaultMeters(), {entry(SpendKind::Offline, "A", 2, NOW)}, options);
  h.http->reply("validate", 403, {{"error", "revoked"}});
  h.storage->holdNextWrite();
  auto refreshing = std::async(std::launch::async, [&] { return h.license().refresh(); });
  EXPECT_TRUE(h.storage->writeGate().waitReached(Millis{5'000}));
  const LicenseStatus later = h.license().status();
  EXPECT_TRUE(later.stalled);
  EXPECT_EQ(later.state, LicenseState::Licensed);
  EXPECT_EQ(later.meters.at("exports").left, 88);
  EXPECT_EQ(later.meters.at("exports").unsynced, 2);
  h.storage->writeGate().open();
  (void)refreshing.get();
}

TEST(MeterReports, ASpendBehindACallThatHasNotFinishedAsksSavesNothingOfTheAnswerAndKeepsTheSpend) {
  HarnessOptions options;
  options.httpTimeout = Millis{100};
  options.timings = shortTimings();
  MeterHarness h(defaultMeters(), {}, options);
  h.http->reply("validate", 403, {{"error", "revoked"}});
  h.storage->holdNextWrite();
  auto refreshing = std::async(std::launch::async, [&] { return h.license().refresh(); });
  EXPECT_TRUE(h.storage->writeGate().waitReached(Millis{5'000}));
  const int writes = h.storage->writes();
  h.answerSpent(89);
  EXPECT_EQ(h.use("exports"), UseResult::spent(89));
  EXPECT_EQ(h.http->count("use"), 1);
  // Offline now: the spend is kept in memory and counts.
  h.http->fail = true;
  // The answer the first call was given was not saved either, so the lease's own figure is the base.
  EXPECT_EQ(h.use("exports", 2), UseResult::spent(88, true));
  EXPECT_EQ(h.storage->writes(), writes);
  h.storage->writeGate().open();
  // The call that was stuck finishes with its verdict, and the spend kept in memory lands beside it.
  (void)refreshing.get();
  EXPECT_EQ(h.stored()->refused, Refusal::Revoked);
  h.license().status();
  EXPECT_EQ(h.shape(), "unanswered:2");
}

TEST(MeterReports, ASpendBehindACallThatHasNotFinishedSavesNeitherTheVerdictNorTheConfirmationsOfItsAnswer) {
  HarnessOptions options;
  options.httpTimeout = Millis{100};
  options.timings = shortTimings();
  MeterHarness h(defaultMeters(), {entry(SpendKind::Offline, "A", 2, NOW)}, options);
  renewal(h);
  h.storage->holdNextWrite();
  auto refreshing = std::async(std::launch::async, [&] { return h.license().refresh(); });
  EXPECT_TRUE(h.storage->writeGate().waitReached(Millis{5'000}));
  const int writes = h.storage->writes();
  h.answerUse(403, {{"error", "revoked"}, {"spends", json::array({confirmedCount("A", 2)})}});
  EXPECT_EQ(h.use("exports"), UseResult::refused(UseError::Revoked));
  EXPECT_EQ(h.http->count("use"), 1);
  EXPECT_EQ(h.storage->writes(), writes);
  h.storage->writeGate().open();
  (void)refreshing.get();
  // The licence stands and the journal holds what it held: the call that was stuck saved only its own renewal.
  EXPECT_EQ(h.stored()->refused, std::nullopt);
  EXPECT_EQ(h.shape(), "offline:2");
  EXPECT_EQ(h.license().status().state, LicenseState::Licensed);
}

TEST(MeterReports, AStatusIsAbsentOnEveryStatusButAKeysLicensedOne) {
  MeterHarness expired;
  expired.setClock(NOW + 40 * DAY);
  EXPECT_EQ(expired.license().status(), status(LicenseState::Expired, LicenseReason::Offline, KEY));
  MeterHarness revoked;
  StoredState state = *revoked.stored();
  state.refused = Refusal::Revoked;
  revoked.store(state);
  EXPECT_EQ(revoked.license().status(), status(LicenseState::Invalid, LicenseReason::Revoked, KEY));
  Harness none;
  EXPECT_EQ(none.license().status(), status(LicenseState::Unlicensed));
}

TEST(MeterReports, AStatusIsAbsentOnATrialWhoseLeaseCarriesMetersAndOnAnOfflineLicenceFileThatDoes) {
  Harness trial;
  LeaseSpec spec;
  spec.sub = "trial:fp-test";
  spec.model = "trial";
  spec.extra["met"] = metClaim(defaultMeters());
  trial.http->reply("trial", 200, {{"trialStart", NOW}, {"trialEndsAt", NOW + 30 * DAY}, {"lease", trial.sign(spec)}});
  const LicenseStatus started = trial.license().startTrial();
  EXPECT_EQ(started.state, LicenseState::Trial);
  EXPECT_TRUE(started.meters.empty());
  EXPECT_TRUE(trial.license().status().meters.empty());

  MeterHarness file;
  EXPECT_TRUE(file.license().importLicenseFile(fileText(offlineFile(file.key))).ok);
  // The file answers before the key beside it, and a file has no meters, so neither has the status or a spend.
  EXPECT_TRUE(file.license().status().meters.empty());
  EXPECT_EQ(file.license().status().source, keygrant::LicenseSource::File);
  EXPECT_EQ(file.use("exports"), UseResult::refused(UseError::NotLicensed));
}

TEST(MeterReports, EachStatusHoldsACopyOfItsOwn) {
  MeterHarness h;
  LicenseStatus a = h.license().status();
  const LicenseStatus b = h.license().status();
  a.meters["exports"].left = 1;
  a.meters.erase("renders");
  EXPECT_EQ(b.meters.at("exports").left, 90);
  EXPECT_EQ(b.meters.size(), 2u);
  EXPECT_EQ(h.license().status().meters.at("exports").left, 90);
}

TEST(MeterReports, AStatusShowsAMeterNamedProtoOrLikeAnObjectsMemberAsAMeterLikeAnyOther) {
  MeterHarness h({{"__proto__", MeterSpec{1, 2, 0, false}}, {"constructor", MeterSpec{3, 3, 1, false}}});
  const auto meters = h.license().status().meters;
  EXPECT_EQ(meters.size(), 2u);
  EXPECT_EQ(meters.at("__proto__"), (MeterStatus{1, 2, 0, 0, false}));
  EXPECT_EQ(meters.at("constructor"), (MeterStatus{3, 3, 1, 0, false}));
}

TEST(MeterReports, AStatusTakesAFigureAUseAnswerGaveInPlaceOfTheLeasesUntilTheLeaseIsRenewed) {
  const auto withFigure = [](std::int64_t at, std::int64_t left) {
    auto h = std::make_unique<MeterHarness>();
    StoredState state = *h->stored();
    state.meterFigures["exports"] = MeterFigure{left, 100, 10, false, at};
    h->store(state);
    return h;
  };
  EXPECT_EQ(withFigure(NOW + 1000, 40)->license().status().meters.at("exports").left, 40);
  // A figure from before the lease's own second is older than it; from the same second, newer.
  EXPECT_EQ(withFigure(NOW - 1000, 40)->license().status().meters.at("exports").left, 90);
  EXPECT_EQ(withFigure(NOW, 40)->license().status().meters.at("exports").left, 40);
}

TEST(MeterReports, ARenewalsStatusShowsItsMetersNetOfTheJournalAfterWhatTheAnswerConfirmed) {
  MeterHarness h(defaultMeters(), {entry(SpendKind::Unanswered, "A", 3, LONG_AGO), entry(SpendKind::Unanswered, "B", 2, LONG_AGO)});
  const std::string renewed = meterLease(h, {{"exports", MeterSpec{50, 100, 10, false}}}, NOW);
  h.http->reply("validate", 200, {{"lease", renewed}, {"spends", json::array({confirmedCount("A", 3)})}});
  const LicenseStatus status = h.license().refresh();
  // The renewal's 50, less the 2 the server did not confirm.
  EXPECT_EQ(status.meters.at("exports"), (MeterStatus{48, 100, 10, 2, false}));
}

TEST(MeterReports, AStatusStopsShowingAMeterTheNewestLeaseNoLongerNames) {
  MeterHarness h;
  const std::string renewed = meterLease(h, {{"exports", MeterSpec{50, 100, 10, false}}}, NOW + 1000);
  h.http->reply("validate", 200, {{"lease", renewed}});
  h.setClock(NOW + 1000);
  const LicenseStatus status = h.license().refresh();
  EXPECT_EQ(status.meters.count("renders"), 0u);
  EXPECT_EQ(h.license().status().meters.count("renders"), 0u);
}

TEST(MeterReports, ASpendOnAnOpenEndedLeaseIsASpendLikeAnyOther) {
  MeterHarness h;
  LeaseSpec spec;
  spec.model = "perpetual";
  spec.major = keygrant::kOpenMajor;
  spec.extra["upgradeUntil"] = (NOW + 10 * DAY) / 1000;
  spec.extra["met"] = metClaim(defaultMeters());
  StoredState state = *h.stored();
  state.lease = h.sign(spec);
  h.store(state);
  h.answerSpent(89);
  EXPECT_EQ(h.use("exports"), UseResult::spent(89));
}

// --- the bodies the reference's own worker parses --------------------------------------------------------------------------

TEST(MeterReports, TheRequestsThatCarryAReportAreTheBodiesTheReferenceSends) {
  const std::optional<std::filesystem::path> path = aboveTests("conformance/wire-bodies.json");
  if (!path) {
    if (inVectorsRepository()) FAIL() << "conformance/wire-bodies.json was not found above the tests: this repository makes it";
    GTEST_SKIP() << "the captured request bodies are not in this checkout (it is not the repository that makes them)";
  }
  std::ifstream in(*path, std::ios::binary);
  std::stringstream text;
  text << in.rdbuf();
  const json captured = json::parse(text.str());

  HarnessOptions options;
  options.major = 3;
  const MeterSpecs meters = defaultMeters();
  MeterHarness h(meters, {SpendEntry{"offlineoffline0000001", "exports", 3, NOW - 2 * HOUR, SpendKind::Offline},
                          SpendEntry{"unansweredunanswered2", "renders", 1, NOW - 2 * HOUR + 1, SpendKind::Unanswered},
                          SpendEntry{"voidvoidvoidvoid00003", "exports", 2, NOW - 2 * HOUR + 2, SpendKind::Void}},
                 options);
  // Answers that confirm nothing, so the journal is still there for the next request.
  h.answerUse(200, {{"spent", true}, {"meters", metClaim(meters)}, {"at", NOW}});
  h.http->reply("validate", 200, json::object());
  h.http->reply("deactivate", 200, json::object());
  h.use("exports", 2);
  h.license().refresh();
  h.license().deactivate();

  // The spend's id is random, and the key ids are this suite's own key's.
  json sent = h.useBody();
  EXPECT_EQ(sent["id"].get<std::string>().size(), 22u);
  sent["id"] = captured["use"]["id"];
  json validate = h.http->bodyOf("validate");
  for (const auto& [name, body] : {std::pair<const char*, json>{"use", sent}, std::pair<const char*, json>{"validate", validate}}) {
    SCOPED_TRACE(name);
    json given = body;
    EXPECT_EQ(given["kids"], h.key.kids());
    given.erase("kids");
    json expected = captured[name];
    expected.erase("kids");
    EXPECT_EQ(given, expected);
  }
  EXPECT_EQ(h.http->bodyOf("deactivate"), captured["deactivate"]);
}

}  // namespace
}  // namespace kgtest
