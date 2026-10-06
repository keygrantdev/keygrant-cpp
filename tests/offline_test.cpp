// The decisions made from what the device holds (sdk/src/offline.test.ts).
#include "clock.hpp"
#include "offline.hpp"
#include "support.hpp"

namespace kgtest {
namespace {

using namespace keygrant::detail;
using keygrant::Refusal;

StoredState refusal(Refusal refused, std::optional<std::int64_t> major = std::nullopt,
                    std::optional<std::vector<std::string>> refusedFor = std::nullopt) {
  StoredState s;
  s.refused = refused;
  s.refusedMajor = major;
  s.refusedFor = std::move(refusedFor);
  return s;
}

TEST(StandingRefusal, AKeyLevelOneForEveryBuildABuildLevelOneForThatBuildAndFurther) {
  struct Case {
    const char* name;
    StoredState state;
    std::int64_t major;
    std::optional<Refusal> expected;
  };
  const std::vector<Case> cases = {
      {"no refusal", StoredState{}, 3, std::nullopt},
      {"revoked, any build", refusal(Refusal::Revoked), 1, Refusal::Revoked},
      {"revoked, whatever major is stored", refusal(Refusal::Revoked, 4), 2, Refusal::Revoked},
      {"upgrade, at the refused major", refusal(Refusal::Upgrade, 4), 4, Refusal::Upgrade},
      {"upgrade, above it", refusal(Refusal::Upgrade, 4), 5, Refusal::Upgrade},
      {"upgrade, below it", refusal(Refusal::Upgrade, 4), 3, std::nullopt},
      {"outdated, at the refused major", refusal(Refusal::Outdated, 2), 2, Refusal::Outdated},
      {"outdated, below it", refusal(Refusal::Outdated, 2), 1, Refusal::Outdated},
      {"outdated, above it", refusal(Refusal::Outdated, 2), 3, std::nullopt},
      {"upgrade stored with no major (legacy), any build", refusal(Refusal::Upgrade), 1, Refusal::Upgrade},
      {"outdated stored with no major (legacy), any build", refusal(Refusal::Outdated), 9, Refusal::Outdated},
  };
  for (const Case& c : cases) EXPECT_EQ(standingRefusal(c.state, c.major, std::nullopt), c.expected) << c.name;
}

TEST(StandingRefusal, ADeviceRefusalStandsForItsDeviceWhileThisRunCanTell) {
  const StoredState forA = refusal(Refusal::Device, std::nullopt, std::vector<std::string>{"A"});
  EXPECT_EQ(standingRefusal(forA, 1, DeviceView{"A", true}), Refusal::Device);
  EXPECT_EQ(standingRefusal(forA, 1, DeviceView{"B", true}), std::nullopt);
  EXPECT_EQ(standingRefusal(forA, 1, DeviceView{std::nullopt, false}), std::nullopt);
  EXPECT_EQ(standingRefusal(forA, 1, std::nullopt), std::nullopt);
  // With no refusedFor (a state from before it), it stands for every device.
  EXPECT_EQ(standingRefusal(refusal(Refusal::Device), 1, DeviceView{"B", true}), Refusal::Device);
}

LeaseClaims lasting(std::int64_t lifeMs) {
  LeaseClaims c;
  c.iat = 1'757'000'000;
  c.exp = c.iat + lifeMs / 1000;
  return c;
}

TEST(RenewsEarly, WithinSevenDaysOrHalfTheLeasesLife) {
  const std::int64_t hour = 3'600'000;
  struct Case {
    const char* name;
    std::int64_t life;
    std::int64_t left;
    bool expected;
  };
  const std::vector<Case> cases = {
      {"30-day lease, exactly RENEW_WITHIN_MS left", 30 * DAY, kRenewWithinMs, false},
      {"30-day lease, RENEW_WITHIN_MS - 1 left", 30 * DAY, kRenewWithinMs - 1, true},
      {"7-day lease, exactly half its life left", 7 * DAY, 7 * DAY / 2, false},
      {"7-day lease, half its life - 1 left", 7 * DAY, 7 * DAY / 2 - 1, true},
      {"1-hour lease, exactly 30 minutes left", hour, hour / 2, false},
      {"1-hour lease, 30 minutes - 1 left", hour, hour / 2 - 1, true},
  };
  for (const Case& c : cases) {
    const LeaseClaims claims = lasting(c.life);
    EXPECT_EQ(renewsEarly(claims, claims.iat * 1000 + c.life - c.left), c.expected) << c.name;
  }
}

// --- entitlements in a status (leaseStatus, trialLeaseOf, trialStatusOf): only a
// lease that licenses, or a trial lease that runs a trial, carries them, and
// only when it grants any.

using Entitlements = std::map<std::string, keygrant::Entitlement>;

Entitlements pro() { return {{"edition", "pro"}, {"seats", 5}}; }

/** A licence lease's check that licenses, its claims granting `ent`. */
LeaseCheck licensing(Entitlements ent, bool test = false) {
  LeaseClaims claims = lasting(30 * DAY);
  claims.ent = std::move(ent);
  if (test) claims.test = true;
  LeaseCheck check;
  check.ok = true;
  check.test = test;
  check.claims = claims;
  return check;
}

TEST(LeaseStatus, LicensedCarriesTheLeasesEntitlementsAndItsTestFlag) {
  LicenseStatus expected = licensed(KEY);
  expected.entitlements = pro();
  EXPECT_EQ(leaseStatus(licensing(pro()), KEY), expected);
  expected.test = true;
  EXPECT_EQ(leaseStatus(licensing(pro(), true), KEY), expected);
}

TEST(LeaseStatus, LicensedByALeaseThatGrantsNoneIsTheStatusItWasBefore) {
  const LicenseStatus granted = leaseStatus(licensing({}), KEY);
  EXPECT_EQ(granted, licensed(KEY));
  EXPECT_TRUE(granted.entitlements.empty());
}

TEST(LeaseStatus, NotLicensedCarriesNoEntitlementsThoughTheLeaseGrantsThem) {
  struct Case {
    LeaseVerdict reason;
    LicenseStatus expected;
  };
  const std::vector<Case> cases = {
      {LeaseVerdict::Expired, status(LicenseState::Expired, LicenseReason::Offline, KEY)},
      {LeaseVerdict::Upgrade, status(LicenseState::Invalid, LicenseReason::Upgrade, KEY)},
      {LeaseVerdict::Outdated, status(LicenseState::Invalid, LicenseReason::Outdated, KEY)},
      {LeaseVerdict::Device, status(LicenseState::Invalid, LicenseReason::Device, KEY)},
      // Not this licence at all: no key either.
      {LeaseVerdict::Tampered, status(LicenseState::Invalid, LicenseReason::Tampered)},
  };
  for (const Case& c : cases) {
    LeaseCheck check = licensing(pro());
    check.ok = false;
    check.reason = c.reason;
    EXPECT_EQ(leaseStatus(check, KEY), c.expected) << to_string(c.reason);
  }
}

/** A trial lease's claims, started under a machine id (`claim-own`), granting pro(). */
LeaseClaims trialClaims() {
  LeaseClaims c = lasting(7 * DAY);
  c.sub = "trial:claim-own";
  c.product = PRODUCT;
  c.aid = "claim-own";
  c.fpk = DeviceKind::Machine;
  c.model = "trial";
  c.lim = 1;
  c.ent = pro();
  return c;
}

VerifyResult verifiedAs(LeaseClaims claims, std::optional<VerifyFailure> failure = std::nullopt) {
  VerifyResult v;
  v.valid = !failure;
  v.reason = failure;
  v.claims = std::move(claims);
  return v;
}

TEST(TrialLeaseOf, AValidTrialLeaseGivesTheEndItWasSignedToAndItsEntitlements) {
  const LeaseClaims claims = trialClaims();
  const TrialLease lease = trialLeaseOf(verifiedAs(claims), PRODUCT, {"claim-own"}, true);
  EXPECT_TRUE(lease.valid);
  EXPECT_EQ(lease.endsAt, claims.exp * 1000);
  EXPECT_EQ(lease.entitlements, pro());
}

TEST(TrialLeaseOf, ATrialLeaseNotValidHereSaysNothingOfItsEndOrItsEntitlements) {
  LeaseClaims licence = trialClaims();
  licence.model = "perpetual";
  struct Case {
    const char* name;
    VerifyResult verified;
    const char* product;
    std::vector<std::string> claims;
  };
  const std::vector<Case> cases = {
      {"expired", verifiedAs(trialClaims(), VerifyFailure::Expired), PRODUCT, {"claim-own"}},
      {"another product's", verifiedAs(trialClaims()), "maindeck", {"claim-own"}},
      {"another device's", verifiedAs(trialClaims()), PRODUCT, {"claim-other"}},
      {"a licence's", verifiedAs(licence), PRODUCT, {"claim-own"}},
  };
  for (const Case& c : cases) {
    const TrialLease lease = trialLeaseOf(c.verified, c.product, c.claims, true);
    EXPECT_FALSE(lease.valid) << c.name;
    EXPECT_EQ(lease.endsAt, std::nullopt) << c.name;
    EXPECT_TRUE(lease.entitlements.empty()) << c.name;
  }
}

TEST(TrialStatusOf, ARunningTrialsEntitlementsAreItsOwnCopyNotTheMapItWasGiven) {
  StoredState state;
  state.trialStart = NOW;
  state.trialLease = "stored";
  Entitlements given = pro();
  const LicenseStatus running = trialStatusOf(state, NOW, true, NOW + DAY, given);
  given.insert_or_assign("edition", "studio");
  EXPECT_EQ(running.entitlements, pro());
}

TEST(TrialStatusOf, ARunningTrialCarriesTheEntitlementsItIsGivenAnEndedOneNone) {
  StoredState state;
  state.trialStart = NOW;
  state.trialLease = "stored";
  LicenseStatus running = status(LicenseState::Trial);
  running.trialEndsAt = NOW + 3 * DAY;
  running.trialDaysLeft = 3;
  EXPECT_EQ(trialStatusOf(state, NOW, true, NOW + 3 * DAY), running);
  EXPECT_EQ(trialStatusOf(state, NOW, true, NOW + 3 * DAY, {}), running);
  LicenseStatus granted = running;
  granted.entitlements = pro();
  EXPECT_EQ(trialStatusOf(state, NOW, true, NOW + 3 * DAY, pro()), granted);
  EXPECT_EQ(trialStatusOf(state, NOW, false, std::nullopt, pro()), status(LicenseState::Expired, LicenseReason::Trial));
  EXPECT_EQ(trialStatusOf(StoredState{}, NOW, true, NOW + 3 * DAY, pro()), status(LicenseState::Unlicensed));
}

TEST(AskDue, ForcedNeverWaitedClockSetBackOrTheWaitOver) {
  const std::int64_t since = 1'757'000'000'000;
  const auto waiting = [&](std::optional<std::int64_t> length) {
    StoredState s;
    s.backoffSince = since;
    s.backoffMs = length;
    return s;
  };
  EXPECT_TRUE(askDue(waiting(kAskAgainMs), since, true));
  EXPECT_TRUE(askDue(StoredState{}, since, false));
  EXPECT_TRUE(askDue(waiting(kAskAgainMs), since - 1, false));
  EXPECT_FALSE(askDue(waiting(kRetryUnansweredMs), since + kRetryUnansweredMs - 1, false));
  EXPECT_TRUE(askDue(waiting(kRetryUnansweredMs), since + kRetryUnansweredMs, false));
  EXPECT_FALSE(askDue(waiting(kAskAgainMs), since + kAskAgainMs - 1, false));
  EXPECT_TRUE(askDue(waiting(kAskAgainMs), since + kAskAgainMs, false));
  // No length stored: the hour.
  EXPECT_FALSE(askDue(waiting(std::nullopt), since + kAskAgainMs - 1, false));
  EXPECT_TRUE(askDue(waiting(std::nullopt), since + kAskAgainMs, false));
}

StoredState stamped(std::optional<std::string> activationId, std::optional<std::int64_t> verdictAt) {
  StoredState s;
  s.activationId = std::move(activationId);
  s.verdictAt = verdictAt;
  return s;
}

TEST(Superseded, AnAnswerOutOfDateByTheTimeItIsSaved) {
  const Asked asked{1'000, "act_1", 500};
  EXPECT_EQ(superseded(stamped("act_1", 500), asked), OutOfDate::No);
  EXPECT_EQ(superseded(stamped("act_1", 2'000), asked), OutOfDate::Answer);
  EXPECT_EQ(superseded(stamped("act_1", 800), asked), OutOfDate::No);
  EXPECT_EQ(superseded(stamped("act_1", 1'000), asked), OutOfDate::No);
  EXPECT_EQ(superseded(stamped("act_2", 500), asked), OutOfDate::Activation);
  EXPECT_EQ(superseded(stamped(std::nullopt, 500), asked), OutOfDate::Activation);
  // One process's own answer over the one it saw, even stamped later (a clock put back).
  EXPECT_EQ(superseded(stamped("act_1", 2'000), Asked{1'000, "act_1", 2'000}), OutOfDate::No);
  // An ask that saw none: no times compared when none was stored since.
  EXPECT_EQ(superseded(stamped("act_1", std::nullopt), Asked{1'000, "act_1", std::nullopt}), OutOfDate::No);
  EXPECT_EQ(superseded(stamped("act_1", 2'000), Asked{1'000, "act_1", std::nullopt}), OutOfDate::Answer);
}

TEST(Merged, LeavesOutTheVerdictOfAnOlderAnswerAndKeepsWhatTheDeviceKnows) {
  StatePatch patch;
  patch.lease = Change<std::string>::set("L");
  patch.refused = Change<Refusal>::set(Refusal::Revoked);
  patch.refusedMajor = Change<std::int64_t>::set(2);
  patch.refusedFor = Change<std::vector<std::string>>::set({"c"});
  patch.backoffSince = Change<std::int64_t>::set(1);
  patch.backoffMs = Change<std::int64_t>::set(2);
  patch.checkedMajor = Change<std::int64_t>::set(3);
  patch.verdictAt = Change<std::int64_t>::set(4);
  patch.keptMajor = Change<std::int64_t>::set(5);
  patch.deviceKind = Change<DeviceKind>::set(DeviceKind::Machine);
  StatePatch kept;
  kept.keptMajor = Change<std::int64_t>::set(5);
  kept.deviceKind = Change<DeviceKind>::set(DeviceKind::Machine);
  EXPECT_EQ(withoutVerdict(patch), kept);
}

StatePatch answerPatch() {
  StatePatch p;
  p.lease = Change<std::string>::set("LA");
  p.checkedMajor = Change<std::int64_t>::set(3);
  p.keptMajor = Change<std::int64_t>::set(3);
  p.deviceKind = Change<DeviceKind>::set(DeviceKind::Machine);
  p.trialStart = Change<std::int64_t>::set(100);
  p.trialEndsAt = Change<std::int64_t>::set(900);
  p.trialLease = Change<std::string>::set("T");
  return p;
}

Basis answerTo(Asked asked) {
  Basis b;
  b.answer = asked;
  return b;
}

TEST(Merged, KeepsOnlyTheTrialEvidenceOfAnAnswerAboutAnActivationNoLongerStored) {
  StoredState deactivated;
  deactivated.trialStart = 200;
  StoredState other;
  other.key = "B";
  other.activationId = "act_2";
  other.lease = "LB";
  other.deviceKind = DeviceKind::Hostname;
  other.trialStart = 200;
  for (const StoredState& latest : {deactivated, other}) {
    StoredState expected = latest;
    expected.trialStart = 100;
    expected.trialEndsAt = 900;
    expected.trialLease = "T";
    EXPECT_EQ(merged(latest, answerPatch(), answerTo(Asked{1'000, "act_1", 500})), expected);
  }
}

TEST(Merged, KeepsNothingOfANoteAboutAnActivationNoLongerStored) {
  StatePatch note;
  note.keptMajor = Change<std::int64_t>::set(3);
  Basis about;
  about.about = About{"act_1"};
  StoredState other;
  other.activationId = "act_2";
  EXPECT_EQ(merged(other, note, about), other);
  StoredState same;
  same.activationId = "act_1";
  StoredState noted = same;
  noted.keptMajor = 3;
  EXPECT_EQ(merged(same, note, about), noted);
  EXPECT_EQ(merged(StoredState{}, note, about), StoredState{});
}

TEST(Merged, KeepsTheVerdictOfANewerAnswerAndSavesTheRestOfAnOlderOne) {
  StoredState latest;
  latest.activationId = "act_1";
  latest.refused = Refusal::Revoked;
  latest.verdictAt = 2'000;
  StoredState expected = latest;
  expected.keptMajor = 3;
  expected.deviceKind = DeviceKind::Machine;
  expected.trialStart = 100;
  expected.trialEndsAt = 900;
  expected.trialLease = "T";
  EXPECT_EQ(merged(latest, answerPatch(), answerTo(Asked{1'000, "act_1", 500})), expected);
}

TEST(Merged, AnActivationsAnswerLandsAsAnAnswerOnTheSameSeatAndWholeOnAnother) {
  StatePatch activated;
  activated.key = Change<std::string>::set("K");
  activated.activationId = Change<std::string>::set("act_1");
  activated.lease = Change<std::string>::set("LA");
  activated.checkedMajor = Change<std::int64_t>::set(2);
  activated += leasedPatch();
  activated.deviceKind = Change<DeviceKind>::set(DeviceKind::Machine);
  StoredState refusedLater;
  refusedLater.key = "K";
  refusedLater.activationId = "act_1";
  refusedLater.refused = Refusal::Revoked;
  refusedLater.verdictAt = 2'000;

  Basis older;
  older.activation = Asked{1'000, "act_1", 500};
  StoredState kept = refusedLater;
  kept.deviceKind = DeviceKind::Machine;
  EXPECT_EQ(merged(refusedLater, activated, older), kept);

  Basis sawIt;
  sawIt.activation = Asked{1'000, "act_1", 2'000};
  const StoredState whole = merged(refusedLater, activated, sawIt);
  EXPECT_EQ(whole.lease, "LA");
  EXPECT_EQ(whole.verdictAt, 1'000);
  EXPECT_EQ(whole.refused, std::nullopt);

  activated.activationId = Change<std::string>::set("act_2");
  Basis another;
  another.activation = Asked{1'000, "act_2", std::nullopt};
  const StoredState replaced = merged(refusedLater, activated, another);
  EXPECT_EQ(replaced.activationId, "act_2");
  EXPECT_EQ(replaced.refused, std::nullopt);
}

TEST(Merged, AKeptMajorOnlyGoesUpForTheStoredActivation) {
  const auto keptAfter = [](std::int64_t stored, std::optional<std::int64_t> noted, const Basis& basis) {
    StoredState latest;
    latest.activationId = "act_1";
    latest.keptMajor = stored;
    StatePatch patch;
    patch.keptMajor = Change<std::int64_t>::of(noted);
    return merged(latest, patch, basis).keptMajor;
  };
  Basis about;
  about.about = About{"act_1"};
  Basis answer;
  answer.answer = Asked{1, "act_1", std::nullopt};
  Basis fresh;
  fresh.activation = Asked{1, "act_2", std::nullopt};
  Basis sameSeat;
  sameSeat.activation = Asked{1, "act_1", std::nullopt};
  EXPECT_EQ(keptAfter(5, 3, about), 5);
  EXPECT_EQ(keptAfter(3, 5, about), 5);
  EXPECT_EQ(keptAfter(5, std::nullopt, answer), 5);
  EXPECT_EQ(keptAfter(5, std::nullopt, Basis{}), std::nullopt);
  EXPECT_EQ(keptAfter(5, std::nullopt, fresh), std::nullopt);
  EXPECT_EQ(keptAfter(5, 3, sameSeat), 5);
}

TEST(Merged, StampsAnAnswerAndATrialAnswerWithWhenTheyWereAskedFor) {
  StoredState latest;
  latest.activationId = "act_1";
  StatePatch lease;
  lease.lease = Change<std::string>::set("L");
  EXPECT_EQ(merged(latest, lease, answerTo(Asked{1'000, "act_1", 500})).verdictAt, 1'000);
  StatePatch trial;
  trial.trialEndsAt = Change<std::int64_t>::set(9);
  Basis basis;
  basis.trial = Stamp{7, std::nullopt};
  StoredState expected;
  expected.trialEndsAt = 9;
  expected.trialAt = 7;
  EXPECT_EQ(merged(StoredState{}, trial, basis), expected);
}

TEST(Merged, ATrialAnswerTakesItsEndAndLeaseTogetherFromTheNewest) {
  StoredState stored;
  stored.trialStart = 50;
  stored.trialEndsAt = 500;
  stored.trialLease = "NEWER";
  stored.trialAt = 2'000;
  const auto trialPatch = [](std::int64_t end, const char* lease) {
    StatePatch p;
    p.trialStart = Change<std::int64_t>::set(60);
    p.trialEndsAt = Change<std::int64_t>::set(end);
    p.trialLease = Change<std::string>::set(lease);
    return p;
  };
  Basis older;
  older.trial = Stamp{1'000, std::nullopt};
  for (const std::int64_t end : {400, 600}) EXPECT_EQ(merged(stored, trialPatch(end, "OLDER"), older), stored);

  Basis newer;
  newer.trial = Stamp{3'000, std::nullopt};
  StoredState extended = stored;
  extended.trialEndsAt = 900;
  extended.trialLease = "EXT";
  extended.trialAt = 3'000;
  EXPECT_EQ(merged(stored, trialPatch(900, "EXT"), newer), extended);

  // Over the one it saw, even stamped later (a clock put back).
  Basis own;
  own.trial = Stamp{1'000, 2'000};
  const StoredState mine = merged(stored, trialPatch(300, "MINE"), own);
  EXPECT_EQ(mine.trialEndsAt, 300);
  EXPECT_EQ(mine.trialAt, 1'000);
}

TEST(Merged, TheTrialStartOnlyMovesEarlierWhoeverSavedIt) {
  StoredState latest;
  latest.trialStart = 100;
  StatePatch later;
  later.trialStart = Change<std::int64_t>::set(200);
  EXPECT_EQ(merged(latest, later).trialStart, 100);
  StatePatch clear;
  clear.trialStart = Change<std::int64_t>::clear();
  EXPECT_EQ(merged(latest, clear).trialStart, 100);
  StatePatch earlier;
  earlier.trialStart = Change<std::int64_t>::set(50);
  EXPECT_EQ(merged(latest, earlier).trialStart, 50);
}

TEST(ClockCorrection, BringsTheGuardBackOnlyOnTheServersEvidence) {
  const SignedAt fresh{KEY, "act_1", NOW / 1000};
  const auto correct = [&](std::optional<std::int64_t> lastSeen, std::optional<SignedAt> claims,
                           std::optional<std::int64_t> heldIat, std::int64_t clockNow = NOW) {
    return clockCorrection(clockNow, lastSeen, claims, heldIat, KEY, "act_1");
  };
  EXPECT_EQ(correct(NOW + kClockToleranceMs, fresh, std::nullopt), std::nullopt);
  EXPECT_EQ(correct(NOW + kClockToleranceMs + 1, fresh, std::nullopt), NOW);
  EXPECT_EQ(correct(NOW + DAY, fresh, std::nullopt, NOW - 3'600'000), NOW);  // never below the signing time
  EXPECT_EQ(correct(NOW + DAY, fresh, NOW / 1000 + 1), std::nullopt);       // older than the lease held
  EXPECT_EQ(correct(NOW + DAY, SignedAt{"sluice-aaaa-bbbb-cccc-dddd", "act_1", NOW / 1000}, std::nullopt), NOW);
  EXPECT_EQ(correct(NOW + DAY, SignedAt{KEY, "act_2", NOW / 1000}, std::nullopt), std::nullopt);
  EXPECT_EQ(correct(NOW + DAY, std::nullopt, std::nullopt), std::nullopt);
  EXPECT_EQ(guardedNow(NOW - 5, NOW), NOW);
  EXPECT_EQ(guardedNow(NOW, std::nullopt), NOW);
}

}  // namespace
}  // namespace kgtest
