// startTrial() and a trial's status.
#include "support.hpp"

namespace kgtest {
namespace {

/** A trial lease as the server signs one: to the trial's own end. */
std::string trialLeaseTo(const Harness& h, std::int64_t ends, std::int64_t signedAt = NOW) {
  return h.sign("trial:fp-test", "trial", (ends - signedAt) / 1000, signedAt);
}

/**
 * The server's answer to a trial ask: a start two days ago, ending in five,
 * with a lease signed to that end (none for a trial that has ended: the
 * server signs one only while time remains).
 */
json trialAnswer(const Harness& h, std::int64_t start = NOW - 2 * DAY, std::int64_t ends = NOW + 5 * DAY) {
  json answer = {{"trialStart", start}, {"trialEndsAt", ends}};
  if (ends > NOW) answer["lease"] = trialLeaseTo(h, ends);
  return answer;
}

TEST(Trial, StartsVerifiesTheLeaseAndStashesTheStart) {
  Harness h;
  h.http->reply("trial", 200, trialAnswer(h));
  const LicenseStatus started = h.license().startTrial();
  EXPECT_EQ(started.state, LicenseState::Trial);
  EXPECT_EQ(started.trialDaysLeft, 5);
  EXPECT_EQ(started.trialEndsAt, NOW + 5 * DAY);
  EXPECT_EQ(h.stored()->trialStart, NOW - 2 * DAY);
  EXPECT_EQ(h.stored()->trialAt, NOW);
  EXPECT_EQ(h.registry->get(), NOW - 2 * DAY);
  EXPECT_EQ(h.http->bodyOf("trial"), json({{"fingerprint", "fp-test"}}));
}

TEST(Trial, NeverMovesTheStartForwardFromTheStoreOrTheStash) {
  StoredState state;
  state.trialStart = NOW - 10 * DAY;
  Harness h(state);
  h.registry->set(NOW - 8 * DAY);
  h.http->reply("trial", 200, trialAnswer(h, NOW - 2 * DAY, NOW + 4 * DAY));
  h.license().startTrial();
  EXPECT_EQ(h.stored()->trialStart, NOW - 10 * DAY);
  EXPECT_EQ(h.http->bodyOf("trial")["clientStart"], NOW - 10 * DAY);

  // The state file wiped, the stash intact: the server is still told the earlier start.
  Harness wiped;
  wiped.registry->set(NOW - 10 * DAY);
  wiped.http->reply("trial", 200, trialAnswer(wiped));
  wiped.license().startTrial();
  EXPECT_EQ(wiped.http->bodyOf("trial")["clientStart"], NOW - 10 * DAY);
  EXPECT_EQ(wiped.stored()->trialStart, NOW - 10 * DAY);

  // The stash earlier than the state file: the stash's start is the one sent.
  StoredState later;
  later.trialStart = NOW - 3 * DAY;
  Harness stashEarlier(later);
  stashEarlier.registry->set(NOW - 9 * DAY);
  stashEarlier.http->reply("trial", 200, trialAnswer(stashEarlier));
  stashEarlier.license().startTrial();
  EXPECT_EQ(stashEarlier.http->bodyOf("trial")["clientStart"], NOW - 9 * DAY);
  EXPECT_EQ(stashEarlier.stored()->trialStart, NOW - 9 * DAY);
}

TEST(Trial, EndsWhenTheServerSaysSoDroppingTheLeaseItSignedBefore) {
  // The server signs a trial lease only while time remains: an ended trial's answer carries none.
  Harness h;
  StoredState state;
  state.trialStart = NOW - 8 * DAY;
  state.trialEndsAt = NOW + DAY;
  state.trialLease = h.sign("trial:fp-test", "trial", 7 * 24 * 3600, NOW - DAY);
  state.lastSeen = NOW;
  h.store(state);
  EXPECT_EQ(h.license().status().state, LicenseState::Trial);
  h.http->reply("trial", 200, {{"trialStart", NOW - 8 * DAY}, {"trialEndsAt", NOW - DAY}});
  EXPECT_EQ(h.license().startTrial(), status(LicenseState::Expired, LicenseReason::Trial));
  EXPECT_EQ(h.stored()->trialLease, std::nullopt);
  EXPECT_EQ(h.stored()->trialEndsAt, NOW - DAY);
  EXPECT_EQ(h.license().status(), status(LicenseState::Expired, LicenseReason::Trial));
}

TEST(Trial, StartsAllTheSameWhenTheStashCannotBeReadSendingNoClientStartFromIt) {
  // The stash is the app's own adapter; one that throws must cost nothing.
  Harness h;
  h.registry->failReads = true;
  h.http->reply("trial", 200, trialAnswer(h));
  const LicenseStatus started = h.license().startTrial();
  EXPECT_EQ(started.state, LicenseState::Trial);
  EXPECT_EQ(started.trialDaysLeft, 5);
  EXPECT_EQ(h.stored()->trialStart, NOW - 2 * DAY);
  EXPECT_FALSE(h.http->bodyOf("trial").contains("clientStart"));
}

TEST(Trial, KeepsTheServersTrialWhenTheStashCannotBeWritten) {
  Harness h;
  h.registry->failWrites = true;
  const json answer = trialAnswer(h);
  h.http->reply("trial", 200, answer);
  const LicenseStatus started = h.license().startTrial();
  EXPECT_EQ(started.state, LicenseState::Trial);
  EXPECT_EQ(started.trialDaysLeft, 5);
  // Saved, though it could not be mirrored: the stash is a copy, not the record.
  EXPECT_EQ(h.stored()->trialStart, NOW - 2 * DAY);
  EXPECT_EQ(h.stored()->trialEndsAt, NOW + 5 * DAY);
  EXPECT_EQ(h.stored()->trialLease, answer["lease"]);
  EXPECT_EQ(h.registry->get(), std::nullopt);
}

TEST(Trial, FallsBackToLocalEvidenceOfflineOrOnAnythingButAUsableAnswer) {
  for (int code : {0, 400, 503}) {
    Harness h;
    StoredState state;
    state.trialStart = NOW - DAY;
    state.trialEndsAt = NOW + 6 * DAY;
    state.trialLease = h.sign("trial:fp-test", "trial", 7 * 24 * 3600);
    state.lastSeen = NOW;
    h.store(state);
    if (code == 0) {
      h.http->fail = true;
    } else {
      h.http->reply("trial", code, {{"error", "bad"}});
    }
    EXPECT_EQ(h.license().startTrial().state, LicenseState::Trial) << code;
  }
}

TEST(Trial, IgnoresAMalformedTrialBodyWritingNoStart) {
  Harness h;
  h.http->reply("trial", 200, {{"trialStart", "oops"}, {"lease", "x"}});
  EXPECT_EQ(h.license().startTrial(), status(LicenseState::Unlicensed));
  EXPECT_FALSE(h.stored() && h.stored()->trialStart);
}

TEST(Trial, RunsOnlyOnAValidTrialLeaseToTheEndItWasSignedTo) {
  // A live trial: its lease signed to its end.
  Harness live;
  StoredState leased;
  leased.trialStart = NOW - 2 * DAY;
  leased.trialEndsAt = NOW + 5 * DAY;
  leased.trialLease = trialLeaseTo(live, NOW + 5 * DAY, NOW - 2 * DAY);
  leased.lastSeen = NOW;
  live.store(leased);
  LicenseStatus expected = status(LicenseState::Trial);
  expected.trialEndsAt = NOW + 5 * DAY;
  expected.trialDaysLeft = 5;
  EXPECT_EQ(live.license().status(), expected);

  // Over: the lease has run out with the trial.
  Harness over;
  StoredState ended = leased;
  ended.trialStart = NOW - 10 * DAY;
  ended.trialEndsAt = NOW - DAY;
  ended.trialLease = trialLeaseTo(over, NOW - DAY, NOW - 10 * DAY);
  over.store(ended);
  EXPECT_EQ(over.license().status(), status(LicenseState::Expired, LicenseReason::Trial));

  // The stored end alone, however far out: unsigned, it runs no trial.
  StoredState byEnd;
  byEnd.trialStart = NOW - 2 * DAY;
  byEnd.trialEndsAt = NOW + 5 * DAY;
  Harness endOnly(byEnd);
  EXPECT_EQ(endOnly.license().status(), status(LicenseState::Expired, LicenseReason::Trial));
}

TEST(Trial, IsNotExtendedByAnEndEditedInTheStateFile) {
  // No valid lease: an end edited a year out is still no trial.
  Harness edited;
  StoredState runOut;
  runOut.trialStart = NOW - 20 * DAY;
  runOut.trialEndsAt = NOW + 365 * DAY;
  runOut.trialLease = trialLeaseTo(edited, NOW - 6 * DAY, NOW - 20 * DAY);
  runOut.lastSeen = NOW;
  edited.store(runOut);
  EXPECT_EQ(edited.license().status(), status(LicenseState::Expired, LicenseReason::Trial));

  // A valid lease: the end shown is the lease's, not the edited one.
  Harness leased;
  StoredState withLease;
  withLease.trialStart = NOW - 2 * DAY;
  withLease.trialEndsAt = NOW + 365 * DAY;
  withLease.trialLease = trialLeaseTo(leased, NOW + 5 * DAY, NOW - 2 * DAY);
  withLease.lastSeen = NOW;
  leased.store(withLease);
  LicenseStatus expected = status(LicenseState::Trial);
  expected.trialEndsAt = NOW + 5 * DAY;
  expected.trialDaysLeft = 5;
  EXPECT_EQ(leased.license().status(), expected);
  // And once the lease's end has passed, the trial is over whatever the file says.
  leased.setClock(NOW + 5 * DAY + 1000);
  EXPECT_EQ(leased.license().status(), status(LicenseState::Expired, LicenseReason::Trial));
}

TEST(Trial, StaysLiveOfflineForEveryDayOfAFourteenDayTrialOnTheLeaseSignedToItsEnd) {
  Harness h;
  h.http->reply("trial", 200, trialAnswer(h, NOW, NOW + 14 * DAY));
  EXPECT_EQ(h.license().startTrial().trialDaysLeft, 14);
  // Offline from here on: no renewal of the trial lease is ever needed.
  h.http->fail = true;
  h.setClock(NOW + 10 * DAY);
  LicenseStatus dayTen = status(LicenseState::Trial);
  dayTen.trialEndsAt = NOW + 14 * DAY;
  dayTen.trialDaysLeft = 4;
  EXPECT_EQ(h.license().status(), dayTen);
  h.setClock(NOW + 14 * DAY - 1000);
  EXPECT_EQ(h.license().status().state, LicenseState::Trial);
  h.setClock(NOW + 14 * DAY + 1000);
  EXPECT_EQ(h.license().status(), status(LicenseState::Expired, LicenseReason::Trial));
  EXPECT_FALSE(h.http->called("validate"));
}

TEST(Trial, ShowsTheLeasesEndWhenNoEndIsStored) {
  Harness h;
  const std::string claim = claimOf("fp-test");
  LeaseSpec spec;
  spec.sub = "trial:" + claim;
  spec.aid = claim;
  spec.model = "trial";
  spec.lim = 1;
  spec.major = keygrant::kOpenMajor;
  spec.ttlSeconds = 7 * 24 * 3600;
  StoredState state;
  state.trialStart = NOW - 2 * DAY;
  state.trialLease = h.sign(spec);
  state.lastSeen = NOW;
  h.store(state);
  const LicenseStatus s = h.license().status();
  EXPECT_EQ(s.state, LicenseState::Trial);
  EXPECT_EQ(s.trialEndsAt, NOW + 7 * DAY);
  EXPECT_EQ(s.trialDaysLeft, 7);
}

TEST(Trial, IsNotRunOnALicencesLeaseInTheTrialsPlace) {
  Harness h;
  StoredState state;
  state.trialStart = NOW - 30 * DAY;
  state.trialLease = h.sign(KEY, "perpetual", 30 * 24 * 3600);
  state.lastSeen = NOW;
  h.store(state);
  EXPECT_EQ(h.license().status(), status(LicenseState::Expired, LicenseReason::Trial));
}

/** A running trial: started two days ago, its lease signed by the harness's key to its end, five days ahead. */
StoredState runningTrial(const Harness& h) {
  StoredState state;
  state.trialStart = NOW - 2 * DAY;
  state.trialEndsAt = NOW + 5 * DAY;
  state.trialLease = trialLeaseTo(h, NOW + 5 * DAY);
  state.lastSeen = NOW;
  return state;
}

TEST(Trial, KeepsTheTrialLeaseItCanVerifyWhenTheServerAnswersWithOneItCannot) {
  // The product's key rotated ahead of this build: every answer is signed with
  // a key the build does not carry. Taking it would end the running trial at
  // the next sync.
  Harness h;
  const StoredState ours = runningTrial(h);
  h.store(ours);
  const std::string rotated = trialLeaseTo(Harness(), NOW + 5 * DAY);
  h.http->reply("trial", 200, {{"trialStart", NOW - 2 * DAY}, {"trialEndsAt", NOW + 5 * DAY}, {"lease", rotated}});
  LicenseStatus expected = status(LicenseState::Trial);
  expected.trialEndsAt = NOW + 5 * DAY;
  expected.trialDaysLeft = 5;
  EXPECT_EQ(h.license().startTrial(), expected);
  EXPECT_EQ(h.stored()->trialLease, ours.trialLease);
}

TEST(Trial, TakesAValidTrialLeaseTheServerAnswersWithOverTheValidOneStored) {
  // An extension, or a lease carrying new entitlements: the server's word wins
  // whenever its lease verifies here.
  Harness h;
  StoredState state;
  state.trialStart = NOW - 4 * DAY;
  state.trialEndsAt = NOW + 3 * DAY;
  state.trialLease = trialLeaseTo(h, NOW + 3 * DAY);
  state.lastSeen = NOW;
  h.store(state);
  LeaseSpec spec;
  spec.sub = "trial:fp-test";
  spec.model = "trial";
  spec.lim = 1;
  spec.ttlSeconds = 6 * 24 * 3600;
  spec.extra = {{"ent", {{"edition", "pro"}}}};
  const std::string extended = h.sign(spec);
  h.http->reply("trial", 200, {{"trialStart", NOW - 4 * DAY}, {"trialEndsAt", NOW + 6 * DAY}, {"lease", extended}});
  LicenseStatus expected = status(LicenseState::Trial);
  expected.trialEndsAt = NOW + 6 * DAY;
  expected.trialDaysLeft = 6;
  expected.entitlements = {{"edition", "pro"}};
  EXPECT_EQ(h.license().startTrial(), expected);
  EXPECT_EQ(h.stored()->trialLease, extended);
}

TEST(Trial, TakesTheServersWordWhenItSaysTheTrialIsOverWhateverLeaseIsHeld) {
  Harness h;
  h.store(runningTrial(h));
  h.http->reply("trial", 200, {{"trialStart", NOW - 9 * DAY}, {"trialEndsAt", NOW - 2 * DAY}});
  EXPECT_EQ(h.license().startTrial(), status(LicenseState::Expired, LicenseReason::Trial));
  EXPECT_EQ(h.stored()->trialLease, std::nullopt);
}

TEST(Trial, AnswersUnlicensedForATrialLeaseStoredWithoutAStartAskingNothing) {
  // Only a stored start is a trial this device began; no save leaves a lease or an end without one.
  Harness h;
  StoredState state;
  state.trialEndsAt = NOW + 5 * DAY;
  state.trialLease = h.sign("trial:fp-test", "trial", 5 * 24 * 3600);
  state.lastSeen = NOW;
  h.store(state);
  EXPECT_EQ(h.license().status(), status(LicenseState::Unlicensed));
  EXPECT_FALSE(h.http->called("trial"));
}

TEST(Trial, IsTheServersAllTheSameWhenItCannotBeSavedKeptAndSavedOnceItCan) {
  Harness h;
  const json answer = trialAnswer(h);
  h.http->reply("trial", 200, answer);
  h.storage->failWrites = true;
  EXPECT_EQ(h.license().startTrial().trialDaysLeft, 5);
  EXPECT_EQ(h.stored(), std::nullopt);
  // The next status() reads no trial on the disk, and answers the one it holds.
  EXPECT_EQ(h.license().status().trialDaysLeft, 5);
  EXPECT_EQ(h.stored(), std::nullopt);
  h.storage->failWrites = false;
  EXPECT_EQ(h.license().status().trialDaysLeft, 5);
  ASSERT_TRUE(h.stored());
  EXPECT_EQ(h.stored()->trialStart, NOW - 2 * DAY);
  EXPECT_EQ(h.stored()->trialEndsAt, NOW + 5 * DAY);
  EXPECT_EQ(h.stored()->trialLease, answer["lease"]);
  // Saved once: not written again by every status() after it.
  const int writes = h.storage->writes();
  h.license().status();
  EXPECT_EQ(h.storage->writes(), writes);
}

TEST(Trial, NeverMovesAStartLaterWhenItSavesTheTrialItKept) {
  Harness h;
  h.http->reply("trial", 200, trialAnswer(h));
  h.storage->failWrites = true;
  h.license().startTrial();
  StoredState earlier;
  earlier.trialStart = NOW - 9 * DAY;
  h.store(earlier);  // another instance saved an earlier start meanwhile
  h.storage->failWrites = false;
  h.license().status();
  EXPECT_EQ(h.stored()->trialStart, NOW - 9 * DAY);
  EXPECT_EQ(h.stored()->trialEndsAt, NOW + 5 * DAY);
}

/** What another instance saved, signed (where it holds a lease) by the harness's key. */
using Theirs = std::function<StoredState(const Harness&)>;

Theirs just(StoredState state) {
  return [state](const Harness&) { return state; };
}

/** startTrial() at NOW kept unsaved; then another instance saves `theirs`; writes work again unless `stillFailing`. */
std::unique_ptr<Harness> keptThen(const Theirs& theirs, bool stillFailing = false) {
  auto h = std::make_unique<Harness>();
  h->http->reply("trial", 200, trialAnswer(*h));
  h->storage->failWrites = true;
  EXPECT_EQ(h->license().startTrial().trialDaysLeft, 5);
  h->store(theirs(*h));
  h->storage->failWrites = stillFailing;
  return h;
}

/** Another instance's trial answer, asked after this one's: ending at `ends`, with the lease the server signs to it while time remains. */
Theirs trialAnsweredLater(std::int64_t ends, std::optional<std::int64_t> at = NOW + 1) {
  return [ends, at](const Harness& h) {
    StoredState s;
    s.trialStart = NOW - 9 * DAY;
    s.trialEndsAt = ends;
    if (ends > NOW) s.trialLease = trialLeaseTo(h, ends, NOW - 9 * DAY);
    s.trialAt = at;
    return s;
  };
}

TEST(KeptTrial, NeverMakesTheTrialLongerTheNewerAnswersEarlierEndStands) {
  const auto h = keptThen(trialAnsweredLater(NOW - 2 * DAY));
  EXPECT_EQ(h->license().status(), status(LicenseState::Expired, LicenseReason::Trial));
  EXPECT_EQ(h->stored()->trialEndsAt, NOW - 2 * DAY);
  EXPECT_EQ(h->stored()->trialAt, NOW + 1);
  const auto failing = keptThen(trialAnsweredLater(NOW - 2 * DAY), true);
  EXPECT_EQ(failing->license().status(), status(LicenseState::Expired, LicenseReason::Trial));
}

TEST(KeptTrial, TakesTheNewerAnswersLaterEndTooATrialTheServerExtended) {
  const auto h = keptThen(trialAnsweredLater(NOW + 20 * DAY));
  EXPECT_EQ(h->license().status().trialDaysLeft, 20);
  EXPECT_EQ(h->stored()->trialEndsAt, NOW + 20 * DAY);
}

TEST(KeptTrial, WinsOverAStoredTrialWithNoStamp) {
  const auto h = keptThen(trialAnsweredLater(NOW - 2 * DAY, std::nullopt));
  EXPECT_EQ(h->license().status().trialDaysLeft, 5);
  EXPECT_EQ(h->stored()->trialStart, NOW - 9 * DAY);
  EXPECT_EQ(h->stored()->trialEndsAt, NOW + 5 * DAY);
  EXPECT_EQ(h->stored()->trialAt, NOW);
}

TEST(KeptTrial, IsSavedAfterADeactivateOrAnotherKeysActivationTheLicenceAsThatLeftIt) {
  StoredState deactivated;
  deactivated.trialStart = NOW - 9 * DAY;
  StoredState another;
  another.key = "SLUICE-BBBB-BBBB-BBBB-BBBB";
  another.activationId = "act_2";
  another.lease = "LB";
  another.checkedMajor = 1;
  for (const StoredState& theirs : {deactivated, another}) {
    const auto h = keptThen(just(theirs));
    h->license().status();
    const StoredState saved = *h->stored();
    EXPECT_EQ(saved.key, theirs.key);
    EXPECT_EQ(saved.activationId, theirs.activationId);
    EXPECT_EQ(saved.lease, theirs.lease);
    EXPECT_EQ(saved.trialStart, theirs.trialStart.value_or(NOW - 2 * DAY));
    EXPECT_EQ(saved.trialEndsAt, NOW + 5 * DAY);
    EXPECT_EQ(saved.trialAt, NOW);
  }
}

std::string trialLeaseFor(const Harness& h, const std::string& device, const char* kind = "machine") {
  const std::string claim = claimOf(device);
  LeaseSpec spec;
  spec.sub = "trial:" + claim;
  spec.aid = claim;
  spec.model = "trial";
  spec.lim = 1;
  spec.major = keygrant::kOpenMajor;
  spec.ttlSeconds = 7 * 24 * 3600;
  spec.extra = {{"fpk", kind}};
  return h.sign(spec);
}

StoredState trialOn(std::string lease, std::int64_t start) {
  StoredState s;
  s.trialStart = start;
  s.trialLease = std::move(lease);
  s.lastSeen = NOW;
  return s;
}

TEST(TrialLease, IsHeldToItsDeviceWhenStartedUnderAMachineId) {
  Harness mine;
  mine.store(trialOn(trialLeaseFor(mine, "fp-test"), NOW));
  EXPECT_EQ(mine.license().status().state, LicenseState::Trial);

  Harness theirs;
  theirs.store(trialOn(trialLeaseFor(theirs, "fp-another-machine"), NOW - 30 * DAY));
  EXPECT_EQ(theirs.license().status(), status(LicenseState::Expired, LicenseReason::Trial));

  // A host name is not held, whatever device it names.
  Harness host;
  host.store(trialOn(trialLeaseFor(host, "fp-another-machine", "hostname"), NOW - 30 * DAY));
  EXPECT_EQ(host.license().status().state, LicenseState::Trial);
}

TEST(TrialLease, IsNotJudgedOnARunThatCannotTellAndStartsNoTrialThere) {
  auto failing = std::make_shared<FakeFingerprinter>();
  failing->failing = true;
  HarnessOptions options;
  options.fingerprint = failing;
  Harness h(std::nullopt, options);
  h.store(trialOn(trialLeaseFor(h, "fp-another-machine"), NOW));
  EXPECT_EQ(h.license().status().state, LicenseState::Trial);
  Harness none(std::nullopt, options);
  none.license().startTrial();
  EXPECT_FALSE(none.http->called("trial"));
}

// --- a trial whose lease does not verify while its stored end is ahead ----------------------

/** A lease another key signed: the product's, before its key was rotated. */
std::string signedByAnotherKey(std::int64_t ends = NOW + 5 * DAY) { return trialLeaseTo(Harness(), ends); }

/** A trial stored with a lease signed by another key, its stored end five days ahead. */
std::unique_ptr<Harness> rotated(const std::function<void(StoredState&)>& over = nullptr) {
  auto h = std::make_unique<Harness>();
  StoredState state;
  state.trialStart = NOW - 2 * DAY;
  state.trialEndsAt = NOW + 5 * DAY;
  state.trialLease = signedByAnotherKey();
  state.lastSeen = NOW;
  if (over) over(state);
  h->store(state);
  return h;
}

TEST(TrialHeal, AsksForTheTrialAgainAndTheServersFreshLeaseRunsIt) {
  const auto h = rotated();
  const std::string fresh = trialLeaseTo(*h, NOW + 5 * DAY);
  h->http->reply("trial", 200, {{"trialStart", NOW - 2 * DAY}, {"trialEndsAt", NOW + 5 * DAY}, {"lease", fresh}});
  LicenseStatus expected = status(LicenseState::Trial);
  expected.trialEndsAt = NOW + 5 * DAY;
  expected.trialDaysLeft = 5;
  EXPECT_EQ(h->license().status(), expected);
  EXPECT_EQ(h->http->count("trial"), 1);
  // Asked as startTrial() asks, bounded as a status check is.
  EXPECT_EQ(h->http->bodyOf("trial"), json({{"fingerprint", "fp-test"}, {"clientStart", NOW - 2 * DAY}}));
  EXPECT_EQ(h->http->timeouts(), std::vector<Millis>{Millis{5'000}});
  EXPECT_EQ(h->stored()->trialLease, fresh);
  // Healed, so no wait is recorded: the next heal (a later rotation) is not held off.
  EXPECT_EQ(h->stored()->backoffSince, std::nullopt);
  EXPECT_EQ(h->stored()->backoffMs, std::nullopt);
  // Healed: the next status() needs no ask.
  h->license().status();
  EXPECT_EQ(h->http->count("trial"), 1);
}

TEST(TrialHeal, TimesTheWaitByTheDeviceClockNotTheGuardWhenTheClockWasSetBack) {
  // The guard (lastSeen) is a day ahead of this device's clock: a wait stamped
  // by the guard would read as not yet started, and every status() would pay the ask.
  const auto h = rotated([](StoredState& s) { s.lastSeen = NOW + DAY; });
  h->http->fail = true;
  EXPECT_EQ(h->license().status(), status(LicenseState::Expired, LicenseReason::Trial));
  EXPECT_EQ(h->stored()->backoffSince, NOW);
  EXPECT_EQ(h->stored()->backoffMs, 60'000);
  h->license().status();
  EXPECT_EQ(h->http->count("trial"), 1);
}

TEST(TrialHeal, HealsATrialLeaseSignedShortOfTheTrialsEnd) {
  // Seven days on a fourteen-day trial.
  Harness h;
  StoredState state;
  state.trialStart = NOW;
  state.trialEndsAt = NOW + 14 * DAY;
  state.trialLease = h.sign("trial:fp-test", "trial", 7 * 24 * 3600);
  state.lastSeen = NOW;
  h.store(state);
  h.setClock(NOW + 8 * DAY);
  const std::string fresh = h.sign("trial:fp-test", "trial", 6 * 24 * 3600, NOW + 8 * DAY);
  h.http->reply("trial", 200, {{"trialStart", NOW}, {"trialEndsAt", NOW + 14 * DAY}, {"lease", fresh}});
  const LicenseStatus s = h.license().status();
  EXPECT_EQ(s.state, LicenseState::Trial);
  EXPECT_EQ(s.trialDaysLeft, 6);
}

TEST(TrialHeal, AnswersExpiredAndWaitsAMinuteWhenTheServerCannotBeReached) {
  const auto h = rotated();
  h->http->fail = true;
  EXPECT_EQ(h->license().status(), status(LicenseState::Expired, LicenseReason::Trial));
  EXPECT_EQ(h->stored()->backoffSince, NOW);
  EXPECT_EQ(h->stored()->backoffMs, 60'000);
  // Within the wait, no ask: a status() offline does not pay a timeout each time.
  h->license().status();
  EXPECT_EQ(h->http->count("trial"), 1);
  h->setClock(NOW + 60'000);
  h->license().status();
  EXPECT_EQ(h->http->count("trial"), 2);
}

TEST(TrialHeal, WaitsAnHourWhenTheServerAnswersWithNothingThisBuildCanVerify) {
  // A build carrying a key the product no longer signs with: the answer cannot verify either.
  const auto h = rotated();
  const std::optional<std::string> stale = h->stored()->trialLease;
  // Signed with yet another key: the two leases differ byte for byte.
  const std::string stillStale = signedByAnotherKey();
  ASSERT_NE(stale, stillStale);
  h->http->reply("trial", 200, {{"trialStart", NOW - 2 * DAY}, {"trialEndsAt", NOW + 5 * DAY}, {"lease", stillStale}});
  EXPECT_EQ(h->license().status(), status(LicenseState::Expired, LicenseReason::Trial));
  EXPECT_EQ(h->stored()->backoffSince, NOW);
  EXPECT_EQ(h->stored()->backoffMs, 3'600'000);
  // Neither verifies, so nothing is kept over the server's: its lease is the one stored.
  EXPECT_EQ(h->stored()->trialLease, stillStale);
}

TEST(TrialHeal, TakesTheServersWordThatTheTrialIsOver) {
  const auto h = rotated();
  h->http->reply("trial", 200, {{"trialStart", NOW - 9 * DAY}, {"trialEndsAt", NOW - 2 * DAY}});
  EXPECT_EQ(h->license().status(), status(LicenseState::Expired, LicenseReason::Trial));
  EXPECT_EQ(h->stored()->trialEndsAt, NOW - 2 * DAY);
}

TEST(TrialHeal, AsksNothingOnceTheStoredEndHasPassedOrWhenTheLeaseVerifies) {
  const auto h = rotated([](StoredState& s) {
    s.trialStart = NOW - 9 * DAY;
    s.trialEndsAt = NOW - 1;
  });
  EXPECT_EQ(h->license().status(), status(LicenseState::Expired, LicenseReason::Trial));

  Harness valid;
  StoredState state;
  state.trialStart = NOW - 2 * DAY;
  state.trialEndsAt = NOW + 5 * DAY;
  state.trialLease = trialLeaseTo(valid, NOW + 5 * DAY);
  // Synced within the day, so no launch report is due either (reportLaunch).
  state.trialAt = NOW - DAY / 2;
  state.lastSeen = NOW;
  valid.store(state);
  EXPECT_EQ(valid.license().status().state, LicenseState::Trial);
  EXPECT_TRUE(valid.reportSettled());
  EXPECT_FALSE(h->http->called("trial"));
  EXPECT_FALSE(valid.http->called("trial"));
}

TEST(TrialHeal, AsksNothingWhenTheStoredEndIsExactlyNow) {
  // Asked only while the stored end is still ahead: an end of exactly now is not.
  const auto h = rotated([](StoredState& s) { s.trialEndsAt = NOW; });
  EXPECT_EQ(h->license().status(), status(LicenseState::Expired, LicenseReason::Trial));
  EXPECT_FALSE(h->http->called("trial"));
}

TEST(TrialHeal, IsAskedAtOnceByARefreshWhateverTheWait) {
  const auto h = rotated();
  h->http->fail = true;
  h->license().status();
  EXPECT_EQ(h->http->count("trial"), 1);
  h->http->fail = false;
  const std::string fresh = trialLeaseTo(*h, NOW + 5 * DAY);
  h->http->reply("trial", 200, {{"trialStart", NOW - 2 * DAY}, {"trialEndsAt", NOW + 5 * DAY}, {"lease", fresh}});
  // Inside the wait: not asked; forced: asked, and healed.
  EXPECT_EQ(h->license().status().state, LicenseState::Expired);
  EXPECT_EQ(h->http->count("trial"), 1);
  EXPECT_EQ(h->license().refresh().state, LicenseState::Trial);
  EXPECT_EQ(h->http->count("trial"), 2);
}

TEST(TrialHeal, IsNotAskedAgainByStartTrialForItsOwnAnswer) {
  // startTrial()'s answer stands as the server gave it: one ask, not a second to heal it.
  const auto h = rotated();
  h->http->reply("trial", 200,
                 {{"trialStart", NOW - 2 * DAY}, {"trialEndsAt", NOW + 5 * DAY}, {"lease", signedByAnotherKey()}});
  EXPECT_EQ(h->license().startTrial(), status(LicenseState::Expired, LicenseReason::Trial));
  EXPECT_EQ(h->http->count("trial"), 1);
}

}  // namespace
}  // namespace kgtest
