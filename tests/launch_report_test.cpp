// A running trial reports its launch from status(), at most once a day, on a thread of its own and
// never awaited.
#include <future>
#include <thread>

#include "offline.hpp"
#include "support.hpp"

namespace kgtest {
namespace {

using namespace std::chrono_literals;

/** A running trial (its lease valid): no trial answer or report stamped. */
StoredState running(const Harness& h) {
  StoredState state;
  state.trialStart = NOW - 2 * DAY;
  state.trialEndsAt = NOW + 5 * DAY;
  state.trialLease = h.sign("trial:fp-test", "trial", 5 * 24 * 3600);
  state.lastSeen = NOW;
  return state;
}

LicenseStatus runningTrial() {
  LicenseStatus s = status(LicenseState::Trial);
  s.trialEndsAt = NOW + 5 * DAY;
  s.trialDaysLeft = 5;
  return s;
}

TEST(LaunchReport, IsMadeFromStatusStampedFirstThenPostedInTheBackgroundAsStartTrialWouldItsAnswerNotRead) {
  Harness h;
  h.store(running(h));
  // An earlier start in the stash: the report sends the stored start, and reads no stash.
  h.registry->set(NOW - 9 * DAY);
  h.http->reply("trial", 200, json{{"trialStart", NOW - 9 * DAY}, {"trialEndsAt", NOW + 30 * DAY}});
  EXPECT_EQ(h.license().status(), runningTrial());
  EXPECT_EQ(h.stored()->trialReportedAt, NOW);
  ASSERT_TRUE(h.reportSettled());
  EXPECT_EQ(h.http->actions(), std::vector<std::string>{"trial"});
  EXPECT_EQ(h.http->bodyOf("trial"), json({{"fingerprint", "fp-test"}, {"clientStart", NOW - 2 * DAY}}));
  EXPECT_EQ(h.registry->reads.load(), 0);
  // Bounded as a status check is.
  EXPECT_EQ(h.http->timeouts(), std::vector<Millis>{h.timings.statusCheck});
  // The answer is not read: the stored trial is as it was.
  EXPECT_EQ(h.stored()->trialStart, NOW - 2 * DAY);
  EXPECT_EQ(h.stored()->trialEndsAt, NOW + 5 * DAY);
  EXPECT_FALSE(h.stored()->trialAt);
}

TEST(LaunchReport, StampsAndJudgesItsDayByTheGuardedClock) {
  // The guarded clock a day ahead of the device clock (lastSeen NOW + DAY, the clock at NOW).
  constexpr std::int64_t HOUR = 3'600'000;
  // Reported an hour ago by the device clock, a day and an hour ago by the guarded one: due.
  Harness h;
  StoredState state = running(h);
  state.lastSeen = NOW + DAY;
  state.trialReportedAt = NOW - HOUR;
  h.store(state);
  EXPECT_EQ(h.license().status().state, LicenseState::Trial);
  ASSERT_TRUE(h.reportSettled());
  EXPECT_EQ(h.http->count("trial"), 1);
  // And stamped by the guarded clock.
  EXPECT_EQ(h.stored()->trialReportedAt, NOW + DAY);
}

TEST(LaunchReport, SendsTheFingerprintsKindAsStartTrialDoes) {
  auto fingerprint = std::make_shared<FakeFingerprinter>("fp-machine");
  fingerprint->identifyAs(DeviceKind::Machine, {});
  HarnessOptions options;
  options.fingerprint = fingerprint;
  Harness h(std::nullopt, options);
  StoredState state = running(h);
  state.trialLease = h.sign("trial:fp-machine", "trial", 5 * 24 * 3600);
  h.store(state);
  EXPECT_EQ(h.license().status().state, LicenseState::Trial);
  ASSERT_TRUE(h.reportSettled());
  EXPECT_EQ(h.http->bodyOf("trial"),
            json({{"fingerprint", "fp-machine"}, {"fingerprintKind", "machine"}, {"clientStart", NOW - 2 * DAY}}));
}

TEST(LaunchReport, IsMadeAtMostOnceADayAStartTrialAnswerCountingAsAReport) {
  Harness h;
  h.store(running(h));
  h.license().status();
  ASSERT_TRUE(h.reportSettled());
  h.setClock(NOW + keygrant::detail::kTrialReportMs - 1);
  h.license().status();
  ASSERT_TRUE(h.reportSettled());
  EXPECT_EQ(h.http->count("trial"), 1);
  h.setClock(NOW + keygrant::detail::kTrialReportMs);
  h.license().status();
  ASSERT_TRUE(h.reportSettled());
  EXPECT_EQ(h.http->count("trial"), 2);

  Harness synced;
  StoredState state = running(synced);
  state.trialAt = NOW - DAY / 2;
  synced.store(state);
  synced.license().status();
  ASSERT_TRUE(synced.reportSettled());
  EXPECT_FALSE(synced.http->called("trial"));
}

TEST(LaunchReport, NeverHoldsUpTheLaunchStatusAnswersWhileTheReportHangs) {
  Harness h;
  h.store(running(h));
  h.http->hang = true;
  LicenseStatus answered;
  const Millis took = timed([&] { answered = h.license().status(); });
  EXPECT_EQ(answered, runningTrial());
  EXPECT_LT(took, 2s);
  EXPECT_TRUE(h.http->waitFor("trial"));
  // In flight, and waited on by nothing but this test.
  EXPECT_FALSE(h.reportSettled(Millis{50}));
}

TEST(LaunchReport, IsBoundedAsAStatusCheckIsSettlingWhileTheRequestStillHangs) {
  HarnessOptions options;
  options.timings = shortTimings();
  Harness h(std::nullopt, options);
  h.store(running(h));
  h.http->hang = true;
  h.license().status();
  ASSERT_TRUE(h.http->waitFor("trial"));
  // The request is abandoned at its bound (150 ms here), its gate still shut.
  EXPECT_TRUE(h.reportSettled(5s));
  EXPECT_EQ(h.http->timeouts(), std::vector<Millis>{h.timings.statusCheck});
}

TEST(LaunchReport, NeverFailsOffline) {
  Harness h;
  h.store(running(h));
  h.http->fail = true;
  EXPECT_EQ(h.license().status(), runningTrial());
  EXPECT_TRUE(h.reportSettled());
  EXPECT_EQ(h.http->count("trial"), 1);
}

TEST(LaunchReport, SendsNothingWhenItsStampCannotBeSavedSoItIsNeverSentTwiceInADay) {
  Harness h;
  h.store(running(h));
  h.storage->failWrites = true;
  EXPECT_EQ(h.license().status(), runningTrial());
  EXPECT_TRUE(h.reportSettled());
  EXPECT_FALSE(h.http->called("trial"));
}

TEST(LaunchReport, ReadsTheDeviceFor2SAsAStatusDoesInTheBackground) {
  // The SDK's own read bounds (Timings{}), on a machine id that does not read:
  // status()'s own read of the trial lease, then the report's.
  auto timed = std::make_shared<TimedId>();
  HarnessOptions options;
  options.fingerprint = timed;
  Harness h(std::nullopt, options);
  h.store(running(h));
  EXPECT_EQ(h.license().status(), runningTrial());
  ASSERT_TRUE(h.reportSettled());
  EXPECT_EQ(timed->timeouts(), (std::vector<Millis>{h.timings.machineId, h.timings.machineId}));
  // No fingerprint of its own: nothing sent.
  EXPECT_FALSE(h.http->called("trial"));
}

TEST(LaunchReport, SendsNothingFromADeviceThatCannotBeTold) {
  auto fingerprint = std::make_shared<FakeFingerprinter>();
  fingerprint->failing = true;
  HarnessOptions options;
  options.fingerprint = fingerprint;
  Harness h(std::nullopt, options);
  h.store(running(h));
  // A trial lease with no machine id is judged valid on a run that cannot tell.
  EXPECT_EQ(h.license().status(), runningTrial());
  EXPECT_EQ(h.stored()->trialReportedAt, NOW);
  EXPECT_TRUE(h.reportSettled());
  EXPECT_FALSE(h.http->called("trial"));
}

TEST(LaunchReport, IsNotMadeByAnExpiredTrialWithAKeyHeldOrBehindACallThatHasNotFinished) {
  Harness expired;
  StoredState over = running(expired);
  over.trialLease.reset();
  over.trialEndsAt = NOW - 1;
  expired.store(over);
  expired.license().status();
  EXPECT_TRUE(expired.reportSettled());
  EXPECT_FALSE(expired.http->called("trial"));
  EXPECT_FALSE(expired.stored()->trialReportedAt);

  Harness keyed;
  StoredState withKey = running(keyed);
  withKey.key = "KEY-1";
  keyed.store(withKey);
  keyed.license().status();
  EXPECT_TRUE(keyed.reportSettled());
  EXPECT_FALSE(keyed.http->called("trial"));

  HarnessOptions quick;
  quick.httpTimeout = Millis{100};
  quick.timings = shortTimings();
  Harness stalledOne(std::nullopt, quick);
  stalledOne.store(running(stalledOne));
  stalledOne.storage->hangNextRead();
  auto stuck = std::async(std::launch::async, [&stalledOne] { return stalledOne.license().status(); });
  ASSERT_TRUE(stalledOne.storage->readGate().waitReached(5s));
  EXPECT_EQ(stalledOne.license().status(), stalled(runningTrial()));
  EXPECT_TRUE(stalledOne.reportSettled());
  EXPECT_FALSE(stalledOne.http->called("trial"));
  EXPECT_EQ(stalledOne.storage->writes(), 0);
  stalledOne.storage->readGate().open();
  finish(stuck);
}

TEST(LaunchReport, LeavesNothingDanglingWhenTheLicenseIsDestroyedFirst) {
  Harness h;
  h.store(running(h));
  h.http->hang = true;
  // A fingerprinter only the License holds (through its device reader): the
  // report must keep the reader alive past the License, then let go of it.
  auto fingerprint = std::make_shared<FakeFingerprinter>();
  const std::weak_ptr<FakeFingerprinter> watch = fingerprint;
  {
    keygrant::LicenseConfig config = h.config;
    config.adapters.fingerprint = fingerprint;
    auto license = std::make_unique<keygrant::License>(config, h.timings);
    fingerprint.reset();
    config.adapters.fingerprint.reset();
    EXPECT_EQ(license->status(), runningTrial());
    ASSERT_TRUE(h.http->waitFor("trial"));
    // The License (and its engine) goes first, its report still in flight.
    license.reset();
  }
  // Only the report holds the reader now; its request is still hanging.
  EXPECT_FALSE(watch.expired());
  h.http->hangGate().open();
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (!watch.expired() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(5ms);
  EXPECT_TRUE(watch.expired()) << "the report never let go of what it held";
}

}  // namespace
}  // namespace kgtest
