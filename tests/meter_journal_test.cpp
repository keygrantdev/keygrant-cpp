// How the journal of unsynced spends and its figures land on the stored state: the rules of each change, the caps, the
// horizon, and the folding of the changes a process holds when its disk refuses them.
#include <algorithm>

#include "meter_support.hpp"

namespace kgtest {
namespace {

using namespace keygrant::detail;
using Op = JournalOp;

constexpr std::int64_t TOP = kMaxSafeInteger;
const char* const ACT = "act_1";

StoredState stateWith(const std::vector<SpendEntry>& spends, const char* activation = ACT) {
  StoredState state;
  if (activation != nullptr) state.activationId = activation;
  state.spends = spends;
  return state;
}

Op spend(SpendKind kind, const std::string& who, std::int64_t n, const std::string& meter = "exports",
         std::optional<std::int64_t> limit = std::nullopt, bool told = false, std::int64_t at = NOW) {
  return Op::spendOf(kind, ACT, spendId(who), meter, n, at, limit, told);
}

// --- spends ----------------------------------------------------------------------------------------------------------

TEST(MeterJournal, GrowsOneOfflineEntryForEachMeterAndOpensTheOthersOnceById) {
  StoredState state = stateWith({});
  Journaled landed = journaled(state, spend(SpendKind::Offline, "A", 2), NOW);
  EXPECT_EQ(landed.spends, std::vector<SpendEntry>({entry(SpendKind::Offline, "A", 2, NOW)}));
  state.spends = landed.spends;
  // A second offline spend of the meter grows the entry; its own id is lost to it.
  landed = journaled(state, spend(SpendKind::Offline, "B", 3, "exports", std::nullopt, false, NOW + 5), NOW);
  EXPECT_EQ(landed.spends, std::vector<SpendEntry>({entry(SpendKind::Offline, "A", 5, NOW)}));
  // Another meter opens an entry of its own.
  landed = journaled(state, spend(SpendKind::Offline, "C", 1, "renders"), NOW);
  EXPECT_EQ(landed.spends.size(), 2u);
  // Unanswered spends and voids are opened once each, by id.
  state.spends = journaled(state, spend(SpendKind::Unanswered, "U", 4), NOW).spends;
  state.spends = journaled(state, spend(SpendKind::Void, "V", 1), NOW).spends;
  EXPECT_EQ(ids(state.spends), (std::vector<std::string>{spendId("A"), spendId("U"), spendId("V")}));
  EXPECT_EQ(journaled(state, spend(SpendKind::Unanswered, "W", 9), NOW).spends.size(), 4u);
}

TEST(MeterJournal, AnIdAlreadyInTheJournalIsLeftAsItIsAnOfflineEntrysIncluded) {
  const StoredState state = stateWith({entry(SpendKind::Offline, "A", 2, NOW), entry(SpendKind::Unanswered, "U", 1, NOW)});
  for (const SpendKind kind : {SpendKind::Unanswered, SpendKind::Void}) {
    for (const char* who : {"A", "U"}) {
      const Journaled landed = journaled(state, spend(kind, who, 9), NOW);
      EXPECT_EQ(landed.spends, state.spends) << who;
      EXPECT_FALSE(landed.refused);
    }
  }
  // Before the limit is judged: a spend the journal holds is the one it is, whatever the limit now says.
  const Journaled held = journaled(state, spend(SpendKind::Unanswered, "U", 1, "exports", 0), NOW);
  EXPECT_EQ(held.spends, state.spends);
  EXPECT_FALSE(held.refused);
}

TEST(MeterJournal, RefusesASpendThatWouldTakeItsMetersUnsyncedPastItsLimitAndHoldsAVoidToNone) {
  const StoredState state = stateWith({entry(SpendKind::Offline, "A", 4, NOW)});
  // 4 held and 3 more is 7: past a limit of 6, refused and changed nothing; at 7, landed.
  for (const SpendKind kind : {SpendKind::Offline, SpendKind::Unanswered}) {
    const Journaled refused = journaled(state, spend(kind, "N", 3, "exports", 6), NOW);
    EXPECT_TRUE(refused.refused);
    EXPECT_EQ(refused.spends, state.spends);
    EXPECT_FALSE(journaled(state, spend(kind, "N", 3, "exports", 7), NOW).refused);
  }
  // Another meter's spends are not counted; a spend with no limit has none.
  EXPECT_FALSE(journaled(state, spend(SpendKind::Offline, "N", 99, "renders", 100), NOW).refused);
  EXPECT_FALSE(journaled(state, spend(SpendKind::Offline, "N", 99), NOW).refused);
  // A void is no spend: a limit it carries is not read.
  const Journaled voided = journaled(state, spend(SpendKind::Void, "V", 50, "exports", 0), NOW);
  EXPECT_FALSE(voided.refused);
  EXPECT_EQ(voided.spends.size(), 2u);
}

TEST(MeterJournal, LandsASpendTheCustomerWasToldWasMadePastItsLimitAndTheCapAndNeverRefusesIt) {
  const StoredState state = stateWith({entry(SpendKind::Offline, "A", 4, NOW)});
  const Journaled past = journaled(state, spend(SpendKind::Offline, "N", 30, "exports", 6, true), NOW);
  EXPECT_FALSE(past.refused);
  EXPECT_EQ(past.spends[0].n, 34);
  StoredState full = stateWith(crowd(kMaxPendingSpends));
  const Journaled opened = journaled(full, spend(SpendKind::Unanswered, "N", 1, "exports", 0, true), NOW);
  EXPECT_FALSE(opened.refused);
  EXPECT_EQ(opened.spends.size(), kMaxPendingSpends + 1);
  // The same spend not told is refused by the cap.
  EXPECT_TRUE(journaled(full, spend(SpendKind::Unanswered, "N", 1), NOW).refused);
  // A void is never told: the flag is not read, and it takes the oldest void's place or finds no room.
  EXPECT_TRUE(journaled(full, spend(SpendKind::Void, "V", 1, "exports", std::nullopt, true), NOW).refused);
}

TEST(MeterJournal, KeepsTheCapOfVoidsANewOneTakingThePlaceOfTheOldestAndNeverRefusingForIt) {
  std::vector<SpendEntry> voids;
  for (std::size_t i = 0; i < kMaxPendingVoids; ++i) {
    voids.push_back(SpendEntry{spendId("v" + std::to_string(i)), "exports", 1, NOW - 1000 + static_cast<std::int64_t>(i), SpendKind::Void});
  }
  StoredState state = stateWith(voids);
  const Journaled landed = journaled(state, spend(SpendKind::Void, "new", 1), NOW);
  EXPECT_FALSE(landed.refused);
  ASSERT_EQ(landed.spends.size(), kMaxPendingVoids);
  EXPECT_EQ(landed.spends.front().id, spendId("v1"));
  EXPECT_EQ(landed.spends.back().id, spendId("new"));
  // One short of the cap: it is added, nothing replaced.
  state.spends.pop_back();
  EXPECT_EQ(journaled(state, spend(SpendKind::Void, "new", 1), NOW).spends.size(), kMaxPendingVoids);
  EXPECT_EQ(journaled(state, spend(SpendKind::Void, "new", 1), NOW).spends.front().id, spendId("v0"));
  // The oldest void is the oldest by place, among entries of other kinds, whatever sits between.
  std::vector<SpendEntry> mixed = voids;
  mixed.insert(mixed.begin() + 3, entry(SpendKind::Unanswered, "mid", 1, NOW));
  const Journaled replaced = journaled(stateWith(mixed), spend(SpendKind::Void, "new", 1), NOW);
  const std::vector<std::string> remaining = ids(replaced.spends);
  EXPECT_EQ(std::count(remaining.begin(), remaining.end(), spendId("v0")), 0);
  EXPECT_EQ(replaced.spends.size(), kMaxPendingVoids + 1);
  // A void that replaces one in a journal otherwise full finds its room.
  std::vector<SpendEntry> fullOfVoids = voids;
  for (const SpendEntry& e : crowd(kMaxPendingSpends - kMaxPendingVoids)) fullOfVoids.push_back(e);
  EXPECT_EQ(fullOfVoids.size(), kMaxPendingSpends);
  EXPECT_FALSE(journaled(stateWith(fullOfVoids), spend(SpendKind::Void, "new", 1), NOW).refused);
}

TEST(MeterJournal, RefusesAnAppendPast64EntriesAndLetsAnOpenEntryGrow) {
  std::vector<SpendEntry> full = crowd(kMaxPendingSpends - 1);
  full.insert(full.begin(), entry(SpendKind::Offline, "o", 2, NOW));
  const StoredState state = stateWith(full);
  const Journaled refused = journaled(state, spend(SpendKind::Unanswered, "N", 1, "credits"), NOW);
  EXPECT_TRUE(refused.refused);
  EXPECT_EQ(refused.spends, full);
  // The same meter's offline entry grows, and appends nothing.
  const Journaled grown = journaled(state, spend(SpendKind::Offline, "N", 1), NOW);
  EXPECT_FALSE(grown.refused);
  EXPECT_EQ(grown.spends.size(), kMaxPendingSpends);
  EXPECT_EQ(grown.spends[0].n, 3);
  // A new meter's offline spend has no room.
  EXPECT_TRUE(journaled(state, spend(SpendKind::Offline, "N", 1, "credits"), NOW).refused);
}

// --- confirmations and figures --------------------------------------------------------------------------------------------

std::vector<SpendEntry> confirm(const std::vector<SpendEntry>& spends, std::vector<Confirmed> entries) {
  return journaled(stateWith(spends), Op::confirmedOf(ACT, std::move(entries)), NOW).spends;
}

TEST(MeterJournal, DropsAnEntryByTheCountTheServerRecordedAVoidByAVoidAndKeepsWhatGrewMeanwhile) {
  const std::vector<SpendEntry> spends = {entry(SpendKind::Unanswered, "A", 3, NOW), entry(SpendKind::Offline, "B", 5, NOW, "renders"),
                                          entry(SpendKind::Void, "C", 1, NOW), entry(SpendKind::Void, "D", 1, NOW)};
  EXPECT_EQ(ids(confirm(spends, {Confirmed::count(spendId("A"), 3)})), (std::vector<std::string>{spendId("B"), spendId("C"), spendId("D")}));
  // More recorded than held drops it; less keeps it; an entry that grew (5) stays when only 4 are named.
  EXPECT_EQ(confirm(spends, {Confirmed::count(spendId("B"), 9)}).size(), 3u);
  EXPECT_EQ(confirm(spends, {Confirmed::count(spendId("B"), 4)}).size(), 4u);
  EXPECT_EQ(confirm(spends, {Confirmed::count(spendId("B"), 5)}).size(), 3u);
  // A void is named void; a count never drops a void, and a void never drops any other.
  EXPECT_EQ(ids(confirm(spends, {Confirmed::taken(spendId("C"))})), (std::vector<std::string>{spendId("A"), spendId("B"), spendId("D")}));
  EXPECT_EQ(confirm(spends, {Confirmed::count(spendId("D"), 1)}).size(), 4u);
  EXPECT_EQ(confirm(spends, {Confirmed::taken(spendId("A"))}).size(), 4u);
  EXPECT_EQ(confirm(spends, {Confirmed::count(spendId("Z"), 1)}).size(), 4u);
}

TEST(MeterJournal, KeepsTheNewerFigureOfEachMeterAndNothingAboutAnotherActivation) {
  const auto given = [](std::int64_t at, std::int64_t left) {
    return Op::figuresOf(ACT, {{"exports", MeterFigure{left, 100, 10, false, at}}});
  };
  StoredState state;
  state.activationId = ACT;
  state.meterFigures = {{"exports", MeterFigure{50, 100, 10, false, 1000}}, {"renders", MeterFigure{5, 5, 0, true, 1000}}};
  const auto left = [&](const Op& op) { return journaled(state, op, NOW).meterFigures.at("exports").left; };
  EXPECT_EQ(left(given(1001, 1)), 1);
  EXPECT_EQ(left(given(999, 1)), 50);
  // The stored one on a tie.
  EXPECT_EQ(left(given(1000, 1)), 50);
  // A meter the answer does not name is kept; one it names alone is added.
  EXPECT_EQ(journaled(state, given(1001, 1), NOW).meterFigures.count("renders"), 1u);
  EXPECT_EQ(journaled(stateWith({}), given(5, 1), NOW).meterFigures.count("exports"), 1u);
  // About another activation, or none: nothing.
  StoredState other = state;
  other.activationId = "act_2";
  EXPECT_EQ(journaled(other, given(2000, 1), NOW).meterFigures.at("exports").left, 50);
  StoredState none = state;
  none.activationId.reset();
  EXPECT_EQ(journaled(none, given(2000, 1), NOW).meterFigures.at("exports").left, 50);
}

TEST(MeterJournal, ClearsBothForAnotherSeatOrADeactivationAndKeepsThemForTheSameSeat) {
  StoredState state = stateWith({entry(SpendKind::Offline, "A", 2, NOW)});
  state.meterFigures = {{"exports", MeterFigure{50, 100, 10, false, NOW}}};
  const Journaled same = journaled(state, Op::activatedOf(ACT), NOW);
  EXPECT_EQ(same.spends, state.spends);
  EXPECT_EQ(same.meterFigures, state.meterFigures);
  const Journaled another = journaled(state, Op::activatedOf("act_9"), NOW);
  EXPECT_TRUE(another.spends.empty());
  EXPECT_TRUE(another.meterFigures.empty());
  StoredState nothing;
  EXPECT_TRUE(journaled(nothing, Op::activatedOf("act_9"), NOW).spends.empty());
  const Journaled released = journaled(state, Op::deactivated(), NOW);
  EXPECT_TRUE(released.spends.empty());
  EXPECT_TRUE(released.meterFigures.empty());
  // A change about another activation, or none, changes nothing.
  EXPECT_EQ(journaled(state, Op::spendOf(SpendKind::Offline, "act_2", spendId("N"), "exports", 1, NOW), NOW).spends, state.spends);
  EXPECT_EQ(journaled(state, Op::confirmedOf("act_2", {Confirmed::count(spendId("A"), 9)}), NOW).spends, state.spends);
  EXPECT_EQ(journaled(nothing, Op::spendOf(SpendKind::Offline, ACT, spendId("N"), "exports", 1, NOW), NOW).spends.size(), 0u);
}

TEST(MeterJournal, DropsEntriesOpenedMoreThan180DaysAgoOnAnyOpThatLandsAndKeepsThoseExactlyThatOld) {
  const std::int64_t horizon = 180 * DAY;
  StoredState state = stateWith({entry(SpendKind::Unanswered, "old", 1, NOW - horizon - 1), entry(SpendKind::Unanswered, "edge", 1, NOW - horizon),
                                 entry(SpendKind::Offline, "new", 1, NOW)});
  const std::vector<Op> ops = {Op::activatedOf(ACT), Op::confirmedOf(ACT, {}), Op::figuresOf(ACT, {}),
                               spend(SpendKind::Void, "V", 1)};
  for (const Op& op : ops) {
    const std::vector<std::string> kept = ids(journaled(state, op, NOW).spends);
    EXPECT_EQ(std::count(kept.begin(), kept.end(), spendId("old")), 0);
    EXPECT_EQ(std::count(kept.begin(), kept.end(), spendId("edge")), 1);
  }
  // An op about another activation does not even drop them.
  EXPECT_EQ(journaled(state, Op::confirmedOf("act_2", {}), NOW).spends.size(), 3u);
}

TEST(MeterJournal, LandsOnTimesAtTheEndsOfWhatItHoldsWithoutOverflowAndJudgesTheHorizonByTheirDistance) {
  StoredState state = stateWith({entry(SpendKind::Unanswered, "top", 1, TOP), entry(SpendKind::Unanswered, "zero", 1, 0)});
  const Journaled early = journaled(state, Op::activatedOf(ACT), 0);
  EXPECT_EQ(early.spends.size(), 2u);
  const Journaled late = journaled(state, Op::activatedOf(ACT), TOP);
  EXPECT_EQ(ids(late.spends), std::vector<std::string>{spendId("top")});
  // The horizon of a clock at the bottom of the range.
  EXPECT_EQ(journaled(state, Op::activatedOf(ACT), -TOP).spends.size(), 2u);
}

TEST(MeterJournal, LandsOpsInOrderOnAStateAndIsThatStateItselfWithNone) {
  const StoredState state = stateWith({entry(SpendKind::Offline, "A", 1, NOW)});
  const LandedJournal none = landJournal(state, {}, NOW);
  EXPECT_EQ(none.state, state);
  EXPECT_TRUE(none.refused.empty());
  const LandedJournal landed =
      landJournal(state, {spend(SpendKind::Offline, "B", 2), spend(SpendKind::Unanswered, "C", 1, "exports", 0), Op::deactivated()}, NOW);
  EXPECT_EQ(landed.refused, (std::vector<bool>{false, true, false}));
  // The deactivation, last, clears it; an empty journal is the field absent.
  EXPECT_TRUE(landed.state.spends.empty());
  const LandedJournal two = landJournal(state, {spend(SpendKind::Offline, "B", 2), spend(SpendKind::Offline, "C", 5, "renders")}, NOW);
  EXPECT_EQ(two.state.spends.size(), 2u);
  EXPECT_EQ(two.state.spends[0].n, 3);
  // Other fields of the state are untouched.
  EXPECT_EQ(two.state.activationId, state.activationId);
}

// --- what a process folds -------------------------------------------------------------------------------------------------------

Op offlineOf(std::int64_t n, const std::string& meter = "exports", const std::string& who = "A", std::int64_t at = NOW) {
  return Op::spendOf(SpendKind::Offline, ACT, spendId(who), meter, n, at);
}

Op figureOp(std::int64_t left, std::int64_t at, const std::string& meter = "exports") {
  return Op::figuresOf(ACT, {{meter, MeterFigure{left, 100, 10, false, at}}});
}

Op confirmedOp(const std::string& who, std::int64_t n) { return Op::confirmedOf(ACT, {Confirmed::count(spendId(who), n)}); }

TEST(MeterJournal, FoldsOfflineSpendsOfOneMeterFiguresAndConfirmationsAndNothingAcrossAnotherKindOfChange) {
  // Offline spends of one meter fold: counts added, the first's id and time kept.
  const std::vector<Op> folded = compacted({offlineOf(1, "exports", "A", NOW), offlineOf(2, "exports", "B", NOW + 5), offlineOf(3)});
  ASSERT_EQ(folded.size(), 1u);
  EXPECT_EQ(folded[0].n, 6);
  EXPECT_EQ(folded[0].id, spendId("A"));
  EXPECT_EQ(folded[0].at, NOW);
  // Meters that take turns are not folded across one another.
  EXPECT_EQ(compacted({offlineOf(1, "exports"), offlineOf(1, "renders"), offlineOf(1, "exports")}).size(), 3u);
  // Nor offline spends across any other change, an unanswered spend, a void, a figure or a confirmation.
  EXPECT_EQ(compacted({offlineOf(1), spend(SpendKind::Unanswered, "U", 1), offlineOf(1)}).size(), 3u);
  EXPECT_EQ(compacted({offlineOf(1), Op::activatedOf(ACT), offlineOf(1)}).size(), 3u);
  EXPECT_EQ(compacted({offlineOf(1), Op::deactivated(), offlineOf(1)}).size(), 3u);
  EXPECT_EQ(compacted({offlineOf(1), figureOp(1, 1), offlineOf(1)}).size(), 3u);
  // Another activation is not folded either: not offline spends, not figures, not confirmations.
  Op other = offlineOf(1);
  other.about = "act_2";
  EXPECT_EQ(compacted({offlineOf(1), other}).size(), 2u);
  Op otherFigures = figureOp(9, 200);
  otherFigures.about = "act_2";
  EXPECT_EQ(compacted({figureOp(5, 100), otherFigures}).size(), 2u);
  Op otherConfirmed = confirmedOp("A", 1);
  otherConfirmed.about = "act_2";
  EXPECT_EQ(compacted({confirmedOp("A", 2), otherConfirmed}).size(), 2u);
  // Unanswered spends and voids never fold.
  EXPECT_EQ(compacted({spend(SpendKind::Unanswered, "U", 1), spend(SpendKind::Unanswered, "W", 1)}).size(), 2u);
  // Figures fold to the newer of each meter, confirmations to each id once with the larger count.
  const std::vector<Op> figures = compacted({figureOp(5, 100), figureOp(9, 50), figureOp(7, 100, "renders")});
  ASSERT_EQ(figures.size(), 1u);
  EXPECT_EQ(figures[0].figures.at("exports").left, 5);
  EXPECT_EQ(figures[0].figures.at("renders").left, 7);
  const std::vector<Op> confirmations = compacted({confirmedOp("A", 2), confirmedOp("A", 5), confirmedOp("B", 1), confirmedOp("A", 3)});
  ASSERT_EQ(confirmations.size(), 1u);
  EXPECT_EQ(confirmations[0].entries,
            (std::vector<Confirmed>{Confirmed::count(spendId("A"), 5), Confirmed::count(spendId("B"), 1)}));
  // A count and a void of one id are two confirmations.
  const std::vector<Op> both = compacted({confirmedOp("A", 2), Op::confirmedOf(ACT, {Confirmed::taken(spendId("A"))})});
  ASSERT_EQ(both.size(), 1u);
  EXPECT_EQ(both[0].entries.size(), 2u);
}

TEST(MeterJournal, WhatIsFoldedLandsAsTheOpsItWasFoldedFromWould) {
  const std::vector<Op> ops = {offlineOf(1, "exports", "A", NOW - 3000), offlineOf(2, "exports", "B", NOW - 2000), figureOp(5, 100),
                               figureOp(9, 50), confirmedOp("Z", 1), confirmedOp("Z", 4), offlineOf(3, "exports", "C", NOW - 1000)};
  StoredState state = stateWith({entry(SpendKind::Unanswered, "Z", 4, NOW - 5000)});
  state.meterFigures = {{"exports", MeterFigure{70, 100, 10, false, 10}}};
  const StoredState one = landJournal(state, ops, NOW).state;
  const StoredState folded = landJournal(state, compacted(ops), NOW).state;
  EXPECT_EQ(folded.spends, one.spends);
  EXPECT_EQ(folded.meterFigures, one.meterFigures);
}

TEST(MeterJournal, KeepsOnlyTheNewestVoidsInMemoryAndEveryOtherChange) {
  std::vector<Op> ops;
  for (int i = 0; i < 20; ++i) {
    ops.push_back(spend(SpendKind::Void, "v" + std::to_string(i), 1));
    if (i % 5 == 0) ops.push_back(offlineOf(1, "exports", "o" + std::to_string(i)));
  }
  const std::vector<Op> bounded = boundedVoids(ops);
  std::vector<std::string> voids;
  std::size_t others = 0;
  for (const Op& op : bounded) {
    if (op.isVoid()) {
      voids.push_back(op.id);
    } else {
      ++others;
    }
  }
  ASSERT_EQ(voids.size(), kMaxPendingVoids);
  EXPECT_EQ(voids.front(), spendId("v4"));
  EXPECT_EQ(voids.back(), spendId("v19"));
  EXPECT_EQ(others, 4u);
  // Fewer than the cap: all of them.
  EXPECT_EQ(boundedVoids({spend(SpendKind::Void, "a", 1), offlineOf(1)}).size(), 2u);
  EXPECT_TRUE(boundedVoids({}).empty());
}

TEST(MeterJournal, OpsAreEqualByEveryMember) {
  EXPECT_EQ(offlineOf(1), offlineOf(1));
  EXPECT_NE(offlineOf(1), offlineOf(2));
  const Op base = spend(SpendKind::Offline, "A", 1, "exports", 3, false);
  EXPECT_EQ(base, spend(SpendKind::Offline, "A", 1, "exports", 3, false));
  const auto differs = [&](const char* member, const std::function<void(Op&)>& change) {
    Op other = base;
    change(other);
    EXPECT_NE(base, other) << member;
  };
  differs("kind", [](Op& o) { o.kind = Op::Kind::Figures; });
  differs("about", [](Op& o) { o.about = "act_2"; });
  differs("spend", [](Op& o) { o.spend = SpendKind::Unanswered; });
  differs("id", [](Op& o) { o.id = spendId("B"); });
  differs("meter", [](Op& o) { o.meter = "renders"; });
  differs("n", [](Op& o) { o.n = 2; });
  differs("at", [](Op& o) { o.at = NOW + 1; });
  differs("limit", [](Op& o) { o.limit = 9; });
  differs("told", [](Op& o) { o.told = true; });
  differs("entries", [](Op& o) { o.entries = {Confirmed::count(spendId("A"), 1)}; });
  differs("figures", [](Op& o) { o.figures = {{"exports", MeterFigure{1, 1, 1, false, 1}}}; });
  EXPECT_NE(confirmedOp("A", 1), confirmedOp("A", 2));
  EXPECT_NE(figureOp(1, 1), figureOp(2, 1));
  EXPECT_NE(Op::deactivated(), Op::activatedOf(ACT));
}

}  // namespace
}  // namespace kgtest
