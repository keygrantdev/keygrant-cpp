// A property test of use() over the real License: seeded random sequences of spends, check-ins and releases, from two processes
// of the app over one state file, against a server whose answers are lost, late, refused or lost after it has recorded the
// spend, with a disk that refuses its writes and a device clock that jumps forward and back. The properties
// (property/property_world.cpp):
//
//  - use() and status() never throw for an outcome;
//  - a spend the offline allowance covers is refused only for the journal's size, and asks the server unless a wait runs;
//  - a spend it does not cover is never refused without asking, unless a void under a minute old stands (by the device clock
//    too), and never asks while one does;
//  - the stored journal holds no more than 64 entries (but for the spends the customer was told were made while only memory
//    could hold them) and 16 voids, and a process holds no more than 16 voids in memory, none of them marked told;
//  - offline, the unsynced spends of a meter never pass its allowance, or what the journal began with if that is more, those
//    told spends apart;
//  - a licensed call is refused only for the allowance, for a meter the key lacks, or because the server has fewer left than
//    asked (this world's server gives no verdict, and a read of the state never fails), a lone one only as the finer rules say
//    (a refusal for the allowance answers the figure less the unsynced), and a meter the key lacks is never spent;
//  - the server never holds more than the customer was told and the refusals it recorded, at any step; once everything is
//    reported it holds every spend the customer was told was made, once (but for what the contract lets a release or the end
//    of a process drop unreported), a spend they were refused after it recorded it is there only when nothing could have lost
//    its void, and every status shows what the server has left;
//  - what use() answers as `left` is the figure less the unsynced less the spend when made offline, and never more than the
//    server has when made online.
//
// The oracle takes its constants, figures and waits from the contract, not from the code it judges. A case replayed runs the
// same way every time (a test below holds it to that), which is what lets a failing one be shrunk and kept. PROPERTY_RUNS and
// PROPERTY_SEED choose how many sequences run and from which seed: the default is 1500 (in ten tests of a share each), a hunt
// is PROPERTY_RUNS=20000. A failing case is shrunk and printed as data, with what happened: add it to the kept cases
// (property/cases.json).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "meter_support.hpp"
#include "property/property_world.hpp"

