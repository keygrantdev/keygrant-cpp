// The rules of metered uses, decided without I/O: what a call may ask for, the figures a device holds, what it may spend
// with no answer, when a spend asks the server, and what an answer does. The vectors pin each of them against the reference;
// these pin the edges the vectors do not reach, from the contract.
#include <limits>

#include "meter_support.hpp"

namespace kgtest {
namespace {

using namespace keygrant::detail;
using keygrant::kMaxMeterUses;
using keygrant::Refusal;

constexpr std::int64_t TOP = kMaxSafeInteger;

std::string asked(const std::string& meter, std::int64_t n) {
  const UseAsked a = askedFor(meter, n);
  return a.refused ? keygrant::to_string(*a.refused) : a.meter + "x" + std::to_string(a.n);
}

// --- what was asked ---------------------------------------------------------------------------------------------

TEST(MeterRules, TakesAWholeNumberFrom1ToTheBoundAndJudgesTheAmountBeforeTheCode) {
  EXPECT_EQ(asked("exports", 1), "exportsx1");
  EXPECT_EQ(asked("exports", kMaxMeterUses), "exportsx1000000000");
  for (const std::int64_t n : {std::int64_t{0}, std::int64_t{-1}, kMaxMeterUses + 1, std::numeric_limits<std::int64_t>::min(),
                               std::numeric_limits<std::int64_t>::max()}) {
    EXPECT_EQ(asked("exports", n), "invalid-amount") << n;
  }
  EXPECT_EQ(asked("", 0), "invalid-amount");
  EXPECT_EQ(asked("", 1), "no-meter");
}

TEST(MeterRules, CountsACodeInUtf16UnitsThirtyTwoAreACodeThirtyThreeAreNotASurrogatePairIsTwo) {
  EXPECT_EQ(asked(std::string(32, 'x'), 1), std::string(32, 'x') + "x1");
  EXPECT_EQ(asked(std::string(33, 'x'), 1), "no-meter");
  const std::string pair = "\xF0\x9F\x98\x80";
  EXPECT_EQ(asked(std::string(30, 'x') + pair, 1), std::string(30, 'x') + pair + "x1");
  EXPECT_EQ(asked(std::string(31, 'x') + pair, 1), "no-meter");
  std::string sixteen;
  for (int i = 0; i < 16; ++i) sixteen += pair;
  EXPECT_EQ(asked(sixteen, 1), sixteen + "x1");
  EXPECT_EQ(asked(sixteen + pair, 1), "no-meter");
  // A character beyond ASCII is one unit, and the code is matched as typed: case included.
  EXPECT_EQ(asked("\xC3\xA9x\xC3\xA9", 2), "\xC3\xA9x\xC3\xA9x2");
  EXPECT_EQ(asked("EXPORTS", 1), "EXPORTSx1");
}

// --- the figures --------------------------------------------------------------------------------------------------

Meters leased() { return Meters{{"exports", BaseFigure{90, 100, 10, false}}, {"renders", BaseFigure{5, 5, 0, true}}}; }

std::map<std::string, MeterFigure> storedAt(std::int64_t at) {
  return {{"exports", MeterFigure{40, 80, 3, true, at}}};
}

TEST(MeterRules, BaseFiguresAreTheLeasesWhenNothingIsStoredAndAStoredFigureFromTheLeasesOwnSecondOn) {
  const std::int64_t iat = NOW / 1000;
  EXPECT_EQ(figuresOf(leased(), iat, {}), leased());
  EXPECT_EQ(figuresOf(leased(), iat, storedAt(NOW - 1)).at("exports"), (BaseFigure{90, 100, 10, false}));
  EXPECT_EQ(figuresOf(leased(), iat, storedAt(NOW)).at("exports").left, 40);
  EXPECT_EQ(figuresOf(leased(), iat, storedAt(NOW + 1)).at("exports").left, 40);
}

TEST(MeterRules, AStoredFigureReplacesTheLeasesWholeMemberItsTotalItsAllowanceAndItsResetsToo) {
  EXPECT_EQ(figuresOf(leased(), NOW / 1000, storedAt(NOW)).at("exports"), (BaseFigure{40, 80, 3, true}));
}

TEST(MeterRules, NamesAMeterOnlyANewerStoredFigureNamesAndDropsOneTheNewestLeaseNoLongerNames) {
  const std::int64_t iat = NOW / 1000;
  const std::map<std::string, MeterFigure> credits = {{"credits", MeterFigure{9, 9, 1, false, NOW}}};
  EXPECT_EQ(figuresOf(leased(), iat, credits).count("credits"), 1u);
  // An older figure of a meter the lease does not name: gone.
  EXPECT_EQ(figuresOf(leased(), iat, {{"credits", MeterFigure{9, 9, 1, false, NOW - 1000}}}).count("credits"), 0u);
  // A lease that names none, and a lease with no claim at all.
  EXPECT_TRUE(figuresOf(Meters{}, iat, {{"exports", MeterFigure{9, 9, 1, false, NOW - 1000}}}).empty());
  EXPECT_EQ(figuresOf(std::nullopt, iat, credits).count("credits"), 1u);
  EXPECT_TRUE(figuresOf(std::nullopt, iat, {}).empty());
}

// --- what a spend with no answer may be -----------------------------------------------------------------------------------

TEST(MeterRules, ASpendIsAllowedUpToTheAllowanceAndUpToWhatIsLeftAndAnswersWhatIsLeftAfterIt) {
  const OfflineVerdict ok = offlineSpend(leased(), {}, "exports", 10);
  EXPECT_TRUE(ok.allowed());
  EXPECT_EQ(ok.left, 80);
  EXPECT_EQ(ok.limit, 10);
  EXPECT_EQ(offlineSpend(leased(), {entry(SpendKind::Offline, "A", 4, NOW)}, "exports", 6).left, 80);
  // The most the unsynced spends may come to is the smaller of the allowance and what is left.
  EXPECT_EQ(offlineSpend(Meters{{"x", BaseFigure{3, 100, 50, false}}}, {}, "x", 1).limit, 3);
}

TEST(MeterRules, ASpendIsRefusedPastEitherWithWhatIsLeftAndAlwaysForAnAllowanceOf0) {
  const OfflineVerdict past = offlineSpend(leased(), {entry(SpendKind::Unanswered, "A", 4, NOW)}, "exports", 7);
  EXPECT_EQ(past.kind, OfflineVerdict::Kind::OfflineLimit);
  EXPECT_EQ(past.left, 86);
  EXPECT_EQ(offlineSpend(Meters{{"x", BaseFigure{3, 100, 50, false}}}, {}, "x", 4).left, 3);
  EXPECT_EQ(offlineSpend(leased(), {}, "renders", 1).kind, OfflineVerdict::Kind::OfflineLimit);
  EXPECT_EQ(offlineSpend(leased(), {}, "renders", 1).left, 5);
}

TEST(MeterRules, CountsTheUnsyncedOfItsOwnMeterAndOfBothKindsNeverAVoidAndKnowsNoMeterWithoutAFigure) {
  const std::vector<SpendEntry> spends = {entry(SpendKind::Offline, "A", 2, NOW), entry(SpendKind::Unanswered, "B", 3, NOW),
                                          entry(SpendKind::Void, "C", 9, NOW), entry(SpendKind::Offline, "D", 7, NOW, "renders")};
  EXPECT_EQ(unsyncedOf(spends, "exports"), 5);
  EXPECT_EQ(unsyncedOf(spends, "renders"), 7);
  EXPECT_EQ(unsyncedOf(spends, "credits"), 0);
  EXPECT_EQ(offlineSpend(leased(), spends, "credits", 1).kind, OfflineVerdict::Kind::NoMeter);
}

TEST(MeterRules, HoldsTheSumsToTheTopOfWhatItHoldsNotPastItAndTheLeftToZero) {
  const std::vector<SpendEntry> top = {entry(SpendKind::Offline, "A", TOP, NOW), entry(SpendKind::Unanswered, "B", TOP, NOW)};
  EXPECT_EQ(unsyncedOf(top, "exports"), TOP);
  const auto shown = meterStatusOf(Meters{{"exports", BaseFigure{TOP, TOP, TOP, false}}}, top);
  EXPECT_EQ(shown.at("exports").unsynced, TOP);
  EXPECT_EQ(shown.at("exports").left, 0);
  const OfflineVerdict refused = offlineSpend(Meters{{"exports", BaseFigure{TOP, TOP, TOP, false}}}, top, "exports", kMaxMeterUses);
  EXPECT_EQ(refused.kind, OfflineVerdict::Kind::OfflineLimit);
  EXPECT_EQ(refused.left, 0);
  EXPECT_EQ(saturatingSum(TOP, TOP), TOP);
  EXPECT_EQ(saturatingSum(TOP - 1, 1), TOP);
  EXPECT_EQ(saturatingSum(0, 0), 0);
}

TEST(MeterRules, DecidesOnFiguresAtTheEndsOfWhatItHoldsWithoutOverflowing) {
  // A lease second so late that its millisecond passes every stored time: nothing stored is newer, nothing multiplies out of range.
  EXPECT_EQ(figuresOf(leased(), TOP, {{"exports", MeterFigure{1, 1, 1, false, TOP}}}).at("exports").left, 90);
  EXPECT_EQ(figuresOf(leased(), TOP / 1000, {{"exports", MeterFigure{1, 1, 1, false, TOP}}}).at("exports").left, 1);
  EXPECT_EQ(figuresOf(leased(), 0, {{"exports", MeterFigure{1, 1, 1, false, 0}}}).at("exports").left, 1);
}

// --- when to ask --------------------------------------------------------------------------------------------------------------

std::vector<SpendEntry> unanswered(int k, std::int64_t ago) {
  std::vector<SpendEntry> out;
  for (int i = 0; i < k; ++i) out.push_back(entry(SpendKind::Unanswered, "u" + std::to_string(i), 1, NOW - ago));
  return out;
}

bool asks(const StoredState& state, bool covered, std::int64_t clockNow = NOW, std::int64_t now = NOW) {
  return useAsksNow(state, clockNow, now, covered);
}

StoredState waitingSince(std::int64_t since, std::int64_t ms) { return withBackoff(StoredState{}, since, ms); }

TEST(MeterRules, AsksEveryTimeButWhileANoAnswerWaitRunsOnTheDeviceClock) {
  EXPECT_TRUE(asks(StoredState{}, true));
  EXPECT_FALSE(asks(waitingSince(NOW - 30'000, kRetryUnansweredMs), true));
  // At the end of the minute, and with a longer wait a validate set: asked.
  EXPECT_TRUE(asks(waitingSince(NOW - kRetryUnansweredMs, kRetryUnansweredMs), true));
  EXPECT_TRUE(asks(waitingSince(NOW - 30'000, kAskAgainMs), true));
  // A wait that begins after the device clock is no wait: the clock was set back.
  EXPECT_TRUE(asks(waitingSince(NOW + 1, kRetryUnansweredMs), true));
  // A wait with no length.
  StoredState half;
  half.backoffSince = NOW;
  EXPECT_TRUE(asks(half, true));
}

TEST(MeterRules, WaitsAMinuteAfterOneUnansweredSpendDoublingWithEachToAnHourFromTheSeventhOn) {
  const std::int64_t waits[] = {1, 2, 4, 8, 16, 32, 60, 60, 60};
  for (int k = 1; k <= 9; ++k) {
    const std::int64_t wait = waits[k - 1] * MINUTE;
    StoredState state;
    state.spends = unanswered(k, 0);
    EXPECT_TRUE(journalWaiting(state.spends, NOW + wait - 1)) << k;
    EXPECT_FALSE(journalWaiting(state.spends, NOW + wait)) << k;
  }
  // A journal of a thousand entries is the hour, with nothing overflowing.
  std::vector<SpendEntry> many;
  for (int i = 0; i < 1000; ++i) many.push_back(entry(SpendKind::Unanswered, "m", 1, NOW));
  EXPECT_TRUE(journalWaiting(many, NOW + HOUR - 1));
  EXPECT_FALSE(journalWaiting(many, NOW + HOUR));
}

TEST(MeterRules, CountsAVoidTowardTheWaitAnOfflineSpendNotAndGoesByTheNewestEntryWhateverTheOrder) {
  EXPECT_TRUE(journalWaiting({entry(SpendKind::Void, "v", 1, NOW)}, NOW + 59'999));
  EXPECT_FALSE(journalWaiting({entry(SpendKind::Offline, "o", 1, NOW)}, NOW));
  EXPECT_FALSE(journalWaiting({}, NOW));
  // The newest by time, in either order: two entries wait two minutes after the newer.
  const SpendEntry older = entry(SpendKind::Unanswered, "a", 1, NOW - 90'000);
  const SpendEntry newer = entry(SpendKind::Unanswered, "b", 1, NOW - 30'000);
  EXPECT_TRUE(journalWaiting({older, newer}, NOW));
  EXPECT_TRUE(journalWaiting({newer, older}, NOW));
  EXPECT_FALSE(journalWaiting({newer, older}, NOW - 30'000 + 2 * MINUTE));
  EXPECT_FALSE(journalWaiting({older, newer}, NOW - 30'000 + 2 * MINUTE));
}

TEST(MeterRules, JudgesTheJournalsWaitByTheGuardedClockAndTheOtherByTheDeviceClock) {
  StoredState state = waitingSince(NOW, kRetryUnansweredMs);
  state.spends = unanswered(1, 2 * HOUR);
  // The journal's wait is over long ago; the no-answer wait runs by the device clock, however the guarded one stands.
  EXPECT_FALSE(asks(state, true, NOW + 10, NOW + 2 * MINUTE));
  EXPECT_TRUE(asks(state, true, NOW + MINUTE, NOW + MINUTE));
  StoredState journal;
  journal.spends = unanswered(1, 0);
  // The journal's wait runs by the guarded clock: a device clock set back is no end to it.
  EXPECT_FALSE(asks(journal, true, NOW - HOUR, NOW + 30'000));
  EXPECT_TRUE(asks(journal, true, NOW - HOUR, NOW + MINUTE));
}

TEST(MeterRules, AsksWhateverTheWaitsASpendTheAllowanceWouldRefuseAndOneItCoversOnlyWhenNoWaitRuns) {
  StoredState state = waitingSince(NOW, kRetryUnansweredMs);
  state.spends = unanswered(3, 1000);
  EXPECT_FALSE(asks(state, true));
  EXPECT_TRUE(asks(state, false));
}

TEST(MeterRules, GivesTheUncoveredSpendAWaitOfItsOwnAVoidOpenedLessThanAMinuteAgoNoneAheadOfEitherClock) {
  StoredState state;
  state.spends = {entry(SpendKind::Void, "v", 1, NOW - 59'999)};
  EXPECT_FALSE(asks(state, false));
  state.spends = {entry(SpendKind::Void, "v", 1, NOW - kRetryUnansweredMs)};
  EXPECT_TRUE(asks(state, false));
  // Not a void that is ahead of the guarded clock; not one ahead of the device clock either.
  state.spends = {entry(SpendKind::Void, "v", 1, NOW + 1000)};
  EXPECT_TRUE(asks(state, false, NOW + 2000, NOW));
  state.spends = {entry(SpendKind::Void, "v", 1, NOW - 1000)};
  EXPECT_FALSE(asks(state, false, NOW, NOW));
  EXPECT_TRUE(asks(state, false, NOW - 2000, NOW));
  // An unanswered spend is no void: it does not stop an uncovered spend.
  state.spends = unanswered(1, 1000);
  EXPECT_TRUE(asks(state, false));
}

TEST(MeterRules, HasNoWaitToMeasureFromAnEntryOpenedAfterTheGuardedClockAndWaitsFromOneOpenedAtIt) {
  EXPECT_FALSE(journalWaiting({entry(SpendKind::Unanswered, "a", 1, NOW + 1)}, NOW));
  EXPECT_TRUE(journalWaiting({entry(SpendKind::Unanswered, "a", 1, NOW)}, NOW));
  // Left out of the count too: one behind and one ahead wait as the one behind alone, a minute.
  EXPECT_TRUE(journalWaiting({entry(SpendKind::Unanswered, "a", 1, NOW - 30'000), entry(SpendKind::Unanswered, "b", 1, NOW + 1)}, NOW));
  EXPECT_FALSE(journalWaiting({entry(SpendKind::Unanswered, "a", 1, NOW - 60'000), entry(SpendKind::Unanswered, "b", 1, NOW + 1)}, NOW));
}

TEST(MeterRules, AValidLeaseAsksToReportAJournalThatHoldsAnEntryOnlyWhileItsWaitIsNotRunning) {
  EXPECT_FALSE(journalAsks({}, NOW));
  EXPECT_TRUE(journalAsks({entry(SpendKind::Offline, "o", 1, NOW)}, NOW));
  EXPECT_FALSE(journalAsks({entry(SpendKind::Unanswered, "u", 1, NOW)}, NOW + 59'999));
  EXPECT_TRUE(journalAsks({entry(SpendKind::Unanswered, "u", 1, NOW)}, NOW + kRetryUnansweredMs));
  // An offline entry beside an unanswered one does not lengthen the wait.
  EXPECT_FALSE(journalAsks({entry(SpendKind::Offline, "o", 1, NOW), entry(SpendKind::Unanswered, "u", 1, NOW)}, NOW + 1000));
}

// --- what an answer does --------------------------------------------------------------------------------------------------------------

UseOutcome outcomeOf(const std::optional<Reply>& response, std::optional<DeviceKind> kind = std::nullopt) {
  UseInput input;
  input.response = response ? &*response : nullptr;
  input.askedAt = NOW;
  input.major = 3;
  input.kind = kind;
  input.ownClaim = "claim";
  return useOutcome(input);
}

Reply answer(int status, const json& body) { return Reply{status, body}; }

TEST(MeterRules, ASpentAnswerIsASpendWithTheFiguresStampedByTheServersTime) {
  const UseOutcome spent = outcomeOf(answer(200, spentBody(80, NOW + 7)));
  EXPECT_EQ(spent.kind, UseOutcomeKind::Spent);
  EXPECT_EQ(spent.figures.at("exports"), (MeterFigure{80, 100, 10, false, NOW + 7}));
  EXPECT_TRUE(spent.patch == StatePatch{});
  // Without a whole time it gives no figures; a spend all the same.
  for (const json& at : {json(nullptr), json(-1), json(1.5), json("7"), json(9007199254740993.0)}) {
    json body = spentBody(80);
    body["at"] = at;
    const UseOutcome undated = outcomeOf(answer(200, body));
    EXPECT_EQ(undated.kind, UseOutcomeKind::Spent) << at;
    EXPECT_TRUE(undated.figures.empty()) << at;
  }
}

TEST(MeterRules, UsedUpCarriesWhatTheAnswerSaysIsLeftOnAnyStatusButA200A5xxAnd429) {
  for (const int status : {400, 404, 409, 418, 499}) {
    const UseOutcome usedUp = outcomeOf(answer(status, {{"error", "used-up"}, {"left", 4}}));
    EXPECT_EQ(usedUp.kind, UseOutcomeKind::UsedUp) << status;
    EXPECT_EQ(usedUp.left, 4) << status;
  }
  for (const int status : {500, 503, 429}) {
    EXPECT_EQ(outcomeOf(answer(status, {{"error", "used-up"}, {"left", 4}})).kind, UseOutcomeKind::Unanswered) << status;
  }
  EXPECT_EQ(outcomeOf(answer(200, {{"error", "used-up"}, {"left", 4}})).kind, UseOutcomeKind::Unanswered);
  EXPECT_EQ(outcomeOf(answer(409, {{"error", "used-up"}, {"left", TOP}})).left, TOP);
  EXPECT_EQ(outcomeOf(answer(409, {{"error", "used-up"}, {"left", -1}})).left, std::nullopt);
}

TEST(MeterRules, StoresAVerdictAsAValidateDoesAndAMissingAnswerAsTheWaitOfAnUnansweredRequest) {
  const UseOutcome verdict = outcomeOf(answer(403, {{"error", "update-required"}}), DeviceKind::Machine);
  EXPECT_EQ(verdict.kind, UseOutcomeKind::Refused);
  EXPECT_EQ(verdict.refusal, Refusal::Outdated);
  EXPECT_EQ(verdict.patch.refused.get(), Refusal::Outdated);
  EXPECT_EQ(verdict.patch.refusedMajor.get(), 3);
  EXPECT_EQ(verdict.patch.checkedMajor.get(), 3);
  EXPECT_EQ(verdict.patch.backoffMs.get(), kAskAgainMs);
  EXPECT_EQ(verdict.patch.deviceKind.get(), DeviceKind::Machine);
  const UseOutcome device = outcomeOf(answer(403, {{"error", "device"}}));
  EXPECT_EQ(device.patch.refusedFor.get(), std::vector<std::string>{"claim"});

  const UseOutcome none = outcomeOf(std::nullopt, DeviceKind::Machine);
  EXPECT_EQ(none.kind, UseOutcomeKind::Unanswered);
  EXPECT_EQ(none.patch.backoffSince.get(), NOW);
  EXPECT_EQ(none.patch.backoffMs.get(), kRetryUnansweredMs);
  EXPECT_EQ(none.patch.deviceKind.get(), DeviceKind::Machine);
  // An answer that is no answer, or no word, saves nothing at all.
  const std::vector<std::pair<int, json>> unusable = {{503, json::object()},
                                                      {404, {{"error", "no-such-thing"}}},
                                                      {200, json::array()},
                                                      {400, "text"}};
  for (const auto& [status, body] : unusable) {
    SCOPED_TRACE(status);
    const UseOutcome nothing = outcomeOf(answer(status, body), DeviceKind::Machine);
    EXPECT_EQ(nothing.kind, UseOutcomeKind::Unanswered);
    EXPECT_TRUE(nothing.patch == StatePatch{});
    EXPECT_TRUE(nothing.confirmed.empty());
  }
}

TEST(MeterRules, ReportsAreTheJournalOldestFirstWithAVoidMarkedNoneForNoneAndAtMost64) {
  EXPECT_FALSE(spendsReported({}).has_value());
  const auto report = spendsReported({entry(SpendKind::Void, "A", 2, NOW), entry(SpendKind::Offline, "B", 4, NOW + 1)});
  ASSERT_TRUE(report.has_value());
  EXPECT_EQ(json::parse(report->dump()), (json::array({{{"id", spendId("A")}, {"meter", "exports"}, {"n", 2}, {"at", NOW}, {"void", true}},
                                                      {{"id", spendId("B")}, {"meter", "exports"}, {"n", 4}, {"at", NOW + 1}}})));
  std::vector<SpendEntry> many;
  for (int i = 0; i < 70; ++i) many.push_back(entry(SpendKind::Unanswered, "n" + std::to_string(i), 1, NOW));
  EXPECT_EQ(spendsReported(many)->size(), kMaxPendingSpends);
  EXPECT_EQ((*spendsReported(many))[63]["id"], spendId("n63"));
}

TEST(MeterRules, ReportsAreConfirmedByAnAnswerThatIsNotA5xxA429OrNoneReadLeniently) {
  const json body = {{"spends", json::array({{{"id", "a"}, {"n", 3}},
                                             {{"id", "b"}, {"void", true}},
                                             {{"id", "c"}, {"void", true}, {"n", "ignored"}},
                                             {{"id", "d"}, {"n", 0}},
                                             {{"id", "e"}, {"n", 1.5}},
                                             {{"id", 4}, {"n", 1}},
                                             {{"id", "f"}, {"void", "true"}},
                                             {{"id", "g"}, {"n", 9007199254740991.0}},
                                             {{"id", "h"}, {"n", 9007199254740993.0}},
                                             "x",
                                             nullptr})}};
  const Reply ok = answer(200, body);
  EXPECT_EQ(spendsConfirmed(&ok), (std::vector<Confirmed>{Confirmed::count("a", 3), Confirmed::taken("b"), Confirmed::taken("c"),
                                                          Confirmed::count("g", 9007199254740991LL)}));
  for (const int status : {403, 404, 409}) {
    const Reply other = answer(status, body);
    EXPECT_EQ(spendsConfirmed(&other).size(), 4u) << status;
  }
  for (const int status : {500, 503, 429}) {
    const Reply down = answer(status, body);
    EXPECT_TRUE(spendsConfirmed(&down).empty()) << status;
  }
  EXPECT_TRUE(spendsConfirmed(nullptr).empty());
  const Reply list = answer(200, json::array({1}));
  EXPECT_TRUE(spendsConfirmed(&list).empty());
  const Reply notAList = answer(200, {{"spends", "x"}});
  EXPECT_TRUE(spendsConfirmed(&notAList).empty());
}

}  // namespace
}  // namespace kgtest
