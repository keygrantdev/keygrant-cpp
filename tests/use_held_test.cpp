// A spend the disk refused, kept in memory by the process that made it: what it counts toward, when it is reported, when it
// lands, and what the allowance is judged against where a spend lands. Also what an answer to a spend stamps, calls made
// together, and the spends a device out of reach cannot cover.
#include <algorithm>
#include <future>
#include <thread>

#include "meter_support.hpp"

namespace kgtest {
namespace {

using keygrant::HttpResponse;
using keygrant::detail::kAskAgainMs;
using keygrant::detail::kMaxPendingSpends;
using keygrant::detail::kMaxPendingVoids;
using keygrant::detail::kRetryUnansweredMs;

std::int64_t total(const std::vector<SpendEntry>& journal) {
  std::int64_t sum = 0;
  for (const SpendEntry& e : journal) sum += e.n;
  return sum;
}

// --- a spend held in memory ----------------------------------------------------------------------------------------

TEST(UseHeld, IsNotReportedUntilItHasLandedSoTheEntryItJoinsIsReportedWithItOnce) {
  MeterHarness h(exportsOnly(90, 100, 10));
  h.store(withBackoff(*h.stored()));
  renewal(h);
  h.storage->failWrites = true;
  EXPECT_EQ(h.use("exports", 2), UseResult::spent(88, true));
  // Held in memory, it counts toward the status but no request carries it: it has no place in the journal yet.
  const LicenseStatus held = h.license().refresh();
  EXPECT_EQ(held.meters.at("exports").left, 88);
  EXPECT_EQ(held.meters.at("exports").unsynced, 2);
  EXPECT_EQ(reported(h, 0), json(nullptr));
  // Another process lands an entry, and the disk is back: the spend lands in it, and the entry is what is reported.
  StoredState other = *h.stored();
  other.spends = {SpendEntry{spendId("Y"), "exports", 1, NOW - 5, SpendKind::Offline}};
  h.store(other);
  h.storage->failWrites = false;
  h.license().refresh();
  EXPECT_EQ(reported(h, 1), json::array({{{"id", spendId("Y")}, {"meter", "exports"}, {"n", 3}, {"at", NOW - 5}}}));
}

TEST(UseHeld, IsReportedWithAReleaseOnceItHasLandedAsEverythingTheJournalHolds) {
  MeterHarness h(exportsOnly(90, 100, 10));
  h.store(withBackoff(*h.stored()));
  h.http->reply("deactivate", 200, json::object());
  h.storage->failWrites = true;
  h.use("exports", 2);
  h.storage->failWrites = false;
  h.license().deactivate();
  const json body = h.http->bodyOf("deactivate");
  ASSERT_EQ(body["spends"].size(), 1u);
  EXPECT_EQ(body["spends"][0]["meter"], "exports");
  EXPECT_EQ(body["spends"][0]["n"], 2);
}

TEST(UseHeld, ReportsAnUnansweredSpendAndAVoidWhichOpenOnceByTheirIdsAndWithholdsOnlyTheOfflineSpends) {
  MeterHarness h;
  renewal(h);
  h.storage->failWrites = true;
  h.http->fail = true;
  // In doubt, then refused after asking: an unanswered spend, and a void. A third, made while the journal waits, never
  // left the device.
  EXPECT_EQ(h.use("exports", 2), UseResult::spent(88, true));
  EXPECT_EQ(h.use("renders"), UseResult::refused(UseError::OfflineLimit, 5));
  EXPECT_EQ(h.use("exports", 1), UseResult::spent(87, true));
  h.http->fail = false;
  h.license().refresh();
  const json report = reported(h, 0);
  ASSERT_EQ(report.size(), 2u);
  EXPECT_EQ(report[0]["meter"], "exports");
  EXPECT_EQ(report[0]["n"], 2);
  EXPECT_FALSE(report[0].contains("void"));
  EXPECT_EQ(report[1]["meter"], "renders");
  EXPECT_EQ(report[1]["n"], 1);
  EXPECT_EQ(report[1]["void"], true);
}

TEST(UseHeld, JudgesAViewThatCarriedHeldSpendsByWhatIsStoredThoughASaveInTheSameCallLandedThem) {
  // An open lease inside an upgrade window: a status notes the run (a save) before it asks.
  std::shared_ptr<MergeStorage> merge;
  MeterHarness h(exportsOnly(90, 100, 10), {}, mergingOptions(merge));
  LeaseSpec spec;
  spec.model = "perpetual";
  spec.major = keygrant::kOpenMajor;
  spec.extra["upgradeUntil"] = (NOW + 10 * DAY) / 1000;
  spec.extra["met"] = metClaim(exportsOnly(90, 100, 10));
  const std::string open = h.sign(spec);
  StoredState state = withBackoff(*h.stored());
  state.lease = open;
  h.store(state);
  h.http->reply("validate", 200, {{"lease", open}});
  merge->failing = true;
  EXPECT_EQ(h.use("exports", 2), UseResult::spent(88, true));
  merge->failing = false;
  // The next call's first save (the held spend's) fails; before the second, which notes the run, another process lands an
  // entry the held spend joins.
  const int savesBefore = merge->asked();
  merge->before(1, [](MemoryStorage&) { throw keygrant::IoError("EBUSY", "the disk said no"); });
  merge->before(2, [](MemoryStorage& disk) {
    StoredState latest = *disk.get();
    latest.spends = {SpendEntry{spendId("Y"), "exports", 1, NOW - 5, SpendKind::Offline}};
    disk.set(latest);
  });
  h.license().refresh();
  // The two hooks are the call's first two saves: the held spend's, then the one that notes the run.
  EXPECT_GE(merge->asked() - savesBefore, 2);
  // The spend landed in that entry before the request was made: the request reports the entry, not the spend under an id of
  // its own.
  EXPECT_EQ(reported(h, 0), json::array({{{"id", spendId("Y")}, {"meter", "exports"}, {"n", 3}, {"at", NOW - 5}}}));
}

TEST(UseHeld, DoesNotMakeStatusAskAtEveryCallForSpendsHeldInMemoryThatNoRequestCarries) {
  MeterHarness h(exportsOnly(90, 100, 10));
  h.store(withBackoff(*h.stored()));
  renewal(h);
  h.storage->failWrites = true;
  h.use("exports", 2);
  h.setClock(NOW + 2 * MINUTE);
  for (int i = 0; i < 3; ++i) h.license().status();
  EXPECT_EQ(h.http->count("validate"), 0);
  // Landed, the journal asks as it does, once.
  h.storage->failWrites = false;
  h.license().status();
  EXPECT_EQ(h.http->count("validate"), 1);
}

TEST(UseHeld, ASpendHeldInMemoryIsNotReportedWithTheNextUseWhichCarriesNoSpendsOfItsOwn) {
  MeterHarness h;
  h.store(withBackoff(*h.stored()));
  h.storage->failWrites = true;
  h.use("exports", 2);
  h.answerUse(200, {{"spent", true},
                    {"meters", {{"renders", {{"left", 4}, {"total", 5}, {"offline", 0}, {"resets", true}}}}},
                    {"at", NOW}});
  EXPECT_EQ(h.use("renders"), UseResult::spent(4));
  EXPECT_EQ(h.useBody()["meter"], "renders");
  EXPECT_FALSE(h.useBody().contains("spends"));
}

TEST(UseHeld, StartsTheHoursWaitAfterAnAnswerOnlyWhenTheRequestCarriedAReportNotForAJournalOnlyItRead) {
  std::shared_ptr<MergeStorage> merge;
  MeterHarness h(exportsOnly(90, 100, 10), {}, mergingOptions(merge));
  h.store(withBackoff(*h.stored()));
  renewal(h);
  merge->failing = true;
  EXPECT_EQ(h.use("exports", 2), UseResult::spent(88, true));
  merge->failing = false;
  // The next call's first save fails and its answer's save lands the spend: it read a journal, and sent none of it.
  merge->before(1, [](MemoryStorage&) { throw keygrant::IoError("EBUSY", "the disk said no"); });
  h.license().refresh();
  EXPECT_EQ(reported(h, 0), json(nullptr));
  EXPECT_EQ(h.journal().size(), 1u);
  EXPECT_NE(h.stored()->backoffMs, kAskAgainMs);
  // So the journal that has landed is offered at the next status() once the minute's wait is over, not an hour on.
  h.setClock(NOW + 2 * MINUTE);
  h.license().status();
  EXPECT_EQ(h.http->count("validate"), 2);
  const json second = reported(h, 1);
  ASSERT_EQ(second.size(), 1u);
  EXPECT_EQ(second[0]["meter"], "exports");
  EXPECT_EQ(second[0]["n"], 2);
}

TEST(UseHeld, StartsTheHoursWaitWhenAnAnswerConfirmsNoneOfTheReportItCarried) {
  MeterHarness h(exportsOnly(90, 100, 10), {entry(SpendKind::Unanswered, "A", 3, NOW - 2 * HOUR)});
  renewal(h);
  h.license().refresh();
  // The server answered with a lease and named nothing recorded: a journal it will not take is not offered at every status.
  EXPECT_EQ(h.stored()->backoffMs, kAskAgainMs);
  EXPECT_EQ(h.stored()->backoffSince, NOW);
  EXPECT_EQ(h.journal().size(), 1u);
  h.setClock(NOW + 10 * MINUTE);
  h.license().status();
  EXPECT_EQ(h.http->count("validate"), 1);
  h.setClock(NOW + HOUR);
  h.license().status();
  EXPECT_EQ(h.http->count("validate"), 2);
}

// --- calls one at a time ------------------------------------------------------------------------------------------

TEST(UseHeld, RunsTwoCallsMadeTogetherOneAfterTheOtherEachASpendOfItsOwn) {
  MeterHarness h(exportsOnly(90, 100, 10));
  h.http->fail = true;
  auto a = std::async(std::launch::async, [&] { return h.use("exports", 3); });
  auto b = std::async(std::launch::async, [&] { return h.use("exports", 4); });
  const UseResult first = a.get();
  const UseResult second = b.get();
  EXPECT_TRUE(first.ok && first.offline);
  EXPECT_TRUE(second.ok && second.offline);
  // Whichever went first, the second saw what it left: 3 then 4 leaves 87 and 83, 4 then 3 leaves 86 and 83.
  const std::int64_t last = std::min(*first.left, *second.left);
  const std::int64_t earlier = std::max(*first.left, *second.left);
  EXPECT_EQ(last, 83);
  EXPECT_TRUE(earlier == 87 || earlier == 86) << earlier;
  EXPECT_EQ(earlier, *first.left == earlier ? 90 - 3 : 90 - 4);
  EXPECT_EQ(total(h.journal()), 7);
}

TEST(UseHeld, WaitsBehindAStatusAlreadyRunningAndSeesWhatItSaved) {
  MeterHarness h;
  const std::string lease = meterLease(h, exportsOnly(60, 100, 10), NOW + 1000);
  h.setClock(NOW + 1000);
  Gate release;
  h.http->on("validate", [&](const json&) {
    release.wait();
    return HttpResponse{200, json{{"lease", lease}}.dump()};
  });
  // The answer to the spend names no figure, so the one it reports is the lease's: 60 only if the spend read the renewed lease.
  h.answerUse(200, {{"spent", true}});
  auto refreshing = std::async(std::launch::async, [&] { return h.license().refresh(); });
  // Not fatal: a failure here must not leave the call waiting on the gate, and the run hanging on its future.
  EXPECT_TRUE(h.http->waitFor("validate"));
  auto spending = std::async(std::launch::async, [&] { return h.use("exports"); });
  // Behind the refresh, the spend has not been asked yet.
  std::this_thread::sleep_for(Millis{50});
  EXPECT_EQ(h.http->count("use"), 0);
  release.open();
  (void)refreshing.get();
  EXPECT_EQ(spending.get(), UseResult::spent(60));
}

// --- a disk that keeps refusing --------------------------------------------------------------------------------------

TEST(UseHeld, CostsTheProcessAShortListOfChangesHoweverManySpendsOfOneMeterItHoldsAndNoneIsLost) {
  MeterHarness h(exportsOnly(1'000'000, 1'000'000, 1'000'000));
  h.http->fail = true;
  h.storage->failWrites = true;
  for (int i = 0; i < 400; ++i) EXPECT_TRUE(h.use("exports").ok);
  const MeterStatus held = h.license().status().meters.at("exports");
  EXPECT_EQ(held.left, 1'000'000 - 400);
  EXPECT_EQ(held.unsynced, 400);
  // The process holds two changes, not four hundred: the first spend asked and got no answer (an entry of its own), and the
  // spends after it, which wait out the minute, fold into one.
  EXPECT_EQ(keygrant::detail::LicenseAccess::heldChanges(h.license()).size(), 2u);
  h.storage->failWrites = false;
  h.license().status();
  EXPECT_EQ(total(h.journal()), 400);
}

TEST(UseHeld, KeepsEverySpendOfTwoMetersThatTakeTurnsAndLandsEachMetersTotalWhenTheDiskIsBack) {
  MeterHarness h({{"exports", MeterSpec{1'000, 1'000, 1'000, false}}, {"credits", MeterSpec{1'000, 1'000, 1'000, false}}});
  h.http->fail = true;
  h.storage->failWrites = true;
  for (int i = 0; i < 40; ++i) {
    EXPECT_TRUE(h.use(i % 2 == 0 ? "exports" : "credits", 1 + i % 3).ok);
  }
  std::int64_t exports = 0;
  std::int64_t credits = 0;
  for (int i = 0; i < 40; ++i) (i % 2 == 0 ? exports : credits) += 1 + i % 3;
  const auto status = h.license().status().meters;
  EXPECT_EQ(status.at("exports").unsynced, exports);
  EXPECT_EQ(status.at("credits").unsynced, credits);
  h.storage->failWrites = false;
  h.license().status();
  std::int64_t landedExports = 0;
  std::int64_t landedCredits = 0;
  for (const SpendEntry& e : h.journal()) (e.meter == "exports" ? landedExports : landedCredits) += e.n;
  EXPECT_EQ(landedExports, exports);
  EXPECT_EQ(landedCredits, credits);
}

TEST(UseHeld, KeepsTheNewestVoidsInMemoryNoneOfThemToldAndRefusesNoCoveredSpendForThem) {
  MeterHarness h;
  h.http->fail = true;
  h.storage->failWrites = true;
  for (int minute = 0; minute < 20; ++minute) {
    h.setClock(NOW + minute * MINUTE);
    EXPECT_EQ(h.use("renders"), UseResult::refused(UseError::OfflineLimit, 5));
  }
  const std::vector<keygrant::detail::JournalOp> voids = keygrant::detail::LicenseAccess::heldChanges(h.license());
  EXPECT_EQ(std::count_if(voids.begin(), voids.end(), [](const auto& op) { return op.isVoid(); }),
            static_cast<std::ptrdiff_t>(kMaxPendingVoids));
  EXPECT_TRUE(std::none_of(voids.begin(), voids.end(), [](const auto& op) { return op.isVoid() && op.told; }));
  // A spend the allowance covers is made, whatever the uncovered ones did to what this process holds.
  EXPECT_EQ(h.use("exports", 3), UseResult::spent(87, true));
  h.storage->failWrites = false;
  h.license().status();
  std::vector<std::int64_t> voidTimes;
  std::vector<std::pair<std::string, std::int64_t>> spends;
  for (const SpendEntry& e : h.journal()) {
    if (e.kind == SpendKind::Void) {
      voidTimes.push_back(e.at);
    } else {
      spends.emplace_back(e.meter, e.n);
    }
  }
  // A void is no spend told: it lands under the journal's rules, the newest sixteen of the twenty.
  std::vector<std::int64_t> expected;
  for (int i = 0; i < static_cast<int>(kMaxPendingVoids); ++i) expected.push_back(NOW + (4 + i) * MINUTE);
  EXPECT_EQ(voidTimes, expected);
  EXPECT_EQ(spends, (std::vector<std::pair<std::string, std::int64_t>>{{"exports", 3}}));
}

// --- the allowance, judged where the spend lands -----------------------------------------------------------------------

/// The first save of a merging storage finds `theirs` already there: another process's entries.
void theirsFirst(const std::shared_ptr<MergeStorage>& merge, std::vector<SpendEntry> theirs) {
  merge->before(1, [theirs = std::move(theirs)](MemoryStorage& disk) {
    StoredState latest = *disk.get();
    latest.spends = theirs;
    latest.rev = 9;
    disk.set(latest);
  });
}

TEST(UseHeld, RefusesASpendThatAnotherProcessTookTheAllowanceFromMeanwhileAndKeepsTheirs) {
  std::shared_ptr<MergeStorage> merge;
  MeterHarness h(exportsOnly(90, 100, 10), {}, mergingOptions(merge));
  h.store(withBackoff(*h.stored()));
  theirsFirst(merge, {SpendEntry{spendId("T"), "exports", 8, NOW - 5, SpendKind::Offline}});
  // Read with nothing unsynced, so the allowance covers 5; by the time it lands, 8 are there: 13 would pass 10. What is
  // left is as the landing found it: the 90, less the 8 the other process had spent by then.
  EXPECT_EQ(h.use("exports", 5), UseResult::refused(UseError::OfflineLimit, 82));
  EXPECT_EQ(h.shape(), "offline:8");
  // What the allowance still covers lands.
  const UseResult covered = h.use("exports", 2);
  EXPECT_TRUE(covered.ok && covered.offline);
  EXPECT_EQ(h.shape(), "offline:10");
}

TEST(UseHeld, HoldsASpendToWhatIsLeftWhenThatIsBelowTheAllowanceTwoProcessesSpendingTheLastTwoOneLands) {
  std::shared_ptr<MergeStorage> merge;
  MeterHarness h(exportsOnly(2, 100, 50), {}, mergingOptions(merge));
  h.store(withBackoff(*h.stored()));
  theirsFirst(merge, {SpendEntry{spendId("T"), "exports", 2, NOW - 5, SpendKind::Offline}});
  // The allowance of 50 would take it; the 2 that are left, and the other process has spent them.
  EXPECT_EQ(h.use("exports", 2), UseResult::refused(UseError::OfflineLimit, 0));
  EXPECT_EQ(h.shape(), "offline:2");

  MeterHarness alone(exportsOnly(2, 100, 50));
  alone.store(withBackoff(*alone.stored()));
  EXPECT_EQ(alone.use("exports", 2), UseResult::spent(0, true));
  EXPECT_EQ(alone.use("exports", 1), UseResult::refused(UseError::OfflineLimit, 0));
}

TEST(UseHeld, JournalsAVoidForASpendThatWasInDoubtAndIsThenRefusedAtLanding) {
  std::shared_ptr<MergeStorage> merge;
  MeterHarness h(exportsOnly(90, 100, 10), {}, mergingOptions(merge));
  h.http->fail = true;
  theirsFirst(merge, {SpendEntry{spendId("T"), "exports", 8, NOW - 5, SpendKind::Offline}});
  EXPECT_EQ(h.use("exports", 5), UseResult::refused(UseError::OfflineLimit, 82));
  const std::string sent = h.useBody()["id"].get<std::string>();
  EXPECT_EQ(h.shape(), "offline:8 void:5");
  EXPECT_EQ(h.journal()[1].id, sent);
}

TEST(UseHeld, NeverTakesBackASpendTheCustomerWasToldWasMadeItLandsPastTheAllowance) {
  MeterHarness h(exportsOnly(90, 100, 10));
  h.store(withBackoff(*h.stored()));
  h.storage->failWrites = true;
  EXPECT_EQ(h.use("exports", 6), UseResult::spent(84, true));
  h.storage->failWrites = false;
  // Meanwhile another process of the app spent 8, and the journal is full.
  StoredState state = *h.stored();
  state.spends = {SpendEntry{spendId("T"), "exports", 8, NOW, SpendKind::Offline}};
  for (const SpendEntry& e : crowd(kMaxPendingSpends - 1)) state.spends.push_back(e);
  h.store(state);
  h.license().status();
  const std::vector<SpendEntry> journal = h.journal();
  EXPECT_EQ(journal.size(), kMaxPendingSpends);
  EXPECT_EQ(journal[0].id, spendId("T"));
  EXPECT_EQ(journal[0].n, 14);
  const MeterStatus shown = h.license().status().meters.at("exports");
  EXPECT_EQ(shown.unsynced, 14);
  EXPECT_EQ(shown.left, 76);
}

TEST(UseHeld, LandsASpendTheCustomerWasToldWasMadeAsA65thEntryPastTheCapWhenItOpensAnEntryOfItsOwn) {
  MeterHarness h(exportsOnly(90, 100, 10));
  h.store(withBackoff(*h.stored()));
  h.storage->failWrites = true;
  EXPECT_TRUE(h.use("exports", 2).ok);
  h.storage->failWrites = false;
  StoredState state = *h.stored();
  state.spends = crowd(kMaxPendingSpends);
  h.store(state);
  h.license().status();
  const std::vector<SpendEntry> journal = h.journal();
  ASSERT_EQ(journal.size(), kMaxPendingSpends + 1);
  EXPECT_EQ(journal.back().meter, "exports");
  EXPECT_EQ(journal.back().n, 2);
  EXPECT_EQ(journal.back().kind, SpendKind::Offline);
}

// --- what is left when a landing refuses a spend and the state cannot say --------------------------------------------------

TEST(UseHeld, Answers0LeftWhenTheStateCannotBeReadAgainJustAfterTheRefusalHoweverMuchTheFigureWas) {
  std::shared_ptr<MergeStorage> merge;
  MeterHarness h(exportsOnly(90, 100, 10), {}, mergingOptions(merge));
  h.store(withBackoff(*h.stored()));
  merge->before(1, [](MemoryStorage& disk) {
    StoredState latest = *disk.get();
    latest.spends = {SpendEntry{spendId("T"), "exports", 8, NOW - 5, SpendKind::Offline}};
    latest.rev = 9;
    disk.set(latest);
    // Every read fails from the save on.
    disk.failReads = true;
  });
  EXPECT_EQ(h.use("exports", 5), UseResult::refused(UseError::OfflineLimit, 0));
  h.storage->failReads = false;
  // The refusal stands, and their spend is kept: only the figure was lost to the customer.
  EXPECT_EQ(h.shape(), "offline:8");
}

TEST(UseHeld, Answers0LeftWhenTheStateAsTheLandingFoundItHasNoFigureForTheMeterAnyMore) {
  // The lease names no credits; the figure is a use answer's, stored beside it, and the other process's state dropped it.
  auto stateWithCredits = [](MeterHarness& h) {
    StoredState state = withBackoff(*h.stored());
    state.meterFigures["credits"] = MeterFigure{20, 20, 10, false, NOW};
    h.store(state);
  };
  std::shared_ptr<MergeStorage> merge;
  MeterHarness h(defaultMeters(), {}, mergingOptions(merge));
  stateWithCredits(h);
  EXPECT_EQ(h.meters().at("credits").left, 20);
  merge->before(1, [](MemoryStorage& disk) {
    StoredState latest = *disk.get();
    latest.spends = {SpendEntry{spendId("T"), "credits", 8, NOW - 5, SpendKind::Offline}};
    latest.meterFigures.clear();
    latest.rev = 9;
    disk.set(latest);
  });
  EXPECT_EQ(h.use("credits", 5), UseResult::refused(UseError::OfflineLimit, 0));
  ASSERT_EQ(h.journal().size(), 1u);
  EXPECT_EQ(h.journal()[0].meter, "credits");
  EXPECT_EQ(h.journal()[0].n, 8);

  // With the figure kept, the same refusal says what is left: 20 less their 8.
  std::shared_ptr<MergeStorage> keptMerge;
  MeterHarness kept(defaultMeters(), {}, mergingOptions(keptMerge));
  stateWithCredits(kept);
  theirsFirst(keptMerge, {SpendEntry{spendId("T"), "credits", 8, NOW - 5, SpendKind::Offline}});
  EXPECT_EQ(kept.use("credits", 5), UseResult::refused(UseError::OfflineLimit, 12));
}

// --- what an answer stamps --------------------------------------------------------------------------------------------------

/// The state after one spend answered as `arrange` says, from a state whose last answer was asked for at 111.
StoredState unconfirmed(const std::function<void(MeterHarness&)>& arrange) {
  MeterHarness h;
  StoredState state = *h.stored();
  state.verdictAt = 111;
  h.store(state);
  arrange(h);
  h.use("exports");
  return *h.stored();
}

TEST(UseHeld, LeavesVerdictAtAloneForASpendUsedUpAndAnUnknownMeterTheyAreNoVerdict) {
  EXPECT_EQ(unconfirmed([](MeterHarness& h) { h.answerSpent(89, NOW + 5); }).verdictAt, 111);
  EXPECT_EQ(unconfirmed([](MeterHarness& h) {
              h.answerUse(409, {{"error", "used-up"}, {"left", 0}, {"at", NOW}});
            }).verdictAt,
            111);
  EXPECT_EQ(unconfirmed([](MeterHarness& h) { h.answerUse(404, {{"error", "unknown-meter"}}); }).verdictAt, 111);
}

TEST(UseHeld, SetsItForAVerdictAndForAWaitAsAValidatesAnswerDoes) {
  EXPECT_EQ(unconfirmed([](MeterHarness& h) { h.answerUse(403, {{"error", "revoked"}}); }).verdictAt, NOW);
  EXPECT_EQ(unconfirmed([](MeterHarness& h) { h.http->fail = true; }).verdictAt, NOW);
}

TEST(UseHeld, LetsAValidateThatWasInFlightAcrossASpendKeepItsLease) {
  MeterHarness h;
  auto other = h.licenseFor(1);
  const std::string renewed = meterLease(h, defaultMeters(), NOW + 1'000);
  Gate release;
  h.http->on("validate", [&](const json&) {
    release.wait();
    return HttpResponse{200, json{{"lease", renewed}}.dump()};
  });
  h.answerSpent(89, NOW + 2'000);
  auto validating = std::async(std::launch::async, [&] { return h.license().refresh(); });
  EXPECT_TRUE(h.http->waitFor("validate"));
  // Another process spends, asked later and answered first.
  h.setClock(NOW + 2'000);
  EXPECT_EQ(other->use("exports"), UseResult::spent(89));
  EXPECT_EQ(h.stored()->verdictAt, std::nullopt);
  release.open();
  (void)validating.get();
  EXPECT_EQ(h.stored()->lease, renewed);
}

// --- a spend kept while a save is in flight ------------------------------------------------------------------------------------

TEST(UseHeld, ASpendKeptWhileASaveIsInFlightIsCountedOnceBesideTheOneThatSaveCarries) {
  HarnessOptions options;
  options.httpTimeout = Millis{100};
  options.timings = shortTimings();
  MeterHarness h(exportsOnly(90, 100, 50), {}, options);
  StoredState state = withBackoff(*h.stored());
  state.lastSeen = NOW;
  h.store(state);
  // A: kept in memory because the disk refused it.
  h.storage->failWrites = true;
  const UseResult a = h.use("exports", 2);
  EXPECT_TRUE(a.ok && a.offline);
  h.storage->failWrites = false;
  // A save in flight carries A and is held inside the storage.
  h.http->fail = true;
  h.storage->holdNextWrite();
  auto refreshing = std::async(std::launch::async, [&] { return h.license().refresh(); });
  EXPECT_TRUE(h.storage->writeGate().waitReached(Millis{5'000}));
  // B, of the same meter, is made behind it (read-only once the call before has outlived its bound): kept in memory, beside A.
  const UseResult b = h.use("exports", 3);
  EXPECT_TRUE(b.ok && b.offline);
  h.storage->writeGate().open();
  (void)refreshing.get();
  h.license().status();
  EXPECT_EQ(total(h.journal()), 5);
  EXPECT_EQ(h.license().status().meters.at("exports").unsynced, 5);
}

// --- a spend the allowance cannot cover, while nothing answers ------------------------------------------------------------------

TEST(UseHeld, AsksOnceAMinuteKeepsAtMostTheCapOfVoidsAndLeavesTheJournalItsRoom) {
  MeterHarness h;
  h.http->fail = true;
  for (int minute = 0; minute < 30; ++minute) {
    h.setClock(NOW + minute * MINUTE);
    EXPECT_EQ(h.use("renders"), UseResult::refused(UseError::OfflineLimit, 5));
    // The same minute: the void is a wait of its own, and nothing is asked.
    EXPECT_EQ(h.use("renders"), UseResult::refused(UseError::OfflineLimit, 5));
  }
  EXPECT_EQ(h.http->count("use"), 30);
  std::vector<std::int64_t> voidTimes;
  for (const SpendEntry& e : h.journal()) {
    if (e.kind == SpendKind::Void) voidTimes.push_back(e.at);
  }
  // The newest sixteen stay: the first void to go was the oldest.
  std::vector<std::int64_t> expected;
  for (int i = 0; i < static_cast<int>(kMaxPendingVoids); ++i) expected.push_back(NOW + (14 + i) * MINUTE);
  EXPECT_EQ(voidTimes, expected);
  // A spend on another meter that its allowance covers is made, whatever the uncovered ones did to the journal.
  EXPECT_EQ(h.use("exports", 3), UseResult::spent(87, true));
  EXPECT_EQ(h.journal().size(), kMaxPendingVoids + 1);
}

TEST(UseHeld, AsksAgainWhenTheDeviceClockIsSetBackBelowTheVoidThoughTheGuardedClocksMinuteIsStillRunning) {
  MeterHarness h;
  h.http->fail = true;
  h.setClock(NOW + 10 * MINUTE);
  EXPECT_EQ(h.use("renders").error, UseError::OfflineLimit);
  // Inside the void's minute by both clocks: nothing is asked.
  h.setClock(NOW + 10 * MINUTE + 1000);
  EXPECT_EQ(h.use("renders").error, UseError::OfflineLimit);
  EXPECT_EQ(h.http->count("use"), 1);
  // The device clock is set back to before the void while the guard stands at its minute: the wait is none, and a
  // customer who is online is served.
  h.http->fail = false;
  h.answerUse(200, {{"spent", true},
                    {"meters", {{"renders", {{"left", 4}, {"total", 5}, {"offline", 0}, {"resets", true}}}}},
                    {"at", NOW}});
  h.setClock(NOW);
  EXPECT_EQ(h.use("renders"), UseResult::spent(4));
  EXPECT_EQ(h.http->count("use"), 2);
}

TEST(UseHeld, IsServedAtOnceWhenTheServerIsBackAndItsMinuteIsOverNotBefore) {
  MeterHarness h;
  h.http->fail = true;
  EXPECT_EQ(h.use("renders").error, UseError::OfflineLimit);
  h.http->fail = false;
  h.answerUse(200, {{"spent", true},
                    {"meters", {{"renders", {{"left", 4}, {"total", 5}, {"offline", 0}, {"resets", true}}}}},
                    {"at", NOW + MINUTE}});
  // Inside the void's minute: refused without asking.
  h.setClock(NOW + MINUTE - 1);
  EXPECT_EQ(h.use("renders").error, UseError::OfflineLimit);
  EXPECT_EQ(h.http->count("use"), 1);
  h.setClock(NOW + MINUTE);
  EXPECT_EQ(h.use("renders"), UseResult::spent(4));
}

TEST(UseHeld, AsksForAnUncoveredMeterWhateverTheCoveredMetersUnansweredSpendWaitsForAndWhileAVoidIsAheadOfTheClock) {
  MeterHarness h(defaultMeters(), {entry(SpendKind::Unanswered, "U", 1, NOW - 1'000), entry(SpendKind::Void, "V", 1, NOW + DAY)});
  h.answerUse(200, {{"spent", true},
                    {"meters", {{"renders", {{"left", 4}, {"total", 5}, {"offline", 0}, {"resets", true}}}}},
                    {"at", NOW}});
  EXPECT_EQ(h.use("renders"), UseResult::spent(4));
}

TEST(UseHeld, LeavesOutOfAWaitOnlyTheEntriesAheadOfTheGuardedClockAndTheOthersWaitAsTheyWouldAlone) {
  const SpendEntry ahead = entry(SpendKind::Unanswered, "Z", 1, NOW + 5 * DAY);
  MeterHarness waiting(defaultMeters(), {entry(SpendKind::Unanswered, "A", 1, NOW - 30'000), ahead});
  waiting.answerSpent(89);
  waiting.use("exports");
  EXPECT_EQ(waiting.http->count("use"), 0);
  MeterHarness over(defaultMeters(), {entry(SpendKind::Unanswered, "A", 1, NOW - 2 * MINUTE), ahead});
  over.answerSpent(89);
  over.use("exports");
  EXPECT_EQ(over.http->count("use"), 1);
}

}  // namespace
}  // namespace kgtest