namespace kgtest {
namespace {

using namespace property;

/// A failing case with steps taken out while `fails` says it still does: the shortest sequence that shows the bug.
template <class Fails>
Case shrinkWhile(const Case& c, Fails fails) {
  Case best = c;
  for (std::size_t size = (best.steps.size() + 1) / 2; size >= 1;) {
    for (std::size_t at = 0; at < best.steps.size();) {
      std::vector<Step> steps = best.steps;
      steps.erase(steps.begin() + static_cast<std::ptrdiff_t>(at),
                  steps.begin() + static_cast<std::ptrdiff_t>(std::min(at + size, steps.size())));
      const Case trial = best.withSteps(std::move(steps));
      if (fails(trial)) {
        best = trial;
      } else {
        at += size;
      }
    }
    if (size == 1) break;
    size /= 2;
  }
  Case lean = best;
  lean.preload.clear();
  return fails(lean) ? lean : best;
}

/// A failing case shrunk while it fails the same way.
Case shrink(const Case& c, const PropertyFailure& failure) {
  return shrinkWhile(c, [&failure](const Case& trial) {
    const auto again = runCase(trial, false).first;
    return again && again->invariant() == failure.invariant();
  });
}

std::int64_t setting(const char* name, std::int64_t fallback) {
  const char* text = std::getenv(name);
  if (text == nullptr || *text == '\0') return fallback;
  char* end = nullptr;
  const long long value = std::strtoll(text, &end, 10);
  if (end == text || *end != '\0') {
    ADD_FAILURE() << name << " must be a whole number: " << text;
    return fallback;
  }
  return value;
}

constexpr int kChunks = 10;

class UseProperty : public testing::TestWithParam<int> {};

TEST_P(UseProperty, KeepsEveryPropertyOfTheContractOverSeededSequences) {
  const std::int64_t runs = setting("PROPERTY_RUNS", 1500);
  const std::int64_t first = setting("PROPERTY_SEED", 1);
  // A count that would run nothing, and pass.
  ASSERT_GE(runs, 1) << "PROPERTY_RUNS must be at least 1";
  if (runs != 1500 || first != 1) {
    std::fprintf(stderr, "[ property ] %lld sequences from seed %lld (the default is 1500 from seed 1)\n", static_cast<long long>(runs),
                 static_cast<long long>(first));
  }
  const std::int64_t share = (runs + kChunks - 1) / kChunks;
  const std::int64_t from = first + GetParam() * share;
  const std::int64_t to = std::min(first + runs, from + share);
  for (std::int64_t seed = from; seed < to; ++seed) {
    const Case c = generate(seed);
    const auto failure = runCase(c, false).first;
    if (!failure) continue;
    const Case small = shrink(c, *failure);
    const auto again = runCase(small, true);
    std::string trace;
    for (const std::string& line : again.second) trace += line + "\n";
    FAIL() << "seed " << seed << ": " << failure->what() << "\nshrunk to " << small.steps.size() << " steps ("
           << (again.first ? again.first->what() : "no longer fails") << "):\n"
           << small.toJson().dump() << "\n"
           << trace;
  }
  SUCCEED();
}

INSTANTIATE_TEST_SUITE_P(Chunks, UseProperty, testing::Range(0, kChunks));

// --- the shrinker, which a passing run never reaches ---------------------------------------------------------------------------

TEST(UsePropertyShrink, TakesStepsOutWhileTheCaseStillFailsAndKeepsOnlyThoseThatMakeItFail) {
  // A case "fails" while it holds the step named `name` twice: the shortest such case is those two steps, preload gone. A case
  // that fails only while its preload stands keeps it.
  std::optional<Case> chosen;
  std::string name;
  for (std::int64_t seed = 1; seed <= 3000 && !chosen; ++seed) {
    const Case c = generate(seed);
    if (c.steps.size() < 12 || c.preload.empty()) continue;
    for (const Step& s : c.steps) {
      if (std::count_if(c.steps.begin(), c.steps.end(), [&](const Step& t) { return std::string(t.name()) == s.name(); }) >= 2) {
        chosen = c;
        name = s.name();
        break;
      }
    }
  }
  ASSERT_TRUE(chosen.has_value());
  const auto fails = [&name](const Case& c) {
    return std::count_if(c.steps.begin(), c.steps.end(), [&](const Step& s) { return std::string(s.name()) == name; }) >= 2;
  };
  ASSERT_TRUE(fails(*chosen));
  const Case small = shrinkWhile(*chosen, fails);
  EXPECT_TRUE(fails(small));
  EXPECT_EQ(small.steps.size(), 2u);
  EXPECT_TRUE(small.preload.empty());
  const auto needsPreload = [&fails](const Case& c) { return fails(c) && !c.preload.empty(); };
  const Case kept = shrinkWhile(*chosen, needsPreload);
  EXPECT_EQ(kept.steps.size(), 2u);
  EXPECT_EQ(kept.preload.size(), chosen->preload.size());
  // A case that does not fail has nothing to take out.
  const Case whole = shrinkWhile(*chosen, [](const Case&) { return false; });
  EXPECT_EQ(whole.steps.size(), chosen->steps.size());
}

/// The first line two traces differ at, with the lines before it.
std::string difference(const std::string& a, const std::string& b) {
  std::vector<std::string> x;
  std::vector<std::string> y;
  for (std::istringstream in(a); in;) {
    std::string line;
    if (std::getline(in, line)) x.push_back(line);
  }
  for (std::istringstream in(b); in;) {
    std::string line;
    if (std::getline(in, line)) y.push_back(line);
  }
  std::size_t at = 0;
  while (at < x.size() && at < y.size() && x[at] == y[at]) ++at;
  std::string before;
  for (std::size_t i = at >= 6 ? at - 6 : 0; i < at; ++i) before += x[i] + "\n";
  return before + "--- first run, line " + std::to_string(at) + ":\n" + (at < x.size() ? x[at] : "(ends)") + "\n--- other run:\n" +
         (at < y.size() ? y[at] : "(ends)");
}

std::string traceOf(const Case& c) {
  const auto run = runCase(c, true);
  std::string text;
  for (const std::string& line : run.second) text += line + "\n";
  if (run.first) text += "failed: " + std::string(run.first->what()) + "\n";
  return text;
}

TEST(UsePropertyReplay, ACaseReplayedRunsTheSameWayEveryTimeTheSameRequestsAnswersAndJournals) {
  for (std::int64_t seed = 1; seed <= 60; ++seed) {
    const Case c = generate(seed);
    const std::string first = traceOf(c);
    for (int again = 0; again < 2; ++again) {
      const std::string next = traceOf(c);
      ASSERT_TRUE(first == next) << "seed " << seed << " ran differently on run " << again + 2 << ":\n" << difference(first, next);
    }
  }
}

// --- the generator is the reference's ---------------------------------------------------------------------------------------

const json& keptCases() {
  static const json loaded = [] {
    const std::optional<std::filesystem::path> path = aboveTests("property/cases.json");
    if (!path) throw std::runtime_error("the property test's kept cases (property/cases.json) were not found");
    std::ifstream in(*path, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return json::parse(text.str());
  }();
  return loaded;
}

TEST(UsePropertyGenerator, MakesTheFirstDrawsTheReferenceMakes) {
  // The reference's generator, line for line: the first draws of mulberry32(1).
  Rng rng(1);
  const double draws[] = {rng.next(), rng.next(), rng.next()};
  EXPECT_EQ(draws[0], 0.6270739405881613);
  EXPECT_EQ(draws[1], 0.002735721180215478);
  EXPECT_EQ(draws[2], 0.5274470399599522);
}

TEST(UsePropertyGenerator, MakesTheCaseTheReferenceMakesFromASeedSoASeedFoundThereMeansTheSameHere) {
  for (const json& golden : keptCases()["golden"]) {
    const std::int64_t seed = golden["seed"].get<std::int64_t>();
    SCOPED_TRACE(seed);
    EXPECT_EQ(Case::fromJson(golden["case"]).toJson(), generate(seed).toJson());
    // And it is data: the case survives its own JSON.
    const Case made = generate(seed);
    EXPECT_EQ(made.toJson(), Case::fromJson(json::parse(made.toJson().dump())).toJson());
  }
}

TEST(UsePropertyGenerator, MakesTheLongCasesTheReferenceMakesByWhatTheyHold) {
  // A storm of voids and a journal past its cap: their size, their steps in order and their first draw.
  for (const json& shape : keptCases()["shapes"]) {
    const std::int64_t seed = shape["seed"].get<std::int64_t>();
    SCOPED_TRACE(seed);
    const Case made = generate(seed);
    EXPECT_EQ(made.preload.size(), shape["preload"].get<std::size_t>());
    EXPECT_EQ(std::count_if(made.preload.begin(), made.preload.end(), [](const Preloaded& e) { return e.kind == keygrant::SpendKind::Void; }),
              shape["voids"].get<std::ptrdiff_t>());
    std::string kinds;
    std::optional<double> draw;
    for (const Step& s : made.steps) {
      kinds += (kinds.empty() ? "" : " ") + std::string(s.name());
      if (!draw && !s.r.empty()) draw = s.r.front();
    }
    EXPECT_EQ(kinds, shape["steps"].get<std::string>());
    EXPECT_EQ(draw, std::optional<double>(shape["firstDraw"].get<double>()));
  }
}

// --- the cases the property test found --------------------------------------------------------------------------------------------

/// A kept case holds: each is a bug the property test found, shrunk to the steps that show it.
void holdsTheFoundCase(std::size_t index) {
  const json& found = keptCases()["found"];
  ASSERT_GT(found.size(), index);
  const auto run = runCase(Case::fromJson(found[index]["case"]), true);
  std::string trace;
  for (const std::string& line : run.second) trace += line + "\n";
  EXPECT_FALSE(run.first.has_value()) << found[index]["name"].get<std::string>() << ": " << (run.first ? run.first->what() : "") << "\n" << trace;
}

TEST(UsePropertyFound, HoldsASpendHeldInMemoryIsNotReportedUnderItsOwnId) { holdsTheFoundCase(0); }
TEST(UsePropertyFound, HoldsASpendHeldInMemoryIsNotReportedInTheEntryItJoinsInItsOwnView) { holdsTheFoundCase(1); }
TEST(UsePropertyFound, HoldsASpendHeldInMemoryIsNotReportedWhileAnotherProcessLandsEntriesBesideIt) { holdsTheFoundCase(2); }
TEST(UsePropertyFound, HoldsSpendsHeldInMemoryAreNotReportedWhileVoidsComeAndGoBesideThem) { holdsTheFoundCase(3); }
TEST(UsePropertyFound, HoldsALandingThatOpensSeveralVoidsInAJournalOfFifteenReplacesTheOldest) { holdsTheFoundCase(4); }
TEST(UsePropertyFound, HoldsALandingThatOpensAVoidAndConfirmsItInTheSameSave) { holdsTheFoundCase(5); }
TEST(UsePropertyFound, HoldsASecondSpendThatMeetsTheJournalTheFirstFilled) { holdsTheFoundCase(6); }
TEST(UsePropertyFound, HoldsAVoidHeldInMemoryThatLandsWithTheConfirmationThatTakesTheOldestOff) { holdsTheFoundCase(7); }
TEST(UsePropertyFound, HoldsTwoCallsOfOneProcessOnOneMeterTheFirstMadeOfflineAndTheSecondRefused) { holdsTheFoundCase(8); }

TEST(UsePropertyFound, EveryKeptCaseHasATest) { EXPECT_EQ(keptCases()["found"].size(), 9u); }

}  // namespace
}  // namespace kgtest
