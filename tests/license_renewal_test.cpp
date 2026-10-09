// Early renewal, the clock guard and its correction, answer stamps, and the
// majors noted inside a key's upgrade window.
#include "support.hpp"

namespace kgtest {
namespace {

const std::int64_t MONTH_S = 30 * 24 * 3600;
const std::int64_t RENEW_WITHIN = 7 * DAY;
const std::int64_t TOLERANCE = 3'600'000;

json leaseAnswer(const std::string& lease) { return {{"lease", lease}, {"activationId", "act_1"}}; }

/** An open lease (every version), carrying its upgrade window's end when given (ms). */
std::string openLease(const Harness& h, std::optional<std::int64_t> windowEnds = std::nullopt) {
  LeaseSpec spec;
  spec.major = keygrant::kOpenMajor;
  if (windowEnds) spec.extra = {{"upgradeUntil", *windowEnds / 1000}};
  return h.sign(spec);
}

HarnessOptions onMajor(std::int64_t major) {
  HarnessOptions o;
  o.major = major;
  return o;
}

TEST(EarlyRenewal, RenewsOnceWithinSevenDaysOfTheEndAndNotBefore) {
  Harness h;
  h.store(held(h.sign(KEY, "perpetual", MONTH_S)));
  const std::string renewed = h.sign(KEY, "perpetual", MONTH_S, NOW + 30 * DAY - RENEW_WITHIN);
  h.http->reply("validate", 200, leaseAnswer(renewed));
  h.setClock(NOW + 30 * DAY - RENEW_WITHIN - 1);
  EXPECT_EQ(h.license().status(), licensed(KEY));
  EXPECT_EQ(h.http->count("validate"), 0);
  h.setClock(NOW + 30 * DAY - RENEW_WITHIN + 1);
  EXPECT_EQ(h.license().status(), licensed(KEY));
  EXPECT_EQ(h.http->count("validate"), 1);
  EXPECT_EQ(h.stored()->lease, renewed);
  // Far from its end again: no ask on the next status().
  h.license().status();
  EXPECT_EQ(h.http->count("validate"), 1);
}

TEST(EarlyRenewal, RenewsAShortLeaseAtHalfItsLifeNotOnEveryStatus) {
  Harness h;
  h.store(held(h.sign(KEY, "subscription", 7 * 24 * 3600)));
  h.http->fail = true;
  h.setClock(NOW + 7 * DAY / 2 - 1);
  h.license().status();
  EXPECT_EQ(h.http->count("validate"), 0);
  h.setClock(NOW + 7 * DAY / 2 + 1);
  h.license().status();
  EXPECT_EQ(h.http->count("validate"), 1);
}

TEST(EarlyRenewal, KeepsTheValidLeaseWhenItGetsNothingAndWaitsBeforeTryingAgain) {
  Harness h;
  const std::string lease = h.sign(KEY, "perpetual", MONTH_S);
  h.store(held(lease));
  h.http->fail = true;
  h.setClock(NOW + 25 * DAY);
  EXPECT_EQ(h.license().status(), licensed(KEY));
  EXPECT_EQ(h.stored()->lease, lease);
  EXPECT_EQ(h.stored()->backoffSince, NOW + 25 * DAY);
  EXPECT_EQ(h.stored()->backoffMs, 60'000);
  h.license().status();
  EXPECT_EQ(h.http->count("validate"), 1);
}

TEST(EarlyRenewal, AnswersARefusalTheServerNoLongerStandsBehindTheLease) {
  Harness h;
  h.store(held(h.sign(KEY, "subscription", 7 * 24 * 3600)));
  h.http->reply("validate", 403, {{"error", "expired"}});
  h.setClock(NOW + 5 * DAY);
  EXPECT_EQ(h.license().status(), status(LicenseState::Invalid, LicenseReason::Revoked, KEY));
}

TEST(EarlyRenewal, HealsALeaseThatRanOutOfflineOnceTheShortWaitIsOver) {
  Harness h;
  h.store(held(h.sign(KEY, "perpetual", 3600, NOW - 2 * DAY)));
  h.http->fail = true;
  EXPECT_EQ(h.license().status(), status(LicenseState::Expired, LicenseReason::Offline, KEY));
  h.http->fail = false;
  h.http->reply("validate", 200, leaseAnswer(h.sign(KEY, "perpetual", MONTH_S, NOW)));
  h.setClock(NOW + 60'000 - 1);
  EXPECT_EQ(h.license().status().state, LicenseState::Expired);
  h.setClock(NOW + 60'000);
  EXPECT_EQ(h.license().status(), licensed(KEY));
}

TEST(EarlyRenewal, WaitsByTheDeviceClockAfterNoAnswerWhenTheGuardIsAheadOfIt) {
  // The clock was put back two hours: the guard is ahead of it. The wait
  // starts at the device clock, as askDue reads it, so the next status()
  // inside it asks nothing, rather than every one asking again.
  Harness h;
  StoredState state = held(h.sign(KEY, "perpetual", MONTH_S, NOW - 25 * DAY));
  state.lastSeen = NOW + 2 * 3'600'000;
  h.store(state);
  h.http->fail = true;
  EXPECT_EQ(h.license().status(), licensed(KEY));
  EXPECT_EQ(h.http->count("validate"), 1);
  EXPECT_EQ(h.stored()->backoffSince, NOW);
  EXPECT_EQ(h.stored()->backoffMs, 60'000);
  h.setClock(NOW + 30'000);
  EXPECT_EQ(h.license().status(), licensed(KEY));
  EXPECT_EQ(h.http->count("validate"), 1);
  h.setClock(NOW + 60'000);
  h.license().status();
  EXPECT_EQ(h.http->count("validate"), 2);
}

TEST(EarlyRenewal, KeysTheInstallByAMachineIdReadWhateverTheAskGets) {
  // No answer at all, an outage, a refusal: the seat is keyed by the machine id
  // the ask read and sent all the same.
  for (const int answer : {0, 503, 403}) {
    auto fingerprint = std::make_shared<FakeFingerprinter>("fp-machine");
    fingerprint->identifyAs(DeviceKind::Machine, {"fp-host"});
    HarnessOptions options;
    options.fingerprint = fingerprint;
    Harness h(std::nullopt, options);
    h.store(held(h.sign(KEY, "perpetual", MONTH_S)));
    if (answer == 0) {
      h.http->fail = true;
    } else {
      h.http->reply("validate", answer, {{"error", answer == 403 ? "revoked" : "unavailable"}});
    }
    h.license().refresh();
    EXPECT_EQ(h.http->bodyOf("validate")["fingerprintKind"], "machine") << answer;
    EXPECT_EQ(h.stored()->deviceKind, DeviceKind::Machine) << answer;
  }
}

TEST(EarlyRenewal, LicensesFromTheServersLeaseWhenTheStateCannotBeReadToJudgeIt) {
  // The disk goes away while the server answers: the fresh lease is judged by
  // the guarded clock the ask began with, not lost to the read that failed.
  Harness h;
  h.store(held(h.sign(KEY, "perpetual", MONTH_S, NOW - 25 * DAY)));
  const std::string renewed = h.sign(KEY, "perpetual", MONTH_S, NOW);
  const std::shared_ptr<MemoryStorage> storage = h.storage;
  h.http->on("validate", [storage, renewed](const json&) {
    storage->failReads = true;
    return keygrant::HttpResponse{200, json{{"lease", renewed}, {"activationId", "act_1"}}.dump()};
  });
  EXPECT_EQ(h.license().refresh(), licensed(KEY));
}

TEST(EarlyRenewal, LicensesAndSavesARenewalWhoseCheckOfTheServersTimeCannotReadTheState) {
  // The renewal reads the state again to bring the clock guard to the server's
  // time (serverNow): a read that fails there costs the correction, never the
  // renewal, which the save after it still writes.
  Harness h;
  h.store(held(h.sign(KEY, "perpetual", 3600, NOW - 2 * DAY)));
  const std::string fresh = h.sign(KEY, "perpetual", MONTH_S, NOW);
  const std::shared_ptr<MemoryStorage> storage = h.storage;
  h.http->on("validate", [storage, fresh](const json&) {
    // The first read after the answer: the check of the server's time.
    storage->failNextRead = true;
    return keygrant::HttpResponse{200, json{{"lease", fresh}}.dump()};
  });
  EXPECT_EQ(h.license().status(), licensed(KEY));
  EXPECT_FALSE(storage->failNextRead);
  EXPECT_EQ(h.stored()->lease, fresh);
}

TEST(Waits, AreTimedByTheDeviceClockNotTheGuard) {
  // The guard (lastSeen) is a day ahead of the device clock, and the last ask
  // was half an hour ago by the device clock: its hour's wait is not over,
  // though by the guard it would be.
  struct Case {
    const char* name;
    std::function<StoredState(const Harness&)> stored;
  };
  const std::vector<Case> cases = {
      {"a stored refusal",
       [](const Harness&) {
         StoredState s;
         s.key = KEY;
         s.activationId = "act_1";
         s.refused = keygrant::Refusal::Revoked;
         return s;
       }},
      {"a lease past its end", [](const Harness& h) { return held(h.sign(KEY, "perpetual", 3600, NOW - 2 * DAY)); }},
      {"a valid lease near its end", [](const Harness& h) { return held(h.sign(KEY, "perpetual", MONTH_S, NOW - 26 * DAY)); }},
  };
  for (const Case& c : cases) {
    Harness h;
    StoredState state = c.stored(h);
    state.backoffSince = NOW - 30 * 60'000;
    state.backoffMs = 3'600'000;
    state.lastSeen = NOW + DAY;
    h.store(state);
    h.http->fail = true;
    h.license().status();
    EXPECT_FALSE(h.http->called("validate")) << c.name;
  }
}

TEST(Waits, StartAfterARenewalByTheDeviceClockNotTheGuard) {
  Harness h;
  StoredState state = held(h.sign(KEY, "perpetual", 3600, NOW - 2 * DAY));
  state.lastSeen = NOW + DAY;
  h.store(state);
  h.http->fail = true;
  EXPECT_EQ(h.license().status(), status(LicenseState::Expired, LicenseReason::Offline, KEY));
  EXPECT_EQ(h.stored()->backoffSince, NOW);
  EXPECT_EQ(h.stored()->backoffMs, 60'000);
}

TEST(EarlyRenewal, ALeaseTheServerStandsBehindForAnotherBuildOrDeviceClearsAStoredRefusalAndItsWait) {
  // The key reinstated after a refusal: the lease the server now signs does
  // not license this build (or this device), and is kept so every later
  // status() says so; the old refusal and its wait must go with it, or the
  // next status() would answer the refusal the server no longer stands behind.
  struct Case {
    const char* name;
    ordered_json claims;
    LicenseReason reason;
  };
  for (const Case& c : std::vector<Case>{
           {"for older builds", {{"major", 2}}, LicenseReason::Upgrade},
           {"for newer builds", {{"major", 5}, {"minMajor", 4}}, LicenseReason::Outdated},
           {"for another device", {{"major", 3}, {"fp", claimOf("fp-other")}, {"fpk", "machine"}}, LicenseReason::Device},
       }) {
    Harness h(std::nullopt, onMajor(3));
    LeaseSpec before;
    before.major = 3;
    StoredState state = held(h.sign(before), KEY, 3);
    state.refused = keygrant::Refusal::Revoked;
    state.backoffSince = NOW - 2 * 3'600'000;
    state.backoffMs = 3'600'000;
    h.store(state);
    LeaseSpec spec;
    spec.major = c.claims["major"].get<std::int64_t>();
    spec.extra = c.claims;
    spec.extra.erase("major");
    const std::string lease = h.sign(spec);
    h.http->reply("validate", 200, leaseAnswer(lease));
    EXPECT_EQ(h.license().status(), status(LicenseState::Invalid, c.reason, KEY)) << c.name;
    EXPECT_EQ(h.stored()->lease, lease) << c.name;
    EXPECT_EQ(h.stored()->refused, std::nullopt) << c.name;
    EXPECT_EQ(h.stored()->refusedMajor, std::nullopt) << c.name;
    EXPECT_EQ(h.stored()->refusedFor, std::nullopt) << c.name;
    EXPECT_EQ(h.stored()->backoffSince, std::nullopt) << c.name;
    EXPECT_EQ(h.stored()->backoffMs, std::nullopt) << c.name;
  }
}

TEST(ClockGuard, LicensesAgainOnlineAfterAClockThatJumpedSixtyDaysAheadIsPutRight) {
  Harness h;
  StoredState state = held(h.sign(KEY, "perpetual", MONTH_S, NOW - DAY));
  state.lastSeen = NOW + 60 * DAY;
  h.store(state);
  h.http->reply("validate", 200, leaseAnswer(h.sign(KEY, "perpetual", MONTH_S, NOW)));
  EXPECT_EQ(h.license().status(), licensed(KEY));
  EXPECT_EQ(h.stored()->lastSeen, NOW);
  h.http->fail = true;
  EXPECT_EQ(h.license().status(), licensed(KEY));
}

TEST(ClockGuard, LicensesFromTheServersLeaseWhenTheGuardItPutsRightCannotBeSaved) {
  Harness h;
  StoredState state = held(h.sign(KEY, "perpetual", MONTH_S, NOW - DAY));
  state.lastSeen = NOW + 60 * DAY;
  h.store(state);
  h.http->reply("validate", 200, leaseAnswer(h.sign(KEY, "perpetual", MONTH_S, NOW)));
  h.storage->failWrites = true;
  EXPECT_EQ(h.license().status(), licensed(KEY));
  EXPECT_EQ(h.stored()->lastSeen, NOW + 60 * DAY);
}

/** The guard after a refresh() answered with a fresh lease signed at NOW, the clock at `clock`. */
std::optional<std::int64_t> guardAfterRefresh(std::int64_t guard, std::int64_t clock = NOW) {
  Harness h;
  const std::string lease = h.sign(KEY, "perpetual", MONTH_S, NOW);
  StoredState state = held(lease);
  state.lastSeen = guard;
  h.store(state);
  h.setClock(clock);
  h.http->reply("validate", 200, leaseAnswer(lease));
  EXPECT_EQ(h.license().refresh().state, LicenseState::Licensed);
  return h.stored()->lastSeen;
}

TEST(ClockGuard, IsLeftForAFreshLeaseWithinTheToleranceAndBroughtBackPastIt) {
  EXPECT_EQ(guardAfterRefresh(NOW + 30 * 60'000), NOW + 30 * 60'000);
  EXPECT_EQ(guardAfterRefresh(NOW + TOLERANCE), NOW + TOLERANCE);
  EXPECT_EQ(guardAfterRefresh(NOW + TOLERANCE + 1), NOW);
  // To the lease's signing time, not to a device clock behind it.
  EXPECT_EQ(guardAfterRefresh(NOW + 60 * DAY, NOW - 2 * 3'600'000), NOW);
}

TEST(ClockGuard, IsBroughtBackForALeaseNamingTheKeyInAnotherCaseAsTheServerMatchesIt) {
  Harness h;
  StoredState state = held(h.sign(KEY, "perpetual", MONTH_S, NOW));
  state.lastSeen = NOW + 60 * DAY;
  h.store(state);
  h.http->reply("validate", 200, leaseAnswer(h.sign("Sluice-aaaa-BBBB-CCCC-DDDD", "perpetual", MONTH_S, NOW)));
  h.license().refresh();
  EXPECT_EQ(h.stored()->lastSeen, NOW);
}

TEST(ClockGuard, IsNotMovedByALeaseForAnotherKeyAnotherActivationOrOlderThanTheOneHeld) {
  struct Case {
    const char* name;
    std::string sub;
    std::string aid;
    std::int64_t at;
  };
  for (const Case& c : std::vector<Case>{{"another key", "SLUICE-OTHR-OTHR-OTHR-OTHR", "act_1", NOW},
                                          {"another activation", KEY, "act_2", NOW},
                                          {"older than the one held", KEY, "act_1", NOW - DAY}}) {
    Harness h;
    StoredState state = held(h.sign(KEY, "perpetual", MONTH_S, NOW));
    state.lastSeen = NOW + 60 * DAY;
    h.store(state);
    LeaseSpec spec;
    spec.sub = c.sub;
    spec.aid = c.aid;
    spec.now = c.at;
    h.http->reply("validate", 200, {{"lease", h.sign(spec)}, {"activationId", c.aid}});
    h.license().refresh();
    EXPECT_EQ(h.stored()->lastSeen, NOW + 60 * DAY) << c.name;
  }
}

TEST(Stamps, AnActivationAndADeactivationAreStampedWithTheirTime) {
  Harness h;
  h.http->reply("activate", 200, leaseAnswer(h.sign(KEY, "perpetual", MONTH_S)));
  h.http->reply("deactivate", 200, {{"ok", true}});
  h.license().activate(KEY);
  EXPECT_EQ(h.stored()->verdictAt, NOW);
  h.setClock(NOW + DAY);
  h.license().deactivate();
  EXPECT_EQ(h.stored()->verdictAt, NOW + DAY);
}

TEST(Stamps, AreByTheGuardedClockNeverEarlierThanTheLastTimeSeen) {
  StoredState seen;
  seen.lastSeen = NOW + 5 * 60'000;
  Harness h(seen);
  h.http->reply("activate", 200, leaseAnswer(h.sign(KEY, "perpetual", MONTH_S)));
  h.http->reply("deactivate", 200, {{"ok", true}});
  h.http->reply("trial", 200, {{"trialStart", NOW}, {"trialEndsAt", NOW + 5 * DAY}, {"lease", h.sign("trial:fp-test", "trial", 7 * 24 * 3600)}});
  h.license().activate(KEY);
  EXPECT_EQ(h.stored()->verdictAt, NOW + 5 * 60'000);
  h.setClock(NOW - DAY);
  h.license().deactivate();
  EXPECT_EQ(h.stored()->verdictAt, NOW + 5 * 60'000);
  h.license().startTrial();
  EXPECT_EQ(h.stored()->trialAt, NOW + 5 * 60'000);
}

TEST(Stamps, OneProcessSavesItsOwnAnswersAfterTheClockIsPutBack) {
  Harness h;
  StoredState state = held(h.sign(KEY, "perpetual", MONTH_S, NOW - DAY));
  // The last answer was stamped by a clock 60 days ahead, since put right.
  state.verdictAt = NOW + 60 * DAY;
  h.store(state);
  h.http->reply("validate", 403, {{"error", "revoked"}});
  EXPECT_EQ(h.license().refresh(), status(LicenseState::Invalid, LicenseReason::Revoked, KEY));
  EXPECT_EQ(h.stored()->refused, keygrant::Refusal::Revoked);
  EXPECT_EQ(h.stored()->verdictAt, NOW);
  EXPECT_EQ(h.stored()->lease, std::nullopt);
}

TEST(KeptMajor, IsNotedWhileTheWindowIsOpenAndSentWithTheNextValidate) {
  Harness h(std::nullopt, onMajor(3));
  h.store(held(openLease(h, NOW + 10 * DAY), KEY, 3));
  h.license().status();
  EXPECT_EQ(h.stored()->keptMajor, 3);
  h.http->reply("validate", 503, {{"error", "unavailable"}});
  h.license().refresh();
  EXPECT_EQ(h.http->bodyOf("validate")["major"], 3);
  EXPECT_EQ(h.http->bodyOf("validate")["keptMajor"], 3);
}

TEST(KeptMajor, IsNotedAllTheSameWhenItCannotBeSavedSentWithTheCheck) {
  Harness h(std::nullopt, onMajor(3));
  const std::string lease = openLease(h, NOW + 10 * DAY);
  h.store(held(lease, KEY, 2));
  h.http->reply("validate", 200, leaseAnswer(lease));
  h.storage->failWrites = true;
  EXPECT_EQ(h.license().status(), licensed(KEY));
  EXPECT_EQ(h.http->bodyOf("validate")["keptMajor"], 3);
}

TEST(KeptMajor, StopsAtTheWindowsEndItsLastSecondClosedTheOneBeforeOpen) {
  const std::int64_t end = NOW + 10 * DAY;
  for (const auto& [clock, kept] : std::vector<std::pair<std::int64_t, std::int64_t>>{{end, 3}, {end - 1000, 4}, {NOW + 11 * DAY, 3}}) {
    Harness h(std::nullopt, onMajor(4));
    StoredState state = held(openLease(h, end), KEY, 4);
    state.keptMajor = 3;
    h.store(state);
    h.setClock(clock);
    EXPECT_EQ(h.license().status().state, LicenseState::Licensed);
    EXPECT_EQ(h.stored()->keptMajor, kept) << clock - end;
  }
}

TEST(KeptMajor, IsNotNotedOnALeaseThatCarriesNoWindow) {
  Harness h(std::nullopt, onMajor(3));
  h.store(held(openLease(h), KEY, 3));
  h.license().status();
  EXPECT_EQ(h.stored()->keptMajor, std::nullopt);
  EXPECT_EQ(h.http->count("validate"), 0);
}

TEST(KeptMajor, StartsAfreshOnActivateAndIsDroppedOnDeactivate) {
  StoredState old;
  old.key = "OLD-KEY";
  old.activationId = "act_old";
  old.keptMajor = 7;
  old.checkedMajor = 7;
  Harness h(old, onMajor(2));
  h.http->reply("activate", 200, leaseAnswer(openLease(h, NOW + 10 * DAY)));
  EXPECT_EQ(h.license().activate(KEY), ok());
  EXPECT_EQ(h.stored()->key, KEY);
  EXPECT_EQ(h.stored()->checkedMajor, 2);
  EXPECT_EQ(h.stored()->keptMajor, 2);
  h.http->reply("deactivate", 200, {{"ok", true}});
  h.license().deactivate();
  EXPECT_EQ(h.stored()->keptMajor, std::nullopt);
  EXPECT_EQ(h.stored()->checkedMajor, std::nullopt);
}

/** A fingerprinter that, the first time it is read, runs `meanwhile` first: another instance saving. */
class SavesMeanwhile final : public keygrant::Fingerprinter {
 public:
  std::function<void()> meanwhile;
  std::string get() override {
    if (!ran_.exchange(true) && meanwhile) meanwhile();
    return "fp-test";
  }

 private:
  std::atomic<bool> ran_{false};
};

TEST(KeptMajor, IsNeverLandedOnAnotherActivationSavedMeanwhile) {
  // Another instance activates another key while this one judges its lease:
  // the major this run noted (keptMajor) is about the activation it judged.
  auto fingerprint = std::make_shared<SavesMeanwhile>();
  HarnessOptions options = onMajor(5);
  options.fingerprint = fingerprint;
  Harness h(std::nullopt, options);
  h.store(held(openLease(h, NOW + 10 * DAY), KEY, 5));
  StoredState theirs;
  theirs.key = "SLUICE-OTHR-OTHR-OTHR-OTHR";
  theirs.activationId = "act_2";
  theirs.lease = "theirs";
  theirs.lastSeen = NOW;
  fingerprint->meanwhile = [&h, theirs] { h.store(theirs); };
  h.http->fail = true;
  EXPECT_EQ(h.license().status().state, LicenseState::Licensed);
  EXPECT_EQ(h.stored()->activationId, "act_2");
  EXPECT_EQ(h.stored()->keptMajor, std::nullopt);
}

}  // namespace
}  // namespace kgtest
