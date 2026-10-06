// Every call bounded, one at a time, and read-only behind one that is stuck
// (sdk/src/license.test.ts "the waits, early renewal, bounded calls..."), in
// real time with the SDK's timings shortened.
#include <future>
#include <thread>

#include "support.hpp"

namespace kgtest {
namespace {

using namespace std::chrono_literals;

HarnessOptions quick(Millis httpTimeout = Millis{100}, std::int64_t major = 1) {
  HarnessOptions o;
  o.httpTimeout = httpTimeout;
  o.timings = shortTimings();
  o.major = major;
  return o;
}

StoredState licensedState(const Harness& h) { return held(h.sign(KEY, "perpetual", 30 * 24 * 3600)); }

/** Major 3 on a valid open lease the server last heard on 2: status() checks it, bounded. */
StoredState onNewMajor(const Harness& h) {
  LeaseSpec spec;
  spec.major = keygrant::kOpenMajor;
  return held(h.sign(spec), KEY, 2);
}

/** A trial whose lease another key signed (the product's, before a rotation), its stored end ahead: status() heals it. */
StoredState rotatedTrial() {
  StoredState state;
  state.trialStart = NOW - 2 * DAY;
  state.trialEndsAt = NOW + 5 * DAY;
  state.trialLease = Harness().sign("trial:fp-test", "trial", 5 * 24 * 3600);
  state.lastSeen = NOW;
  return state;
}

/** A fingerprinter whose every read hangs until the harness lets it go. */
std::shared_ptr<FakeFingerprinter> hangingFingerprinter() {
  auto hanging = std::make_shared<FakeFingerprinter>();
  hanging->hanging = true;
  return hanging;
}

TEST(Bounds, AStatusCheckOnAValidLeaseAnswersTheLeaseAfterAtMostItsBound) {
  Harness h(std::nullopt, quick(Millis{10'000}, 3));
  h.store(onNewMajor(h));
  h.http->hang = true;
  LicenseStatus answered;
  const Millis took = timed([&] { answered = h.license().status(); });
  EXPECT_EQ(answered, licensed(KEY));
  // statusCheck (150 ms here, 5 s by default), not the 10 s HTTP bound.
  EXPECT_GE(took, Millis{140});
  EXPECT_LT(took, Millis{3'000});
  EXPECT_EQ(h.http->timeouts(), std::vector<Millis>{Millis{150}});
  EXPECT_EQ(h.stored()->backoffSince, NOW);
  EXPECT_EQ(h.stored()->backoffMs, 60'000);
}

TEST(Bounds, EveryRequestIsHeldToTheConfiguredHttpTimeoutWhenTheCallNamesNone) {
  Harness h(std::nullopt, quick(Millis{300}));
  h.http->hang = true;
  ActivateResult result;
  const Millis took = timed([&] { result = h.license().activate(KEY); });
  EXPECT_EQ(result, failed(ActivateError::Network));
  EXPECT_GE(took, Millis{290});
  EXPECT_LT(took, Millis{3'000});
  EXPECT_EQ(h.http->timeouts(), std::vector<Millis>{Millis{300}});
}

TEST(Bounds, ACallWhoseAdapterNeverSettlesStillAnswersAndSoDoTheCallsBehindIt) {
  // The HTTP bound is short; the grace long enough that the call behind waits
  // for the first to settle however slow this machine is just now, and so is
  // not read-only (stalled) behind it.
  HarnessOptions options = quick();
  options.timings.lockGrace = Millis{10'000};
  Harness h(std::nullopt, options);
  h.store(licensedState(h));
  h.http->hang = true;
  auto refreshed = std::async(std::launch::async, [&] { return h.license().refresh(); });
  auto later = std::async(std::launch::async, [&] { return h.license().status(); });
  EXPECT_EQ(refreshed.get(), licensed(KEY));
  EXPECT_EQ(later.get(), licensed(KEY));
}

TEST(Bounds, AFingerprinterThatNeverAnswersIsHeldToItsBoundAndNotKept) {
  auto hanging = std::make_shared<FakeFingerprinter>();
  hanging->hanging = true;
  HarnessOptions options = quick();
  options.fingerprint = hanging;
  Harness h(std::nullopt, options);
  LeaseSpec spec;
  spec.extra = {{"fp", claimOf("fp-test")}, {"fpk", "machine"}};
  h.store(held(h.sign(spec)));
  h.http->fail = true;
  LicenseStatus answered;
  const Millis took = timed([&] { answered = h.license().status(); });
  EXPECT_EQ(answered, licensed(KEY));
  EXPECT_GE(took, Millis{90});
  EXPECT_LT(took, Millis{3'000});
  // Waited on again while it runs, not asked again beside it: one thread, however many calls.
  h.license().status();
  EXPECT_EQ(hanging->calls, 1);
  // Not kept: once it has ended (here, failed), the next call asks again.
  hanging->hanging = false;
  hanging->gate().open();
  for (int i = 0; i < 100 && hanging->calls.load() < 2; ++i) {
    h.license().status();
    if (hanging->calls.load() < 2) std::this_thread::sleep_for(Millis{10});
  }
  EXPECT_EQ(hanging->calls, 2);
}

TEST(Bounds, StartTrialWaitsTheActivationBoundForTheMachineIdNotTheOneAStatusGets) {
  HarnessOptions options = quick();
  options.fingerprint = hangingFingerprinter();
  Harness h(std::nullopt, options);
  LicenseStatus answered;
  const Millis took = timed([&] { answered = h.license().startTrial(); });
  // activateMachineId (400 ms here, 10 s by default), not machineId (100 ms here, 2 s by default).
  EXPECT_GE(took, Millis{390});
  EXPECT_LT(took, Millis{3'000});
  // This device could not be told: no trial is asked for, and none is held.
  EXPECT_EQ(answered, status(LicenseState::Unlicensed));
  EXPECT_FALSE(h.http->called("trial"));
}

TEST(Bounds, ATrialHealsAskIsHeldToTheStatusCheckBoundAndWaitsAMinuteAfterNoAnswer) {
  Harness h(std::nullopt, quick(Millis{10'000}));
  h.store(rotatedTrial());
  h.http->hang = true;
  LicenseStatus answered;
  const Millis took = timed([&] { answered = h.license().status(); });
  EXPECT_EQ(answered, status(LicenseState::Expired, LicenseReason::Trial));
  // statusCheck (150 ms here, 5 s by default), not the 10 s HTTP bound.
  EXPECT_GE(took, Millis{140});
  EXPECT_LT(took, Millis{3'000});
  EXPECT_EQ(h.http->timeouts(), std::vector<Millis>{Millis{150}});
  EXPECT_EQ(h.stored()->backoffSince, NOW);
  EXPECT_EQ(h.stored()->backoffMs, 60'000);
}

TEST(Bounds, ATrialHealReadsTheMachineIdWithTheBoundAStatusGetsNotTheActivations) {
  HarnessOptions options = quick();
  options.fingerprint = hangingFingerprinter();
  options.timings.activateMachineId = Millis{10'000};  // what the heal must not wait
  Harness h(std::nullopt, options);
  h.store(rotatedTrial());
  LicenseStatus answered;
  const Millis took = timed([&] { answered = h.license().status(); });
  EXPECT_EQ(answered, status(LicenseState::Expired, LicenseReason::Trial));
  // The stored lease's check reads the device, and the heal waits on that same
  // read again: machineId each (100 ms here, 2 s by default).
  EXPECT_GE(took, Millis{190});
  EXPECT_LT(took, Millis{3'000});
  // A device that could not be told asks for no trial, and waits a minute.
  EXPECT_FALSE(h.http->called("trial"));
  EXPECT_EQ(h.stored()->backoffSince, NOW);
  EXPECT_EQ(h.stored()->backoffMs, 60'000);
}

TEST(Stalled, ALaterCallGoesAheadReadOnlyWhenOneIsStuckOnAnAdapterTheSdkDoesNotBound) {
  Harness h(std::nullopt, quick());
  h.store(licensedState(h));
  h.storage->hangNextRead();
  auto stuck = std::async(std::launch::async, [&] { return h.license().status(); });
  ASSERT_TRUE(h.storage->readGate().waitReached(5s));
  LicenseStatus answered;
  const Millis took = timed([&] { answered = h.license().status(); });
  // Answered from storage behind the stuck call, and saying so.
  EXPECT_EQ(answered, stalled(licensed(KEY)));
  EXPECT_GE(took, Millis{150});
  h.storage->readGate().open();
  EXPECT_EQ(stuck.get(), licensed(KEY));
}

/** A refresh() whose refusal write is held inside the adapter, and time past its grace. */
struct BehindAStuckCall {
  Harness& h;
  std::future<LicenseStatus> stuck;

  BehindAStuckCall(Harness& harness, const StoredState& state) : h(harness) {
    h.store(state);
    h.http->reply("validate", 403, {{"error", "revoked"}});
    h.storage->holdNextWrite();
    stuck = std::async(std::launch::async, [this] { return h.license().refresh(); });
    EXPECT_TRUE(h.storage->writeGate().waitReached(5s));
    EXPECT_EQ(h.storage->writes(), 1);
    std::this_thread::sleep_for(Millis{300});  // past its grace: 100 ms + 150 ms
  }
  ~BehindAStuckCall() {
    h.storage->writeGate().open();
    if (stuck.valid()) stuck.wait();
  }
};

TEST(Stalled, NoCallBehindAStuckWriteWritesAnythingALateStaleWriteCouldRevert) {
  Harness h(std::nullopt, quick());
  const std::string lease = h.sign(KEY, "perpetual", 30 * 24 * 3600);
  StoredState state = held(lease);
  {
    BehindAStuckCall behind(h, state);
    const std::string renewed = h.sign(KEY, "perpetual", 30 * 24 * 3600);
    h.http->reply("validate", 200, {{"lease", renewed}, {"activationId", "act_1"}});
    // Read-only: the lease still stored, licensed; not asked, nothing written.
    EXPECT_EQ(h.license().refresh(), stalled(licensed(KEY)));
    EXPECT_EQ(h.http->count("validate"), 1);
    EXPECT_EQ(h.storage->writes(), 1);
    EXPECT_EQ(h.stored()->lease, lease);
    h.storage->writeGate().open();
    EXPECT_EQ(behind.stuck.get(), status(LicenseState::Invalid, LicenseReason::Revoked, KEY));
    EXPECT_EQ(h.stored()->refused, keygrant::Refusal::Revoked);
  }
  // Settled: the next call writes again, and heals.
  EXPECT_EQ(h.license().refresh(), licensed(KEY));
  EXPECT_EQ(h.http->count("validate"), 2);
}

TEST(Stalled, ActivateIsNetworkBehindAStuckCallNoRequestNoWrite) {
  Harness h(std::nullopt, quick());
  BehindAStuckCall behind(h, licensedState(h));
  EXPECT_EQ(h.license().activate(KEY), failed(ActivateError::Network));
  EXPECT_FALSE(h.http->called("activate"));
  EXPECT_EQ(h.storage->writes(), 1);
}

TEST(Stalled, DeactivateThrowsBehindAStuckCallNoRequestAndTheKeyStillStored) {
  Harness h(std::nullopt, quick());
  BehindAStuckCall behind(h, licensedState(h));
  EXPECT_THROW(h.license().deactivate(), keygrant::StalledError);
  EXPECT_FALSE(h.http->called("deactivate"));
  EXPECT_EQ(h.stored()->key, KEY);
  EXPECT_EQ(h.storage->writes(), 1);
}

TEST(Stalled, StartTrialAnswersTheStoredTrialBehindAStuckCallNoRequestNoWrite) {
  Harness h(std::nullopt, quick());
  StoredState state = licensedState(h);
  state.trialStart = NOW;
  state.trialEndsAt = NOW + 5 * DAY;
  // Signed to the trial's end, as the server signs it.
  state.trialLease = h.sign("trial:fp-test", "trial", 5 * 24 * 3600);
  BehindAStuckCall behind(h, state);
  LicenseStatus expected = status(LicenseState::Trial);
  expected.trialEndsAt = NOW + 5 * DAY;
  expected.trialDaysLeft = 5;
  EXPECT_EQ(h.license().startTrial(), stalled(expected));
  EXPECT_FALSE(h.http->called("trial"));
  EXPECT_EQ(h.storage->writes(), 1);
}

TEST(Stalled, ATrialWhoseLeaseDoesNotVerifyIsNotAskedForAgainBehindAStuckCall) {
  // The stuck call: a refresh() that asked for the trial again (its lease signed
  // by another key, its stored end ahead) and is held saving the wait.
  Harness h(std::nullopt, quick());
  StoredState state;
  state.trialStart = NOW - 2 * DAY;
  state.trialEndsAt = NOW + 5 * DAY;
  state.trialLease = Harness().sign("trial:fp-test", "trial", 5 * 24 * 3600);
  state.lastSeen = NOW;
  BehindAStuckCall behind(h, state);
  EXPECT_EQ(h.http->count("trial"), 1);
  // Read-only behind it: the stored answer, no ask, no write.
  EXPECT_EQ(h.license().status(), stalled(status(LicenseState::Expired, LicenseReason::Trial)));
  EXPECT_EQ(h.http->count("trial"), 1);
  EXPECT_EQ(h.storage->writes(), 1);
}

TEST(Stalled, AStoredRefusalOrAnExpiredLeaseIsAnsweredAsStoredWithoutAsking) {
  Harness refused(std::nullopt, quick());
  StoredState state;
  state.key = KEY;
  state.activationId = "act_1";
  state.refused = keygrant::Refusal::Revoked;
  state.lastSeen = NOW;
  {
    BehindAStuckCall behind(refused, state);
    EXPECT_EQ(refused.license().status(), stalled(status(LicenseState::Invalid, LicenseReason::Revoked, KEY)));
    EXPECT_EQ(refused.http->count("validate"), 1);
  }
  Harness expired(std::nullopt, quick());
  {
    BehindAStuckCall behind(expired, held(expired.sign(KEY, "perpetual", 3600, NOW - 2 * DAY)));
    EXPECT_EQ(expired.license().status(), stalled(status(LicenseState::Expired, LicenseReason::Offline, KEY)));
    EXPECT_EQ(expired.http->count("validate"), 1);
  }
}

TEST(Serial, TimesTheGraceFromWhenTheStuckCallStartedHoweverLateTheNextIsQueued) {
  HarnessOptions options = quick(Millis{200});
  options.timings.lockGrace = Millis{800};  // a 1 s grace in all
  Harness h(std::nullopt, options);
  h.store(licensedState(h));
  h.http->reply("validate", 403, {{"error", "revoked"}});
  h.storage->holdNextWrite();
  const auto started = std::chrono::steady_clock::now();
  auto stuck = std::async(std::launch::async, [&] { return h.license().refresh(); });
  ASSERT_TRUE(h.storage->writeGate().waitReached(5s));
  std::this_thread::sleep_until(started + Millis{500});
  LicenseStatus answered;
  const Millis waited = timed([&] { answered = h.license().status(); });
  EXPECT_EQ(answered, stalled(licensed(KEY)));
  // About 500 ms (what was left of the second), not a whole second from when it was queued.
  EXPECT_GE(waited, Millis{300});
  EXPECT_LT(waited, Millis{900});
  h.storage->writeGate().open();
  stuck.wait();
}

TEST(Serial, OverlappingStatusCallsAskOneQuestionAndTheRefusalSurvives) {
  // Both run on a new major. Without the order, the second asked too and wrote
  // back its own stale snapshot after the first's refusal, bringing the lease back.
  HarnessOptions options;
  options.major = 3;
  Harness h(std::nullopt, options);
  h.store(onNewMajor(h));
  Gate answer;
  std::atomic<int> asked{0};
  h.http->on("validate", [&](const json&) {
    if (++asked > 1) return keygrant::HttpResponse{503, R"({"error":"busy"})"};
    answer.wait();
    return keygrant::HttpResponse{403, R"({"error":"revoked"})"};
  });
  auto first = std::async(std::launch::async, [&] { return h.license().status(); });
  ASSERT_TRUE(answer.waitReached(5s));
  auto second = std::async(std::launch::async, [&] { return h.license().status(); });
  std::this_thread::sleep_for(Millis{50});
  answer.open();
  const LicenseStatus revoked = status(LicenseState::Invalid, LicenseReason::Revoked, KEY);
  EXPECT_EQ(first.get(), revoked);
  EXPECT_EQ(second.get(), revoked);
  EXPECT_EQ(asked, 1);
  EXPECT_EQ(h.stored()->refused, keygrant::Refusal::Revoked);
  EXPECT_EQ(h.stored()->checkedMajor, 3);
  EXPECT_EQ(h.stored()->lease, std::nullopt);
}

TEST(Serial, ThreeCallsQueuedAtOnceOnASlowNetworkRunOneAfterAnotherEachAsking) {
  // Each answer takes 250 ms inside a 600 ms bound: the second and third wait for the one before.
  Harness h(std::nullopt, quick(Millis{600}));
  const std::string lease = h.sign(KEY, "perpetual", 30 * 24 * 3600);
  h.store(held(lease));
  std::atomic<int> inFlight{0};
  std::atomic<int> most{0};
  h.http->on("validate", [&](const json&) {
    const int now = ++inFlight;
    int seen = most.load();
    while (now > seen && !most.compare_exchange_weak(seen, now)) {
    }
    std::this_thread::sleep_for(Millis{250});
    --inFlight;
    return keygrant::HttpResponse{200, json({{"lease", lease}, {"activationId", "act_1"}}).dump()};
  });
  std::vector<std::future<LicenseStatus>> calls;
  for (int i = 0; i < 3; ++i) calls.push_back(std::async(std::launch::async, [&] { return h.license().refresh(); }));
  for (auto& call : calls) EXPECT_EQ(call.get(), licensed(KEY));
  EXPECT_EQ(h.http->count("validate"), 3);
  EXPECT_EQ(most, 1);
}

TEST(Lifetime, DestroyingALicenseWhileACallIsStuckInAnAdapterIsSafe) {
  auto storage = std::make_shared<MemoryStorage>();
  storage->hangNextRead();
  std::future<LicenseStatus> stuck;
  {
    Harness h(std::nullopt, quick());
    h.storage = storage;
    keygrant::LicenseConfig config = h.config;
    config.adapters.storage = storage;
    auto license = std::make_unique<keygrant::License>(config, h.timings);
    keygrant::License* raw = license.get();
    stuck = std::async(std::launch::async, [raw] { return raw->status(); });
    ASSERT_TRUE(storage->readGate().waitReached(5s));
    // The call holds the engine; the License object can go now.
    license.reset();
  }
  storage->readGate().open();
  EXPECT_EQ(stuck.get(), status(LicenseState::Unlicensed));
}

}  // namespace
}  // namespace kgtest
